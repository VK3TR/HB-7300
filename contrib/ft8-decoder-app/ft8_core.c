/* ft8_core — slot-oriented wrapper over kgoba/ft8_lib (pinned 9fec6ca) for the IC-7300 app.
 *
 * Pure C + ft8_lib (no radio-SDK types), so host_test.c runs this exact decode path on a PC.
 * The decode is a cooperative step function: ft8_slot_decode() processes a bounded slice of
 * candidates per call so the app's ~1.37 s audio ring never overruns (see DESIGN.md §4a).
 */
#include "ft8_core.h"

#include <ft8/decode.h>
#include <ft8/message.h>
#include <ft8/constants.h>
#include <common/monitor.h>

#include <stdlib.h>
#include <string.h>

/* decode tuning — same values as ft8_lib's reference decoder */
#define FT8_MIN_SCORE       10
#define FT8_MAX_CANDIDATES  140
#define FT8_LDPC_ITERS      25
#define FT8_DECODE_SLICE     16   /* candidates processed per ft8_slot_decode() call (§4a) */

struct ft8_rx {
    monitor_t   mon;
    ft8_config  cfg;

    float      *frame;      /* block_size accumulator for ft8_feed() */
    int         fill;

    ftx_candidate_t *cands;
    int         ncands;     /* -1 until the sync search has run this slot */
    int         cand_idx;

    ftx_message_t *seen;    /* de-dup table of emitted messages this slot */
    int         nseen, max_seen;

    uint8_t    *wf_view;    /* transposed [num_bins][num_blocks] for the UI */
    int         wf_view_cap;
};

ft8_config ft8_default_config(void)
{
    ft8_config c;
    c.sample_rate = 12000;        /* radio-native; no resampling */
    c.f_min_hz    = 200;
    c.f_max_hz    = 3000;
    c.time_osr    = 2;
    c.freq_osr    = 2;
    c.max_decodes = 50;
    return c;
}

/* Mirror monitor_init()'s allocation math so the caller can pick a profile that fits 448 KB. */
size_t ft8_heap_estimate(const ft8_config *cfg)
{
    const float symbol_period = FT8_SYMBOL_PERIOD;         /* 0.16 s */
    int block_size = (int)(cfg->sample_rate * symbol_period);
    int nfft       = block_size * cfg->freq_osr;
    int max_blocks = (int)(FT8_SLOT_TIME / symbol_period); /* 93 */
    int min_bin    = (int)(cfg->f_min_hz * symbol_period);
    int max_bin    = (int)(cfg->f_max_hz * symbol_period) + 1;
    int num_bins   = max_bin - min_bin;

    size_t mag   = (size_t)max_blocks * cfg->time_osr * cfg->freq_osr * num_bins; /* uint8 */
    size_t win   = (size_t)nfft * sizeof(float);          /* window */
    size_t frame = (size_t)nfft * sizeof(float);          /* last_frame */
    size_t td    = (size_t)nfft * sizeof(float);          /* monitor timedata (heap, HB-7300) */
    size_t fd    = (size_t)(nfft / 2 + 1) * 2 * sizeof(float); /* monitor freqdata (cpx) */
    size_t fftwk = 0;
    kiss_fftr_alloc(nfft, 0, 0, &fftwk);                  /* exact kiss work size (query) */
    size_t cands = (size_t)FT8_MAX_CANDIDATES * sizeof(ftx_candidate_t);
    size_t seen  = (size_t)cfg->max_decodes * sizeof(ftx_message_t);
    size_t view  = (size_t)num_bins * max_blocks;         /* UI waterfall view */
    size_t acc   = (size_t)block_size * sizeof(float);    /* ft8_feed accumulator */
    return mag + win + frame + td + fd + fftwk + cands + seen + view + acc;
}

ft8_rx *ft8_create(const ft8_config *cfg)
{
    if (!cfg || cfg->sample_rate <= 0 || cfg->f_max_hz <= cfg->f_min_hz ||
        cfg->time_osr < 1 || cfg->freq_osr < 1 || cfg->max_decodes < 1)
        return NULL;

    ft8_rx *rx = (ft8_rx *)calloc(1, sizeof *rx);
    if (!rx) return NULL;
    rx->cfg = *cfg;

    monitor_config_t mc;
    mc.f_min       = (float)cfg->f_min_hz;
    mc.f_max       = (float)cfg->f_max_hz;
    mc.sample_rate = cfg->sample_rate;
    mc.time_osr    = cfg->time_osr;
    mc.freq_osr    = cfg->freq_osr;
    mc.protocol    = FTX_PROTOCOL_FT8;
    monitor_init(&rx->mon, &mc);

    rx->frame   = (float *)malloc((size_t)rx->mon.block_size * sizeof(float));
    rx->cands   = (ftx_candidate_t *)malloc((size_t)FT8_MAX_CANDIDATES * sizeof(ftx_candidate_t));
    rx->max_seen = cfg->max_decodes;
    rx->seen    = (ftx_message_t *)malloc((size_t)rx->max_seen * sizeof(ftx_message_t));
    rx->wf_view_cap = rx->mon.wf.num_bins * rx->mon.wf.max_blocks;
    rx->wf_view = (uint8_t *)malloc((size_t)rx->wf_view_cap);

    if (!rx->mon.wf.mag || !rx->frame || !rx->cands || !rx->seen || !rx->wf_view) {
        ft8_destroy(rx);
        return NULL;
    }
    ft8_slot_begin(rx);
    return rx;
}

void ft8_destroy(ft8_rx *rx)
{
    if (!rx) return;
    monitor_free(&rx->mon);
    free(rx->frame);
    free(rx->cands);
    free(rx->seen);
    free(rx->wf_view);
    free(rx);
}

void ft8_slot_begin(ft8_rx *rx)
{
    monitor_reset(&rx->mon);
    rx->fill = 0;
    rx->ncands = -1;
    rx->cand_idx = 0;
    rx->nseen = 0;
}

int ft8_feed(ft8_rx *rx, const int16_t *x, int n)
{
    const int bs = rx->mon.block_size;
    for (int i = 0; i < n; i++) {
        rx->frame[rx->fill++] = (float)x[i] * (1.0f / 32768.0f);
        if (rx->fill == bs) {
            monitor_process(&rx->mon, rx->frame);   /* one symbol-block into the waterfall */
            rx->fill = 0;
        }
    }
    return n;
}

/* de-dup by hash + payload (same policy as ft8_lib's reference decoder) */
static int already_seen(ft8_rx *rx, const ftx_message_t *m)
{
    for (int i = 0; i < rx->nseen; i++)
        if (rx->seen[i].hash == m->hash &&
            0 == memcmp(rx->seen[i].payload, m->payload, sizeof m->payload))
            return 1;
    return 0;
}

int ft8_slot_decode(ft8_rx *rx, ft8_msg_fn on_msg, void *ctx)
{
    const monitor_t *mon = &rx->mon;
    const ftx_waterfall_t *wf = &mon->wf;

    if (rx->ncands < 0) {                 /* first slice: run the sync search once */
        rx->ncands = ftx_find_candidates(wf, FT8_MAX_CANDIDATES, rx->cands, FT8_MIN_SCORE);
        rx->cand_idx = 0;
    }

    int end = rx->cand_idx + FT8_DECODE_SLICE;
    if (end > rx->ncands) end = rx->ncands;

    for (; rx->cand_idx < end; rx->cand_idx++) {
        const ftx_candidate_t *cand = &rx->cands[rx->cand_idx];
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(wf, cand, FT8_LDPC_ITERS, &msg, &st))
            continue;
        if (already_seen(rx, &msg))
            continue;
        if (rx->nseen < rx->max_seen)
            rx->seen[rx->nseen++] = msg;

        ft8_decode d;
        d.freq_hz = (int)((mon->min_bin + cand->freq_offset +
                           (float)cand->freq_sub / wf->freq_osr) / mon->symbol_period + 0.5f);
        d.dt_sec  = (cand->time_offset + (float)cand->time_sub / wf->time_osr) * mon->symbol_period;
        d.score   = cand->score;
        d.snr_db  = cand->score * 0.5f;   /* crude, matches ft8_lib's placeholder */
        d.text[0] = 0;
        ftx_message_offsets_t off;
        if (ftx_message_decode(&msg, NULL, d.text, &off) != FTX_MESSAGE_RC_OK)
            d.text[0] = 0;
        if (on_msg && d.text[0])
            on_msg(ctx, &d);
    }
    return rx->cand_idx < rx->ncands ? 1 : 0;   /* 1 = more work, 0 = slot done */
}

const uint8_t *ft8_waterfall(const ft8_rx *rx, int *cols, int *rows)
{
    const ftx_waterfall_t *wf = &rx->mon.wf;
    int nb = wf->num_blocks, bins = wf->num_bins;
    if (nb <= 0 || bins <= 0) { if (cols) *cols = 0; if (rows) *rows = 0; return NULL; }

    /* transpose the mag[block][time_osr][freq_osr][bin] cube (sub 0,0) into [bin][block] */
    uint8_t *v = rx->wf_view;
    for (int b = 0; b < nb; b++) {
        const uint8_t *col = &wf->mag[(size_t)b * wf->block_stride]; /* time_sub=0, freq_sub=0 */
        for (int k = 0; k < bins; k++)
            v[(size_t)k * nb + b] = col[k];
    }
    if (cols) *cols = nb;
    if (rows) *rows = bins;
    return v;
}

float ft8_capture_progress(const ft8_rx *rx)
{
    int mb = rx->mon.wf.max_blocks;
    return mb > 0 ? (float)rx->mon.wf.num_blocks / mb : 0.0f;
}
