/*
 * Motor "USER" para el MFX Pedal v118: se registra en el hueco libre nº 20
 * y ofrece varios MODES con 4 parámetros cuyos nombres cambian según el modo.
 *
 * Reglas de este código (se ejecuta dentro del firmware de ALM):
 *  - nada de variables globales escribibles (.data/.bss): no hay RAM libre.
 *    El estado de DSP vive en la RAM CCM, que es nuestra mientras USER está activo.
 *  - los valores de los parámetros viven en las estructuras del firmware
 *    (así funcionan presets, FRAM, knobs A/B, MIDI y expresión sin hacer nada).
 */
#include "mfx_fw.h"
#include "user_dsp.h"

enum { IDX_MODE = 0, IDX_P1, IDX_P2, IDX_P3, IDX_P4, IDX_MIX, IDX_PRESET, NUM_PARAMS };

#define ST ((user_state_t *)MFX_CCM)

/* ---------- presets de fábrica del motor USER ---------- */
#define PV(mode, a, b, c, d, mix) \
    { (mode), (a), (b), (c), (d), (mix), 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, \
      0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }
#define NOA { MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, \
              MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, \
              MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, \
              MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN, MFX_NO_ASSIGN }

__attribute__((used, aligned(4)))
static const mfx_preset_t user_presets[] = {
    { "SOFT\nTREMOLO", PV(MODE_TREMOLO, 0x9000, 0x9000, 0x0000, 0x0000, 0xFFFF), NOA },
    { "AUTO\nPAN",     PV(MODE_TREMOLO, 0x7000, 0xFFFF, 0x2000, 0xFFFF, 0xFFFF), NOA },
    { "BELL\nRING",    PV(MODE_RINGMOD, 0xA000, 0x1800, 0x2000, 0x0000, 0xB000), NOA },
    { "FOLD\nFUZZ",    PV(MODE_FOLDER,  0x9000, 0x6000, 0xA000, 0x9000, 0xFFFF), NOA },
    { "TEST\nA440",    PV(MODE_TEST440, 0x8000, 0, 0, 0, 0xFFFF),              NOA },
};
#define NUM_PRESETS (sizeof(user_presets) / sizeof(user_presets[0]))

/* ---------- utilidades ---------- */
static void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; i < 15 && src[i]; i++) dst[i] = src[i];
    for (; i < 16; i++) dst[i] = 0;
}

static int cur_mode(mfx_engine_t *e)
{
    int m = mfx_value(e, IDX_MODE);
    return (m < 0 || m >= NUM_MODES) ? 0 : m;
}

static void rename_params(mfx_engine_t *e)
{
    int m = cur_mode(e);
    for (int k = 0; k < 4; k++)
        copy_name(e->params[IDX_P1 + k].name, user_param_names[m][k]);
}

/* ---------- callbacks ---------- */
static const char *mode_label(mfx_param_t *p)
{
    int m = p->value;
    return (m >= 0 && m < NUM_MODES) ? user_mode_names[m] : "?";
}

static void on_param(mfx_param_t *p, uint8_t idx, int arg)
{
    (void)arg;
    if (idx == IDX_MODE && p->engine) rename_params(p->engine);
}

static void dsp_noop(mfx_engine_t *e, uint16_t idx, int v) { (void)e; (void)idx; (void)v; }

static void user_activate(mfx_engine_t *e)
{
    mfx_set_dsp_handler(e, dsp_noop);
    user_dsp_init(ST, MFX_SAMPLE_RATE);
    rename_params(e);
}

static void user_deactivate(mfx_engine_t *e)
{
    (void)e;
    ST->magic = 0;
}

static void user_process(mfx_engine_t *e, const int16_t *in, int16_t *out, int16_t n)
{
    user_state_t *s = ST;
    if (s->magic != USER_MAGIC) {            /* por seguridad: paso directo */
        for (int i = 0; i < n; i++) out[i] = in[i];
        return;
    }
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = (float)(mfx_value(e, IDX_P1 + k) & 0xFFFF) * (1.f / 65535.f);
    float wet = 1.f, dry = 0.f;
    mfx_mix_gains((uint16_t)mfx_value(e, IDX_MIX), &wet, &dry);
    user_dsp_process(s, cur_mode(e), t, wet, dry, in, out, n);
}

/* ---------- constructor ---------- */
static void user_ctor(mfx_engine_t *e)
{
    copy_name(e->name, "USER");
    for (int i = 0; i < 12; i++) e->category[i] = 0;
    { const char *c = "LAB MODES"; for (int i = 0; c[i] && i < 11; i++) e->category[i] = c[i]; }

    e->process    = user_process;
    e->activate   = user_activate;
    e->deactivate = user_deactivate;
    e->nparams    = NUM_PARAMS;

    mfx_add_param(e, IDX_MODE, MFX_PT_ENUM, "MODE", 0, 0, NUM_MODES - 1, on_param);
    e->params[IDX_MODE].label = mode_label;
    e->params[IDX_MODE].flag2e = 0;          /* igual que los MODE de fábrica (flag +0x2E = 0) */

    mfx_add_param(e, IDX_P1, MFX_PT_LEVEL, "P1", 0x8000, 0, 0xFFFF, on_param);
    mfx_add_param(e, IDX_P2, MFX_PT_LEVEL, "P2", 0x8000, 0, 0xFFFF, on_param);
    mfx_add_param(e, IDX_P3, MFX_PT_LEVEL, "P3", 0x4000, 0, 0xFFFF, on_param);
    mfx_add_param(e, IDX_P4, MFX_PT_LEVEL, "P4", 0x0000, 0, 0xFFFF, on_param);
    mfx_add_param(e, IDX_MIX, MFX_PT_MIX,  "MIX", 0xFFFF, 0, 0xFFFF, on_param);
    mfx_add_presets(e, IDX_PRESET, user_presets, (uint8_t)NUM_PRESETS);

    mfx_set_io(e, MFX_IO_STEREO);
    rename_params(e);
}

/* ---------- enganche: sustituye la llamada al constructor de UTILITIES ---------- */
__attribute__((used, section(".hook")))
void user_hook(mfx_engine_t *utilities_slot)
{
    MFX_HOOK_ORIG_FN(utilities_slot);                /* registrar UTILITIES como siempre */
    uint8_t n = MFX_REG_COUNT;
    if (n < MFX_MAX_ENGINES) {
        mfx_engine_t *e = (mfx_engine_t *)(MFX_REGISTRY + n * MFX_ENGINE_SIZE);
        MFX_REG_COUNT = n + 1;
        user_ctor(e);
    }
}

/* gcc puede emitir llamadas a memset/memcpy */
void *memset(void *d, int c, unsigned n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
