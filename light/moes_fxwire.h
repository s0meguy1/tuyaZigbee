/********************************************************************************************************
 * @file    moes_fxwire.h
 *
 * @brief   Light-show wire format on the Tuya manufacturer cluster (0xEF00),
 *          deliberately free of SDK, ZCL and hardware dependencies so the exact
 *          parser and report encoder the firmware runs execute on the host
 *          (tools/fxwire_hosttest). Build 36.
 *
 * A Tuya datapoint frame is
 *
 *     seq u16 BE, then one or more of: dp u8, type u8, len u16 BE, data[len]
 *
 * zigbee-herdsman's writeListTuyaDataPointValues() emits exactly that, and its
 * sendDataPoints() will carry SEVERAL datapoints in ONE frame. Build 27-35 read
 * only the first. Build 36 reads them all and applies the frame as a unit, so a
 * single group broadcast can say "in 400 ms, start explode at speed 100, hue 30,
 * level 254" and every member schedules the same moment. That is what turns a
 * cue from three or four frames on a ~1.55 frame/s transport into one.
 *
 * DATAPOINTS (0x6E-0x72 unchanged since build 27; 0x73+ new in build 36)
 *
 *   0x6E effect     enum   0..MOES_EF_MAX-1, 0 = stop (releases the output AND
 *                          aborts a running cue list)
 *   0x6F speed      value  1..100
 *   0x70 phase      value  0..359 degrees, this fixture's own timeline offset
 *   0x71 hue        value  0..359 degrees; >= 360 = follow the fixture's colour
 *   0x72 saturation value  0..100 percent
 *   0x73 index      value  0..254 fixture index, 255 = none. PERSISTED in NV.
 *   0x74 spread     value  0..359 degrees of extra phase per index step
 *   0x75 level      value  0..254 show brightness; 255 = follow the fixture
 *   0x76 fade       value  ms, ramp for a level carried in the same frame
 *   0x77 delay      value  ms, defer THIS WHOLE FRAME (0 = now)
 *   0x78 duration   value  ms an effect runs before stopping itself, 0 = forever
 *   0x79 takeover   enum   0 = a ZCL command stops the effect, 1 = it persists
 *   0x7A density    value  0 = auto (from speed), 1..100 percent of burst slots
 *   0x7B cue_load   raw    [start u8][entry x N], entry = MOES_FX_CUE_WIRE_LEN
 *                          bytes, applied at once (never deferred)
 *   0x7C cue_run    enum   0 = abort + stop, 1 = run once, 2 = loop
 *   0x7D cue_count  value  report only
 *
 * A frame is atomic: one bad value rejects the whole frame and nothing in it is
 * applied, so a show can never half-land. Unknown datapoint ids are rejected
 * too - a converter newer than the firmware fails loudly rather than partially.
 *
 * Everything the chip sends back (dataReport, command 0x02, same layout) uses
 * the same datapoint ids, so zigbee2mqtt's tuya.fz.datapoints decodes it with
 * one table.
 *
 * @date    2026
 *******************************************************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MOES_DP_EFFECT      0x6E
#define MOES_DP_SPEED       0x6F
#define MOES_DP_PHASE       0x70
#define MOES_DP_HUE         0x71
#define MOES_DP_SAT         0x72
#define MOES_DP_INDEX       0x73
#define MOES_DP_SPREAD      0x74
#define MOES_DP_LEVEL       0x75
#define MOES_DP_FADE        0x76
#define MOES_DP_DELAY       0x77
#define MOES_DP_DURATION    0x78
#define MOES_DP_TAKEOVER    0x79
#define MOES_DP_DENSITY     0x7A
#define MOES_DP_CUE_LOAD    0x7B
#define MOES_DP_CUE_RUN     0x7C
#define MOES_DP_CUE_COUNT   0x7D

/* Tuya datapoint payload types. */
#define MOES_DPT_RAW        0
#define MOES_DPT_BOOL       1
#define MOES_DPT_VALUE      2
#define MOES_DPT_STRING     3
#define MOES_DPT_ENUM       4
#define MOES_DPT_BITMAP     5

/* Tuya cluster command ids. */
#define MOES_TUYA_CMD_DATA_REQUEST   0x00
#define MOES_TUYA_CMD_DATA_RESPONSE  0x01
#define MOES_TUYA_CMD_DATA_REPORT    0x02
#define MOES_TUYA_CMD_DATA_QUERY     0x03

/* Sentinels. "Follow" means take the value from the fixture's own ZCL state. */
#define MOES_FX_HUE_FOLLOW   0xFFFFu   /* wire: any hue >= 360 */
#define MOES_FX_HUE_WIRE_FOLLOW 360u   /* what a report says for follow */
#define MOES_FX_LEVEL_FOLLOW 0xFFu
#define MOES_FX_INDEX_NONE   0xFFu
#define MOES_FX_KEEP8        0xFFu     /* cue entry: leave this parameter alone */
#define MOES_FX_KEEP16       0xFFFFu

/* Which datapoints a parsed frame carries. */
#define MOES_FXF_EFFECT     (1u << 0)
#define MOES_FXF_SPEED      (1u << 1)
#define MOES_FXF_PHASE      (1u << 2)
#define MOES_FXF_HUE        (1u << 3)
#define MOES_FXF_SAT        (1u << 4)
#define MOES_FXF_INDEX      (1u << 5)
#define MOES_FXF_SPREAD     (1u << 6)
#define MOES_FXF_LEVEL      (1u << 7)
#define MOES_FXF_FADE       (1u << 8)
#define MOES_FXF_DELAY      (1u << 9)
#define MOES_FXF_DURATION   (1u << 10)
#define MOES_FXF_TAKEOVER   (1u << 11)
#define MOES_FXF_DENSITY    (1u << 12)
#define MOES_FXF_CUE_RUN    (1u << 13)

typedef struct {
	unsigned short present;      /* MOES_FXF_* bits */
	unsigned char  effect;
	unsigned char  speed;
	unsigned short phase;
	unsigned short spread;
	unsigned char  index;
	unsigned short hue;          /* 0..359 or MOES_FX_HUE_FOLLOW */
	unsigned char  sat;
	unsigned char  level;        /* 0..254 or MOES_FX_LEVEL_FOLLOW */
	unsigned short fade;
	unsigned short delay;
	unsigned short duration;
	unsigned char  takeover;
	unsigned char  density;
	unsigned char  cueRun;
} moes_fxFrame_t;

/*
 * One cue-list entry, 9 bytes on the wire, all big-endian:
 *
 *     t u16 | effect u8 | speed u8 | hue u16 | sat u8 | level u8 | fade u8
 *
 * t is ms from the start of the sequence. effect 0xFF, speed 0, hue 0xFFFF,
 * sat 0xFF and level 0xFF mean "keep what is set". effect 0 releases the output
 * (the sequence carries on). fade is in 100 ms units and ramps the level. hue
 * >= 360 (other than 0xFFFF) means follow.
 */
typedef struct {
	unsigned short tMs;
	unsigned char  effect;
	unsigned char  speed;
	unsigned short hue;
	unsigned char  sat;
	unsigned char  level;
	unsigned char  fade;
} moes_fxCue_t;

#define MOES_FX_CUE_WIRE_LEN  9u
#define MOES_FX_CUE_MAX       32u

typedef enum {
	MOES_FXW_OK = 0,
	MOES_FXW_MALFORMED,   /* structure does not parse: reject as malformed */
	MOES_FXW_INVALID,     /* parsed, but a value or id is not acceptable */
	MOES_FXW_EMPTY        /* parsed, nothing to apply (e.g. only a cue load) */
} moes_fxWireStatus_e;

/* Called during parsing for every cue_load datapoint. Return non-zero to
 * accept, zero to reject the frame. `entries` points at n wire entries. */
typedef int (*moes_fxCueLoadFn)(void *ctx, unsigned char start,
                                const unsigned char *entries, unsigned char n);

/* Parse a dataRequest/dataResponse payload. `effectMax` is MOES_EF_MAX. On
 * MOES_FXW_OK or MOES_FXW_EMPTY `out` is fully populated; on any other status
 * nothing in `out` may be used. Cue loads are handed to `cueFn` as they are
 * met (NULL rejects them). */
moes_fxWireStatus_e moes_fxWireParse(const unsigned char *payload, unsigned int len,
                                     unsigned char effectMax, moes_fxFrame_t *out,
                                     moes_fxCueLoadFn cueFn, void *cueCtx);

/* Decode one wire entry. */
void moes_fxCueDecode(const unsigned char *wire, moes_fxCue_t *out);

/* Everything a report carries. */
typedef struct {
	unsigned char  effect;
	unsigned char  speed;
	unsigned short phase;
	unsigned short hue;       /* MOES_FX_HUE_FOLLOW reports as 360 */
	unsigned char  sat;
	unsigned char  index;
	unsigned short spread;
	unsigned char  level;
	unsigned char  takeover;
	unsigned short duration;
	unsigned char  density;
	unsigned char  cueRun;
	unsigned char  cueCount;
} moes_fxReport_t;

/* Worst-case encoded size of moes_fxWireReportBuild(). */
#define MOES_FX_REPORT_MAX_LEN  80u

/* Encode a dataReport payload (seq + datapoints). Returns the byte count, or 0
 * if `cap` is too small. Values are sent as compact 1- or 2-byte "value" types;
 * zigbee-herdsman folds any length big-endian. */
unsigned int moes_fxWireReportBuild(unsigned char *buf, unsigned int cap,
                                    unsigned short seq, const moes_fxReport_t *st);

#ifdef __cplusplus
}
#endif
