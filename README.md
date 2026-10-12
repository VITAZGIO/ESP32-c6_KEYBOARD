<div align="right">

🌐 **English** · [Русский](https://github.com/VITAZGIO/ESP32-c6_KEYBOARD/blob/main/README.ru.md)

</div>

<div align="center">

# ⌨️ AULA F75 → Zigbee buttons

### A regular keyboard that can control your smart home

![Zigbee](https://img.shields.io/badge/Zigbee-End%20Device-blue)
![MCU](https://img.shields.io/badge/MCU-ESP32--C6%20SuperMini-red)
![Zigbee2MQTT](https://img.shields.io/badge/Zigbee2MQTT-supported-green)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.4-orange)
![Sleep](https://img.shields.io/badge/sleep-%3C1%20mA-brightgreen)
![OTA](https://img.shields.io/badge/OTA-Wi--Fi-lightgrey)

<img src="Photo/Keyboard.png" alt="AULA F75" width="900">

</div>

---

## 👋 What is this

A tiny **ESP32-C6 SuperMini** board (a microcontroller with a Zigbee radio) built inside
an **AULA F75** mechanical keyboard.

The keys **End + 1…6** become **12 wireless buttons** for Home Assistant. Press a combo
and an automation fires: lights, a socket, the kettle, a scene, anything.

- 🖥️ **No PC needed.** The board works on its own, even when the computer is off.
- 🔌 **Works in every keyboard mode:** wired, 2.4G and Bluetooth.
- 🧠 **Doesn't break the keyboard.** The board only listens to the key matrix and never drives it. End stays a normal key.
- 🔋 **Runs from the keyboard's own Li-Po** (4000 mAh). Under **1 mA** while asleep.
- ⚡ **Instant.** Wakes up in fractions of a millisecond, even a quick tap is caught.
- 📡 Shows up in **Zigbee2MQTT** as **VITAZGIO / AulaKeys**.
- 🔄 **Over-the-air updates (OTA)** via Wi-Fi, no need to open the keyboard.

> 💡 **Zigbee** is a low-power wireless protocol for smart homes. **Zigbee2MQTT** connects
> such devices to Home Assistant without vendor hubs. **End Device** is a device that only
> sends its own data and does not relay others' traffic, so it can sleep and save battery.

---

## 🧰 What you need

| What | Why |
|---|---|
| AULA F75 keyboard (board `SI-2635-916-3632-V02`) | the keyboard itself |
| **ESP32-C6 SuperMini** (4 MB flash, `B+`/`B−` battery pads) | the brain and the Zigbee radio |
| 1S BMS (battery protection board, DW01A + 8205A) | keeps the Li-Po from being drained to zero |
| Electrolytic capacitor ~220 µF 16 V | smooths current spikes when the radio transmits |
| 8 × 10 kΩ resistors + 2 × 20 kΩ | matrix taps + battery divider |
| Thin wires, kapton/electrical tape | wiring and insulation |
| Zigbee coordinator + Zigbee2MQTT | receiving the buttons |

A soldering iron and patience are mandatory 🙂

---

## 🔧 Inside

The whole keyboard board with the ESP32-C6 SuperMini on tape. Yellow wires go to the
digit keys, white/blue to End, the battery comes in from the bottom.

<div align="center">
<img src="Photo/keyboard_inside.jpg" alt="Keyboard board with the ESP32-C6 SuperMini" width="500">
</div>

Close-up of the board. All wires go to the edge pins, the series resistors sit right
at the matrix points.

<div align="center">
<img src="Photo/supermini_wired.jpg" alt="ESP32-C6 SuperMini wired" width="900">
</div>

Battery protection board (BMS) with the capacitor. Its `P+`/`P−` output feeds the
`B+`/`B−` pads of the SuperMini.

<div align="center">
<img src="Photo/bms_wiring.jpg" alt="BMS and capacitor" width="900">
</div>

The ESP32-C6 SuperMini itself, top and bottom. The `B+`/`B−` pads on the bottom are the
battery input.

<div align="center">
<img src="Photo/supermini_top.jpg" alt="ESP32-C6 SuperMini top" width="440">
<img src="Photo/supermini_bottom.jpg" alt="ESP32-C6 SuperMini bottom" width="440">
</div>

---

## 🧭 Contents

[How it works](#how-it-works) ·
[Key combos](#key-combos) ·
[Pinout](#pinout) ·
[Power](#-power) ·
[Flashing](#flashing-over-usb-first-time) ·
[OTA](#over-the-air-update) ·
[Zigbee2MQTT](#zigbee2mqtt) ·
[Troubleshooting](#troubleshooting)

---

## How it works

Keyboard keys are wired as a **matrix** (a grid of rows and columns). The keyboard's own
controller pulses one **scan line** at a time and checks which **read line** goes low.
That's how it knows which key is pressed.

The ESP doesn't take part in this, it **eavesdrops**: on every falling edge of a scan
line it takes an interrupt (an instant signal to the CPU) and reads the read lines at
that moment. Two scans in a row with the line low means the key is really pressed.

**Sleep.** When nothing is pressed, the keyboard stops scanning and the matrix is
silent. The ESP sits in **light sleep** (CPU stopped, memory kept, wakes in under a
millisecond) and waits for End's read line to go low. Any key in End's row wakes it up,
then the ESP checks End's own scan line to make sure it really was End.

**Radio.** The board is a sleepy Zigbee End Device. It stays awake for 10 s after joining,
then turns the receiver off and only checks in with the coordinator every 3 s. Button
reports go straight to the coordinator.

Because of this it works in **every** keyboard mode (wired, 2.4G, BT) and doesn't depend
on what the keyboard's firmware decides to send.

---

## Key combos

What you did with End **before** the working press decides the mode. Digits always fire
instantly.

| What you did with End before | Then hold End + digit | Result |
|---|---|---|
| nothing | digit 1…6 | `Btn 1`…`Btn 6` |
| short tap (under 0.6 s), released | digit 1…6 | `Btn 1 dbl`…`Btn 6 dbl` |
| held 2 s, released | End + any of **1, 2, 3** | Zigbee network reset |
| held 2 s, released | End + any of **4, 5, 6** | OTA mode (Wi-Fi, 10 min) |

The flag is set **when End is released** and lives for **3 seconds**. It's one-shot: use
it once and it's gone. If you pressed digits while holding End, that was normal work and
no flag is set.

In service mode nothing is sent to the network. A **single digit press** is enough.

---

## Pinout

All pins are on the board edges. Every matrix line goes through a **10 kΩ series resistor**.

| GPIO | What | Via |
|---|---|---|
| GPIO6 | End: read line (switch leg) | 10k series |
| GPIO7 | End: scan line (diode side) | 10k series |
| GPIO4 | Digit row: scan line (common, diode side) | 10k series |
| GPIO19 | Digit 1: read line | 10k series |
| GPIO2 | Digit 2: read line | 10k series |
| GPIO5 | Digit 3: read line | 10k series |
| GPIO20 | Digit 4: read line | 10k series |
| GPIO18 | Digit 5: read line | 10k series |
| GPIO14 | Digit 6: read line | 10k series |
| GPIO1 | Battery voltage | 20k/20k divider |
| `B+` / `B−` | Battery after the BMS | directly |

Don't use: GPIO8 (RGB LED), GPIO9 (BOOT), GPIO15 (LED), GPIO12/13 (USB), GPIO16/17 (UART).

**Series resistor only, no resistor to ground and no pull-ups.** Anything to ground or
3.3 V loads the matrix and the keyboard starts seeing stuck keys.

### Points on the keyboard board (SI-2635-916-3632-V02)

- **read line**: the switch socket pad that does NOT go to the diode
- **scan line**: the diode side. The small three-legged parts marked `A3` are dual
  diodes (they look like transistors). One package serves two neighbouring keys.
- `J3Y` parts are backlight transistors, not part of the matrix.

If End doesn't wake the board (`пробуждений 0` in the log), swap `PIN_END` and `PIN_END_SCAN`
in `src/config.h`.

---

## ⚡ Power

```
Li-Po (+) ─────────────────────────┬── keyboard (as from the factory)
Li-Po (−) ─────────────────────────┤
                                   │
                    [BMS 1S]  B+/B− in, P+/P− out
                         P+ ──┬────── B+  (ESP32-C6 SuperMini, bottom pad)
                              ═ 220 µF
                         P− ──┴────── B−

B+ ──[20k]──┬── GPIO1
            └─[20k]── GND
```

- The SuperMini has its **own Li-Po input** (`B+`/`B−`) with a charger and a low-dropout
  regulator. **No boost converter is needed**, the battery goes straight in.
- The **BMS** (battery protection board) cuts the ESP off when the battery is nearly
  empty, so it never drains the Li-Po to zero.
- The **capacitor** (electrolytic, mind the polarity: stripe = minus) supplies the short
  current spikes when the radio transmits.
- **USB and battery together are fine** on this board. But while USB is plugged in, the
  board's charger also charges the keyboard battery, so don't leave it like that for long.
- Never feed anything into the `3V3` pin, and leave the `5V` pin unconnected.

### Measured current

| State | Current from battery |
|---|---|
| Sending a button (~2 s) | ~24 mA |
| Asleep, connected to Zigbee | **under 1 mA** (below the 1 mA resolution of my multimeter) |

At this rate the ESP is a drop in the bucket next to the keyboard itself.

### Battery level

The divider halves the voltage: 4.2 V → 2.1 V. Reported to Zigbee on join and every
6 hours. If the percentage looks wrong, adjust `BAT_MIN_MV` / `BAT_MAX_MV` in `config.h`.

---

## Antenna

The keyboard's top plate is metal. Put the board near the plastic bottom, antenna (the
red ceramic chip) towards the edge of the case, away from the plate, the battery and the
keyboard's own `ANT2` antenna.

---

## Flashing over USB (first time)

The firmware is written in **ESP-IDF 5.4** (Espressif's native framework) and built with
**PlatformIO** (`framework = espidf`).

Before the first build:

```
src/secrets.example.h  →  src/secrets.h     (Wi-Fi and OTA password)
secrets.example.ini    →  secrets.ini       (board IP and OTA password for uploading)
```

Both are in `.gitignore` and never reach the repository.

1. Use a **real data cable** (USB-A → USB-C is safest) plugged into a **rear USB port**
   of the PC, not a hub. A bad cable gives "device descriptor request failed".
2. Enter download mode: **hold BOOT → plug in USB (or press RST) → release BOOT**.
3. Upload:

```
pio run -e esp32-c6 -t upload
pio device monitor -p COMxx
```

No external USB-UART adapter is needed, the SuperMini has native USB.

> ⚠️ Keep the project in a plain folder like `C:\pio\aula-keys`: no Cyrillic, no spaces,
> not inside Nextcloud/OneDrive. Sync tools lock files and the build fails with
> `PermissionError ... .sconsign`.

After changing `sdkconfig.defaults` or `ota.csv`, delete `sdkconfig.esp32-c6*` and `.pio/`.

---

## Over-the-air update

1. On the keyboard: **hold End 2 s → release → End + any of 4, 5, 6**
2. The ESP reboots into Wi-Fi with a **static IP `192.168.1.210`** and waits **10 minutes**.
   Check it at `http://192.168.1.210/`.
3. Upload:

```
pio run -e esp32-c6-ota -t upload
```

The IP lives in `src/config.h` (`OTA_IP`, `OTA_GATEWAY`) and `secrets.ini`
(`upload_port`). The password is `OTA_PASSWORD` in `secrets.h` and `password` in
`secrets.ini`; they must match.

The flash layout `ota.csv` keeps two app slots (`ota_0` / `ota_1`) on the 4 MB flash.
If the new firmware crashes on boot, the bootloader rolls back to the previous one.

---

## Zigbee2MQTT

1. Copy `zigbee2mqtt/aulakeys.js` next to z2m's `configuration.yaml`
2. In `configuration.yaml`:

```yaml
external_converters:
  - aulakeys.js
```

3. Restart z2m
4. **Permit Join on the coordinator only** (not via routers: through some routers the
   sleepy device loses its parent)
5. The board joins as **VITAZGIO / AulaKeys**

A new board means a new address, so it shows up as a new device. Delete the old one and
give the new one the same name, and your Home Assistant entities stay the same.

### Entities

| Entity | Combo |
|---|---|
| `Btn 1`…`Btn 6` | hold End + digit |
| `Btn 1 dbl`…`Btn 6 dbl` | tap End, then hold End + digit |
| `battery` | charge %, on join and every 6 h |

Each button is a pulse: `ON` for 300 ms, then `OFF` (plus two more `OFF` repeats in case a
packet is lost). In HA, trigger your automation on the change to `ON`.

---

## AutoHotkey for the PC

End stays a normal key, so to keep End + digits from typing in Windows, put this in
autostart (`Win+R` → `shell:startup`):

```
#Requires AutoHotkey v2.0

End & 1::return
End & 2::return
End & 3::return
End & 4::return
End & 5::return
End & 6::return
```

Single digits keep typing as usual. With the PC off the script isn't needed.

---

## Settings in code (`src/config.h`)

| Constant | Meaning |
|---|---|
| `TAP_MAX_MS` | End shorter than this = second-row request (600) |
| `SERVICE_HOLD_MS` | End longer than this = service request (2000) |
| `ALT_WINDOW_MS` / `SERVICE_WINDOW_MS` | how long the flag lives (3000) |
| `END_PRESS_SCANS` / `DIGIT_PRESS_SCANS` | scans in a row to accept a press (2) |
| `SCAN_SILENCE_MS` | no scans this long = everything released (40) |
| `PULSE_MS` | button pulse length (300) |
| `OFF_RETRIES` / `OFF_RETRY_MS` | extra `OFF` packets (2 × 900 ms) |
| `BAT_REPORT_MS` | battery report period (6 h) |
| `OTA_TIMEOUT_MS` | how long OTA mode waits (10 min) |
| `TEST_NO_ZIGBEE` | 1 = don't start Zigbee (current test), normally 0 |

`DEBUG_PM` in `main.c` prints sleep statistics every 30 s (`пробуждений N, нажатий End M` = wakes / End presses). Log messages are in Russian.

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "Device descriptor request failed" | bad cable or front-panel/hub port. Data cable, rear port |
| `Write timeout` / `No serial data` on upload | board not in download mode: hold BOOT, press RST, release BOOT |
| `PermissionError ... .sconsign` | project inside Nextcloud/OneDrive, move it to a plain folder |
| End does nothing, `пробуждений 0` | `PIN_END` / `PIN_END_SCAN` swapped |
| Sleep current tens of mA | `PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` or `ESP_SLEEP_POWER_DOWN_FLASH` turned on. On C6 they make the chip wake ~300 times a second. Keep both off |
| Radio dead after the first sleep | `CONFIG_ESP_PHY_MAC_BB_PD=y` is required |
| Keyboard sees stuck keys | resistor to ground/pull-up on a matrix line, keep series only |
| Not appearing in z2m | flash not erased, use the service combo (End 2 s → End + 1) or `erase_flash`, then Permit Join |
| Status `0x09` / device unavailable | joined through a router: re-pair with Permit Join on the coordinator |
| Board reboots when measuring current through a resistor | radio spikes ~100 mA. Use ≤10 Ω plus a big capacitor, or 1 Ω |

---

## Project files

```
ESP32-c6_KEYBOARD/
├── platformio.ini            build config, two environments (USB / OTA)
├── sdkconfig.defaults        ESP-IDF settings (Zigbee, sleep, console)
├── ota.csv                   4 MB flash layout with two OTA slots
├── CMakeLists.txt
├── secrets.example.ini       template → secrets.ini
├── README.md                 docs (English)
├── README.ru.md              docs (Russian)
├── src/
│   ├── config.h              all pins and timings
│   ├── main.c                matrix, modes, sleep
│   ├── zb.c / zb.h           Zigbee
│   ├── ota.c / ota.h         Wi-Fi OTA
│   ├── battery.c / .h        battery measurement
│   ├── idf_component.yml     esp-zigbee-lib dependency
│   └── secrets.example.h     template → secrets.h
└── zigbee2mqtt/
    └── aulakeys.js           external converter for z2m
```

Not in git: `src/secrets.h`, `secrets.ini`, `.pio/`, `sdkconfig.*`, `managed_components/`.

---

**Project:** AULA F75 Zigbee Keys · Zigbee End Device · VITAZGIO
