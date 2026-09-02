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
 * BUILD 36 (2026-09-02). Everything from build 27 still works exactly as it
 * did; the additions were written against the wish list of the first real
 * show, which found itself rationing a ~1.55 frame/s transport:
 *
 *   light_show_cue       ONE frame carrying several parameters plus an
 *                        optional delay, applied atomically on every member.
 *                        `{"light_show_cue":{"effect":"explode","level":254,
 *                        "delay":400}}` arms an explosion 400 ms ahead.
 *   light_show_cue_list  up to 32 timed entries uploaded to the chip, then
 *   light_show_cue_run   started with one frame and played at 50 fps locally.
 *   light_show_index     a persisted per-fixture index (set once, unicast!),
 *   light_show_spread    plus degrees of phase per index step, so one group
 *                        broadcast of pulse/strobe/chase/rainbow/wave runs
 *                        across the house in order. Phase itself now works.
 *   light_show_level     show brightness independent of the fixture's own
 *                        level (255 = follow it, the default). Sparks from a
 *                        dark fixture without a flare; 0 = blackout.
 *   light_show_takeover  'release' (default, a ZCL command stops the show) or
 *                        'hold' (the show survives; OFF blacks it out, ON
 *                        brings it back, level/colour update underneath).
 *   light_show_duration  ms before an effect stops itself (0 = forever).
 *   light_show_density   burst texture, percent of one-second slots.
 *   'solid'              a new effect: the show colour at the show level.
 *
 *   Colour is now SHOW state. light_show_hue/saturation no longer recolour an
 *   idle fixture (build 27-35 wrote the ZCL attributes, which turned an
 *   occupied kitchen blue during a pre-show); they are staged until an effect
 *   runs, apply live to a running one, and STOP returns the fixture to its own
 *   colour on every path. hue 360 = follow the fixture's colour.
 *
 *   State is REAL: every value is reported by the chip after a unicast write
 *   and on `/get`. Group writes are deliberately not reported (nineteen
 *   fixtures answering every cue would eat the show's airtime), so after a
 *   group write the published state is optimistic, as before.
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
 * --------------------------------------------------------------------------
 * WIRE FORMAT - verified against both ends, not assumed
 *
 * Firmware (light/zcl_tuyaMfg.c, light/moes_fxwire.c) registers cluster
 * 0xEF00 with MANUFACTURER_CODE_NONE and accepts commands 0x00/0x01, parsing:
 *     seq u16, then repeated: dp u8, type u8, len u16 BIG endian, data[len]
 * reading each value big-endian from up to 4 bytes. A frame is atomic: one
 * bad value rejects the whole frame. Command 0x03 (dataQuery) is answered
 * with a dataReport (0x02) carrying every datapoint.
 *
 * herdsman defines 0xEF00 as `manuSpecificTuya` with manufacturerCode
 * undefined, so it emits plain (non manufacturer-specific) frames - which is
 * exactly what the firmware's MANUFACTURER_CODE_NONE registration requires.
 * A dataRequest carries a whole list of datapoints in one frame; the compact
 * 1/2-byte values built here are folded by the firmware exactly like
 * herdsman's 4-byte ones, and the firmware's reports use the same widths
 * (convertBufferToNumber folds any length).
 *
 * Datapoints (light/moes_fxwire.h):
 *     0x6E effect     enum  0..14, 0 = stop     0x75 level    0..254, 255 follow
 *     0x6F speed      1..100                    0x76 fade     ms (same frame)
 *     0x70 phase      0..359                    0x77 delay    ms (whole frame)
 *     0x71 hue        0..359, >=360 follow      0x78 duration ms, 0 forever
 *     0x72 saturation 0..100                    0x79 takeover 0 release/1 hold
 *     0x73 index      0..254, 255 none (NV)     0x7A density  0 auto, 1..100
 *     0x74 spread     0..359                    0x7B cue_load raw, 0x7C cue_run
 *                                               0x7D cue_count (report only)
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
const globalStore = require('zigbee-herdsman-converters/lib/store');
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
    // 12. Chaotic short bursts of hard strobe with darkness between - a
    // "sparks / failing electronics" look rather than a decorative pattern.
    // light_show_speed (or light_show_density) sets how OFTEN bursts land;
    // every fixture draws its own random schedule, so a room scatters from one
    // group broadcast with no phase needed.
    'burst',
    // 13. One-shot bloom: a golden hue swelling into full bright white, then
    // HELD rather than looped - the explosion at the end of a countdown.
    // light_show_speed sets how violent it is (100 = near-instant flash,
    // 1 = slow swell). Unlike the other effects this ignores the current hue:
    // here the colour IS the effect. Send 'stop' to hand the output back.
    'explode',
    // 14. The show colour at the show level, held. Lights a dark fixture in a
    // chosen colour from one frame without touching its own state, or blacks
    // it out (level 0) and brings it back.
    'solid',
];

const DP = {
    EFFECT: 0x6e, SPEED: 0x6f, PHASE: 0x70, HUE: 0x71, SATURATION: 0x72,
    INDEX: 0x73, SPREAD: 0x74, LEVEL: 0x75, FADE: 0x76, DELAY: 0x77,
    DURATION: 0x78, TAKEOVER: 0x79, DENSITY: 0x7a, CUE_LOAD: 0x7b, CUE_RUN: 0x7c,
    CUE_COUNT: 0x7d,
};

const HUE_FOLLOW = 360;      // wire value meaning "follow the fixture's colour"
const LEVEL_FOLLOW = 255;    // wire value meaning "follow the fixture's brightness"
const INDEX_NONE = 255;
const TAKEOVER = ['release', 'hold'];   // index IS the wire value
const CUE_RUN = ['stop', 'run', 'loop']; // index IS the wire value
const CUE_MAX = 32;
const CUE_WIRE_LEN = 9;
const CUE_PER_FRAME = 6;     // 6 * 9 + 1 + 6 = 61 bytes: inside one unfragmented group frame
const FRAME_MAX = 76;        // bytes of Tuya payload that still fit one group broadcast

// --- value helpers -------------------------------------------------------

const num = (key, value, lo, hi) => {
    const n = Number(value);
    if (!Number.isFinite(n) || !Number.isInteger(n) || n < lo || n > hi) {
        throw new Error(`${key} must be an integer ${lo}-${hi}, got '${value}'`);
    }
    return n;
};

const effectIndex = (value) => {
    const index = typeof value === 'string' ? EFFECTS.indexOf(value) : Number(value);
    if (!Number.isInteger(index) || index < 0 || index >= EFFECTS.length) {
        throw new Error(`unknown light_show '${value}'; one of: ${EFFECTS.join(', ')}`);
    }
    return index;
};

const enumIndex = (key, table, value) => {
    const index = typeof value === 'string' ? table.indexOf(value) : Number(value);
    if (!Number.isInteger(index) || index < 0 || index >= table.length) {
        throw new Error(`${key} must be one of ${table.join(', ')}, got '${value}'`);
    }
    return index;
};

// hue accepts 0-359, 'follow' or 360; level accepts 0-254, 'follow' or 255
const hueValue = (value) => (value === 'follow' ? HUE_FOLLOW : num('hue', value, 0, HUE_FOLLOW));
const levelValue = (value) => (value === 'follow' ? LEVEL_FOLLOW : num('level', value, 0, LEVEL_FOLLOW));

// Compact datapoint values. herdsman's sendDataPointValue always sends four
// bytes; the firmware folds any length, so u8 fields go as one byte and u16 as
// two. That is what lets a complete cue fit one group broadcast.
const dpEnum = (dp, v) => ({dp, datatype: tuya.dataTypes.enum, data: Buffer.from([v & 0xff])});
const dpRaw = (dp, buf) => ({dp, datatype: tuya.dataTypes.raw, data: buf});
const dpU8 = (dp, v) => ({dp, datatype: tuya.dataTypes.number, data: Buffer.from([v & 0xff])});
const dpU16 = (dp, v) => ({dp, datatype: tuya.dataTypes.number, data: Buffer.from([(v >> 8) & 0xff, v & 0xff])});

const frameBytes = (dpValues) => 2 + dpValues.reduce((n, d) => n + 4 + d.data.length, 0);

// The library exports only the one-datapoint senders (sendDataPointValue and
// friends); its multi-datapoint sender is internal. This is the same call it
// makes, with the same per-entity sequence counter, so the seq stream stays
// continuous with the library's own senders.
const send = async (entity, dpValues) => {
    if (frameBytes(dpValues) > FRAME_MAX) {
        throw new Error(`light show frame is ${frameBytes(dpValues)} bytes; more than ${FRAME_MAX} will not fit one group broadcast - split it`);
    }
    const seq = globalStore.getValue(entity, 'sequence', 0);
    globalStore.putValue(entity, 'sequence', (seq + 1) % 0xffff);
    await entity.command('manuSpecificTuya', 'dataRequest', {seq, dpValues}, {disableDefaultResponse: true});
};

// One field of a cue -> its datapoint plus the optimistic state it implies.
const CUE_FIELDS = {
    effect: (v) => {
        const i = effectIndex(v);
        return [dpEnum(DP.EFFECT, i), {light_show: EFFECTS[i]}];
    },
    speed: (v) => {
        const n = num('speed', v, 1, 100);
        return [dpU8(DP.SPEED, n), {light_show_speed: n}];
    },
    phase: (v) => {
        const n = num('phase', v, 0, 359);
        return [dpU16(DP.PHASE, n), {light_show_phase: n}];
    },
    spread: (v) => {
        const n = num('spread', v, 0, 359);
        return [dpU16(DP.SPREAD, n), {light_show_spread: n}];
    },
    hue: (v) => {
        const n = hueValue(v);
        return [dpU16(DP.HUE, n), {light_show_hue: n}];
    },
    saturation: (v) => {
        const n = num('saturation', v, 0, 100);
        return [dpU8(DP.SATURATION, n), {light_show_saturation: n}];
    },
    level: (v) => {
        const n = levelValue(v);
        return [dpU8(DP.LEVEL, n), {light_show_level: n}];
    },
    fade: (v) => {
        const n = num('fade', v, 0, 65535);
        return [dpU16(DP.FADE, n), {}];
    },
    delay: (v) => {
        const n = num('delay', v, 0, 65535);
        return [dpU16(DP.DELAY, n), {}];
    },
    duration: (v) => {
        const n = num('duration', v, 0, 65535);
        return [dpU16(DP.DURATION, n), {light_show_duration: n}];
    },
    takeover: (v) => {
        const i = enumIndex('takeover', TAKEOVER, v);
        return [dpEnum(DP.TAKEOVER, i), {light_show_takeover: TAKEOVER[i]}];
    },
    density: (v) => {
        const n = num('density', v, 0, 100);
        return [dpU8(DP.DENSITY, n), {light_show_density: n}];
    },
    cue_run: (v) => {
        const i = enumIndex('cue_run', CUE_RUN, v);
        return [dpEnum(DP.CUE_RUN, i), {light_show_cue_run: CUE_RUN[i]}];
    },
};

// Cue-list entry -> 9 wire bytes:
//   t u16 | effect u8 | speed u8 | hue u16 | sat u8 | level u8 | fade u8 (100 ms)
// Missing fields mean "keep what is set". effect 'stop' releases the output.
const cueEntryBytes = (entry, i) => {
    if (typeof entry !== 'object' || entry === null) throw new Error(`cue ${i} must be an object`);
    const t = num(`cue ${i} t`, entry.t ?? 0, 0, 65535);
    const effect = entry.effect === undefined || entry.effect === 'keep' ? 0xff : effectIndex(entry.effect);
    const speed = entry.speed === undefined ? 0 : num(`cue ${i} speed`, entry.speed, 1, 100);
    const hue = entry.hue === undefined ? 0xffff : hueValue(entry.hue);
    const sat = entry.saturation === undefined ? 0xff : num(`cue ${i} saturation`, entry.saturation, 0, 100);
    const level = entry.level === undefined ? 0xff : num(`cue ${i} level`, entry.level, 0, 254);
    const fadeMs = entry.fade === undefined ? 0 : num(`cue ${i} fade`, entry.fade, 0, 25500);
    const fade = Math.round(fadeMs / 100);
    return [t >> 8, t & 0xff, effect, speed, hue >> 8, hue & 0xff, sat, level, fade];
};

// --- toZigbee ------------------------------------------------------------

const tzLightShow = {
    key: [
        'light_show', 'light_show_speed', 'light_show_phase', 'light_show_hue', 'light_show_saturation',
        'light_show_index', 'light_show_spread', 'light_show_level', 'light_show_takeover',
        'light_show_density', 'light_show_duration',
        'light_show_cue', 'light_show_cue_list', 'light_show_cue_run',
    ],
    convertSet: async (entity, key, value, meta) => {
        if (key === 'light_show_cue') {
            if (typeof value !== 'object' || value === null || Array.isArray(value)) {
                throw new Error('light_show_cue must be an object, e.g. {"effect":"burst","level":254,"hue":220,"delay":300}');
            }
            const dpValues = [];
            let state = {};
            for (const [field, raw] of Object.entries(value)) {
                const pack = CUE_FIELDS[field];
                if (!pack) throw new Error(`light_show_cue: unknown field '${field}'; one of ${Object.keys(CUE_FIELDS).join(', ')}`);
                const [dp, st] = pack(raw);
                dpValues.push(dp);
                state = {...state, ...st};
            }
            if (!dpValues.length) throw new Error('light_show_cue is empty');
            await send(entity, dpValues);
            return {state};
        }

        if (key === 'light_show_cue_list') {
            if (!Array.isArray(value) || value.length === 0 || value.length > CUE_MAX) {
                throw new Error(`light_show_cue_list must be an array of 1-${CUE_MAX} entries`);
            }
            const entries = value.map(cueEntryBytes);
            for (let i = 1; i < entries.length; i++) {
                const tPrev = (entries[i - 1][0] << 8) | entries[i - 1][1];
                const tThis = (entries[i][0] << 8) | entries[i][1];
                if (tThis < tPrev) throw new Error(`light_show_cue_list: entry ${i} (t=${tThis}) is earlier than entry ${i - 1} (t=${tPrev}); entries must be in time order`);
            }
            // Frame 0 starts a new list on the chip (start index 0), the rest
            // append. Sent in order; the chip refuses to run a list with a gap.
            for (let start = 0; start < entries.length; start += CUE_PER_FRAME) {
                const chunk = entries.slice(start, start + CUE_PER_FRAME);
                const raw = Buffer.from([start, ...chunk.flat()]);
                await send(entity, [dpRaw(DP.CUE_LOAD, raw)]);
            }
            return {state: {light_show_cue_count: entries.length, light_show_cue_run: 'stop'}};
        }

        if (key === 'light_show_cue_run') {
            const [dp, state] = CUE_FIELDS.cue_run(value);
            await send(entity, [dp]);
            return {state};
        }

        if (key === 'light_show') {
            const [dp, state] = CUE_FIELDS.effect(value);
            await send(entity, [dp]);
            return {state};
        }

        if (key === 'light_show_index') {
            // Per fixture, persisted on the chip. Sending it to a GROUP would
            // give every member the same index - refuse rather than let it.
            if (utils.isGroup(entity)) throw new Error('light_show_index is per fixture; publish it to a device topic, never a group');
            const n = value === 'none' ? INDEX_NONE : num('light_show_index', value, 0, INDEX_NONE);
            await send(entity, [dpU8(DP.INDEX, n)]);
            return {state: {light_show_index: n}};
        }

        const simple = {
            light_show_speed: 'speed', light_show_phase: 'phase', light_show_hue: 'hue',
            light_show_saturation: 'saturation', light_show_spread: 'spread', light_show_level: 'level',
            light_show_takeover: 'takeover', light_show_density: 'density', light_show_duration: 'duration',
        };
        const field = simple[key];
        if (field) {
            const [dp, state] = CUE_FIELDS[field](value);
            await send(entity, [dp]);
            return {state};
        }
    },
    convertGet: async (entity, key, meta) => {
        // One query answers with every datapoint (build 36); the fromZigbee
        // converter below publishes them all.
        await entity.command('manuSpecificTuya', 'dataQuery', {}, {disableDefaultResponse: true});
    },
};

// --- fromZigbee ----------------------------------------------------------
//
// tuya.fz.datapoints decodes dataReport/dataResponse frames through this table.
// The firmware reports after a unicast write, on a dataQuery, and when it
// changes state on its own (a duration ran out, a cue list finished) - but
// never in answer to a group frame.
const asIs = {from: (v) => v};
const tuyaDatapoints = [
    [DP.EFFECT, 'light_show', {from: (v) => EFFECTS[v] ?? v}],
    [DP.SPEED, 'light_show_speed', asIs],
    [DP.PHASE, 'light_show_phase', asIs],
    [DP.HUE, 'light_show_hue', asIs],
    [DP.SATURATION, 'light_show_saturation', asIs],
    [DP.INDEX, 'light_show_index', asIs],
    [DP.SPREAD, 'light_show_spread', asIs],
    [DP.LEVEL, 'light_show_level', asIs],
    [DP.TAKEOVER, 'light_show_takeover', {from: (v) => TAKEOVER[v] ?? v}],
    [DP.DURATION, 'light_show_duration', asIs],
    [DP.DENSITY, 'light_show_density', asIs],
    [DP.CUE_RUN, 'light_show_cue_run', {from: (v) => CUE_RUN[v] ?? v}],
    [DP.CUE_COUNT, 'light_show_cue_count', asIs],
];

// --- definition ----------------------------------------------------------

/*
 * The fingerprint matches on modelID + manufacturerName, which every fixture
 * in this fleet shares - there is no way to fingerprint one physical device.
 * A fleet mid-migration still has stock lights, and stock ignores these
 * datapoint ids, so a flat exposes list would hand every stock light controls
 * that silently do nothing.
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

// exposes.composite has withFeature() only; add the whole list in one go.
exposes.Composite.prototype.withFeatures = function (features) {
    for (const f of features) this.withFeature(f);
    return this;
};

const cueFeatures = () => [
    exposes.enum('effect', ea.SET, EFFECTS).withDescription('Effect to (re)start; "stop" releases the output and aborts a cue list.'),
    exposes.numeric('speed', ea.SET).withValueMin(1).withValueMax(100),
    exposes.numeric('phase', ea.SET).withValueMin(0).withValueMax(359).withUnit('°'),
    exposes.numeric('spread', ea.SET).withValueMin(0).withValueMax(359).withUnit('°'),
    exposes.numeric('hue', ea.SET).withValueMin(0).withValueMax(360).withUnit('°').withDescription('0-359; 360 = follow the fixture'),
    exposes.numeric('saturation', ea.SET).withValueMin(0).withValueMax(100).withUnit('%'),
    exposes.numeric('level', ea.SET).withValueMin(0).withValueMax(255).withDescription('0-254; 255 = follow the fixture'),
    exposes.numeric('fade', ea.SET).withValueMin(0).withValueMax(65535).withUnit('ms').withDescription('Ramp the level in this same cue over this long.'),
    exposes.numeric('delay', ea.SET).withValueMin(0).withValueMax(65535).withUnit('ms').withDescription('Apply this whole cue this long after receipt. Every member of a group lands it together.'),
    exposes.numeric('duration', ea.SET).withValueMin(0).withValueMax(65535).withUnit('ms'),
    exposes.enum('takeover', ea.SET, TAKEOVER),
    exposes.numeric('density', ea.SET).withValueMin(0).withValueMax(100).withUnit('%'),
    exposes.enum('cue_run', ea.SET, CUE_RUN),
];

const cueListEntry = () => exposes.composite('cue', 'cue', ea.SET)
    .withFeature(exposes.numeric('t', ea.SET).withValueMin(0).withValueMax(65535).withUnit('ms').withDescription('Time from the start of the sequence. Entries must be in time order; in loop mode the last entry\'s t is the loop length.'))
    .withFeature(exposes.enum('effect', ea.SET, EFFECTS).withDescription('Omit to keep the running effect; "stop" releases the output but the sequence carries on.'))
    .withFeature(exposes.numeric('speed', ea.SET).withValueMin(1).withValueMax(100))
    .withFeature(exposes.numeric('hue', ea.SET).withValueMin(0).withValueMax(360).withUnit('°'))
    .withFeature(exposes.numeric('saturation', ea.SET).withValueMin(0).withValueMax(100).withUnit('%'))
    .withFeature(exposes.numeric('level', ea.SET).withValueMin(0).withValueMax(254))
    .withFeature(exposes.numeric('fade', ea.SET).withValueMin(0).withValueMax(25500).withUnit('ms').withDescription('Ramp the level over this long (100 ms steps).'));

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
    // a definition's own toZigbee/fromZigbee/exposes with those contributed by
    // extend, so the light's on/off, brightness and colour behaviour is
    // inherited intact. An earlier draft assigned `exposes` from
    // `builtin.exposes` - which is undefined on a modern extend-based
    // definition - and would have replaced every light control with just these.
    toZigbee: [...(builtin.toZigbee ?? []), tzLightShow],
    fromZigbee: [...(builtin.fromZigbee ?? []), tuya.fz.datapoints],
    meta: {...(builtin.meta ?? {}), tuyaDatapoints},

    // A function rather than an array, so the controls appear only on fixtures
    // actually running the custom firmware. z2m composes this with the exposes
    // contributed by `extend`, so the light itself is unaffected either way.
    exposes: (device, options) => !hasCustomFirmware(device) ? [] : [
        exposes.enum('light_show', ea.ALL, EFFECTS)
            .withDescription(
                'On-device light show (custom firmware only; does nothing on stock). ' +
                'Renders on the chip, so it is not limited by the coordinator\'s command rate. ' +
                'Select "stop" to hand the output back to normal on/off/brightness/colour control. ' +
                '"burst" is deliberately mostly-dark: short chaotic strobe bursts with darkness between; ' +
                '"explode" blooms gold into full white and then holds it; "solid" holds the show colour at the show level. ' +
                'Re-sending a running effect restarts it from its first frame. ' +
                'From build 36 the value is a real device report after unicast writes and on /get; after a group write it is what was asked for.'),
        exposes.numeric('light_show_speed', ea.ALL).withValueMin(1).withValueMax(100)
            .withDescription('Light show speed, 1-100 (firmware default 50). Applies live to a running show without restarting it.'),
        exposes.numeric('light_show_hue', ea.ALL).withValueMin(0).withValueMax(360).withUnit('°')
            .withDescription(
                'Show colour for the colour-following effects (pulse, twinkle, strobe, wave, burst, solid), in degrees; ' +
                '360 = follow the fixture\'s own colour (the default). Since build 36 this is SHOW state: it never recolours an idle fixture, ' +
                'it can be staged while the fixture is dark, it applies live to a running show, and "stop" returns the fixture to its own colour. ' +
                'Own-colour effects (rainbow, candle, fire, lightning, chase, color_step, snow, explode) ignore it.'),
        exposes.numeric('light_show_saturation', ea.ALL).withValueMin(0).withValueMax(100).withUnit('%')
            .withDescription('Show saturation for the colour-following effects, 0-100% (default 100). Show state, like light_show_hue.'),
        exposes.numeric('light_show_level', ea.ALL).withValueMin(0).withValueMax(255)
            .withDescription(
                'Show brightness 0-254, independent of the fixture\'s own level; 255 = follow the fixture (default). ' +
                'With an explicit level an effect renders at full from an OFF fixture, no flare needed, and 0 is a blackout the effect survives. ' +
                'An explicit level also scales the own-colour effects (fire, explode, ...), which otherwise render at full.'),
        exposes.numeric('light_show_phase', ea.ALL).withValueMin(0).withValueMax(359).withUnit('°')
            .withDescription(
                'This fixture\'s own timeline offset into the effect\'s period (works from build 36: 180° on a 4 s pulse is 2 s). ' +
                'Applies to the periodic effects (pulse, strobe, rainbow, wave, chase, color_step); the random ones scatter on their own.'),
        exposes.numeric('light_show_index', ea.ALL).withValueMin(0).withValueMax(255)
            .withDescription(
                'Fixture index 0-254 in spatial order, 255 = none. PERSISTED on the chip: set it once per fixture at commissioning, ' +
                'on the device topic only (never a group). Effective phase = phase + index x spread.'),
        exposes.numeric('light_show_spread', ea.ALL).withValueMin(0).withValueMax(359).withUnit('°')
            .withDescription(
                'Degrees of extra phase per index step. With 19 fixtures indexed 0-18, spread 19 lays a full wheel across the house, ' +
                'so ONE group broadcast of pulse/strobe/chase/rainbow runs in order from fixture 0. 0 (default) = index has no effect.'),
        exposes.enum('light_show_takeover', ea.ALL, TAKEOVER)
            .withDescription(
                '"release" (default): any on/off, level, colour or scene command stops a running show, as before. ' +
                '"hold": the show keeps the output; level/colour commands update the fixture\'s own state underneath it ' +
                '(seen live where the show follows the fixture), OFF blacks the show out and ON brings it back.'),
        exposes.numeric('light_show_duration', ea.ALL).withValueMin(0).withValueMax(65535).withUnit('ms')
            .withDescription('An effect stops itself after this long (0 = runs until stopped). Persists until changed.'),
        exposes.numeric('light_show_density', ea.ALL).withValueMin(0).withValueMax(100).withUnit('%')
            .withDescription(
                'burst only: percent of one-second slots that contain sparks, independent of speed; speed then sets the flash rate inside a burst. ' +
                '0 (default) = the build-25 behaviour, where speed picks the density (3-35%) and sparks flash flat out.'),
        exposes.composite('light_show_cue', 'light_show_cue', ea.SET)
            .withDescription(
                'ONE Zigbee frame carrying any combination of the controls above plus an optional delay, applied atomically ' +
                '(parameters first, then the effect, then cue_run). {"effect":"burst","hue":220,"level":254} lights sparks on a dark room in one frame; ' +
                'add "delay":400 and every member of the group starts them 400 ms after receipt, together. A new cue replaces a pending delayed one.')
            .withFeatures(cueFeatures()),
        exposes.list('light_show_cue_list', ea.SET, cueListEntry())
            .withDescription(
                'Upload a sequence of up to 32 timed entries to the chip (6 per frame, so 32 entries cost 6 frames - do it before the show). ' +
                'Then light_show_cue_run "run" plays the whole thing at 50 fps locally: one frame for the show. ' +
                'Omitted fields keep their value; effect "stop" releases the output while the sequence continues. ' +
                'Held in RAM: re-upload after a power cut.'),
        exposes.enum('light_show_cue_run', ea.ALL, CUE_RUN)
            .withDescription('"run" plays the uploaded list once, "loop" repeats it (the last entry\'s t is the loop length), "stop" aborts it and stops the effect.'),
        exposes.numeric('light_show_cue_count', ea.STATE)
            .withDescription('Entries the chip holds (device report).'),
    ],
});

module.exports = definition;
