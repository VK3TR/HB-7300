/* FT8 decoder app for the IC-7300 homebrew SDK — UI + slot scheduler SCAFFOLD.
 *
 * ---------------------------------------------------------------------------------------------
 *  SCAFFOLD, NOT A WORKING APP. It shows the app's shape against the real SDK + the proposed
 *  ft8_core interface. It compiles (syntax) against jeskko's headers, but ft8_core.c and the
 *  vendored ft8_lib are not implemented yet (see DESIGN.md). Do not flash this.
 * ---------------------------------------------------------------------------------------------
 *
 * Flow (DESIGN.md §4/§4a): drain 12 kHz RX audio every pass into ft8_feed(); on the UTC 15 s grid,
 * ft8_slot_begin() at the boundary, capture ~12.8 s, then run ft8_slot_decode() INCREMENTALLY
 * (a bounded slice per pass) so the 1.37 s audio ring never overruns. Decodes scroll as text; the
 * ft8_lib waterfall is drawn up top. The SDK exposes no UTC clock, so the operator sets it once and
 * we free-run from hb_millis() (DESIGN.md §5).
 */
#include "hb/app.h"
#include "hb/audio.h"
#include "hb/gfx.h"
#include "hb/input.h"

#include "ft8_core.h"
#include <stddef.h>

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
static int  g_nlines;                            /* total pushed; newest = bottom */

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

/* append "HH:MM:SS +dB  FREQ  TEXT" — tiny int formatter, no libc */
static void on_msg(void *ctx, const ft8_decode *d)
{
    (void)ctx;
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
}

/* ---- UTC slot clock (free-run from hb_millis; operator sets the anchor) ---- */
static uint32_t g_anchor_ms;                     /* hb_millis() at g_anchor_utc */
static int32_t  g_anchor_utc = -1;               /* seconds; -1 = not set yet */

static int32_t now_utc(void) { return g_anchor_utc + (int32_t)((hb_millis() - g_anchor_ms) / 1000); }
static int     slot_phase_ms(void)               /* 0..14999 within the current 15 s slot */
{
    if (g_anchor_utc < 0) return -1;
    uint32_t d = hb_millis() - g_anchor_ms;
    int32_t ms = (int32_t)(d % 15000) + (g_anchor_utc % 15) * 1000;
    return ms % 15000;
}

/* ---- app state ---- */
static ft8_rx    *g_rx;
static ft8_config g_cfg;
static enum { CAP, DECODE } g_state = CAP;
static int g_last_slot = -1;
#define CAP_MS 12800                             /* capture window end within the slot */

static void draw(hb_canvas *cv)
{
    hb_clear(cv, BG);

    /* waterfall from ft8_core (owned by it; may be NULL early) */
    int wc = 0, wr = 0;
    const uint8_t *wf = ft8_waterfall(g_rx, &wc, &wr);
    if (wf && wc > 0 && wr > 0) {
        for (int x = 0; x < wc && x < HB_SCREEN_W; x++)
            for (int y = 0; y < wr && y < WF_H; y++) {
                uint8_t m = wf[y * wc + x];       /* 0..255 magnitude */
                hb_fill_rect(cv, x, WF_Y + y, 1, 1, HB_RGB(m / 2, m / 3, m));
            }
    }

    /* status bar: UTC phase countdown + decode profile */
    char s[40]; int n = 0;
    const char *lbl = g_anchor_utc < 0 ? "SET UTC (tap)  " : (g_state == CAP ? "RX  " : "DECODE ");
    for (const char *p = lbl; *p; p++) s[n++] = *p;
    int ph = slot_phase_ms();
    if (ph >= 0) { int t = (15000 - ph) / 1000; s[n++] = 'T'; s[n++] = '-'; s[n++] = '0' + t / 10; s[n++] = '0' + t % 10; }
    s[n] = 0;
    hb_text(cv, 4, STAT_Y + 4, s, 1, g_state == CAP ? ACCENT : FG);

    /* PROF hint + EXIT box */
    hb_text(cv, EXIT_X - 64, STAT_Y + 4, "PROF", 1, DIM);
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
    if (t.down && !touched_prev) {
        if (t.x >= EXIT_X && t.y < EXIT_S) { *running = false; }
        else if (t.y < STAT_Y + STAT_H) {
            /* SCAFFOLD: tapping the status bar stands in for the UTC-entry UI (DESIGN.md §5).
             * Real app: a digit entry screen. Here: anchor "now" to a placeholder so the grid runs. */
            if (g_anchor_utc < 0) { g_anchor_ms = hb_millis(); g_anchor_utc = 0; }
            /* TODO: PROF tap cycles decode-BW/osr profiles via ft8_heap_estimate() + recreate g_rx */
        }
    }
    touched_prev = t.down;
}

int main(void)
{
    if (!hb_gfx_open()) return 1;
    g_cfg = ft8_default_config();
    /* Pick a profile that fits the heap before allocating (DESIGN.md §6). */
    if (ft8_heap_estimate(&g_cfg) > 400 * 1024) { g_cfg.time_osr = 1; }   /* fall back */
    g_rx = ft8_create(&g_cfg);
    if (!g_rx) { hb_gfx_close(); return 2; }
    hb_audio_open();

    bool running = true;
    while (running) {
        /* 1) always drain audio first — the ring is only ~1.37 s (DESIGN.md §4a) */
        int16_t buf[1024]; int nn;
        while ((nn = hb_audio_read(buf, 1024)) > 0) {
            if (g_state == CAP && g_anchor_utc >= 0) ft8_feed(g_rx, buf, nn);
            if (nn < 1024) break;
        }

        /* 2) advance the UTC slot state machine */
        if (g_anchor_utc >= 0) {
            int slot = (int)(now_utc() / 15);
            int ph   = slot_phase_ms();
            if (slot != g_last_slot) {            /* new 15 s slot boundary */
                g_last_slot = slot;
                ft8_slot_begin(g_rx);
                g_state = CAP;
            }
            if (g_state == CAP && ph >= CAP_MS) g_state = DECODE;
            if (g_state == DECODE) {
                /* one bounded decode slice per pass; audio keeps draining above (§4a) */
                if (ft8_slot_decode(g_rx, on_msg, NULL) == 0) g_state = CAP; /* done; wait for next slot */
            }
        }

        handle_input(&running);
        draw(hb_gfx_canvas());
        hb_gfx_present();                         /* yields to the radio until vsync */
    }

    ft8_destroy(g_rx);
    hb_audio_close();
    hb_gfx_close();
    return 0;
}
