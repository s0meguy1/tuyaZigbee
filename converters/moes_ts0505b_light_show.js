/**
 * zigbee2mqtt external converter - Moes ZB-TDD6-RCW-4 (TS0505B / _TZ3210_b8jdosxo)
 *
 * Does two things the built-in definition cannot:
 *   1. `ota: true`, without which z2m refuses these lights any OTA at all
 *      (dist/extension/otaUpdate.js bails on `if (!device.definition?.ota)`).
 *   2. Exposes the custom firmware's on-device light-show engine, which is
 *      implemented and running on the chip but unreachable from Home
 *      Assistant because nothing in the stock ZCL surface addresses it.
 *
 * SUPERSEDES the three earlier drafts in this directory. Keep exactly one
 * external converter matching this fingerprint - two definitions claiming the
 * same device is not a supported configuration.
 *
 * --------------------------------------------------------------------------
 * WHY NOT THE STOCK `effect` EXPOSE
 *
 * The built-in light extend already exposes a ZCL `effect` (blink, breathe,
 * okay, channel_change, finish_effect, stop_effect, colorloop,
 * stop_colorloop). Those map to genIdentify.triggerEffect and
 * lightingColorCtrl.colorLoopSet, and none of them reaches the firmware's
 * engine. (colorLoopSet in particular lands on a do-nothing stub, so that
 * option silently does nothing on this firmware.) These controls are
 * therefore named light_show* rather than effect* - reusing the name would
 * collide in both the exposes list and the toZigbee key lookup.
 *
 * Per-command network strobing is not an alternative to this: the coordinator
 * tops out around 2 commands/sec, so a burst loses exactly the commands that
 * exceed it. The engine renders locally on the chip, which is the entire
 * point - one command starts it and the animation runs at full frame rate
 * without further traffic.
 *
 * --------------------------------------------------------------------------
 * WIRE FORMAT - verified against both ends, not assumed
 *
 * Firmware (light/zcl_tuyaMfg.c) registers cluster 0xEF00 with
 * MANUFACTURER_CODE_NONE and accepts commands 0x00/0x01, parsing:
 *     seq u16, dp u8, type u8, len u16 BIG endian, data[len]
 * reading the value big-endian from up to 4 bytes.
 *
 * herdsman defines 0xEF00 as `manuSpecificTuya` with manufacturerCode
 * undefined, so it emits plain (non manufacturer-specific) frames - which is
 * exactly what the firmware's MANUFACTURER_CODE_NONE registration requires.
 * `dataRequest` is command 0x00. The lib helpers below produce:
 *     sendDataPointEnum  -> datatype 4, 1 byte
 *     sendDataPointValue -> datatype 2, 4 bytes big-endian
 * Both match the firmware's parser exactly. Using the helpers rather than
 * hand-rolling the frame also picks up herdsman's per-device sequence
 * handling.
 *
 * Datapoints (light/light_effects.h, moes_effect_e):
 *     0x6E effect enum  0..11, 0 = steady/stop (firmware rejects >= MOES_EF_MAX)
 *     0x6F speed  value 1..100, default 50   (firmware rejects out of range)
 *     0x70 phase  value 0..359               (firmware rejects out of range)
 *
 * Phase exists so several fixtures can be choreographed against one shared
 * timeline: give each light a different phase, then send the same effect to
 * the group to get a chase or wave across a room.
 *
 * --------------------------------------------------------------------------
 * STATE IS OPTIMISTIC
 *
 * light_show* values reported back are what was last commanded, not a device
 * report. The firmware does mirror them in readable/reportable attributes on
 * 0xEF00 (0xF000/0xF001/0xF002), but those are custom attribute ids that
 * herdsman's cluster definition does not know, so reading them back would
 * need a custom cluster definition. Until that exists, treat the published
 * value as "what we asked for".
 *
 * On a light still running STOCK firmware these controls do nothing at all -
 * the stock app ignores these datapoint ids - so one definition safely covers
 * the fleet mid-migration.
 *
 * --------------------------------------------------------------------------
 * Install: data/external_converters/ (z2m >= 2.0 auto-loads every file in
 * that directory; the old `external_converters:` YAML key is ignored).
 * Requires a z2m restart to take effect.
 * To revert: delete the file and restart.
 */

const {definitions} = require('zigbee-herdsman-converters/devices/tuya');
const tuya = require('zigbee-herdsman-converters/lib/tuya');
const utils = require('zigbee-herdsman-converters/lib/utils');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const ea = exposes.access;

const builtin = definitions.find((d) => (d.whiteLabel || []).some((w) => w.model === 'ZB-TDD6-RCW-4'));
if (!builtin) {
    throw new Error('moes_ts0505b_light_show: built-in TS0505B_1 definition not found - z2m changed, review before using');
}

// Index IS the wire value; order must match moes_effect_e in
// light/light_effects.h. Do not reorder or insert.
const EFFECTS = [
    'stop',      //  0 MOES_EF_STEADY - hands the output stage back to normal ZCL control
    'rainbow',   //  1
    'pulse',     //  2
    'candle',    //  3
    'twinkle',   //  4
    'fire',      //  5
    'strobe',    //  6
    'wave',      //  7
    'lightning', //  8
    'chase',     //  9
    'color_step', // 10
    'snow',      // 11
];

const DP_EFFECT = 0x6e;
const DP_SPEED = 0x6f;
const DP_PHASE = 0x70;

const clamp = (value, lo, hi) => Math.min(hi, Math.max(lo, value));

/*
 * The fingerprint matches on modelID + manufacturerName, which every fixture
 * in this fleet shares - there is no way to fingerprint one physical device.
 * Most of the fleet is still on stock firmware, and stock ignores these
 * datapoint ids, so a flat exposes list would hand every stock light three
 * controls that silently do nothing.
 *
 * Discriminate at expose time instead. Measured across the live fleet:
 *
 *     stock      softwareBuildID null,          applicationVersion 101
 *     converted  softwareBuildID "v1.18s3.3",   applicationVersion 19
 *
 * so the presence of our version string is a clean separator. It is matched by
 * SHAPE, not by build number - z2m only refreshes softwareBuildID on interview,
 * so the stored value lags the running build (it read v1.18s3.3 while the
 * device was running build 20) and pinning an exact build would break on every
 * OTA. A device that has never been interviewed since conversion simply keeps
 * the plain light until its next interview, which is a safe way to be wrong.
 */
const CUSTOM_BUILD_ID = /^v\d+\.\d+s\d+\.\d+$/;

const hasCustomFirmware = (device) => {
    // The dummy device carries no attributes and creates no real entity; show
    // the controls there so the definition still documents itself.
    if (utils.isDummyDevice(device)) return true;
    return CUSTOM_BUILD_ID.test(device?.softwareBuildID ?? '');
};

const tzLightShow = {
    key: ['light_show', 'light_show_speed', 'light_show_phase'],
    convertSet: async (entity, key, value, meta) => {
        if (key === 'light_show') {
            // Accept the enum name or the raw index, so an automation can use
            // either without a lookup table.
            const index = typeof value === 'string' ? EFFECTS.indexOf(value) : Number(value);
            if (!Number.isInteger(index) || index < 0 || index >= EFFECTS.length) {
                throw new Error(`unknown light_show '${value}'; one of: ${EFFECTS.join(', ')}`);
            }
            await tuya.sendDataPointEnum(entity, DP_EFFECT, index);
            return {state: {light_show: EFFECTS[index]}};
        }

        if (key === 'light_show_speed') {
            const speed = clamp(Math.round(Number(value)), 1, 100);
            if (!Number.isFinite(speed)) throw new Error(`light_show_speed must be a number 1-100, got '${value}'`);
            await tuya.sendDataPointValue(entity, DP_SPEED, speed);
            return {state: {light_show_speed: speed}};
        }

        if (key === 'light_show_phase') {
            const phase = clamp(Math.round(Number(value)), 0, 359);
            if (!Number.isFinite(phase)) throw new Error(`light_show_phase must be a number 0-359, got '${value}'`);
            await tuya.sendDataPointValue(entity, DP_PHASE, phase);
            return {state: {light_show_phase: phase}};
        }
    },
};

const definition = {...builtin};

// We are the specific model now, not the generic multi-white-label one.
delete definition.zigbeeModel;
delete definition.whiteLabel;

Object.assign(definition, {
    fingerprint: [{modelID: 'TS0505B', manufacturerName: '_TZ3210_b8jdosxo'}],
    model: 'ZB-TDD6-RCW-4',
    vendor: 'Moes',
    description: 'RGB+CCT 6W Smart Downlight',
    ota: true,

    // `extend` is deliberately left untouched. z2m's processExtensions MERGES
    // a definition's own toZigbee/exposes with those contributed by extend, so
    // the light's on/off, brightness and colour behaviour is inherited intact.
    // An earlier draft assigned `exposes` from `builtin.exposes` - which is
    // undefined on a modern extend-based definition - and would have replaced
    // every light control with just these three.
    toZigbee: [...(builtin.toZigbee ?? []), tzLightShow],

    // A function rather than an array, so the controls appear only on fixtures
    // actually running the custom firmware. z2m composes this with the exposes
    // contributed by `extend`, so the light itself is unaffected either way.
    exposes: (device, options) => !hasCustomFirmware(device) ? [] : [
        exposes.enum('light_show', ea.STATE_SET, EFFECTS)
            .withDescription(
                'On-device light show (custom firmware only; does nothing on stock). ' +
                'Renders on the chip, so it is not limited by the coordinator\'s command rate. ' +
                'Select "stop" to hand the output back to normal on/off/brightness/colour control. ' +
                'Reported value is the last one commanded, not a device report.'),
        exposes.numeric('light_show_speed', ea.STATE_SET).withValueMin(1).withValueMax(100)
            .withDescription('Light show speed, 1-100 (firmware default 50). Applies immediately while a show is running.'),
        exposes.numeric('light_show_phase', ea.STATE_SET).withValueMin(0).withValueMax(359).withUnit('°')
            .withDescription(
                'Timeline offset in degrees. Give each fixture a different phase and send the same ' +
                'effect to the group to get a chase or wave across the room.'),
    ],
});

module.exports = definition;
