/* Freestanding libc/libm shim so vendored ft8_lib links against the jeskko SDK runtime.
 *
 * The SDK is -nostdlib/-fno-builtin and provides only memcpy/memset/strlen (libc.c),
 * hb_sinf/hb_cosf (math.c) and the hb_malloc family (heap.c, 448 KB heap). ft8_lib needs a
 * handful more libc/libm symbols; this file supplies them, mapping allocation onto the SDK heap
 * and transcendentals onto compact implementations. Logging and asserts are compiled out
 * (-D'LOG_PRINTF(...)=' and -DNDEBUG), so no stdio/_impure_ptr is pulled in.
 *
 * Only built into the on-radio FT8.BIN; the host test uses the real libc/libm.
 */
#include <stddef.h>
#include <stdint.h>
#include "hb/heap.h"
#include "hb/math.h"

/* ---- allocation -> SDK heap ---- */
void *malloc(size_t n)            { return hb_malloc(n); }
void *calloc(size_t c, size_t s)  { return hb_calloc(c, s); }
void *realloc(void *p, size_t n)  { return hb_realloc(p, n); }
void  free(void *p)               { hb_free(p); }

/* ---- string helpers the SDK libc lacks ---- */
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; i++) if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    return 0;
}
char *strcpy(char *d, const char *s)        { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0; return d;
}
char *strcat(char *d, const char *s)        { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
char *stpcpy(char *d, const char *s)        { while ((*d = *s)) { d++; s++; } return d; }

/* ---- math ---- */
/* exact enough for FT8 magnitude/sync (hb_* are ~1e-6; FFT is used for detection, not resynthesis) */
float  sinf(float x)  { return hb_sinf(x); }
float  cosf(float x)  { return hb_cosf(x); }
double sin(double x)  { return (double)hb_sinf((float)x); }
double cos(double x)  { return (double)hb_cosf((float)x); }

float sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u = { x };
    u.i = (1u << 29) + (u.i >> 1) - (1u << 22);          /* fast inverse-free seed */
    float y = u.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}
double sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double y = (double)sqrtf((float)x);                   /* seed from the float sqrt */
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return y;
}

float logf(float x)
{
    if (x <= 0.0f) return -1.0e30f;
    union { float f; uint32_t i; } u = { x };
    int e = (int)((u.i >> 23) & 0xFF) - 127;              /* exponent */
    u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;              /* mantissa in [1,2) */
    float m = u.f;
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;        /* atanh series: ln m = 2(t + t^3/3 + ...) */
    float s = 2.0f * t * (1.0f + t2 * (1.0f/3 + t2 * (1.0f/5 + t2 * (1.0f/7))));
    return (float)e * 0.69314718055994531f + s;
}
float log10f(float x) { return logf(x) * 0.43429448190325176f; }

double floor(double x)
{
    long long i = (long long)x;                           /* FFT indices are small, in range */
    double t = (double)i;
    return (t > x) ? t - 1.0 : t;
}

/* last-resort stub (should be unreachable with -DNDEBUG; no clean exit on the radio) */
void exit(int code) { (void)code; for (;;) {} }
