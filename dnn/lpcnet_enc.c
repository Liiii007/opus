/* Copyright (c) 2017-2019 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kiss_fft.h"
#include "common.h"
#include <math.h>
#include "freq.h"
#include "pitch.h"
#include "arch.h"
#include <assert.h>
#include "lpcnet_private.h"
#include "lpcnet.h"
#include "os_support.h"
#include "_kiss_fft_guts.h"


int lpcnet_encoder_get_size() {
  return sizeof(LPCNetEncState);
}

int lpcnet_encoder_init(LPCNetEncState *st) {
  memset(st, 0, sizeof(*st));
  st->exc_mem = lin2ulaw(0.f);
  pitchdnn_init(&st->pitchdnn);
  return 0;
}

LPCNetEncState *lpcnet_encoder_create() {
  LPCNetEncState *st;
  st = malloc(lpcnet_encoder_get_size());
  lpcnet_encoder_init(st);
  return st;
}

void lpcnet_encoder_destroy(LPCNetEncState *st) {
  free(st);
}

static void frame_analysis(LPCNetEncState *st, kiss_fft_cpx *X, float *Ex, const float *in) {
  float x[WINDOW_SIZE];
  OPUS_COPY(x, st->analysis_mem, OVERLAP_SIZE);
  OPUS_COPY(&x[OVERLAP_SIZE], in, FRAME_SIZE);
  OPUS_COPY(st->analysis_mem, &in[FRAME_SIZE-OVERLAP_SIZE], OVERLAP_SIZE);
  apply_window(x);
  forward_transform(X, x);
  lpcn_compute_band_energy(Ex, X);
}

static void biquad(float *y, float mem[2], const float *x, const float *b, const float *a, int N) {
  int i;
  for (i=0;i<N;i++) {
    float xi, yi;
    xi = x[i];
    yi = x[i] + mem[0];
    mem[0] = mem[1] + (b[0]*(double)xi - a[0]*(double)yi);
    mem[1] = (b[1]*(double)xi - a[1]*(double)yi);
    y[i] = yi;
  }
}


void compute_frame_features(LPCNetEncState *st, const float *in) {
  float aligned_in[FRAME_SIZE];
  int i;
  float E = 0;
  float Ly[NB_BANDS];
  float follow, logMax;
  kiss_fft_cpx X[FREQ_SIZE];
  float Ex[NB_BANDS];
  float xcorr[PITCH_MAX_PERIOD];
  float ener0;
  int sub;
  float ener;
  /* [b,a]=ellip(2, 2, 20, 1200/8000); */
  static const float lp_b[2] = {-0.84946f, 1.f};
  static const float lp_a[2] = {-1.54220f, 0.70781f};
  OPUS_COPY(aligned_in, &st->analysis_mem[OVERLAP_SIZE-TRAINING_OFFSET], TRAINING_OFFSET);
  frame_analysis(st, X, Ex, in);
  st->if_features[0] = MAX16(-1, MIN16(1, (1.f/64)*(10.f*log10(1e-15 + X[0].r*X[0].r)-6)));
  for (i=1;i<PITCH_IF_MAX_FREQ;i++) {
    kiss_fft_cpx prod;
    float norm_1;
    C_MULC(prod, X[i], st->prev_if[i]);
    norm_1 = 1.f/sqrt(1e-15 + prod.r*prod.r + prod.i*prod.i);
    C_MULBYSCALAR(prod, norm_1);
    st->if_features[3*i-2] = prod.r;
    st->if_features[3*i-1] = prod.i;
    st->if_features[3*i] = MAX16(-1, MIN16(1, (1.f/64)*(10.f*log10(1e-15 + X[i].r*X[i].r + X[i].i*X[i].i)-6)));
  }
  OPUS_COPY(st->prev_if, X, PITCH_IF_MAX_FREQ);
  /*for (i=0;i<88;i++) printf("%f ", st->if_features[i]);printf("\n");*/
  logMax = -2;
  follow = -2;
  for (i=0;i<NB_BANDS;i++) {
    Ly[i] = log10(1e-2+Ex[i]);
    Ly[i] = MAX16(logMax-8, MAX16(follow-2.5f, Ly[i]));
    logMax = MAX16(logMax, Ly[i]);
    follow = MAX16(follow-2.5f, Ly[i]);
    E += Ex[i];
  }
  dct(st->features, Ly);
  st->features[0] -= 4;
  lpc_from_cepstrum(st->lpc, st->features);
  for (i=0;i<LPC_ORDER;i++) st->features[NB_BANDS+2+i] = st->lpc[i];
  OPUS_MOVE(st->exc_buf, &st->exc_buf[FRAME_SIZE], PITCH_MAX_PERIOD);
  OPUS_MOVE(st->lp_buf, &st->lp_buf[FRAME_SIZE], PITCH_MAX_PERIOD);
  OPUS_COPY(&aligned_in[TRAINING_OFFSET], in, FRAME_SIZE-TRAINING_OFFSET);
  for (i=0;i<FRAME_SIZE;i++) {
    int j;
    float sum = aligned_in[i];
    for (j=0;j<LPC_ORDER;j++)
      sum += st->lpc[j]*st->pitch_mem[j];
    OPUS_MOVE(st->pitch_mem+1, st->pitch_mem, LPC_ORDER-1);
    st->pitch_mem[0] = aligned_in[i];
    st->lp_buf[PITCH_MAX_PERIOD+i] = sum;
    st->exc_buf[PITCH_MAX_PERIOD+i] = sum + .7f*st->pitch_filt;
    st->pitch_filt = sum;
    /*printf("%f\n", st->exc_buf[PITCH_MAX_PERIOD+i]);*/
  }
  biquad(&st->lp_buf[PITCH_MAX_PERIOD], st->lp_mem, &st->lp_buf[PITCH_MAX_PERIOD], lp_b, lp_a, FRAME_SIZE);
  {
    double ener1;
    float *buf = st->exc_buf;
    celt_pitch_xcorr(&buf[PITCH_MAX_PERIOD], buf, xcorr, FRAME_SIZE, PITCH_MAX_PERIOD-PITCH_MIN_PERIOD, st->arch);
    ener0 = celt_inner_prod_c(&buf[PITCH_MAX_PERIOD], &buf[PITCH_MAX_PERIOD], FRAME_SIZE);
    ener1 = celt_inner_prod_c(&buf[0], &buf[0], FRAME_SIZE-1);
    /*printf("%f\n", st->frame_weight[sub]);*/
    for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) {
      ener1 += buf[i+FRAME_SIZE-1]*buf[i+FRAME_SIZE-1];
      ener = 1 + ener0 + ener1;
      st->xcorr_features[i] = 2*xcorr[i] / ener;
      ener1 -= buf[i]*buf[i];
      /*printf("%f ", st->xcorr_features[i]);*/
    }
    /*printf("\n");*/
  }
  st->dnn_pitch = compute_pitchdnn(&st->pitchdnn, st->if_features, st->xcorr_features);
  /*printf("%f\n", st->dnn_pitch);*/
  /* Cross-correlation on half-frames. */
  for (sub=0;sub<2;sub++) {
    int off = sub*FRAME_SIZE/2;
    double ener1;
    celt_pitch_xcorr(&st->exc_buf[PITCH_MAX_PERIOD+off], st->exc_buf+off, xcorr, FRAME_SIZE/2, PITCH_MAX_PERIOD, st->arch);
    ener0 = celt_inner_prod_c(&st->exc_buf[PITCH_MAX_PERIOD+off], &st->exc_buf[PITCH_MAX_PERIOD+off], FRAME_SIZE/2);
    ener1 = celt_inner_prod_c(&st->exc_buf[off], &st->exc_buf[off], FRAME_SIZE/2-1);
    st->frame_weight[sub] = ener0;
    /*printf("%f\n", st->frame_weight[sub]);*/
    for (i=0;i<PITCH_MAX_PERIOD;i++) {
      ener1 += st->exc_buf[i+off+FRAME_SIZE/2-1]*st->exc_buf[i+off+FRAME_SIZE/2-1];
      ener = 1 + ener0 + ener1;
      st->xc[sub][i] = 2*xcorr[i] / ener;
      ener1 -= st->exc_buf[i+off]*st->exc_buf[i+off];
    }
    if (1) {
      /* Upsample correlation by 3x and keep the max. */
      float interpolated[PITCH_MAX_PERIOD]={0};
      /* interp=sinc([-3:3]+1/3).*(.5+.5*cos(pi*[-3:3]/4.5)); interp=interp/sum(interp); */
      static const float interp[7] = {0.026184f, -0.098339f, 0.369938f, 0.837891f, -0.184969f, 0.070242f, -0.020947f};
      for (i=4;i<PITCH_MAX_PERIOD-4;i++) {
        float val1=0, val2=0;
        int j;
        for (j=0;j<7;j++) {
          val1 += st->xc[sub][i-3+j]*interp[j];
          val2 += st->xc[sub][i+3-j]*interp[j];
          interpolated[i] = MAX16(st->xc[sub][i], MAX16(val1, val2));
        }
      }
      for (i=4;i<PITCH_MAX_PERIOD-4;i++) {
        st->xc[sub][i] = interpolated[i];
      }
    }
#if 0
    for (i=0;i<PITCH_MAX_PERIOD;i++)
      printf("%f ", st->xc[sub][i]);
    printf("\n");
#endif
  }
}

void process_single_frame(LPCNetEncState *st, FILE *ffeat) {
  int i;
  int sub;
  int best_i;
  int best[4];
  int pitch_prev[2][PITCH_MAX_PERIOD];
  float frame_corr;
  float frame_weight_sum = 1e-15f;
  for(sub=0;sub<2;sub++) frame_weight_sum += st->frame_weight[sub];
  for(sub=0;sub<2;sub++) st->frame_weight[sub] *= (2.f/frame_weight_sum);
  for(sub=0;sub<2;sub++) {
    float max_path_all = -1e15f;
    best_i = 0;
    for (i=0;i<PITCH_MAX_PERIOD-2*PITCH_MIN_PERIOD;i++) {
      float xc_half = MAX16(MAX16(st->xc[sub][(PITCH_MAX_PERIOD+i)/2], st->xc[sub][(PITCH_MAX_PERIOD+i+2)/2]), st->xc[sub][(PITCH_MAX_PERIOD+i-1)/2]);
      if (st->xc[sub][i] < xc_half*1.1f) st->xc[sub][i] *= .8f;
    }
    for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) {
      int j;
      float max_prev;
      max_prev = st->pitch_max_path_all - 6.f;
      pitch_prev[sub][i] = st->best_i;
      for (j=IMAX(-4, -i);j<=4 && i+j<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;j++) {
        if (st->pitch_max_path[0][i+j] - .02f*abs(j)*abs(j) > max_prev) {
          max_prev = st->pitch_max_path[0][i+j] - .02f*abs(j)*abs(j);
          pitch_prev[sub][i] = i+j;
        }
      }
      st->pitch_max_path[1][i] = max_prev + st->frame_weight[sub]*st->xc[sub][i];
      if (st->pitch_max_path[1][i] > max_path_all) {
        max_path_all = st->pitch_max_path[1][i];
        best_i = i;
      }
    }
    /* Renormalize. */
    for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) st->pitch_max_path[1][i] -= max_path_all;
    /*for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) printf("%f ", st->pitch_max_path[1][i]);
    printf("\n");*/
    OPUS_COPY(&st->pitch_max_path[0][0], &st->pitch_max_path[1][0], PITCH_MAX_PERIOD);
    st->pitch_max_path_all = max_path_all;
    st->best_i = best_i;
  }
  best_i = st->best_i;
  frame_corr = 0;
  /* Backward pass. */
  for (sub=1;sub>=0;sub--) {
    best[2+sub] = PITCH_MAX_PERIOD-best_i;
    frame_corr += st->frame_weight[sub]*st->xc[sub][best_i];
    best_i = pitch_prev[sub][best_i];
  }
  frame_corr /= 2;
  if (1) {
    float xy, xx, yy;
    int pitch = (best[2]+best[3])/2;
    xx = celt_inner_prod_c(&st->lp_buf[PITCH_MAX_PERIOD], &st->lp_buf[PITCH_MAX_PERIOD], FRAME_SIZE);
    yy = celt_inner_prod_c(&st->lp_buf[PITCH_MAX_PERIOD-pitch], &st->lp_buf[PITCH_MAX_PERIOD-pitch], FRAME_SIZE);
    xy = celt_inner_prod_c(&st->lp_buf[PITCH_MAX_PERIOD], &st->lp_buf[PITCH_MAX_PERIOD-pitch], FRAME_SIZE);
    //printf("%f %f\n", frame_corr, xy/sqrt(1e-15+xx*yy));
    frame_corr = xy/sqrt(1+xx*yy);
    //frame_corr = MAX32(0, xy/sqrt(1+xx*yy));
    frame_corr = log(1.f+exp(5.f*frame_corr))/log(1+exp(5.f));
  }
  st->features[NB_BANDS] = .01f*(IMAX(66, IMIN(510, best[2]+best[3]))-200);
  st->features[NB_BANDS] = st->dnn_pitch;
  st->features[NB_BANDS + 1] = frame_corr-.5f;
  if (ffeat) {
    fwrite(st->features, sizeof(float), NB_TOTAL_FEATURES, ffeat);
  }
}

void preemphasis(float *y, float *mem, const float *x, float coef, int N) {
  int i;
  for (i=0;i<N;i++) {
    float yi;
    yi = x[i] + *mem;
    *mem = -coef*x[i];
    y[i] = yi;
  }
}

static int lpcnet_compute_single_frame_features_impl(LPCNetEncState *st, float *x, float features[NB_TOTAL_FEATURES]) {
  preemphasis(x, &st->mem_preemph, x, PREEMPHASIS, FRAME_SIZE);
  compute_frame_features(st, x);
  process_single_frame(st, NULL);
  OPUS_COPY(features, &st->features[0], NB_TOTAL_FEATURES);
  return 0;
}

int lpcnet_compute_single_frame_features(LPCNetEncState *st, const opus_int16 *pcm, float features[NB_TOTAL_FEATURES]) {
  int i;
  float x[FRAME_SIZE];
  for (i=0;i<FRAME_SIZE;i++) x[i] = pcm[i];
  lpcnet_compute_single_frame_features_impl(st, x, features);
  return 0;
}

int lpcnet_compute_single_frame_features_float(LPCNetEncState *st, const float *pcm, float features[NB_TOTAL_FEATURES]) {
  int i;
  float x[FRAME_SIZE];
  for (i=0;i<FRAME_SIZE;i++) x[i] = pcm[i];
  lpcnet_compute_single_frame_features_impl(st, x, features);
  return 0;
}
