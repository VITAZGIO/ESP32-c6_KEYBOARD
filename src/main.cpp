/*
 * AULA F75 — Zigbee-кнопки внутри клавиатуры
 * ESP32-C6, Zigbee End Device (питание от аккумулятора клавиатуры)
 *
 * Три режима — всё решает то, что было с End ДО рабочего зажатия.
 * Цифры везде срабатывают мгновенно.
 *
 *   просто зажал End + цифра 1..6                 -> btn1..btn6
 *   тап End (коротко), отпустил, зажал + цифра    -> btn1_dbl..btn6_dbl
 *   держал End 2 с, отпустил, зажал + цифры       -> сервисный режим:
 *       любые 2 из цифр 1,2,3  -> сброс сети Zigbee
 *       любые 2 из цифр 4,5,6  -> режим OTA (Wi-Fi, прошивка по воздуху)
 *
 * Флаг взводится в момент ОТПУСКАНИЯ End и живёт 3 секунды. Если во время
 * удержания нажимались цифры — это была обычная работа, флаг не взводится.
 *
 * VITAZGIO / AulaKeys
 */

#include "Zigbee.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

// Пароли Wi-Fi и OTA лежат в secrets.h — он в .gitignore.
// Нет файла? Скопируй src/secrets.example.h в src/secrets.h и впиши свои данные.
#include "secrets.h"

// --------------------------- ПИНЫ ---------------------------

#define END_SCAN    5     // сканирующая линия End
#define END_READ    4     // линия чтения End
#define DIGIT_SCAN  6     // сканирующая линия цифрового ряда (общая)
#define BAT_ADC     1     // ADC: делитель 20k/20k с аккумулятора

const int DIGIT_PINS[6] = {7, 10, 21, 20, 19, 18};   // цифры 1..6
#define N_DIGITS 6

// ------------------------- НАСТРОЙКИ ------------------------

#define POLL_MS         20      // период опроса матрицы
#define DEBOUNCE_CYCLES 1       // подтверждений для цифр (1 x 20 мс)
                                // больше — теряются быстрые нажатия
#define MIN_SCANS       6       // меньше сканов за цикл — выборка недостоверна
#define END_PRESS_CYCLES   1    // нажатие End: 1 цикл, иначе теряется быстрый тап
#define END_RELEASE_CYCLES 6    // отпускание: 6 циклов, иначе удержание рвётся
                                // в момент нажатия цифры
#define TAP_MAX_MS      600     // тап End: короче этого = заявка на второй ряд
#define SERVICE_HOLD_MS 2000    // удержание End дольше этого = заявка на настройки
#define ALT_WINDOW_MS   3000    // окно между тапом и повторным зажатием
#define SERVICE_WINDOW_MS 3000  // окно между удержанием и повторным зажатием
#define PULSE_MS        300     // сколько держать эндпоинт в TRUE
#define OFF_RETRIES     2       // повторные OFF (Zigbee-отчёты теряются)
#define OFF_RETRY_MS    900     // интервал между повторными OFF
#define SWEEP_MS        30000   // уборка залипших состояний
#define REFIRE_MS       400     // минимум между срабатываниями одной кнопки
#define HEARTBEAT_MS    300000  // отчёт в сеть раз в 5 мин (батарея + linkquality)
#define BAT_MIN_MV      3300    // 0% заряда
#define BAT_MAX_MV      4200    // 100% заряда
#define CHORD_MIN_KEYS  1       // хватит одной цифры из группы
#define MAX_SIMULTANEOUS 2      // больше цифр разом — не кнопка, игнорируем

// ----------------------- НАСТРОЙКИ OTA ----------------------
// WIFI_SSID, WIFI_PASS и OTA_PASSWORD — в secrets.h

#define OTA_HOSTNAME  "aulakeys"
#define OTA_TIMEOUT_MS 600000   // 10 минут, потом сам вернётся в Zigbee

// Статический IP в режиме OTA. Чтобы брать адрес от роутера (DHCP),
// поставь OTA_STATIC_IP в 0.
#define OTA_STATIC_IP 1
#define OTA_IP        192, 168, 1, 210
#define OTA_GATEWAY   192, 168, 1, 1
#define OTA_SUBNET    255, 255, 255, 0

// Сон. Сначала отладь БЕЗ сна (0), потом включи (1).
#define USE_LIGHT_SLEEP 0
#define IDLE_BEFORE_SLEEP_MS 3000

// -------------------- ZIGBEE ЭНДПОИНТЫ ----------------------
// 10..15 — базовые (зажал End), 20..25 — dbl (тап + зажал)

ZigbeeOccupancySensor zbB1(10), zbB2(11), zbB3(12), zbB4(13), zbB5(14), zbB6(15);
ZigbeeOccupancySensor zbA1(20), zbA2(21), zbA3(22), zbA4(23), zbA5(24), zbA6(25);

ZigbeeOccupancySensor *epBase[N_DIGITS] = {&zbB1, &zbB2, &zbB3, &zbB4, &zbB5, &zbB6};
ZigbeeOccupancySensor *epAlt [N_DIGITS] = {&zbA1, &zbA2, &zbA3, &zbA4, &zbA5, &zbA6};

// ---------------- СЧЁТЧИКИ ИЗ ПРЕРЫВАНИЙ --------------------

volatile uint32_t endScanCount = 0, endLowCount = 0;
volatile uint32_t digitScanCount = 0;
volatile uint32_t digitLowCount[N_DIGITS] = {0};
volatile uint32_t lastEdgeMs = 0;

void IRAM_ATTR onEndScan() {
  endScanCount = endScanCount + 1;
  lastEdgeMs = millis();
  if (!digitalRead(END_READ)) endLowCount = endLowCount + 1;
}

void IRAM_ATTR onDigitScan() {
  digitScanCount = digitScanCount + 1;
  lastEdgeMs = millis();
  for (int i = 0; i < N_DIGITS; i++) {
    if (!digitalRead(DIGIT_PINS[i])) digitLowCount[i] = digitLowCount[i] + 1;
  }
}

// -------------------- СОСТОЯНИЕ ЛОГИКИ ----------------------

Preferences prefs;
bool otaMode = false;

// что было с End до текущего зажатия
enum PendingMode { PEND_NONE, PEND_DBL, PEND_SERVICE };
PendingMode pending = PEND_NONE;
uint32_t pendingAt = 0;        // когда взвели флаг

// режим текущего зажатия End
enum Mode { MODE_BASE, MODE_DBL, MODE_SERVICE };
Mode mode = MODE_BASE;

bool endHeld = false;
uint8_t endHit = 0, endMiss = 0;
uint32_t endDownAt = 0;
bool sessionUsed = false;      // в этом удержании уже жали цифры

bool digitPressed[N_DIGITS] = {false};
uint8_t digitHit[N_DIGITS] = {0};
uint8_t digitMiss[N_DIGITS] = {0};
bool comboActive[N_DIGITS] = {false};

uint32_t pulseOffBase[N_DIGITS] = {0};
uint32_t pulseOffAlt [N_DIGITS] = {0};

bool sentOnBase[N_DIGITS] = {false};
bool sentOnAlt [N_DIGITS] = {false};
uint8_t retryBase[N_DIGITS] = {0};
uint8_t retryAlt [N_DIGITS] = {0};
uint32_t retryAtBase[N_DIGITS] = {0};
uint32_t retryAtAlt [N_DIGITS] = {0};
uint32_t lastFireBase[N_DIGITS] = {0};
uint32_t lastFireAlt [N_DIGITS] = {0};

uint32_t lastSweepMs = 0;
uint32_t lastBeatMs = 0;
bool wasConnected = false;

// ------------------------- УТИЛИТЫ --------------------------

bool isActive(uint32_t lowCnt, uint32_t totalCnt) {
  if (totalCnt < MIN_SCANS) return false;
  return (lowCnt * 100 / totalCnt) > 50;
}

uint8_t readBatteryPercent() {
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) { acc += analogReadMilliVolts(BAT_ADC); delay(2); }
  uint32_t mv = (acc / 8) * 2;              // делитель 1:2
  if (mv <= BAT_MIN_MV) return 0;
  if (mv >= BAT_MAX_MV) return 100;
  return (uint8_t)((mv - BAT_MIN_MV) * 100 / (BAT_MAX_MV - BAT_MIN_MV));
}

void sendOff(int idx, bool alt) {
  if (alt) {
    epAlt[idx]->setOccupancy(false);
    epAlt[idx]->report();
    sentOnAlt[idx] = false;
  } else {
    epBase[idx]->setOccupancy(false);
    epBase[idx]->report();
    sentOnBase[idx] = false;
  }
}

void fireButton(int idx, bool alt) {
  uint32_t now = millis();
  if (alt) {
    if (now - lastFireAlt[idx] < REFIRE_MS) return;
    lastFireAlt[idx] = now;
    epAlt[idx]->setOccupancy(true);
    epAlt[idx]->report();
    sentOnAlt[idx] = true;
    pulseOffAlt[idx] = now + PULSE_MS;
    retryAlt[idx] = 0;
  } else {
    if (now - lastFireBase[idx] < REFIRE_MS) return;
    lastFireBase[idx] = now;
    epBase[idx]->setOccupancy(true);
    epBase[idx]->report();
    sentOnBase[idx] = true;
    pulseOffBase[idx] = now + PULSE_MS;
    retryBase[idx] = 0;
  }
  Serial.printf(">>> BTN %d %s\n", idx + 1, alt ? "DBL" : "BASE");
}

void releasePulses() {
  uint32_t now = millis();
  for (int i = 0; i < N_DIGITS; i++) {
    if (pulseOffBase[i] && now >= pulseOffBase[i]) {
      sendOff(i, false);
      pulseOffBase[i] = 0;
      retryBase[i] = OFF_RETRIES;
      retryAtBase[i] = now + OFF_RETRY_MS;
    }
    if (pulseOffAlt[i] && now >= pulseOffAlt[i]) {
      sendOff(i, true);
      pulseOffAlt[i] = 0;
      retryAlt[i] = OFF_RETRIES;
      retryAtAlt[i] = now + OFF_RETRY_MS;
    }
    if (retryBase[i] && now >= retryAtBase[i] && !pulseOffBase[i]) {
      sendOff(i, false);
      retryBase[i]--;
      retryAtBase[i] = now + OFF_RETRY_MS;
    }
    if (retryAlt[i] && now >= retryAtAlt[i] && !pulseOffAlt[i]) {
      sendOff(i, true);
      retryAlt[i]--;
      retryAtAlt[i] = now + OFF_RETRY_MS;
    }
  }
}

void sweepStuck() {
  for (int i = 0; i < N_DIGITS; i++) {
    if (sentOnBase[i] && !pulseOffBase[i]) {
      Serial.printf("[FIX] залипла BASE %d\n", i + 1);
      sendOff(i, false);
    }
    if (sentOnAlt[i] && !pulseOffAlt[i]) {
      Serial.printf("[FIX] залипла DBL %d\n", i + 1);
      sendOff(i, true);
    }
  }
}

void heartbeat() {
  uint8_t bat = readBatteryPercent();
  epBase[0]->setBatteryPercentage(bat);
  epBase[0]->reportBatteryPercentage();
  Serial.printf("[BEAT] bat %u%%  zb %s\n", bat, Zigbee.connected() ? "online" : "OFFLINE");
}

// ставим флаг и перезагружаемся в режим прошивки по воздуху
void rebootToOta() {
  Serial.println("[OTA] запрошен режим прошивки, перезагрузка");
  prefs.begin("aula", false);
  prefs.putBool("ota", true);
  prefs.end();
  delay(300);
  ESP.restart();
}

// --------------------- РЕЖИМ OTA (Wi-Fi) --------------------

void runOtaMode() {
  Serial.println("\n[OTA] режим прошивки по воздуху");

  // флаг сразу снимаем: если что-то пойдёт не так, следующий старт обычный
  prefs.begin("aula", false);
  prefs.putBool("ota", false);
  prefs.end();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(200);

  Serial.printf("[OTA] MAC: %s\n", WiFi.macAddress().c_str());

#if OTA_STATIC_IP
  IPAddress ip(OTA_IP), gw(OTA_GATEWAY), mask(OTA_SUBNET);
  if (!WiFi.config(ip, gw, mask, gw)) {
    Serial.println("[OTA] статический IP задать не вышло, беру от роутера");
  }
#endif

  // Сканируем эфир: если список пустой — радио занято Zigbee,
  // если сеть видна, но коннекта нет — дело в пароле или роутере.
  Serial.println("[OTA] сканирую сети...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[OTA] сетей не видно вообще — радио не отдано Wi-Fi");
  } else {
    Serial.printf("[OTA] найдено сетей: %d\n", n);
    bool found = false;
    for (int i = 0; i < n; i++) {
      bool mine = (WiFi.SSID(i) == String(WIFI_SSID));
      if (mine) found = true;
      Serial.printf("   %s%s  rssi %d  ch %d\n",
                    mine ? "* " : "  ",
                    WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
    Serial.println(found ? "[OTA] наша сеть в эфире есть"
                         : "[OTA] нашей сети в списке НЕТ");
  }
  WiFi.scanDelete();

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[OTA] подключаюсь");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    int st = WiFi.status();
    const char *why = "неизвестно";
    switch (st) {
      case WL_NO_SSID_AVAIL:  why = "сеть не найдена"; break;
      case WL_CONNECT_FAILED: why = "пароль не подошёл"; break;
      case WL_CONNECTION_LOST:why = "связь потеряна"; break;
      case WL_DISCONNECTED:   why = "отключено роутером"; break;
      case WL_IDLE_STATUS:    why = "радио не стартовало"; break;
    }
    Serial.printf("[OTA] Wi-Fi не поднялся: %s (код %d)\n", why, st);
    Serial.println("[OTA] возврат в Zigbee");
    delay(3000);
    ESP.restart();
  }

  Serial.printf("\n[OTA] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[OTA] хост: %s.local, пароль: %s\n", OTA_HOSTNAME, OTA_PASSWORD);
  Serial.println("[OTA] жду прошивку 10 минут");

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { Serial.println("[OTA] приём прошивки"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] готово, перезагрузка"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] ошибка %u\n", e); });

  ArduinoOTA.begin();

  uint32_t started = millis();
  while (millis() - started < OTA_TIMEOUT_MS) {
    ArduinoOTA.handle();
    delay(10);
  }

  Serial.println("[OTA] время вышло, возврат в Zigbee");
  ESP.restart();
}

// -------------------------- SETUP ---------------------------

void setup() {
  Serial.begin(115200);
  delay(300);

  // проверяем флаг до инициализации Zigbee
  prefs.begin("aula", true);
  otaMode = prefs.getBool("ota", false);
  prefs.end();

  if (otaMode) {
    runOtaMode();     // не возвращается, внутри ESP.restart()
    return;
  }

  Serial.println("\n[AULA] boot");

  pinMode(END_SCAN, INPUT);
  pinMode(END_READ, INPUT);
  pinMode(DIGIT_SCAN, INPUT);
  for (int i = 0; i < N_DIGITS; i++) pinMode(DIGIT_PINS[i], INPUT);

  analogReadResolution(12);
  analogRead(BAT_ADC);                        // создаём канал АЦП
  analogSetPinAttenuation(BAT_ADC, ADC_11db); // теперь можно задать диапазон

  attachInterrupt(END_SCAN, onEndScan, FALLING);
  attachInterrupt(DIGIT_SCAN, onDigitScan, FALLING);

  uint8_t bat = readBatteryPercent();

  for (int i = 0; i < N_DIGITS; i++) {
    epBase[i]->setManufacturerAndModel("VITAZGIO", "AulaKeys");
    epBase[i]->setPowerSource(ZB_POWER_SOURCE_BATTERY, bat);
    Zigbee.addEndpoint(epBase[i]);

    epAlt[i]->setManufacturerAndModel("VITAZGIO", "AulaKeys");
    Zigbee.addEndpoint(epAlt[i]);
  }

  Zigbee.setTimeout(10000);

  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    Serial.println("[ZB] begin failed, reboot");
    delay(2000);
    ESP.restart();
  }

  Serial.print("[ZB] connecting");
  uint32_t t0 = millis();
  while (!Zigbee.connected() && millis() - t0 < 30000) {
    Serial.print(".");
    delay(500);
  }

  wasConnected = Zigbee.connected();
  Serial.println(wasConnected ? "\n[ZB] connected" : "\n[ZB] offline");

  if (wasConnected) {
    delay(1000);
    heartbeat();
  }

  lastBeatMs = millis();
  lastSweepMs = millis();

  // на старте гасим всё — вдруг после ребута в z2m остались включённые
  for (int i = 0; i < N_DIGITS; i++) { sendOff(i, false); sendOff(i, true); }

  Serial.println("[AULA] ready");
}

// --------------------------- LOOP ---------------------------

void loop() {
  if (otaMode) { delay(1000); return; }

  uint32_t now = millis();

  // снимок счётчиков
  noInterrupts();
  uint32_t eS = endScanCount, eL = endLowCount, dS = digitScanCount;
  uint32_t dL[N_DIGITS];
  for (int i = 0; i < N_DIGITS; i++) dL[i] = digitLowCount[i];
  endScanCount = 0; endLowCount = 0; digitScanCount = 0;
  for (int i = 0; i < N_DIGITS; i++) digitLowCount[i] = 0;
  uint32_t lastEdge = lastEdgeMs;
  interrupts();

  // -- End: режим текущего зажатия определяет взведённый флаг --
  bool endMatch = isActive(eL, eS);
  if (endMatch) { endHit++; endMiss = 0; } else { endMiss++; endHit = 0; }

  // зажали End
  if (!endHeld && endHit >= END_PRESS_CYCLES) {
    endHeld = true;
    endDownAt = now;
    sessionUsed = false;

    if (pending == PEND_DBL)          { mode = MODE_DBL;     Serial.println("[MODE] второй ряд"); }
    else if (pending == PEND_SERVICE) { mode = MODE_SERVICE; Serial.println("[MODE] НАСТРОЙКИ"); }
    else                                mode = MODE_BASE;

    pending = PEND_NONE;   // флаг одноразовый
  }

  // отпустили End — решаем, что взвести на следующий раз
  if (endHeld && endMiss >= END_RELEASE_CYCLES) {
    endHeld = false;
    uint32_t heldFor = now - endDownAt;

    if (sessionUsed) {
      pending = PEND_NONE;                      // жали цифры — обычная работа
    } else if (heldFor < TAP_MAX_MS) {
      pending = PEND_DBL;  pendingAt = now;
      Serial.println("[MODE] тап -> жду второй ряд");
    } else if (heldFor >= SERVICE_HOLD_MS) {
      pending = PEND_SERVICE;  pendingAt = now;
      Serial.println("[MODE] удержание -> жду настройки");
    } else {
      pending = PEND_NONE;                      // между 0.6 и 2 с — ничего
    }
    mode = MODE_BASE;
  }

  // флаг протух
  if (!endHeld && pending != PEND_NONE) {
    uint32_t window = (pending == PEND_DBL) ? ALT_WINDOW_MS : SERVICE_WINDOW_MS;
    if (now - pendingAt >= window) {
      pending = PEND_NONE;
      Serial.println("[MODE] окно закрылось");
    }
  }

  // -- цифры: обновляем состояние всех --
  uint8_t nPressed = 0;
  for (int i = 0; i < N_DIGITS; i++) {
    bool m = isActive(dL[i], dS);
    if (m) { digitHit[i]++; digitMiss[i] = 0; } else { digitMiss[i]++; digitHit[i] = 0; }

    if (!digitPressed[i] && digitHit[i] >= DEBOUNCE_CYCLES) digitPressed[i] = true;
    if (digitPressed[i] && digitMiss[i] >= DEBOUNCE_CYCLES) digitPressed[i] = false;

    if (digitPressed[i]) nPressed++;
  }

  // -- сервисный режим: цифры = аккорд, в сеть ничего не шлём --
  if (mode == MODE_SERVICE && endHeld) {
    uint8_t lowGroup  = 0;   // цифры 1,2,3 -> сброс сети Zigbee
    uint8_t highGroup = 0;   // цифры 4,5,6 -> режим OTA
    for (int i = 0; i < 3; i++)        if (digitPressed[i]) lowGroup++;
    for (int i = 3; i < N_DIGITS; i++) if (digitPressed[i]) highGroup++;

    if (lowGroup >= CHORD_MIN_KEYS && highGroup == 0) {
      Serial.printf("[ZB] аккорд сброса: цифр из 1-3 нажато %u\n", lowGroup);
      Serial.println("[ZB] FACTORY RESET");
      sessionUsed = true;
      delay(500);
      Zigbee.factoryReset();
    }
    if (highGroup >= CHORD_MIN_KEYS && lowGroup == 0) {
      Serial.printf("[OTA] аккорд: цифр из 4-6 нажато %u\n", highGroup);
      sessionUsed = true;
      rebootToOta();
    }
    if (nPressed > 0) sessionUsed = true;

    releasePulses();
    delay(POLL_MS);
    return;                     // в сервисном режиме кнопки не шлём
  }

  // -- отправка кнопок: мгновенно --
  bool sane = (nPressed <= MAX_SIMULTANEOUS);

  for (int i = 0; i < N_DIGITS; i++) {
    bool combo = endHeld && digitPressed[i] && sane;

    if (combo && !comboActive[i]) {
      comboActive[i] = true;
      sessionUsed = true;
      fireButton(i, mode == MODE_DBL);
    }
    if (!combo && comboActive[i]) comboActive[i] = false;
  }

  releasePulses();

  // -- состояние связи в лог --
  bool conn = Zigbee.connected();
  if (conn != wasConnected) {
    wasConnected = conn;
    Serial.printf("[ZB] %s\n", conn ? "online" : "OFFLINE");
  }

  // -- уборка залипших --
  if (now - lastSweepMs > SWEEP_MS) {
    lastSweepMs = now;
    sweepStuck();
  }

  // -- heartbeat --
  if (now - lastBeatMs > HEARTBEAT_MS) {
    lastBeatMs = now;
    heartbeat();
  }

#if USE_LIGHT_SLEEP
  bool quiet = (now - lastEdge) > IDLE_BEFORE_SLEEP_MS;
  bool busy  = endHeld || (pending != PEND_NONE);
  for (int i = 0; i < N_DIGITS; i++)
    if (pulseOffBase[i] || pulseOffAlt[i] || retryBase[i] || retryAlt[i]) busy = true;

  if (quiet && !busy) {
    gpio_wakeup_enable((gpio_num_t)DIGIT_SCAN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)END_SCAN,   GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable((gpio_num_t)DIGIT_SCAN);
    gpio_wakeup_disable((gpio_num_t)END_SCAN);
    return;
  }
#endif

  delay(POLL_MS);
}