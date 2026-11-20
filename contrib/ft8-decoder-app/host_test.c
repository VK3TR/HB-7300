/* Host self-test for ft8_core: encode an FT8 message, synthesize GFSK audio at 12 kHz, add
 * calibrated noise, and decode it back through the exact ft8_core path the radio app uses.
 *
 * Build (see Makefile):
 *   make            (see Makefile — compiles ft8_core + the vendored ft8_lib subset)
 *
 * Usage:
 *   ./ft8_test                         # SNR sweep of the default message, report decode floor
 *   ./ft8_test "CQ HB9XYZ JN47" 0 1200 # one run: message, SNR(dB, model), audio freq(Hz)
 *
 * The GFSK synthesis (gfsk_pulse/synth_gfsk) is adapted from kgoba/ft8_lib demo/gen_ft8.c (MIT).
 * SNR is a 2500 Hz-referenced model for comparability, not a lab-calibrated figure.
 */
#include "ft8_core.h"
#include <ft8/message.h>
#include <ft8/encode.h>
#include <ft8/constants.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS      12000
#define GFSK_K  5.336446f               /* pi*sqrt(2/ln2) */
#define FT8_BT  2.0f                    /* GFSK bandwidth factor for FT8 (demo/gen_ft8.c) */

/* ---- deterministic RNG (xorshift32) + Gaussian (Box-Muller) ---- */
static uint32_t g_rng = 1;
static float frand(void){ g_rng ^= g_rng<<13; g_rng ^= g_rng>>17; g_rng ^= g_rng<<5; return (g_rng>>8)*(1.0f/16777216.0f); }
static float gauss(void){ float u1=frand()+1e-7f, u2=frand(); return sqrtf(-2.0f*logf(u1))*cosf(6.2831853f*u2); }

/* ---- GFSK synthesis (adapted from ft8_lib demo/gen_ft8.c, MIT) ---- */
static void gfsk_pulse(int n_spsym, float bt, float *pulse){
    for (int i=0;i<3*n_spsym;i++){ float t=i/(float)n_spsym-1.5f;
        pulse[i]=(erff(GFSK_K*bt*(t+0.5f))-erff(GFSK_K*bt*(t-0.5f)))/2; }
}
static void synth_gfsk(const uint8_t *sym,int n_sym,float f0,float bt,float sp,int sr,float *sig){
    int n_spsym=(int)(0.5f+sr*sp), n_wave=n_sym*n_spsym;
    float dphi_peak=2*M_PI/n_spsym;
    float *dphi=(float*)malloc((n_wave+2*n_spsym)*sizeof(float));
    float *pulse=(float*)malloc(3*n_spsym*sizeof(float));
    for (int i=0;i<n_wave+2*n_spsym;i++) dphi[i]=2*M_PI*f0/sr;
    gfsk_pulse(n_spsym,bt,pulse);
    for (int i=0;i<n_sym;i++){ int ib=i*n_spsym;
        for (int j=0;j<3*n_spsym;j++) dphi[j+ib]+=dphi_peak*sym[i]*pulse[j]; }
    for (int j=0;j<2*n_spsym;j++){ dphi[j]+=dphi_peak*pulse[j+n_spsym]*sym[0];
        dphi[j+n_sym*n_spsym]+=dphi_peak*pulse[j]*sym[n_sym-1]; }
    float phi=0; for (int k=0;k<n_wave;k++){ sig[k]=sinf(phi); phi=fmodf(phi+dphi[k+n_spsym],2*M_PI); }
    int nr=n_spsym/8; for (int i=0;i<nr;i++){ float e=(1-cosf(2*M_PI*i/(2*nr)))/2; sig[i]*=e; sig[n_wave-1-i]*=e; }
    free(dphi); free(pulse);
}

/* ---- decode collection ---- */
static char g_got[64][32]; static int g_ngot;
static void on_msg(void *ctx,const ft8_decode *d){ (void)ctx;
    if (g_ngot<64){ strncpy(g_got[g_ngot],d->text,31); g_got[g_ngot][31]=0; g_ngot++; } }

static void rstrip(char *s){ int n=(int)strlen(s); while(n>0&&s[n-1]==' ') s[--n]=0; }

/* Build 15 s of audio for `text` at `freq`, add noise for `snr_db`, decode, return 1 if `text`
 * (trailing-space-trimmed) appears among the decodes. Prints the decodes when verbose. */
static int run_one(const char *text,float snr_db,float freq,int verbose,int *out_ndecode){
    ftx_message_t msg;
    if (ftx_message_encode(&msg,NULL,text)!=FTX_MESSAGE_RC_OK){ printf("encode failed: \"%s\"\n",text); return -1; }
    uint8_t tones[FT8_NN]; ft8_encode(msg.payload,tones);

    int n_sym=FT8_NN, n_spsym=(int)(0.5f+FS*FT8_SYMBOL_PERIOD), n_data=n_sym*n_spsym;
    int n_total=(int)(FT8_SLOT_TIME*FS), n_sil=(n_total-n_data)/2;
    float *sig=(float*)calloc(n_total,sizeof(float));
    synth_gfsk(tones,n_sym,freq,FT8_BT,FT8_SYMBOL_PERIOD,FS,sig+n_sil);

    /* noise for a 2500 Hz-referenced SNR: Psig=A^2/2 (A~1 tone), Pn_2500=sigma^2*(2500/(FS/2)) */
    double Psig=0.5, sigma=sqrt(Psig/(pow(10.0,snr_db/10.0)*(2500.0/(FS/2.0))));
    int16_t *pcm=(int16_t*)malloc(n_total*sizeof(int16_t));
    for (int i=0;i<n_total;i++){ double v=(sig[i]*0.3 + sigma*gauss()*0.3)*32767.0;
        if(v>32767)v=32767; if(v<-32768)v=-32768; pcm[i]=(int16_t)v; }
    free(sig);

    ft8_config cfg=ft8_default_config();
    ft8_rx *rx=ft8_create(&cfg);
    if(!rx){ printf("ft8_create failed\n"); free(pcm); return -1; }
    ft8_slot_begin(rx);
    /* feed in realistic ~1024-sample chunks, like hb_audio_read */
    for (int p=0;p<n_total;p+=1024){ int n=(n_total-p<1024?n_total-p:1024); ft8_feed(rx,pcm+p,n); }
    g_ngot=0;
    while (ft8_slot_decode(rx,on_msg,NULL)) { /* drain all slices */ }
    ft8_destroy(rx); free(pcm);

    char want[32]; strncpy(want,text,31); want[31]=0; rstrip(want);
    int hit=0; for (int i=0;i<g_ngot;i++){ char g[32]; strncpy(g,g_got[i],31); g[31]=0; rstrip(g);
        if(0==strcmp(g,want)) hit=1; }
    if (verbose){ printf("  decoded %d message(s):\n",g_ngot);
        for (int i=0;i<g_ngot;i++) printf("    \"%s\"\n",g_got[i]); }
    if (out_ndecode) *out_ndecode=g_ngot;
    return hit;
}

int main(int argc,char**argv){
    const char *msg = argc>1 ? argv[1] : "CQ HB9XYZ JN47";
    if (argc>1){
        float snr = argc>2 ? atof(argv[2]) : 0.0f;
        float frq = argc>3 ? atof(argv[3]) : 1200.0f;
        g_rng = 12345;
        int nd=0, hit=run_one(msg,snr,frq,1,&nd);
        printf("sent:    \"%s\"  (SNR=%.0f dB model, %.0f Hz)\n",msg,snr,frq);
        printf("result:  %s\n", hit==1 ? "EXACT COPY ✓" : (hit==0?"not decoded":"error"));
        return hit==1?0:1;
    }
    /* no args: sweep SNR and report the decode floor for the default message */
    printf("FT8 host self-test — message \"%s\" at 1200 Hz, SNR sweep (2500 Hz model)\n",msg);
    printf("  SNR dB |  decoded? | #msgs\n  -------+-----------+------\n");
    float floor_db=999;
    for (float snr=10; snr>=-18; snr-=2){ g_rng=(uint32_t)(1000+snr*7); int nd=0;
        int hit=run_one(msg,snr,1200.0f,0,&nd);
        printf("  %+5.0f  |    %-3s    |  %d\n",snr,hit==1?"yes":"no",nd);
        if (hit==1) floor_db=snr;
    }
    printf("\ndecode floor in this model: %.0f dB%s\n", floor_db,
           floor_db<900?"":"  (never decoded — check build)");
    return floor_db<900?0:1;
}
