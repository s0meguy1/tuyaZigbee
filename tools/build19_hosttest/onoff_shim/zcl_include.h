/* Minimal ZCL surface for the real zcl_onOffCb.c host test. */
#pragma once

#define ZCL_BASIC_MAX_LENGTH 32
#define ZCL_ONOFF_STATUS_OFF 0
#define ZCL_ONOFF_STATUS_ON 1
#define ZCL_CMD_ONOFF_OFF 0
#define ZCL_CMD_ONOFF_ON 1
#define ZCL_CMD_ONOFF_TOGGLE 2
#define ZCL_CMD_OFF_WITH_EFFECT 0x40
#define ZCL_CMD_ON_WITH_RECALL_GLOBAL_SCENE 0x41
#define ZCL_CMD_ON_WITH_TIMED_OFF 0x42
#define ZCL_CMD_LEVEL_MOVE_TO_LEVEL 0x00
#define ZCL_CMD_LEVEL_MOVE 0x01
#define ZCL_CMD_LEVEL_STEP 0x02
#define ZCL_CMD_LEVEL_STOP 0x03
#define ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF 0x04
#define ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF 0x05
#define ZCL_CMD_LEVEL_STEP_WITH_ON_OFF 0x06
#define ZCL_CMD_LEVEL_STOP_WITH_ON_OFF 0x07
#define LEVEL_MOVE_UP 0x00
#define LEVEL_MOVE_DOWN 0x01
#define LEVEL_STEP_UP 0x00
#define LEVEL_STEP_DOWN 0x01
#define ZCL_LEVEL_ATTR_MIN_LEVEL 0x01
#define ZCL_LEVEL_ATTR_MAX_LEVEL 0xFE
#define ZCL_STA_SUCCESS 0

typedef u8 status_t;

typedef struct { u8 unused; } zcl_specClusterInfo_t;
typedef struct { u8 unused; } af_simple_descriptor_t;
typedef struct { u8 unused; } zclIncoming_t;

typedef struct {
    u8 dstEp;
} zclIncomingAddrInfo_t;

typedef union {
    u8 onOffCtrl;
    struct {
        u8 acceptOnlyWhenOn : 1;
        u8 reserved : 7;
    } bits;
} zcl_onoffCtrl_t;

typedef struct {
    zcl_onoffCtrl_t onOffCtrl;
    u16 onTime;
    u16 offWaitTime;
} zcl_onoff_onWithTimeOffCmd_t;

typedef struct { u8 unused; } zcl_onoff_offWithEffectCmd_t;

typedef struct {
    u8 level;
    u16 transitionTime;
    u8 optPresent;
    u8 optionsMask;
    u8 optionsOverride;
} moveToLvl_t;

typedef struct {
    u8 moveMode;
    u8 rate;
    u8 optPresent;
    u8 optionsMask;
    u8 optionsOverride;
} move_t;

typedef struct {
    u8 stepMode;
    u8 stepSize;
    u16 transitionTime;
    u8 optPresent;
    u8 optionsMask;
    u8 optionsOverride;
} step_t;

typedef struct {
    u8 optPresent;
    u8 optionsMask;
    u8 optionsOverride;
} stop_t;

#define min2(a, b) ((a) < (b) ? (a) : (b))
#define max2(a, b) ((a) > (b) ? (a) : (b))
