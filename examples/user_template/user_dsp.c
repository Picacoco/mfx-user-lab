#include "user_dsp.h"

const char *const user_mode_names[NUM_MODES] = {
    "TREMOLO", "RING MOD", "FOLDER", "TEST A440",
};

const char *const user_param_names[NUM_MODES][4] = {
    { "RATE",   "DEPTH",  "SHAPE",    "STEREO"  },
    { "FREQ",   "SPREAD", "DRIVE",    "WOBBLE"  },
    { "GAIN",   "SYMMETRY","TONE",    "OUTPUT"  },
    { "LEVEL",  "-",      "-",        "-"       },
};

#define PI_F 3.14159265358979f

/* seno de buena precisión para construir la tabla (sin libm) */
static float sin_poly(float x)   /* x en [-pi, pi] */
{
    /* reducir a [-pi/2, pi/2] */
    if (x >  PI_F * 0.5f) x =  PI_F - x;
    if (x < -PI_F * 0.5f) x = -PI_F - x;
    float x2 = x * x;
    /* Taylor hasta x^11: error < 1e-7 en el rango */
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f *
           (1.f - x2 / 72.f * (1.f - x2 / 110.f)))));
}

void user_dsp_init(user_state_t *s, float sample_rate)
{
    uint8_t *b = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); i++) b[i] = 0;
    s->sr = sample_rate;
    for (int i = 0; i <= 1024; i++) {
        float ph = (float)i / 1024.f;          /* 0..1 */
        float x = ph * 2.f * PI_F;
        if (x > PI_F) x -= 2.f * PI_F;
        s->sine[i] = sin_poly(x);
    }
    for (int i = 0; i < 4; i++) s->p[i] = -1.f;   /* forzar salto inicial */
    s->last_mode = -1;
    s->magic = USER_MAGIC;
}

static inline float tsin(const user_state_t *s, float ph)  /* ph 0..1 */
{
    float f = ph * 1024.f;
    int i = (int)f;
    float fr = f - (float)i;
    i &= 1023;
    return s->sine[i] + (s->sine[i + 1] - s->sine[i]) * fr;
}

static inline float wrap01(float x) { return x - (float)(int)x; }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

/* exp2 aproximada (suficiente para mapear knobs) */
static float exp2_approx(float x)
{
    int ip = (int)x; if (x < 0 && (float)ip != x) ip--;
    float f = x - (float)ip;
    float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union { float f; int32_t i; } u = { p };
    u.i += ip << 23;
    return u.f;
}
/* mapa exponencial 0..1 -> lo..hi */
static inline float expmap(float x, float lo, float hi)
{
    /* log2(hi/lo) precalculado por el que llama no merece la pena: se hace por bloque */
    float r = hi / lo, l2 = 0.f;
    while (r >= 2.f) { r *= 0.5f; l2 += 1.f; }
    l2 += (r - 1.f) * (1.4425f - 0.4425f * (r - 1.f));  /* log2 aprox en [1,2) */
    return lo * exp2_approx(x * l2);
}

static inline float soft_clip(float x)
{
    x = clampf(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

/* plegado triangular: refleja cualquier valor dentro de [-1, 1] */
static inline float fold(float x)
{
    float t = (x + 1.f) * 0.25f;
    t = t - (float)(int)t; if (t < 0.f) t += 1.f;
    float y = 4.f * (t > 0.5f ? t - 0.5f : 0.5f - t) - 1.f;
    return -y;
}

static inline int16_t to_i16(float x)
{
    x *= 32767.f;
    if (x > 32767.f) x = 32767.f;
    if (x < -32768.f) x = -32768.f;
    return (int16_t)x;
}

void user_dsp_process(user_state_t *s, int mode, const float target[4],
                      float wet, float dry,
                      const int16_t *in, int16_t *out, int n)
{
    const float inv = 1.f / 32768.f;
    const float sr = s->sr;
    int frames = n >> 1;
    if (frames <= 0) return;

    if (mode != s->last_mode) {
        s->lp_l = s->lp_r = 0.f;
        s->last_mode = mode;
    }

    /* suavizado lineal de parámetros a lo largo del bloque */
    float p0[4], dp[4];
    for (int k = 0; k < 4; k++) {
        if (s->p[k] < 0.f) s->p[k] = target[k];
        p0[k] = s->p[k];
        dp[k] = (target[k] - s->p[k]) / (float)frames;
        s->p[k] = target[k];
    }

    switch (mode) {
    default:
    case MODE_TREMOLO: {
        float rate = expmap(target[0], 0.1f, 20.f);
        float inc = rate / sr;
        float stereo = target[3] * 0.5f;               /* 0..180 grados */
        for (int i = 0; i < n; i += 2) {
            float depth = p0[1], shape = p0[2];
            p0[1] += dp[1]; p0[2] += dp[2];
            float l = in[i] * inv, r = in[i + 1] * inv;
            float sl = tsin(s, s->lfo_ph);
            float sr_ = tsin(s, wrap01(s->lfo_ph + stereo));
            /* forma: de seno a casi cuadrada */
            float g = 1.f + shape * 8.f;
            sl = soft_clip(sl * g) / soft_clip(g);
            sr_ = soft_clip(sr_ * g) / soft_clip(g);
            float gl = 1.f - depth * (0.5f - 0.5f * sl);
            float gr = 1.f - depth * (0.5f - 0.5f * sr_);
            s->lfo_ph = wrap01(s->lfo_ph + inc);
            out[i]     = to_i16(dry * l + wet * l * gl);
            out[i + 1] = to_i16(dry * r + wet * r * gr);
        }
    } break;

    case MODE_RINGMOD: {
        float f = expmap(target[0], 20.f, 2000.f);
        float spread = target[1];                      /* R = f * (1 + spread) */
        float wob_inc = 0.37f / sr;
        for (int i = 0; i < n; i += 2) {
            float drive = 1.f + p0[2] * 6.f, wob = p0[3];
            p0[2] += dp[2]; p0[3] += dp[3];
            float l = in[i] * inv, r = in[i + 1] * inv;
            float w = 1.f + wob * 0.5f * tsin(s, s->wob_ph);
            float incl = f * w / sr, incr = f * (1.f + spread) * w / sr;
            float ml = tsin(s, s->car_ph), mr = tsin(s, s->car_ph_r);
            s->car_ph = wrap01(s->car_ph + incl);
            s->car_ph_r = wrap01(s->car_ph_r + incr);
            s->wob_ph = wrap01(s->wob_ph + wob_inc);
            float yl = soft_clip(l * drive) * ml;
            float yr = soft_clip(r * drive) * mr;
            out[i]     = to_i16(dry * l + wet * yl);
            out[i + 1] = to_i16(dry * r + wet * yr);
        }
    } break;

    case MODE_FOLDER: {
        float fc = expmap(target[2], 400.f, 16000.f);
        float a = 1.f - exp2_approx(-2.f * PI_F * fc / sr * 1.4427f);  /* 1-e^(-wc) */
        const float R = 0.995f;                        /* bloqueo de DC */
        for (int i = 0; i < n; i += 2) {
            float gain = 1.f + p0[0] * p0[0] * 19.f;
            float bias = (p0[1] - 0.5f) * 1.2f;
            float outl = p0[3] * 0.5f;
            p0[0] += dp[0]; p0[1] += dp[1]; p0[3] += dp[3];
            float l = in[i] * inv, r = in[i + 1] * inv;
            float yl = fold(l * gain + bias), yr = fold(r * gain + bias);
            /* quitar DC introducido por la simetría */
            float dl = yl - s->dc_xl + R * s->dc_yl; s->dc_xl = yl; s->dc_yl = dl;
            float dr = yr - s->dc_xr + R * s->dc_yr; s->dc_xr = yr; s->dc_yr = dr;
            s->lp_l += a * (dl - s->lp_l);
            s->lp_r += a * (dr - s->lp_r);
            out[i]     = to_i16(dry * l + wet * s->lp_l * outl);
            out[i + 1] = to_i16(dry * r + wet * s->lp_r * outl);
        }
    } break;

    case MODE_TEST440: {
        /* seno de 440 Hz calculado con MFX_SAMPLE_RATE. Si el afinador marca La (A4),
           la frecuencia supuesta es correcta. ~404 Hz => el pedal va a 44.1 kHz. */
        float inc = 440.f / sr;
        for (int i = 0; i < n; i += 2) {
            float lvl = p0[0] * 0.5f; p0[0] += dp[0];
            float v = tsin(s, s->car_ph) * lvl;
            s->car_ph = wrap01(s->car_ph + inc);
            out[i] = out[i + 1] = to_i16(v);
        }
    } break;
    }
}
