/* FT8 decoder app for the IC-7300 homebrew SDK (jeskko/ic7300-homebrew, loader ABI v4).
 *
 * Taps the 12 kHz demodulated RX audio, builds an ft8_lib waterfall, and every ~15 s decodes the
 * slot: Costas sync -> LDPC -> message text, scrolled on a full-screen canvas with a live
 * waterfall strip. DSP/decode is ft8_core.c over vendored ft8_lib (third_party/ft8_lib/).
 *
 * Timing: the SDK exposes no UTC clock (DESIGN.md §5), so this build FREE-RUNS — it captures until
 * the waterfall is full (~14.9 s), decodes, and repeats, with no operator time entry. That decodes
 * any transmission that falls in the window (ft8_lib's sync search finds the DT); a future build can
 * add UTC-slot alignment for higher yield. Free-run is also what lets the emulator decode a WAV
 * played into the RX slot with no input injection.
 */
#include "hb/app.h"
#include "hb/audio.h"
#include "hb/gfx.h"
#include "hb/input.h"

#include "ft8_core.h"
#include <stddef.h>
#include <stdint.h>

/* ---- layout ---- */
#define WF_Y     0
#define WF_H     70                              /* waterfall strip height */
#define STAT_Y   (WF_H + 2)
#define STAT_H   18
#define LIST_Y   (STAT_Y + STAT_H + 2)
#define SCALE    2
#define LH       (HB_FONT_H * SCALE + 4)         /* decode-list line height */
#define ROWS     ((HB_SCREEN_H - LIST_Y) / LH)
#define COLS     ((HB_SCREEN_W - 8) / (HB_FONT_ADVANCE * SCALE))
#define EXIT_S   40
#define EXIT_X   (HB_SCREEN_W - EXIT_S)

#define BG       HB_RGB(8, 10, 16)
#define FG       HB_RGB(210, 230, 210)
#define DIM      HB_RGB(120, 130, 140)
#define ACCENT   HB_RGB(90, 200, 255)

/* ---- decode scroll buffer ---- */
static char g_line[ROWS][COLS + 1];
static int  g_nlines;

static void push_line(const char *s)
{
    int row = g_nlines < ROWS ? g_nlines : ROWS - 1;
    if (g_nlines >= ROWS)                         /* scroll up */
        for (int r = 0; r < ROWS - 1; r++)
            for (int c = 0; c <= COLS; c++) g_line[r][c] = g_line[r + 1][c];
    int c = 0;
    for (; s[c] && c < COLS; c++) g_line[row][c] = s[c];
    g_line[row][c] = 0;
    g_nlines++;
}

/* observable globals for the emulator test (read over QMP by test_emu.py) */
volatile int      g_decodes;      /* total messages decoded */
volatile int      g_slot;         /* free-run slot counter */
volatile uint32_t g_samples;      /* RX samples fed to the decoder */
char              g_last[32];     /* text of the most recent decode */
/* debug observability for the emulator bring-up */
volatile uint32_t g_dbg_create;   /* g_rx value captured right after ft8_create (0 = NULL) */
volatile int      g_dbg_state;    /* current state (0=CAP 1=DECODE) */
volatile int      g_dbg_prog;     /* capture_progress * 1000 */
volatile int      g_dbg_heap;     /* ft8_heap_estimate bytes */

/* "+dB FFFF TEXT" per decode — tiny int formatter, no libc */
static void on_msg(void *ctx, const ft8_decode *d)
{
    (void)ctx;
    for (int i = 0; i < 31; i++) { g_last[i] = d->text[i]; if (!d->text[i]) break; }
    g_last[31] = 0;
    char b[COLS + 1]; int n = 0;
    int f = d->freq_hz; if (f < 0) f = 0; if (f > 9999) f = 9999;
    int snr = d->snr_db >= 0 ? (int)(d->snr_db + 0.5f) : (int)(d->snr_db - 0.5f);
    b[n++] = snr < 0 ? '-' : '+'; if (snr < 0) snr = -snr; if (snr > 99) snr = 99;
    b[n++] = '0' + snr / 10; b[n++] = '0' + snr % 10; b[n++] = ' ';
    b[n++] = '0' + (f / 1000) % 10; b[n++] = '0' + (f / 100) % 10;
    b[n++] = '0' + (f / 10) % 10;  b[n++] = '0' + f % 10; b[n++] = ' ';
    for (const char *p = d->text; *p && n < COLS; p++) b[n++] = *p;
    b[n] = 0;
    push_line(b);
    g_decodes++;
}

/* ---- app state ---- */
static ft8_rx    *g_rx;
static ft8_config g_cfg;
static enum { CAP, DECODE } g_state = CAP;

static void draw(hb_canvas *cv)
{
    hb_clear(cv, BG);

    /* waterfall from ft8_core (owned by it; NULL until the first blocks arrive) */
    int wc = 0, wr = 0;
    const uint8_t *wf = ft8_waterfall(g_rx, &wc, &wr);
    if (wf && wc > 0 && wr > 0) {
        int xs = wc > HB_SCREEN_W ? HB_SCREEN_W : wc;
        for (int x = 0; x < xs; x++)
            for (int y = 0; y < wr && y < WF_H; y++) {
                uint8_t m = wf[y * wc + x];
                hb_fill_rect(cv, x, WF_Y + y, 1, 1, HB_RGB(m / 2, m / 3, m));
            }
    }

    /* status bar */
    char s[48]; int n = 0;
    const char *lbl = g_state == CAP ? "RX " : "DECODING ";
    for (const char *p = lbl; *p; p++) s[n++] = *p;
    if (g_state == CAP) {                          /* capture % */
        int pct = (int)(ft8_capture_progress(g_rx) * 100.0f + 0.5f); if (pct > 99) pct = 99;
        s[n++] = '0' + pct / 10; s[n++] = '0' + pct % 10; s[n++] = '%';
    }
    s[n++] = ' '; s[n++] = 'S'; s[n++] = '0' + (g_slot / 10) % 10; s[n++] = '0' + g_slot % 10;
    s[n++] = ' '; s[n++] = 'N'; s[n++] = '0' + (g_decodes / 10) % 10; s[n++] = '0' + g_decodes % 10;
    s[n] = 0;
    hb_text(cv, 4, STAT_Y + 4, s, 1, g_state == CAP ? ACCENT : FG);

    /* EXIT box */
    hb_fill_rect(cv, EXIT_X, 0, EXIT_S, EXIT_S, HB_RGB(60, 30, 30));
    hb_line(cv, EXIT_X + 10, 10, EXIT_X + EXIT_S - 10, EXIT_S - 10, FG);
    hb_line(cv, EXIT_X + EXIT_S - 10, 10, EXIT_X + 10, EXIT_S - 10, FG);

    /* decoded messages */
    for (int r = 0; r < ROWS; r++)
        if (g_line[r][0]) hb_text(cv, 4, LIST_Y + r * LH, g_line[r], SCALE, FG);
}

static bool touched_prev;
static void handle_input(bool *running)
{
    if (hb_key_down(HB_KEY_EXIT)) { *running = false; return; }
    hb_touch t; hb_touch_read(&t);
    if (t.down && !touched_prev && t.x >= EXIT_X && t.y < EXIT_S) *running = false;
    touched_prev = t.down;
}

int main(void)
{
    if (!hb_gfx_open()) return 1;

    g_cfg = ft8_default_config();
    g_dbg_heap = (int)ft8_heap_estimate(&g_cfg);
    if ((unsigned)g_dbg_heap > 430 * 1024) g_cfg.time_osr = 1;   /* keep margin in 448 KB */
    g_rx = ft8_create(&g_cfg);
    g_dbg_create = (uint32_t)(uintptr_t)g_rx;      /* captured before the NULL check */
    if (!g_rx && g_cfg.time_osr > 1) {             /* retry smaller if the first profile failed */
        g_cfg.time_osr = 1; g_rx = ft8_create(&g_cfg);
        g_dbg_create = (uint32_t)(uintptr_t)g_rx;
    }
    if (!g_rx) { hb_gfx_close(); return 2; }
    hb_audio_open();
    ft8_slot_begin(g_rx);

    bool running = true;
    while (running) {
        /* 1) drain audio every pass — the capture ring is only ~1.37 s (DESIGN.md §4a) */
        int16_t buf[1024]; int nn;
        while ((nn = hb_audio_read(buf, 1024)) > 0) {
            if (g_state == CAP) { ft8_feed(g_rx, buf, nn); g_samples += (uint32_t)nn; }
            if (nn < 1024) break;
        }

        /* 2) free-run slot machine: fill the waterfall, then decode, then restart */
        g_dbg_prog = (int)(ft8_capture_progress(g_rx) * 1000.0f);
        g_dbg_state = (int)g_state;
        if (g_state == CAP && ft8_capture_progress(g_rx) >= 0.99f)
            g_state = DECODE;
        if (g_state == DECODE) {
            /* one bounded decode slice per pass; audio keeps draining above */
            if (ft8_slot_decode(g_rx, on_msg, NULL) == 0) {
                g_slot++;
                ft8_slot_begin(g_rx);
                g_state = CAP;
            }
        }

        handle_input(&running);
        draw(hb_gfx_canvas());
        hb_gfx_present();                          /* yields to the radio until vsync */
    }

    ft8_destroy(g_rx);
    hb_audio_close();
    hb_gfx_close();
    return 0;
}
