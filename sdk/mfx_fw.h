/*
 * mfx_fw.h — reverse-engineered interface to the ALM MFX Pedal v118 firmware
 *
 * All addresses are for v118 ONLY (alm "mfx-pedal-v118"). A different ALM release
 * would need them re-located.
 *
 * ABI: Cortex-M4F, softfp (floats passed in core registers), build with
 *   -mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
#define _Static_assert static_assert
#endif

/* ---------- Memory ---------- */
#define MFX_REGISTRY        0x20002218u   /* 20 engines x 0x4E4 bytes            */
#define MFX_ENGINE_SIZE     0x4E4u
#define MFX_MAX_ENGINES     20
#define MFX_REG_COUNT       (*(volatile uint8_t  *)(MFX_REGISTRY + 0x61D4))
#define MFX_REG_ACTIVE      (*(volatile uint32_t *)(MFX_REGISTRY + 0x61D0))

#ifndef MFX_DELAYBUF
#define MFX_DELAYBUF        ((int16_t *)0x20008428u)  /* shared delay buffer          */
#endif
#define MFX_DELAYBUF_LEN    42000                     /* int16 samples                */
#ifndef MFX_CCM
#define MFX_CCM             ((void *)0x10000000u)     /* 64 KB, shared between engines   */
#endif
#define MFX_CCM_SIZE        0x10000u

/* Sample rate: the I2S is configured with I2S_AudioFreq = 44100 (0xAC44). */
#ifndef MFX_SAMPLE_RATE
#define MFX_SAMPLE_RATE     44100.0f
#endif
#define MFX_BLOCK_FRAMES    16          /* firmware calls process() every 16 frames */
#define MFX_SYSCLK          (*(volatile uint32_t *)0x20000040u)   /* SystemCoreClock (168 MHz) */

/* ---------- Structures ---------- */
typedef struct mfx_engine mfx_engine_t;
typedef struct mfx_param  mfx_param_t;

/* Parameter: 0x48 bytes, at engine + 0x1C + idx*0x48 */
struct mfx_param {
    mfx_engine_t *engine;        /* +0x00 */
    char     name[16];           /* +0x04  text shown on the display */
    uint8_t  type;               /* +0x14 */
    uint8_t  index;              /* +0x15 */
    uint8_t  _pad16[2];
    int32_t  value;              /* +0x18  current value (incl. knob/MIDI/exp) */
    int32_t  saved;              /* +0x1C */
    int32_t  def;                /* +0x20 */
    int32_t  min;                /* +0x24 */
    int32_t  max;                /* +0x28 */
    uint8_t  wrap;               /* +0x2C */
    uint8_t  _2d;
    uint8_t  flag2e;             /* +0x2E */
    uint8_t  hires;              /* +0x2F  1 = modulation (knobs/exp) at 12 bits */
    uint8_t  _30[4];
    uint8_t  assign[3];          /* +0x34 source, amount, offset */
    uint8_t  _37;
    void   (*on_change)(mfx_param_t *p, uint8_t idx, int arg);  /* +0x38 */
    const char *(*label)(mfx_param_t *p);                       /* +0x3C */
    void   (*draw)(mfx_param_t *p, uint32_t flags);             /* +0x40 */
    int32_t  style;              /* +0x44 */
};
#if !defined(NUBES_HOST) && !defined(LAB_HOST)
_Static_assert(sizeof(struct mfx_param) == 0x48, "param size");
#endif

typedef void (*mfx_process_fn)(mfx_engine_t *e, const int16_t *in, int16_t *out, int16_t n);
typedef void (*mfx_engine_fn)(mfx_engine_t *e);

/* Engine: 0x4E4 bytes */
struct mfx_engine {
    char        name[16];        /* +0x000 */
    char        category[12];    /* +0x010 */
    mfx_param_t params[16];      /* +0x01C */
    uint8_t     nparams;         /* +0x49C */
    uint8_t     order[16];       /* +0x49D  on-screen order */
    uint8_t     norder;          /* +0x4AD */
    uint8_t     _4ae[2];
    const void *presets;         /* +0x4B0  factory preset table (0xA0 each) */
    uint8_t     npresets;        /* +0x4B4 */
    uint8_t     nuser;           /* +0x4B5 */
    uint8_t     _4b6[2];
    uint16_t    io;              /* +0x4B8  I/O indicator */
    uint8_t     _4ba[2];
    mfx_process_fn process;      /* +0x4BC  audio (ISR)            */
    mfx_engine_fn  activate;     /* +0x4C0                         */
    mfx_engine_fn  deactivate;   /* +0x4C4                         */
    void (*tap)(mfx_engine_t *e, int period, char flag);  /* +0x4C8 */
    int  (*poll)(mfx_engine_t *e);                        /* +0x4CC */
    void (*fn4d0)(mfx_engine_t *e);                       /* +0x4D0 */
    void (*fn4d4)(mfx_engine_t *e, int x);                /* +0x4D4 */
    void (*fn4d8)(mfx_engine_t *e);                       /* +0x4D8 */
    uint8_t     _4dc[4];
    void       *state;           /* +0x4E0 */
};
#if !defined(NUBES_HOST) && !defined(LAB_HOST)
_Static_assert(sizeof(struct mfx_engine) == 0x4E4, "engine size");
#endif

/* Factory preset record: 0xA0 bytes */
typedef struct {
    char     name[32];           /* "LINEA1\nLINEA2" */
    uint32_t value[16];          /* value per parameter index */
    uint32_t assign[16];         /* 0x0000630F = no assignment */
} mfx_preset_t;
#if !defined(NUBES_HOST) && !defined(LAB_HOST)
_Static_assert(sizeof(mfx_preset_t) == 0xA0, "preset size");
#endif
#define MFX_NO_ASSIGN 0x0000630Fu

/* Observed parameter types */
enum {
    MFX_PT_LEVEL   = 0,   /* 0..0xFFFF continuous */
    MFX_PT_TIME    = 1,
    MFX_PT_TOGGLE  = 2,
    MFX_PT_ENUM    = 3,   /* uses label() */
    MFX_PT_FREQ    = 4,
    MFX_PT_PRESET  = 5,
    MFX_PT_RATE    = 6,
    MFX_PT_REGEN   = 7,
    MFX_PT_EMPTY   = 8,
    MFX_PT_PERCENT = 11,  /* 0..100 */
    MFX_PT_MIX     = 12,
};

/* I/O indicator values seen in factory engines */
#define MFX_IO_MONO        10
#define MFX_IO_MONO_STEREO 0x12
#define MFX_IO_STEREO      0x14

/* ---------- Firmware entry points (v118) ---------- */
#define FW(addr, type) ((type)((addr) | 1u))

#define mfx_snprintf   FW(0x0801ECF8, int (*)(char *, int, const char *, ...))
#define mfx_printf     FW(0x0801EC24, int (*)(const char *, ...))
#define mfx_add_param  FW(0x0802666C, void (*)(mfx_engine_t *, uint8_t idx, uint8_t type, \
                          const char *name, int32_t def, int32_t min, int32_t max,      \
                          void (*cb)(mfx_param_t *, uint8_t, int)))
#define mfx_add_presets FW(0x080267E8, void (*)(mfx_engine_t *, uint8_t idx, const void *tbl, uint8_t count))
#define mfx_add_empty  FW(0x08026870, void (*)(mfx_engine_t *, uint8_t idx))
#define mfx_set_io     FW(0x0802664A, void (*)(mfx_engine_t *, uint16_t))
/* constant-power wet/dry gains (respects the global "mix disabled" setting) */
#ifndef mfx_mix_gains
#define mfx_mix_gains  FW(0x08026024, void (*)(uint16_t mix, float *wet, float *dry))
#endif
#ifndef mfx_set_dsp_handler
#define mfx_set_dsp_handler FW(0x08029B20, void (*)(mfx_engine_t *, void (*)(mfx_engine_t *, uint16_t, int)))
#endif
#ifndef mfx_clear_delaybuf
#define mfx_clear_delaybuf  FW(0x08027A90, void (*)(void))
#endif
/* global WET/DRY flag: 1 = mix mode (MIX visible), 0 = insert (MIX hidden, 100% wet) */
#ifndef mfx_mix_enabled
#define mfx_mix_enabled FW(0x08026174, uint8_t (*)(void))
#endif
/* set a parameter value as the UI would (calls on_change and saves to FRAM) */
#define mfx_set_value  FW(0x080272D2, void (*)(mfx_param_t *, int32_t))
/* save a parameter's value + assignment to FRAM (slot 0 = current state) */
#define mfx_param_save FW(0x080271A2, void (*)(mfx_param_t *, uint8_t slot))

/* Works around an ALM firmware bug: in a never-used FRAM slot (0xFF), validation
 * leaves the assignment "offset" at 0x99 (153), so a parameter assigned to A/B/exp
 * gets stuck at 100%. */
static inline int mfx_fix_assign(mfx_param_t *p)
{
    int bad = 0;
    if (p->assign[1] > 99) { p->assign[1] = 99; bad = 1; }
    if (p->assign[2] > 99) { p->assign[2] = 0;  bad = 1; }
    return bad;
}

/* Hook site: BL to the last factory constructor (UTILITIES) */
#define MFX_HOOK_SITE      0x08025E70u
#define MFX_HOOK_ORIG_FN   FW(0x0803B15C, void (*)(mfx_engine_t *))

static inline mfx_param_t *mfx_param(mfx_engine_t *e, int i) { return &e->params[i]; }
static inline int32_t mfx_value(mfx_engine_t *e, int i) { return e->params[i].value; }
