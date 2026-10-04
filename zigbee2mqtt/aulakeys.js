const {deviceEndpoints, binary, battery} = require('zigbee-herdsman-converters/lib/modernExtend');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

// ключ = суффикс имени сущности, значение = номер эндпоинта
// '1' -> "Btn 1"   |   '1_dbl' -> "Btn 1 dbl"
const EP = {
    '1': 10, '2': 11, '3': 12, '4': 13, '5': 14, '6': 15,
    '1_dbl': 20, '2_dbl': 21, '3_dbl': 22, '4_dbl': 23, '5_dbl': 24, '6_dbl': 25,
};

const mkBtn = (epName, desc) => binary({
    name: 'btn',
    cluster: 'msOccupancySensing',
    attribute: 'occupancy',
    valueOn: ['ON', 1],
    valueOff: ['OFF', 0],
    access: 'STATE',
    endpointName: epName,
    description: desc,
});

module.exports = [
    {
        zigbeeModel: ['AulaKeys'],
        model: 'AulaKeys',
        vendor: 'VITAZGIO',
        description: 'AULA F75 — 12 кнопок умного дома внутри клавиатуры',
        extend: [
            deviceEndpoints({endpoints: EP}),

            mkBtn('1', 'Зажал End + 1'),
            mkBtn('2', 'Зажал End + 2'),
            mkBtn('3', 'Зажал End + 3'),
            mkBtn('4', 'Зажал End + 4'),
            mkBtn('5', 'Зажал End + 5'),
            mkBtn('6', 'Зажал End + 6'),

            mkBtn('1_dbl', 'Тап End, затем зажал End + 1'),
            mkBtn('2_dbl', 'Тап End, затем зажал End + 2'),
            mkBtn('3_dbl', 'Тап End, затем зажал End + 3'),
            mkBtn('4_dbl', 'Тап End, затем зажал End + 4'),
            mkBtn('5_dbl', 'Тап End, затем зажал End + 5'),
            mkBtn('6_dbl', 'Тап End, затем зажал End + 6'),

            battery({percentage: true, percentageReporting: true}),
        ],

        // Привязка: без неё устройство шлёт отчёты в пустоту
        configure: async (device, coordinatorEndpoint, logger) => {
            for (const id of Object.values(EP)) {
                const endpoint = device.getEndpoint(id);
                if (!endpoint) continue;
                try {
                    await endpoint.bind('msOccupancySensing', coordinatorEndpoint);
                    await reporting.occupancy(endpoint, {min: 0, max: 3600, change: 0});
                } catch (e) {
                    // некоторые эндпоинты могут не ответить с первого раза — не фатально
                }
            }
            const first = device.getEndpoint(10);
            if (first) {
                try {
                    await first.bind('genPowerCfg', coordinatorEndpoint);
                    await reporting.batteryPercentageRemaining(first, {min: 3600, max: 65000, change: 1});
                } catch (e) {}
            }
        },
    },
];
