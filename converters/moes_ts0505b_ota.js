/**
 * zigbee2mqtt external converter - Moes ZB-TDD6-RCW-4 (TS0505B / _TZ3210_b8jdosxo)
 *
 * PURPOSE: enable OTA for these lights and nothing else.
 *
 * z2m's OTA extension refuses any device whose definition lacks `ota`
 * (dist/extension/otaUpdate.js: `if (!device.definition?.ota)`), and the
 * built-in definition for this light (Tuya "TS0505B_1", 51 white labels)
 * does not set it - so supports_ota is false and no OTA of any kind is
 * possible without this file.
 *
 * DESIGN: derive from z2m's OWN built-in definition rather than hand-writing
 * one, so light behaviour, exposes, converters and HA entities stay
 * byte-identical to what they are today. The only differences:
 *
 *   1. `ota: true`                     - the point of the file
 *   2. fingerprint restricted to       - so ONLY the 46 Moes downlights use
 *      TS0505B + _TZ3210_b8jdosxo        this definition; every other
 *                                        TS0505B device on the network keeps
 *                                        the built-in one untouched
 *   3. model/vendor/description are    - identical strings to what the white
 *      pinned to the white-label values   label resolved to before, so HA
 *                                        entity naming does not change
 *
 * The added "update" entity in Home Assistant is the expected visible change.
 *
 * TO REVERT: delete this file from data/external_converters/ and restart z2m.
 *
 * Install: data/external_converters/moes_ts0505b_ota.js (z2m >= 2.0 auto-loads
 * every file in that directory; the old `external_converters:` YAML key is
 * ignored).
 */

const {definitions} = require('zigbee-herdsman-converters/devices/tuya');

const builtin = definitions.find((d) => (d.whiteLabel || []).some((w) => w.model === 'ZB-TDD6-RCW-4'));

if (!builtin) {
    throw new Error('moes_ts0505b_ota: built-in TS0505B_1 definition not found - z2m changed, review before using');
}

const definition = {...builtin};

// we are the specific model now, not the generic multi-white-label one
delete definition.zigbeeModel;
delete definition.whiteLabel;

Object.assign(definition, {
    fingerprint: [{modelID: 'TS0505B', manufacturerName: '_TZ3210_b8jdosxo'}],
    model: 'ZB-TDD6-RCW-4',
    vendor: 'Moes',
    description: 'RGB+CCT 6W Smart Downlight',
    ota: true,
});

module.exports = definition;
