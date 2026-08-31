/**
 * @file pcm512x_regs.h
 * @brief TI PCM5121 / PCM5122 register map (datasheet SLAS763C)
 *
 * Page 0 is shared verbatim with the TAS575x this project already drives —
 * that part is a PCM512x core with a class-D output stage bolted on. Page 1 is
 * the piece that differs: it configures a line driver rather than an
 * amplifier.
 */
#pragma once

/* ---------- Page selection ---------- */

/** Page select. Bit 7 enables register auto-increment. */
#define PCM512X_REG_PAGE 0x00

#define PCM512X_PAGE_CONTROL 0x00 /**< Clocking, DSP, volume, status. */
#define PCM512X_PAGE_ANALOG  0x01 /**< Line driver and charge pump.   */

/* ---------- Page 0: reset and power ---------- */

#define PCM512X_REG_RESET      0x01
#define PCM512X_RESET_MODULES  0x10 /**< RSTM — reset the DSP modules. */
#define PCM512X_RESET_REGISTER 0x01 /**< RSTR — reset the register map. */

#define PCM512X_REG_POWER     0x02
#define PCM512X_POWER_ACTIVE  0x00
#define PCM512X_POWER_STANDBY 0x10 /**< RQST — analogue off, clocks stopped. \
                                    */
#define PCM512X_POWER_DOWN 0x01 /**< RQPD — everything but the register map. \
                                 */

#define PCM512X_REG_MUTE  0x03
#define PCM512X_MUTE_NONE 0x00
#define PCM512X_MUTE_BOTH 0x11 /**< RQML (b4) | RQMR (b0). */

/** PLL enable / lock. b4 PLCK is read-only and *inverted*: 0 means locked. */
#define PCM512X_REG_PLL_ENABLE 0x04
#define PCM512X_PLL_UNLOCKED   0x10
#define PCM512X_PLL_ENABLED    0x01

/* ---------- Page 0: clocking ---------- */

/** SREF, b6:4 — which pin the PLL takes its reference from. */
#define PCM512X_REG_PLL_REF  0x0D
#define PCM512X_PLL_REF_SCK  0x00 /**< SCK/MCLK pin (reset default). */
#define PCM512X_PLL_REF_BCK  0x10 /**< Bit clock — for boards with no MCLK. */
#define PCM512X_PLL_REF_MASK 0x70

/** Which clock errors are ignored rather than faulting the DAC. */
#define PCM512X_REG_IGNORE_ERR        0x25
#define PCM512X_IGNORE_FS             0x40 /**< IDFS */
#define PCM512X_IGNORE_BCK            0x20 /**< IDBK */
#define PCM512X_IGNORE_SCK            0x10 /**< IDSK */
#define PCM512X_IGNORE_CLOCK_HALT     0x08 /**< IDCH — SCK stopped. */
#define PCM512X_IGNORE_CLOCK_MISSING  0x04 /**< IDCM — LRCK/BCK missing. */
#define PCM512X_DISABLE_CLOCK_AUTOSET 0x02 /**< DCAS */
#define PCM512X_IGNORE_PLL_LOCK       0x01 /**< IPLK */

/** Audio interface: b5:4 format, b1:0 word length. Reset = I2S, 24-bit. */
#define PCM512X_REG_I2S_FORMAT 0x28

/** PSEL — process flow selection. A PCM512x has no user program RAM. */
#define PCM512X_REG_DSP_PROGRAM 0x2B

/* ---------- Page 0: volume ---------- */

#define PCM512X_REG_VOL_CONTROL 0x3C /**< Independent / left-follows / right. \
                                      */
#define PCM512X_REG_VOL_LEFT  0x3D
#define PCM512X_REG_VOL_RIGHT 0x3E

/** 0x00 = +24 dB, 0x30 = 0 dB, 0.5 dB per step, 0xFE = -103 dB, 0xFF = mute. */
#define PCM512X_VOL_REG_MAX_DB 24.0f
#define PCM512X_VOL_MIN_DB     (-103.0f)
#define PCM512X_VOL_REG_MUTE   0xFF

/* ---------- Page 0: read-only status ---------- */

#define PCM512X_REG_DSP_OVERFLOW  0x5A
#define PCM512X_REG_FS_SPEED      0x5B /**< Detected sample-rate class. */
#define PCM512X_REG_BCK_RATIO_MSB 0x5C
#define PCM512X_REG_BCK_RATIO_LSB 0x5D

#define PCM512X_REG_CLOCK_STATUS   0x5E
#define PCM512X_CLK_SCK_MISSING    0x40 /**< CDST — nothing on the SCK pin. */
#define PCM512X_CLK_PLL_UNLOCKED   0x20
#define PCM512X_CLK_LRCK_BCK_GONE  0x10
#define PCM512X_CLK_FS_SCK_INVALID 0x08
#define PCM512X_CLK_SCK_INVALID    0x04
#define PCM512X_CLK_BCK_INVALID    0x02
#define PCM512X_CLK_FS_INVALID     0x01

#define PCM512X_REG_ANALOG_MUTE_MON 0x6C /**< Active low: 0 = muted. */
#define PCM512X_REG_SHORT_DETECT    0x6D
#define PCM512X_REG_MUTE_STATUS     0x72

#define PCM512X_REG_POWER_STATE  0x76
#define PCM512X_POWER_BOOT_DONE  0x80 /**< BOTM */
#define PCM512X_POWER_STATE_MASK 0x0F

#define PCM512X_REG_AUTO_MUTE_FLAGS 0x78

/* ---------- Page 1: analogue line driver ---------- */

/** OSEL — 0 = VREF (2.1 Vrms, reset default), 1 = VCOM (1 Vrms). */
#define PCM512X_P1_REG_OUTPUT_AMPLITUDE 0x01

/** LAGN (b4) / RAGN (b0) — 0 = 0 dB, 1 = -6 dB. */
#define PCM512X_P1_REG_ANALOG_GAIN       0x02
#define PCM512X_P1_ANALOG_GAIN_MINUS_6DB 0x11

/** UEPD (b1) / UIPD (b0) — undervoltage protection. Reset 0x00 on a PCM512x
 *  and 0x11 on a TAS575x, which is the cleanest way to tell them apart. */
#define PCM512X_P1_REG_UVP           0x05
#define PCM512X_P1_UVP_TAS575X_RESET 0x11

/** AMCT (b0) — 0 = analogue mute follows the digital mute (reset default). */
#define PCM512X_P1_REG_ANALOG_MUTE 0x06

/** AGBL (b4) / AGBR (b0) — +0.8 dB analogue boost. */
#define PCM512X_P1_REG_GAIN_BOOST 0x07
