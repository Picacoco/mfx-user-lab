/* DSP del motor USER — código portable (compila igual en el pedal y en el PC) */
#pragma once
#include <stdint.h>

enum { MODE_TREMOLO, MODE_RINGMOD, MODE_FOLDER, MODE_TEST440, NUM_MODES };

typedef struct {
    uint32_t magic;
    float sr;
    float sine[1025];          /* tabla de seno (1024 + guarda) */
    /* parámetros suavizados 0..1 */
    float p[4];
    /* osciladores */
    float lfo_ph, car_ph, car_ph_r, wob_ph;
    /* filtros */
    float lp_l, lp_r;
    float dc_xl, dc_yl, dc_xr, dc_yr;
    int   last_mode;
} user_state_t;

#define USER_MAGIC 0x55534552u  /* 'USER' */

void user_dsp_init(user_state_t *s, float sample_rate);
/* in/out: int16 estéreo intercalado, n = número de int16 (2 por frame) */
void user_dsp_process(user_state_t *s, int mode, const float target[4],
                      float wet, float dry,
                      const int16_t *in, int16_t *out, int n);

extern const char *const user_mode_names[NUM_MODES];
extern const char *const user_param_names[NUM_MODES][4];
