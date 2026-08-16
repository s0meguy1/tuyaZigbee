/********************************************************************************************************
 * @file    light_effects.h
 *
 * @brief   On-device light-show engine.
 *
 * An effect takes over the 5-channel output stage and renders frames at
 * 25 fps until stopped (effect 0 returns the light to its ZCL attributes).
 * Effects are started/stopped through the Tuya manufacturer cluster
 * (cluster 0xEF00, see zcl_tuyaMfg.c) which zigbee2mqtt drives from a
 * small external converter, and can be broadcast to a Zigbee group so
 * the whole fleet runs one show. A per-light phase parameter offsets the
 * animation timeline so a group broadcast can lay out a chase or a wave
 * across rooms.
 *
 * @date    2026
 *******************************************************************************************************/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
	MOES_EF_STEADY = 0,
	MOES_EF_RAINBOW = 1,
	MOES_EF_PULSE = 2,
	MOES_EF_CANDLE = 3,
	MOES_EF_TWINKLE = 4,
	MOES_EF_FIRE = 5,
	MOES_EF_STROBE = 6,
	MOES_EF_WAVE = 7,
	MOES_EF_LIGHTNING = 8,
	MOES_EF_CHASE = 9,
	MOES_EF_COLOR_STEP = 10,
	MOES_EF_SNOW = 11,
	MOES_EF_MAX
} moes_effect_e;

typedef struct {
	u8  effect;      /* moes_effect_e */
	u8  speed;       /* 1..100, 50 default */
	u16 phase;       /* 0..359, timeline offset for group choreography */
	u32 t;           /* ms since start, phase applied */
} moes_fx_t;

extern moes_fx_t g_moesFx;

/* Start an effect / stop (0). Returns TRUE if the request was valid. */
bool lightFx_start(u8 effect, u8 speed, u16 phase);

/* True while an effect owns the output stage. */
bool lightFx_active(void);

/* Call from the main poll; drives the animation timer. */
void lightFx_init(void);

#if defined(__cplusplus)
}
#endif
