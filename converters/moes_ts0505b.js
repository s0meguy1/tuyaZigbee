// zigbee2mqtt external converter for the Moes ZB-TDD6-RCW-4 downlight
// (Tuya TS0505B, _TZ3210_b8jdosxo) - works for STOCK firmware (plain light)
// and for the custom firmware (adds the effect engine + OTA updates).
//
// Custom firmware effect control over the Tuya manufacturer cluster
// (0xEF00, manufacturerCode 0x1002):
//   datapoint 0x6E  effect  enum u8    0..11 (0 = stop)
//   datapoint 0x6F  speed   value u32  1..100 (default 50)
//   datapoint 0x70  phase   value u32  0..359 (timeline offset for group shows)
//
// Written for zigbee2mqtt 2.11 / zigbee-herdsman-converters 26.x:
// `ota: true` (z2m core OTA), light via modernExtend, effects via raw
// Tuya setData frames.

const modernExtend = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const ea = exposes.access;

const EFFECTS = ['stop', 'rainbow', 'pulse', 'candle', 'twinkle', 'fire',
    'strobe', 'wave', 'lightning', 'chase', 'color_step', 'snow'];

let tuyaSeq = Math.floor(Math.random() * 255);

async function sendDp(entity, dp, datatype, data) {
    tuyaSeq = (tuyaSeq + 1) & 0xff;
    await entity.command('manuSpecificTuya', 'setData',
        {seq: tuyaSeq, dp: dp, datatype: datatype, fn: 0, length: data.length, data: data},
        {manufacturerCode: 0x1002});
}

const effectSet = {
    key: ['effect'],
    convertSet: async (entity, key, value, meta) => {
        const idx = typeof value === 'string' ? EFFECTS.indexOf(value) : value;
        if (idx < 0 || idx >= EFFECTS.length) {
            throw new Error(`unknown effect '${value}'; one of: ${EFFECTS.join(', ')}`);
        }
        await sendDp(entity, 0x6e, 0x04, [idx]);
        return {state: {effect: EFFECTS[idx]}};
    },
};

const effectSpeed = {
    key: ['effect_speed'],
    convertSet: async (entity, key, value, meta) => {
        const v = Math.min(100, Math.max(1, value));
        await sendDp(entity, 0x6f, 0x02, [0, 0, 0, v]);
        return {state: {effect_speed: v}};
    },
};

const effectPhase = {
    key: ['effect_phase'],
    convertSet: async (entity, key, value, meta) => {
        const v = Math.min(359, Math.max(0, value));
        await sendDp(entity, 0x70, 0x02, [0, 0, (v >> 8) & 0xff, v & 0xff]);
        return {state: {effect_phase: v}};
    },
};

module.exports = {
    fingerprint: [{modelID: 'TS0505B', manufacturerName: '_TZ3210_b8jdosxo'}],
    model: 'ZB-TDD6-RCW-4',
    vendor: 'Moes',
    description: 'RGBCW downlight (stock or custom firmware)',
    ota: true,
    extend: [
        modernExtend.light({
            color: true,
            colorTemp: {range: [153, 454], startup: false},
            powerOnBehavior: true,
        }),
    ],
    exposes: [
        exposes.enum('effect', ea.SET, EFFECTS)
            .withDescription('Light show (custom firmware; any normal command stops it)'),
        exposes.numeric('effect_speed', ea.SET).withValueMin(1).withValueMax(100)
            .withDescription('Effect speed 1..100'),
        exposes.numeric('effect_phase', ea.SET).withValueMin(0).withValueMax(359)
            .withDescription('Phase offset (deg): different phase per light + group broadcast = chase'),
    ],
    toZigbee: [effectSet, effectSpeed, effectPhase],
};
