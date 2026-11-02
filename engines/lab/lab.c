/*
 * USER LAB / FX VIBES - a "USER" program for the ALM MFX Pedal (firmware v118).
 * Six modes that emulate the CHARACTER of classic gear and cult pedals (not ports):
 *
 *   GRID     - ring modulator + delay with self-oscillating feedback (OP-1 field GRID).
 *   NITRO    - dual resonant "turbo" filter (LP+HP) with envelope follower (OP-1 NITRO).
 *   CHORD    - chord harmonizer with Plaits-style controls (HARMONICS/TIMBRE/MORPH/ROOT).
 *   FLANGER  - creative flanger blended with a spring reverb.
 *   TERMINAL - bit-crusher + sample-rate reducer + multimode filter (OP-1 TERMINAL).
 *   CWO      - pitch-shifting delay / frequency-shifter sidebands (OP-1 CWO).
 *
 * Same structure as the USER engine that flashes cleanly: all code fits in the free
 * tail of flash sector 8, no writable globals (state lives in CCM RAM), audio buffers
 * use the firmware delay buffer (BUF) plus a second buffer in CCM (EXT).
 */
#include "mfx_fw.h"

enum { M_NITRO, M_CHORD, M_FLANGER, M_TERMINAL, M_PHONE, M_MURF, M_HAZE, NUM_MODES };
#define MURF_NPAT 12
enum { P_MODE, P1, P2, P3, P4, P_MIX, P_PRESET, P_CLOCK, P_CLKMUL, NUM_P };

#define BUF      MFX_DELAYBUF
#define BUF_LEN  MFX_DELAYBUF_LEN     /* 42000 */
#define EXT_LEN  20000
#define MAGIC    0x4C414234u
#define PI_F     3.14159265358979f

typedef struct {
    uint32_t magic; float sr; int last_mode, named_mode; uint32_t rng;
    float sine[1025];
    int16_t ext[EXT_LEN];
    int16_t fl[1536];
    char lbl[16];
    int   w[12];
    float ph[3];
    float sm[5];
    float lp[8], ap[6];
    float lfo1, lfo2;
    int   trig, hold;
    float env, dglide;
    float hbA[16];                     /* MURF: 8 band-pass filter states (2 per band) */
    float tempo;                       /* beat interval in ms from TAP / MIDI clock (0 = none) */
    int   mstep;                       /* MURF animation step 0..7 */
    int   clocked;                     /* 1 when CLOCK=MIDI and a tempo is present */
    float clkmul;                      /* CLK MUL multiplier (0.25 .. 4) */
} state_t;
#define ST ((state_t *)MFX_CCM)

/* ---------- math without libm ---------- */
static float sin_poly(float x){ if(x>PI_F*0.5f)x=PI_F-x; if(x<-PI_F*0.5f)x=-PI_F-x; float x2=x*x;
    return x*(1.f-x2/6.f*(1.f-x2/20.f*(1.f-x2/42.f*(1.f-x2/72.f*(1.f-x2/110.f))))); }
static float exp2_approx(float x){ int ip=(int)x; if(x<0&&(float)ip!=x)ip--; float f=x-(float)ip;
    float p=1.f+f*(0.6931472f+f*(0.2402265f+f*(0.0555041f+f*0.0096181f)));
    union{float f;int32_t i;}u={p}; u.i+=ip<<23; return u.f; }
static float expmap(float x,float lo,float hi){ float r=hi/lo,l2=0.f; while(r>=2.f){r*=0.5f;l2+=1.f;}
    l2+=(r-1.f)*(1.4425f-0.4425f*(r-1.f)); return lo*exp2_approx(x*l2); }
static inline float clampf(float x,float a,float b){ return x<a?a:(x>b?b:x); }
static inline int   clampi(int v,int a,int b){ return v<a?a:(v>b?b:v); }
static inline float frand(state_t*s){ s->rng^=s->rng<<13; s->rng^=s->rng>>17; s->rng^=s->rng<<5;
    return (float)(int32_t)s->rng*(1.f/2147483648.f); }  /* white noise -1..1 */
static inline float tsin(state_t*s,float ph){ ph-=(float)(int)ph; if(ph<0)ph+=1.f;
    float f=ph*1024.f; int i=(int)f; float fr=f-(float)i; return s->sine[i]+(s->sine[i+1]-s->sine[i])*fr; }
static inline float hann(state_t*s,float x){ return 0.5f-0.5f*tsin(s,x+0.25f); }
static inline float rd(const int16_t*b,int len,float pos){ int i=(int)pos; float fr=pos-(float)i;
    i%=len; if(i<0)i+=len; int j=i+1; if(j>=len)j-=len;
    return ((float)b[i]+((float)b[j]-(float)b[i])*fr)*(1.f/32768.f); }
static inline int16_t i16(float x){ x*=32767.f; if(x>32767.f)x=32767.f; if(x<-32768.f)x=-32768.f; return (int16_t)x; }
static inline float soft(float x){ x=clampf(x,-2.f,2.f); return x*(27.f+x*x)/(27.f+9.f*x*x); }
static inline float onepole(float*st,float x,float a){ *st+=a*(x-*st); return *st; }
static float pshift(state_t*s,int16_t*buf,int len,int*wp,float*ph,float ratio,float in){
    buf[*wp]=i16(in);
    *ph += (1.f-ratio); while(*ph>=len)*ph-=len; while(*ph<0)*ph+=len;
    float ph2=*ph+len*0.5f; if(ph2>=len)ph2-=len;
    float a=rd(buf,len,(float)*wp-*ph), b=rd(buf,len,(float)*wp-ph2);
    float wa=hann(s,*ph/(float)len);
    if(++(*wp)>=len)*wp=0;
    return a*wa + b*(1.f-wa);
}
static float allpass(int16_t*buf,int len,int*wp,float g,float in){
    float d=(float)buf[*wp]*(1.f/32768.f);
    float y=-g*in+d; buf[*wp]=i16(in+g*y);
    if(++(*wp)>=len)*wp=0; return y;
}
/* 8 fixed resonant band-pass centre frequencies (Hz), mid voicing. */
static const float murf_f[8]={200.f,300.f,450.f,675.f,1000.f,1500.f,2200.f,3400.f};
/* Twelve original animation patterns: one 8-bit mask per step, bit b = filter b open. */
static const unsigned char murf_pat[MURF_NPAT][8]={
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},   /*  1: all on (tremolo when env short) */
    {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80},   /*  2: upward staircase */
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01},   /*  3: downward staircase */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0x81},   /*  4: rolling pairs */
    {0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA},   /*  5: odd/even alternation */
    {0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81},   /*  6: converge / diverge */
    {0x01,0x03,0x07,0x0F,0x1F,0x3F,0x7F,0xFF},   /*  7: filling up */
    {0xFF,0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01},   /*  8: emptying down */
    {0x01,0x02,0x04,0x08,0x08,0x04,0x02,0x01},   /*  9: bounce (up then down) */
    {0x99,0x66,0x99,0x66,0x99,0x66,0x99,0x66},   /* 10: two-group interlock */
    {0x01,0x00,0x04,0x00,0x10,0x00,0x40,0x00},   /* 11: sparse quarter pulses */
    {0x0F,0xF0,0x0F,0xF0,0xC3,0x3C,0xC3,0x3C},   /* 12: low/high call-and-response */
};
/* TPT (zero-delay-feedback) state-variable filter: stable and strongly resonant.
   ic1/ic2 are state; res 0..1 (higher = more resonance). Outputs via pointers. */
static void svf(state_t*s,float*ic1,float*ic2,float in,float fc,float res,float*lo,float*hi,float*bpo){
    fc=clampf(fc,20.f,s->sr*0.45f);
    float wd=fc/s->sr;
    float cs=tsin(s,wd*0.5f+0.25f); if(cs<0.02f)cs=0.02f;
    float g=tsin(s,wd*0.5f)/cs;                    /* tan(pi*fc/sr) */
    float k=2.f-1.95f*clampf(res,0.f,1.f);         /* damping: res 0->k2, res 1->k~0.05 */
    float a1=1.f/(1.f+g*(g+k)), a2=g*a1, a3=g*a2;
    float v3=in-*ic2;
    float v1=a1*(*ic1)+a2*v3;
    float v2=*ic2+a2*(*ic1)+a3*v3;
    *ic1=2.f*v1-*ic1; *ic2=2.f*v2-*ic2;
    if(lo)*lo=v2; if(hi)*hi=in-k*v1-v2; if(bpo)*bpo=v1;
}

/* ---------- strings ---------- */
static const char *const mode_names[NUM_MODES]={ "NITRO","CHORD","FLANGER","TERMINAL","PHONE","MURF","HAZE" };
static const char *const pnames[NUM_MODES][4]={
    { "FREQ","SPAN","RATE","DEPTH"             },   /* NITRO    */
    { "HARMONICS","TIMBRE","MORPH","ROOT"      },   /* CHORD    */
    { "RATE","DEPTH","REGEN","SPRING"          },   /* FLANGER  */
    { "BITS","RATE","GLITCH","FILTER"          },   /* TERMINAL */
    { "DRIVE","BAND","CRUSH","NOISE"           },   /* PHONE    */
    { "PATTERN","RATE","ENV","FREQ"            },   /* MURF     */
    { "DECAY","TONE","MOD","TREM"              },   /* HAZE     */
};
static const char *const clk_name[2]={ "INT","MIDI" };
#define MUL_N 27
#define MUL_DEF 10   /* index of x1 (default) */
static const char *const mul_name[MUL_N]={
    "0.1","0.25","0.3","0.37","0.46","0.5","0.75","0.87","0.9","0.93",
    "1","1.1","1.3","1.5","1.7","1.8","2","2.6","3","3.5","3.7","4","5","6","7","7.5","8" };
static const float mul_val[MUL_N]={
    0.1f,0.25f,0.3f,0.37f,0.46f,0.5f,0.75f,0.87f,0.9f,0.93f,
    1.f,1.1f,1.3f,1.5f,1.7f,1.8f,2.f,2.6f,3.f,3.5f,3.7f,4.f,5.f,6.f,7.f,7.5f,8.f };
static const char *const pitch_name[7]={ "-OCT","-5th","DOWN","OFF","UP","+5th","+OCT" };
static const int   pitch_semi[7]={ -12,-7,-5,0,5,7,12 };
/* CHORD: 11 types (Plaits order); three upper voices over the dry root */
static const char *const chord_name[11]={ "OCT","5","SUS4","m","m7","m9","m11","69","M9","M7","M" };
static const int chord_iv[11][3]={
    {-12,12,24},{7,12,19},{5,7,12},{3,7,12},{3,7,10},{3,10,14},
    {3,10,17},{4,9,14},{4,11,14},{4,7,11},{4,7,12} };
static const char *label_mode(mfx_param_t*p){ return mode_names[clampi(p->value,0,NUM_MODES-1)]; }
static const char *label_clk(mfx_param_t*p){ return clk_name[clampi(p->value,0,1)]; }
static const char *label_mul(mfx_param_t*p){ return mul_name[clampi(p->value,0,MUL_N-1)]; }

static const char *val_label(mfx_param_t*p){
    state_t*s=ST; int pi=p->index, mode=p->engine? (int)p->engine->params[P_MODE].value:0; int v=p->value&0xFFFF;
    if(mode==M_MURF&&pi==P1){ mfx_snprintf(s->lbl,16,"PAT %i",1+(int)((float)v/65535.f*(MURF_NPAT-0.01f))); return s->lbl; }
    if(mode==M_CHORD&&pi==P1) return chord_name[clampi((int)((float)v/65535.f*10.99f),0,10)];
    if(mode==M_CHORD&&pi==P4){ mfx_snprintf(s->lbl,16,"%i st",(int)((float)v/65535.f*24.f)-12); return s->lbl; }
    if(mode==M_TERMINAL&&pi==P1){ mfx_snprintf(s->lbl,16,"%i bit",1+(int)((float)v/65535.f*15.f)); return s->lbl; }
    mfx_snprintf(s->lbl,16,"%i%%",(v*100)/65535); return s->lbl;
}

/* ---------- presets ---------- */
#define X 0xDEADBEEFu
#define NA MFX_NO_ASSIGN
#define NOA { NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA }
#define PR(m,a,b,c,d,mx) { m,a,b,c,d,mx,0, X,X,X,X,X,X,X,X,X }
__attribute__((used, aligned(4)))
static const mfx_preset_t presets[]={
    { "TURBO\nSWEEP",  PR(M_NITRO,   0x4000,0xC000,0x7000,0xD000,0xFFFF), NOA },
    { "FUNK\nFILTER",  PR(M_NITRO,   0x6000,0x9000,0xA000,0xFFFF,0xFFFF), NOA },
    { "MINOR\n9 PAD",  PR(M_CHORD,   0x5000,0x6000,0x4000,0x8000,0xB000), NOA },
    { "MINOR\n7",      PR(M_CHORD,   0x6000,0x6000,0x4000,0x8000,0xB000), NOA },
    { "MAJOR\nSHEEN",  PR(M_CHORD,   0xF000,0x5000,0x6000,0x8000,0xB000), NOA },
    { "JET\nSPRING",   PR(M_FLANGER, 0x3000,0x9000,0x9000,0x7000,0x9000), NOA },
    { "CRUSH\n8BIT",   PR(M_TERMINAL,0x7000,0x8000,0x3000,0x0000,0xFFFF), NOA },
    { "PHONE\nLINE",   PR(M_PHONE,   0x6000,0x7000,0x6000,0x3000,0xFFFF), NOA },
    { "STAIR\nMURF",   PR(M_MURF,    0x2000,0x9000,0x3000,0x8000,0xFFFF), NOA },
    { "TREMOLO\nMURF", PR(M_MURF,    0x0000,0xB000,0x5000,0x8000,0xFFFF), NOA },
    { "GALAXIE\nHAZE", PR(M_HAZE,    0xC000,0x6000,0x7000,0x3000,0xC000), NOA },
    { "DREAM\nWASH",   PR(M_HAZE,    0xFFFF,0x4000,0xB000,0x0000,0xE000), NOA },
};
#undef X
#undef NA
#undef NOA
#undef PR

static inline float pv(mfx_engine_t*e,int i){ return (float)(mfx_value(e,i)&0xFFFF)*(1.f/65535.f); }

/* ================= modes ================= */
/* MURF: 8 fixed resonant band-pass filters whose levels are gated by an 8-step animation
   sequencer. PATTERN picks the sequence, RATE sets step speed, ENV shapes each gate's
   attack/release (low=plucky, high=smooth/amorphous), FREQ shifts the 8 bands +-1 octave
   (down=bass voicing, up=mid). Filter states live in hbA[16] (2 per band), levels in lp[8].
   RATE free-runs; when a TAP/MIDI-clock tempo is present it syncs, RATE choosing the
   clock division (1/4, 1/2, x1, x2, x4 of the beat). */
static void m_murf(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float pat=s->sm[0],rate=s->sm[1],env=s->sm[2],fshift=s->sm[3];
    int P=clampi((int)(pat*(MURF_NPAT-0.01f)),0,MURF_NPAT-1);
    float in=(il+ir)*0.5f;
    float stephz = s->clocked ? (1000.f/s->tempo)*s->clkmul   /* MIDI: beat x CLK MUL */
                              : expmap(rate,0.5f,18.f);        /* INT: RATE free-run */
    s->lfo1+=stephz/s->sr; if(s->lfo1>=1.f){ s->lfo1-=1.f; if(++s->mstep>=8)s->mstep=0; }
    unsigned char mask=murf_pat[P][s->mstep];
    float base=0.0006f+(1.f-env)*0.06f;               /* ENV: high=slow/smooth, low=snappy */
    float atk=clampf(base*2.f,0.f,0.9f), rel=base;
    float shift=exp2_approx((fshift-0.5f)*2.f);        /* FREQ: +-1 octave band shift */
    float outp=0.f;
    for(int b=0;b<8;b++){
        float tgt=((mask>>b)&1)?1.f:0.f;
        float c=(tgt>s->lp[b])?atk:rel; s->lp[b]+=c*(tgt-s->lp[b]);
        float bp; svf(s,&s->hbA[b*2],&s->hbA[b*2+1],in,murf_f[b]*shift,0.86f,0,0,&bp);
        outp+=bp*s->lp[b];
    }
    float y=soft(outp*0.6f);
    *ol=dry*il+wet*y; *or_=dry*ir+wet*y;
}

/* NITRO: a high-resonance BAND-PASS whose centre frequency is swept by a sine LFO -> the
   vocal "wah / scream". FREQ=base centre, SPAN=sweep width (octaves), RATE=LFO speed,
   DEPTH=how much of the sweep is applied. A small baseline keeps it audibly moving. */
static void m_nitro(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float freq=s->sm[0],span=s->sm[1],rate=s->sm[2],depth=s->sm[3];
    float in=(il+ir)*0.5f;
    float lfohz = s->clocked ? (1000.f/s->tempo)*s->clkmul   /* MIDI: cycles per beat x CLK MUL */
                             : expmap(rate,0.05f,32.f);       /* INT: RATE -> 0.05..32 Hz */
    float l=0.5f+0.5f*tsin(s,s->lfo2); s->lfo2+=lfohz/s->sr; if(s->lfo2>1.f)s->lfo2-=1.f;
    float base=expmap(freq,120.f,2500.f);              /* FREQ -> base centre */
    float oct=(0.3f+span*3.7f)*(0.12f+depth*0.88f)*l;  /* SPAN x DEPTH octaves, min motion built in */
    float fc=clampf(base*exp2_approx(oct),60.f,s->sr*0.45f);
    float bp;
    svf(s,&s->ap[0],&s->ap[1],in,fc,0.9f,0,0,&bp);     /* resonant band-pass, centre swept by LFO */
    float y=soft(bp*2.2f);                             /* resonant gain -> "nitro scream" */
    *ol=dry*il+wet*y; *or_=dry*ir+wet*y;
}

/* CHORD: three pitch-shifted voices forming a chord over the dry root (Plaits-style). */
#define CH_V 6500
static void m_chord(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    int chord=clampi((int)(s->sm[0]*10.99f),0,10);
    float timbre=s->sm[1], morph=s->sm[2], root=s->sm[3];
    int rootT=(int)(root*24.f)-12;                    /* global transpose */
    int invT=(int)(timbre*12.f);                      /* inversion: lift voices by octaves */
    float in=(il+ir)*0.5f;
    float det=morph*0.04f;
    float d0=det*tsin(s,s->lfo1), d1=det*tsin(s,s->lfo2), d2=det*tsin(s,s->lfo1+0.33f);
    s->lfo1+=0.13f/s->sr; if(s->lfo1>1)s->lfo1-=1; s->lfo2+=0.19f/s->sr; if(s->lfo2>1)s->lfo2-=1;
    float iv0=(float)(chord_iv[chord][0]+rootT)+(invT>8?12:0)+d0;
    float iv1=(float)(chord_iv[chord][1]+rootT)+(invT>4?12:0)+d1;
    float iv2=(float)(chord_iv[chord][2]+rootT)+d2;
    float v0=pshift(s,s->ext,        CH_V,&s->w[0],&s->ph[0],exp2_approx(iv0/12.f),in);
    float v1=pshift(s,s->ext+CH_V,   CH_V,&s->w[1],&s->ph[1],exp2_approx(iv1/12.f),in);
    float v2=pshift(s,s->ext+2*CH_V, CH_V,&s->w[2],&s->ph[2],exp2_approx(iv2/12.f),in);
    float voices=(v0+v1+v2)*0.5f;
    *ol=dry*il+wet*(in*0.5f+v0*0.6f+v2*0.3f);
    *or_=dry*ir+wet*(in*0.5f+v1*0.6f+v2*0.3f); (void)voices;
}

/* FLANGER: creative flanger blended with a spring reverb (unchanged - you liked it). */
#define FL_SP1 1100
#define FL_SP2 1600
#define FL_SPD 11000
static void m_flanger(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float rate=s->sm[0],depth=s->sm[1],regen=s->sm[2],spring=s->sm[3];
    float in=(il+ir)*0.5f;
    float lfohz = s->clocked ? (1000.f/s->tempo)*s->clkmul : expmap(rate,0.05f,4.f);  /* CLOCK sync */
    float lfo=0.5f+0.5f*tsin(s,s->lfo1); s->lfo1+=lfohz/s->sr; if(s->lfo1>1)s->lfo1-=1;
    float dly=20.f+depth*depth*480.f*lfo;
    float fr=rd(s->fl,1536,(float)s->w[0]-dly);
    s->fl[s->w[0]]=i16(soft(in+regen*0.9f*fr)); if(++s->w[0]>=1536)s->w[0]=0;
    float flo=(in+fr)*0.6f;
    float a1=allpass(BUF,FL_SP1,&s->w[1],0.6f,flo);
    float a2=allpass(BUF+FL_SP1,FL_SP2,&s->w[2],0.6f,a1);
    float tail=rd(BUF+FL_SP1+FL_SP2,FL_SPD,(float)s->w[3]-(FL_SPD-1));
    float damp=onepole(&s->lp[0],tail,0.5f);
    BUF[FL_SP1+FL_SP2+s->w[3]]=i16(soft(a2+0.6f*damp)); if(++s->w[3]>=FL_SPD)s->w[3]=0;
    *ol=dry*il+wet*(flo*(1.f-spring)+(a2+damp)*spring);
    *or_=dry*ir+wet*(flo*(1.f-spring)+(a1+damp)*spring);
}

/* TERMINAL (OP-1): envelope-tracked CRT noise + bit-crusher + sample-rate reducer,
   then a filter. BITS=bit depth (1..16), RATE=downsample freq, GLITCH=noise leak that
   follows the input envelope, FILTER=low-pass cutoff on the result. */
static void m_terminal(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float bits=s->sm[0],rate=s->sm[1],glitch=s->sm[2],filt=s->sm[3];
    float in=(il+ir)*0.5f;
    float a=in<0?-in:in; onepole(&s->env,a,0.01f);     /* envelope follower */
    /* CRT glitch: noise * envelope, wobbling on an independent slow LFO (lfo1) */
    float clk=0.5f+0.5f*tsin(s,s->lfo1); s->lfo1+=3.f/s->sr; if(s->lfo1>1.f)s->lfo1-=1.f;
    float n=frand(s)*glitch*0.6f*s->env*(0.4f+clk);
    float sig=in+n;
    /* bit-depth reduction (BITS: knob up = more bits/cleaner, down = crunchy) */
    float depth=1.f+bits*15.f; float lv=exp2_approx(depth-1.f);
    sig=(float)((int)(sig*lv+(sig<0?-0.5f:0.5f)))/lv;
    /* sample-and-hold downsampler on its OWN phase (lfo2); 1 kHz floor so it never freezes */
    float fsr=expmap(rate,1000.f,44100.f);
    s->lfo2+=fsr/s->sr; if(s->lfo2>=1.f){ s->lfo2-=1.f; s->ap[0]=sig; }
    float x=s->ap[0];
    /* filter (raw foldback kept; just a gentle tone shaping) */
    float fc=expmap(filt,200.f,16000.f); float lo;
    svf(s,&s->lp[0],&s->lp[1],x,fc,0.2f,&lo,0,0);
    float y=soft(lo);
    *ol=dry*il+wet*y; *or_=dry*ir+wet*y;
}

/* HAZE: a big, slow, modulated reverb wash with output tremolo -> the Galaxie-500 /
   dream-pop ambience (very long sustain, drifting). A 6-tap modulated feedback delay in
   the shared buffer forms a long dense tail; a damping low-pass sits in the loop.
   DECAY=tail length (up to many seconds), TONE=brightness, MOD=drift depth, TREM=tremolo. */
static void m_haze(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float decay=s->sm[0],tone=s->sm[1],modd=s->sm[2],trem=s->sm[3];
    float in=(il+ir)*0.5f;
    float mod=tsin(s,s->lfo2)*(6.f+modd*90.f); s->lfo2+=0.15f/s->sr; if(s->lfo2>1.f)s->lfo2-=1.f;
    float a=rd(BUF,BUF_LEN,(float)s->w[0]-(4993.f+mod));   /* 6 long incommensurate taps */
    float b=rd(BUF,BUF_LEN,(float)s->w[0]-(7177.f-mod));
    float c=rd(BUF,BUF_LEN,(float)s->w[0]-10453.f);
    float d=rd(BUF,BUF_LEN,(float)s->w[0]-(14723.f+mod*0.5f));
    float e=rd(BUF,BUF_LEN,(float)s->w[0]-19477.f);
    float f=rd(BUF,BUF_LEN,(float)s->w[0]-26903.f);
    float sum=(a+b+c+d+e+f)*0.16667f;                     /* average -> bounded, keeps headroom */
    float damp=0.12f+tone*0.82f; onepole(&s->lp[0],sum,damp);  /* TONE: dark..bright */
    float fb=clampf(0.8f+decay*0.197f,0.f,0.997f);        /* DECAY: 0.8..0.997 -> very long, near-freeze */
    BUF[s->w[0]]=i16(soft(in*0.6f + fb*s->lp[0])); if(++s->w[0]>=BUF_LEN)s->w[0]=0;
    float tl=0.5f+0.5f*tsin(s,s->lfo1); s->lfo1+=4.5f/s->sr; if(s->lfo1>1.f)s->lfo1-=1.f;
    float wetv=soft(s->lp[0]*3.8f)*(1.f-trem*tl);         /* boosted to full level + TREM tremolo */
    *ol=dry*il+wet*wetv; *or_=dry*ir+wet*wetv;
}

/* ---------- glue ---------- */
static void dsp_noop(mfx_engine_t*e,uint16_t i,int v){ (void)e;(void)i;(void)v; }
static void settext(char*d,const char*s,int n){ int i=0; for(;i<n-1&&s[i];i++)d[i]=s[i]; for(;i<n;i++)d[i]=0; }
/* PHONE (OP-1): a voice down a phone line. Reconstructed from behaviour, not code:
   gritty pre-drive -> narrow telephony band-pass (~300 Hz..3.4 kHz, movable) -> bit
   reduction -> line hiss/crackle. DRIVE=input grit, BAND=centre of the passband,
   CRUSH=bit depth (up=dirtier), NOISE=line noise floor. */
static void m_phone(state_t*s,float il,float ir,float*ol,float*or_,float wet,float dry){
    float drive=s->sm[0],band=s->sm[1],crush=s->sm[2],noise=s->sm[3];
    float in=(il+ir)*0.5f;
    float x=soft(in*(1.f+drive*18.f));                 /* carbon-mic grit */
    float loc=expmap(band,180.f,900.f);                /* HP corner: 180..900 Hz */
    float hic=expmap(band,2200.f,3800.f);              /* LP corner: 2.2..3.8 kHz */
    float hp,lo;
    svf(s,&s->ap[0],&s->ap[1],x,loc,0.35f,0,&hp,0);    /* high-pass */
    svf(s,&s->ap[2],&s->ap[3],hp,hic,0.35f,&lo,0,0);   /* -> narrow band */
    float lv=exp2_approx(15.f-crush*12.f);             /* CRUSH up -> 3..15 bits */
    float sig=(float)((int)(lo*lv+(lo<0?-0.5f:0.5f)))/lv;
    float ia=in<0?-in:in; onepole(&s->env,ia,0.02f);   /* noise gate: real threshold, not a fade */
    float gate=clampf((s->env-0.01f)*30.f,0.f,1.f);    /* below ~-40 dB -> fully closed */
    sig+=frand(s)*noise*0.12f*gate;                    /* hiss + crackle only above the gate */
    float y=soft(sig)*0.8f;                            /* trimmed to match the other modes' level */
    *ol=dry*il+wet*y; *or_=dry*ir+wet*y;
}
static void rename_params(mfx_engine_t*e){ int m=clampi(mfx_value(e,P_MODE),0,NUM_MODES-1);
    for(int k=0;k<4;k++) settext(e->params[P1+k].name,pnames[m][k],16); ST->named_mode=m; }
static void reset_mode(state_t*s){
    for(int i=0;i<12;i++)s->w[i]=0; for(int i=0;i<3;i++)s->ph[i]=0;
    for(int i=0;i<8;i++)s->lp[i]=0; for(int i=0;i<6;i++)s->ap[i]=0;
    s->lfo1=0;s->lfo2=0; s->trig=0; s->hold=0; s->env=0; s->dglide=0;
    for(int i=0;i<16;i++)s->hbA[i]=0; s->mstep=0;   /* keep s->tempo across mode switches */
    for(int i=0;i<1536;i++)s->fl[i]=0;
    mfx_clear_delaybuf();   /* shared buffer holds stale audio -> would otherwise re-feed */
}
static void lab_activate(mfx_engine_t*e){
    state_t*s=ST;
    for(int i=0;i<1025;i++){ float x=(float)i/1024.f*2.f*PI_F; if(x>PI_F)x-=2.f*PI_F; s->sine[i]=sin_poly(x); }
    for(int i=0;i<EXT_LEN;i++)s->ext[i]=0;
    s->rng=0x2468ace0u; s->sr=MFX_SAMPLE_RATE; s->last_mode=-1;
    s->tempo=0.f; s->clocked=0; s->clkmul=1.f;   /* no sync until a tempo arrives */
    reset_mode(s); mfx_clear_delaybuf(); mfx_set_dsp_handler(e,dsp_noop);
    s->magic=MAGIC; rename_params(e);
}
static void lab_deactivate(mfx_engine_t*e){ (void)e; ST->magic=0; }
static void lab_tick(mfx_engine_t*e){
    for(int i=0;i<NUM_P;i++) mfx_fix_assign(&e->params[i]);
    if(clampi(mfx_value(e,P_MODE),0,NUM_MODES-1)!=ST->named_mode) rename_params(e);
    /* MIX follows the global WET/DRY setting: +0x2d != 0 hides the screen while browsing */
    e->params[P_MIX]._2d = mfx_mix_enabled() ? 0 : 1;
}
static void lab_tap(mfx_engine_t*e,int period,char flag){ (void)e;(void)flag;
    if(period>0) ST->tempo=(float)period;   /* beat interval in ms from TAP / MIDI clock */ }
static void lab_process(mfx_engine_t*e,const int16_t*in,int16_t*out,int16_t n){
    state_t*s=ST;
    if(s->magic!=MAGIC){ for(int i=0;i<n;i++)out[i]=in[i]; return; }
    int mode=clampi(mfx_value(e,P_MODE),0,NUM_MODES-1);
    if(mode!=s->last_mode){ reset_mode(s); s->last_mode=mode; }
    for(int k=0;k<5;k++){ float t=pv(e,P1+k); s->sm[k]+=0.02f*(t-s->sm[k]); }
    s->clocked = (s->tempo>1.f);                             /* synced whenever a tempo (TAP or MIDI) is present */
    s->clkmul  = mul_val[clampi(mfx_value(e,P_CLKMUL),0,MUL_N-1)]; /* CLK MUL divider/multiplier */
    float wet=1.f,dry=0.f; mfx_mix_gains((uint16_t)mfx_value(e,P_MIX),&wet,&dry);
    for(int i=0;i<n;i+=2){
        float il=in[i]*(1.f/32768.f), ir=in[i+1]*(1.f/32768.f), ol,or_;
        switch(mode){
            case M_NITRO:    m_nitro(s,il,ir,&ol,&or_,wet,dry); break;
            case M_CHORD:    m_chord(s,il,ir,&ol,&or_,wet,dry); break;
            case M_FLANGER:  m_flanger(s,il,ir,&ol,&or_,wet,dry); break;
            case M_TERMINAL: m_terminal(s,il,ir,&ol,&or_,wet,dry); break;
            case M_PHONE:    m_phone(s,il,ir,&ol,&or_,wet,dry); break;
            case M_MURF:     m_murf(s,il,ir,&ol,&or_,wet,dry); break;
            default:         m_haze(s,il,ir,&ol,&or_,wet,dry); break;
        }
        out[i]=i16(ol); out[i+1]=i16(or_);
    }
}
static void on_param(mfx_param_t*p,uint8_t i,int a){ (void)i;(void)a;
    if(p->index==P_MODE && p->engine) rename_params(p->engine);
}
void engine_ctor(mfx_engine_t*e){
    settext(e->name,"USER LAB",16); settext(e->category,"FX VIBES",12);
    e->process=lab_process; e->activate=lab_activate; e->deactivate=lab_deactivate;
    e->tap=lab_tap; e->fn4d0=lab_tick; e->nparams=NUM_P;
    mfx_add_param(e,P_MODE,MFX_PT_ENUM,"MODE",0,0,NUM_MODES-1,on_param);
    e->params[P_MODE].label=label_mode; e->params[P_MODE].flag2e=0;
    mfx_add_param(e,P1,MFX_PT_LEVEL,"P1",0x6000,0,0xFFFF,on_param);
    mfx_add_param(e,P2,MFX_PT_LEVEL,"P2",0x8000,0,0xFFFF,on_param);
    mfx_add_param(e,P3,MFX_PT_LEVEL,"P3",0x6000,0,0xFFFF,on_param);
    mfx_add_param(e,P4,MFX_PT_LEVEL,"P4",0x8000,0,0xFFFF,on_param);
    for(int k=0;k<4;k++) e->params[P1+k].label=val_label;
    mfx_add_param(e,P_MIX,MFX_PT_MIX,"MIX",0x9000,0,0xFFFF,on_param);
    mfx_add_presets(e,P_PRESET,presets,(uint8_t)(sizeof(presets)/sizeof(presets[0])));
    mfx_add_param(e,P_CLOCK, MFX_PT_ENUM,"CLOCK",  0,0,1,on_param);  /* INT / MIDI */
    e->params[P_CLOCK].label=label_clk;  e->params[P_CLOCK].flag2e=0;
    mfx_add_param(e,P_CLKMUL,MFX_PT_ENUM,"CLK MUL",MUL_DEF,0,MUL_N-1,on_param);  /* 27 steps, default x1 */
    e->params[P_CLKMUL].label=label_mul; e->params[P_CLKMUL].flag2e=0;
    static const uint8_t cont[]={P1,P2,P3,P4,P_MIX};
    for(unsigned i=0;i<sizeof(cont);i++) e->params[cont[i]].hires=1;
    for(int i=0;i<NUM_P;i++) if(mfx_fix_assign(&e->params[i])) mfx_param_save(&e->params[i],0);
    mfx_set_io(e,MFX_IO_STEREO);
}
__attribute__((used, section(".hook")))
void user_hook(mfx_engine_t*u){ MFX_HOOK_ORIG_FN(u);
    uint8_t n=MFX_REG_COUNT;
    if(n<MFX_MAX_ENGINES){ mfx_engine_t*e=(mfx_engine_t*)(MFX_REGISTRY+n*MFX_ENGINE_SIZE); MFX_REG_COUNT=n+1; engine_ctor(e); }
}
#ifndef LAB_HOST
void *memset(void*d,int c,unsigned n){ unsigned char*p=d; while(n--)*p++=(unsigned char)c; return d; }
void *memcpy(void*d,const void*s,unsigned n){ unsigned char*p=d; const unsigned char*q=s; while(n--)*p++=*q++; return d; }
#endif
