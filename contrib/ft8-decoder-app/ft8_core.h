/* ft8_core — a thin, slot-oriented interface over kgoba/ft8_lib for the IC-7300 homebrew app.
 *
 * DESIGN SKETCH / PROPOSED INTERFACE — ft8_core.c is not yet implemented (see DESIGN.md).
 * This header is deliberately free of any radio-SDK and ft8_lib types so that main.c and
 * host_test.c can be written against it now, and so the host test builds the exact decode
 * path on a PC (as cw_core does for cw-rx).
 *
 * Model: FT8 runs on UTC-aligned 15 s slots. The caller owns the clock/scheduler (main.c) and
 * drives three phases per slot:
 *     ft8_slot_begin(rx);                         // at the UTC slot boundary
 *     ft8_feed(rx, samples, n);  // repeatedly    // ~0..12.8 s of 12 kHz mono int16 audio
 *     while (ft8_slot_decode(rx, on_msg, ctx))    // incremental: a bounded work-slice per call,
 *         { drain audio; hb_yield(); }            // so the 1.37 s audio ring never overruns
 *
 * The decode is a STEP FUNCTION, not one blocking call: the SDK's capture ring holds only ~1.37 s,
 * and the app is cooperative, so ft8_slot_decode() does a bounded amount of sync/LDPC work and
 * returns nonzero while more remains. The caller keeps draining audio and yielding between slices
 * (see DESIGN.md §4a). All DSP is integer/float and self-contained; the only large allocation is
 * the ft8_lib waterfall, sized by the config below (see ft8_heap_estimate).
 */
#ifndef FT8_CORE_H
#define FT8_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded FT8 message. text is NUL-terminated ASCII (e.g. "CQ HB9XYZ JN47"). */
typedef struct {
    int   freq_hz;     /* audio frequency of the signal within the passband        */
    float snr_db;      /* ft8_lib score mapped to dB (WSJT-X-style, approximate)    */
    float dt_sec;      /* time offset of the signal vs the slot grid               */
    int   score;       /* raw ft8_lib candidate/sync score (for sorting/debug)      */
    char  text[32];    /* decoded message text                                      */
} ft8_decode;

/* Called once per decoded message during ft8_slot_decode(). */
typedef void (*ft8_msg_fn)(void *ctx, const ft8_decode *d);

/* Opaque decoder state (holds the ft8_lib monitor + waterfall on the heap). */
typedef struct ft8_rx ft8_rx;

/* Configuration chosen at startup to fit the 448 KB heap (see DESIGN.md §6).
 *   sample_rate : 12000 on the radio (ft8_lib native; no resampling)
 *   f_min/f_max : decode passband in Hz; narrower = less memory
 *   time_osr    : time oversampling (1 or 2); 2 = best sensitivity, ~2x waterfall
 *   freq_osr    : frequency oversampling (1 or 2)
 *   max_decodes : cap on messages per slot (bounds the candidate/output work)
 */
typedef struct {
    int sample_rate;
    int f_min_hz, f_max_hz;
    int time_osr, freq_osr;
    int max_decodes;
} ft8_config;

/* A reasonable default profile for the radio: 12 kHz, 300..3000 Hz, osr 2/2, 32 decodes.
 * May be too large for 448 KB at osr 2/2 across the full passband — check ft8_heap_estimate()
 * and fall back to osr 1 or a narrower band if needed. */
ft8_config ft8_default_config(void);

/* Bytes of heap ft8_create() will need for this config (waterfall + FFT scratch + buffers).
 * Pure function — call it before ft8_create() to pick a profile that fits. */
size_t ft8_heap_estimate(const ft8_config *cfg);

/* Allocate/configure the decoder. Returns NULL on bad config or allocation failure. */
ft8_rx *ft8_create(const ft8_config *cfg);
void    ft8_destroy(ft8_rx *rx);

/* --- per-slot lifecycle --- */

/* Begin a new 15 s slot: reset the waterfall/monitor for fresh capture. */
void ft8_slot_begin(ft8_rx *rx);

/* Feed captured audio (12 kHz mono int16). Call as many times as needed during the ~12.8 s
 * capture window; ft8_core blocks the stream into ft8_lib's symbol-sized chunks internally.
 * Returns the number of samples consumed (normally n). */
int  ft8_feed(ft8_rx *rx, const int16_t *x, int n);

/* Advance the decode for the captured slot by one bounded work-slice: Costas sync search ->
 * candidates -> LDPC -> messages. Calls on_msg(ctx, &d) for each NEW decode found this slice
 * (de-duplicated across the slot). Cooperative by design (see §4a):
 *   returns  >0  : more work remains — call again after draining audio and yielding
 *   returns   0  : this slot's decode is complete
 * Call ft8_slot_begin() before the next slot to reset. (First call after capture starts the pass.) */
int  ft8_slot_decode(ft8_rx *rx, ft8_msg_fn on_msg, void *ctx);

/* --- read-only UI helpers --- */

/* Pointer to the current magnitude waterfall for drawing, with its dimensions.
 * Data is owned by ft8_core; valid until the next ft8_slot_begin(). Returns NULL if none yet.
 * Values are 0..255 magnitude; cols = time blocks so far, rows = frequency bins in-band. */
const uint8_t *ft8_waterfall(const ft8_rx *rx, int *cols, int *rows);

/* Fraction (0..1) of the capture window filled so far — for a progress/countdown UI. */
float ft8_capture_progress(const ft8_rx *rx);

#ifdef __cplusplus
}
#endif

#endif /* FT8_CORE_H */
