/* PC test of the LAB DSP: same code as the pedal, 44.1 kHz, 16-frame blocks.
 * Checks levels and that there is no NaN or silence. One WAV per mode.
 *   gcc -O2 -I ../sdk -I ../engines/lab render_lab.c -lm -o render_lab && ./render_lab
 */
#define LAB_HOST 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int16_t host_delaybuf[42000];
static uint8_t host_ccm[0x10000];
#define MFX_DELAYBUF (host_delaybuf)
#define MFX_CCM      ((void *)host_ccm)
typedef struct mfx_engine mfx_engine_t;
typedef struct mfx_param  mfx_param_t;
static void host_clear(void){ memset(host_delaybuf,0,sizeof host_delaybuf); }
static void host_dsp(mfx_engine_t *e, void (*cb)(mfx_engine_t*,uint16_t,int)){ (void)e;(void)cb; }
static void host_mix(uint16_t mix, float *wet, float *dry){ float m=mix/65535.f; *wet=sinf(m*1.5707963f); *dry=cosf(m*1.5707963f); }
#define mfx_clear_delaybuf  host_clear
#define mfx_set_dsp_handler host_dsp
#define mfx_mix_gains       host_mix

#include "mfx_fw.h"
#include "lab.c"

#define SR 44100
#define SECS 8

static void wav(const char *fn, const int16_t *d, int frames){
    FILE *f=fopen(fn,"wb"); int32_t bytes=frames*4,v; int16_t s;
    fwrite("RIFF",1,4,f); v=36+bytes; fwrite(&v,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); v=16; fwrite(&v,4,1,f); s=1; fwrite(&s,2,1,f);
    s=2; fwrite(&s,2,1,f); v=SR; fwrite(&v,4,1,f); v=SR*4; fwrite(&v,4,1,f);
    s=4; fwrite(&s,2,1,f); s=16; fwrite(&s,2,1,f);
    fwrite("data",1,4,f); fwrite(&bytes,4,1,f); fwrite(d,4,frames,f); fclose(f);
}
static mfx_engine_t E;
static void setp(int i,int v){ E.params[i].value=v; E.params[i].index=i; E.params[i].engine=&E; }

int main(void){
    int N=SR*SECS; int16_t *in=calloc(N*2,2),*out=calloc(N*2,2);
    float notes[]={82.41f,110.0f,146.83f,196.0f,246.94f,329.63f}; static float ks[2048]; srand(1);
    for(int n=0;n<12;n++){ float fr=notes[n%6]; int L=(int)(SR/fr);
        for(int i=0;i<L;i++) ks[i]=(rand()/(float)RAND_MAX*2-1)*0.5f;
        int start=n*SR*2/3;
        for(int i=0;i<SR*2 && start+i<N;i++){ int k=i%L; float y=ks[k]; ks[k]=0.5f*(y+ks[(k+1)%L])*0.998f;
            int idx=start+i; float a=in[2*idx]/32768.f + y*0.6f;
            if(a>0.99f)a=0.99f; if(a<-0.99f)a=-0.99f; in[2*idx]=(int16_t)(a*32767); in[2*idx+1]=in[2*idx]; } }
    wav("dry.wav",in,N);

    struct { const char *name; int mode,p1,p2,p3,p4,mix; int tap1,tap2; } pr[]={
        {"murf",     M_MURF,    0x2000,0x9000,0x3000,0x8000,0xFFFF, -1,-1},
        {"haze",     M_HAZE,    0xC000,0x6000,0x7000,0x3000,0xC000, -1,-1},
        {"nitro",    M_NITRO,   0x9000,0xC000,0xB000,0x3000,0xFFFF, -1,-1},
        {"chord_min9",M_CHORD,  0x5000,0x6000,0x5000,0x8000,0xB000, -1,-1},
        {"chord_maj", M_CHORD,  0xF000,0x5000,0x6000,0x8000,0xB000, -1,-1},
        {"flanger",  M_FLANGER, 0x3000,0x9000,0x9000,0x7000,0x9000, -1,-1},
        {"terminal", M_TERMINAL,0x7000,0x8000,0x3000,0x0000,0xFFFF, -1,-1},
        {"phone",    M_PHONE,   0x6000,0x7000,0x6000,0x3000,0xFFFF, -1,-1},
    };
    for(unsigned k=0;k<sizeof pr/sizeof pr[0];k++){
        memset(&E,0,sizeof E); E.nparams=NUM_P;
        setp(P_MODE,pr[k].mode); setp(P1,pr[k].p1); setp(P2,pr[k].p2);
        setp(P3,pr[k].p3); setp(P4,pr[k].p4); setp(P_MIX,pr[k].mix);
        lab_activate(&E);
        double peak=0,rms=0; int nans=0;
        for(int i=0;i<N*2;i+=32){
            int fr=i/2;
            if(pr[k].tap1>=0 && fr<=pr[k].tap1 && fr+16>pr[k].tap1) lab_tap(&E,0,0);
            if(pr[k].tap2>=0 && fr<=pr[k].tap2 && fr+16>pr[k].tap2) lab_tap(&E,0,0);
            lab_process(&E,in+i,out+i,32);
        }
        for(int i=0;i<N*2;i++){ double v=out[i]/32768.0; if(v!=v)nans++; if(fabs(v)>peak)peak=fabs(v); rms+=v*v; }
        char fn[64]; sprintf(fn,"%s.wav",pr[k].name); wav(fn,out,N);
        printf("%-12s peak %.3f  rms %.3f  nan %d\n",pr[k].name,peak,sqrt(rms/(N*2)),nans);
    }
    return 0;
}
