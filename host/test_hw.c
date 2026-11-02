/* Hardware-faithful checks: the shared delay buffer persists across mode switches.
   Reproduces the "continuous feedback" bug and proves the reset_mode clear fixes it. */
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
static double goertzel(const float*x,int n,double f);
static mfx_engine_t E;
static void setp(int i,int v){ E.params[i].value=v; E.params[i].index=i; E.params[i].engine=&E; }
static void dirty(void){ for(int i=0;i<42000;i++) host_delaybuf[i]=(int16_t)((rand()%65535)-32768); }

int main(void){
    int N=(SR*4/16)*16; int16_t*in=calloc(N*2,2),*out=calloc(N*2,2);
    #define S16(v) ((int16_t)((v)*32767.f))
    srand(3);

    /* Engine comes up in NITRO, then user switches into GRID / CWO with the shared
       buffer full of old audio. Feed SILENCE after switching: a correct effect is quiet. */
    int modes[]={M_GRID,M_CWO}; const char*nm[]={"GRID","CWO"};
    int p[][4]={ {0x9000,0x5000,0x7000,0x8000}, {0xC000,0x6000,0x7000,0x8000} };
    for(int m=0;m<2;m++){
        memset(&E,0,sizeof E); E.nparams=NUM_P;
        setp(P_MODE,M_NITRO); setp(P1,0x8000);setp(P2,0x8000);setp(P3,0x8000);setp(P4,0x8000); setp(P_MIX,0xFFFF);
        lab_activate(&E);
        /* run 0.2 s of audio through NITRO to fill the shared buffer, then dirty it hard */
        for(int i=0;i<N;i++){ int16_t v=S16(sinf(2*M_PI*200*i/SR)*0.5f); in[2*i]=v;in[2*i+1]=v; }
        for(int i=0;i<SR/5*2;i+=32) lab_process(&E,in+i,out+i,32);
        dirty();
        /* now switch to GRID/CWO and feed pure SILENCE */
        setp(P_MODE,modes[m]); setp(P1,p[m][0]);setp(P2,p[m][1]);setp(P3,p[m][2]);setp(P4,p[m][3]);
        for(int i=0;i<N*2;i++)in[i]=0;
        for(int i=0;i<N*2;i+=32) lab_process(&E,in+i,out+i,32);
        double e0=0,e1=0; int h=N/2;              /* first half vs last half energy */
        for(int i=0;i<h;i++){double v=out[2*i]/32768.0;e0+=v*v;}
        for(int i=h;i<N;i++){double v=out[2*i]/32768.0;e1+=v*v;}
        printf("%-5s after switch-in + silence:  rms start=%.5f  rms end=%.5f  %s\n",
               nm[m],sqrt(e0/h),sqrt(e1/(N-h)),
               sqrt(e1/(N-h))<1e-4?"OK (decays to silence)":"STILL FEEDING");
    }

    /* NITRO: steady noise in; the LFO must make 4 kHz energy PULSE over time. */
    memset(&E,0,sizeof E); E.nparams=NUM_P;
    setp(P_MODE,M_NITRO); setp(P1,0x4000);setp(P2,0xC000);setp(P3,0x9000);setp(P4,0xD000); setp(P_MIX,0xFFFF);
    lab_activate(&E);
    srand(11);
    for(int i=0;i<N;i++){ int16_t v=S16((rand()/(float)RAND_MAX*2-1)*0.4f); in[2*i]=v;in[2*i+1]=v; }
    for(int i=0;i<N*2;i+=32) lab_process(&E,in+i,out+i,32);
    { int win=SR/4, nb=N/win; double mn=1e30,mx=0;   /* energy per 0.25 s window */
      float*w=malloc(win*4);
      for(int b=1;b<nb;b++){ for(int i=0;i<win;i++)w[i]=out[2*(b*win+i)]/32768.f;
          double e=goertzel(w,win,4000); if(e<mn)mn=e; if(e>mx)mx=e; }
      printf("NITRO  4kHz energy over time: min=%.3g max=%.3g  ratio=%.1fx  %s\n",
             mn,mx,mx/(mn+1e-12),(mx/(mn+1e-12))>3?"OK (LFO is sweeping)":"FLAT (no LFO motion)");
      free(w); }

    /* CWO: 300 Hz in, +200 Hz shift, no feedback. A real frequency shifter rejects one
       sideband; a ring-mod keeps both. Measure E@500 (up) vs E@100 (down) vs E@300 (carrier). */
    memset(&E,0,sizeof E); E.nparams=NUM_P;
    setp(P_MODE,M_CWO); setp(P1,(int)(65535.0*200/1000));setp(P2,0x0400);setp(P3,0);setp(P4,0xFFFF); setp(P_MIX,0xFFFF);
    lab_activate(&E);
    for(int i=0;i<N;i++){ int16_t v=S16(sinf(2*M_PI*300*i/SR)*0.5f); in[2*i]=v;in[2*i+1]=v; }
    for(int i=0;i<N*2;i+=32) lab_process(&E,in+i,out+i,32);
    { int off=SR*2; float*w=malloc((N-off)*4); for(int i=off;i<N;i++)w[i-off]=out[2*i]/32768.f;
      double up=goertzel(w,N-off,500),dn=goertzel(w,N-off,100),car=goertzel(w,N-off,300);
      printf("CWO SSB(+200Hz, SIDE=up): E@500up=%.3g  E@100dn=%.3g  E@300carrier=%.3g  reject=%.1f dB\n",
             up,dn,car,10*log10((up+1e-12)/(dn+1e-12))); free(w); }

    /* TERMINAL: low RATE must not freeze (output keeps changing, not stuck). */
    memset(&E,0,sizeof E); E.nparams=NUM_P;
    setp(P_MODE,M_TERMINAL); setp(P1,0x3000);setp(P2,0x0800);setp(P3,0x2000);setp(P4,0xC000); setp(P_MIX,0xFFFF);
    lab_activate(&E);
    for(int i=0;i<N;i++){ int16_t v=S16(sinf(2*M_PI*220*i/SR)*0.5f); in[2*i]=v;in[2*i+1]=v; }
    for(int i=0;i<N*2;i+=32) lab_process(&E,in+i,out+i,32);
    { int changes=0; for(int i=N/2+1;i<N;i++) if(out[2*i]!=out[2*i-1])changes++;
      printf("TERMINAL low-rate: %d sample-changes in last 2s (0 = frozen/stuck)\n",changes); }
    return 0;
}
static double goertzel(const float*x,int n,double f){ double w=2*M_PI*f/SR,c=cos(w),s1=0,s2=0;
    for(int i=0;i<n;i++){double s0=x[i]+2*c*s1-s2;s2=s1;s1=s0;} return s1*s1+s2*s2-2*c*s1*s2; }
