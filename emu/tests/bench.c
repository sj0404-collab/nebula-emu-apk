#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
/* integer + float + memory benchmark to measure emulator speed */
static unsigned int rng = 12345; static unsigned int next(void) { rng = rng * 1664525u + 1013904223u; return rng; }
int main(void) {
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    /* 1. integer: sieve */
    int N = 2000000; unsigned char *s = calloc(N, 1); int primes = 0;
    for (int i = 2; i < N; i++) { if (s[i]) continue; primes++; for (long long j = (long long)i * i; j < N; j += i) s[j] = 1; }
    QueryPerformanceCounter(&t1); printf("sieve(2M): %d primes in %.1f ms\n", primes, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    /* 2. float: mandelbrot 200x100 */
    QueryPerformanceCounter(&t0); long inside = 0;
    for (int py = 0; py < 100; py++) for (int px = 0; px < 200; px++) { double cx = -2.0 + px * 3.0 / 200, cy = -1.0 + py * 2.0 / 100, zx = 0, zy = 0; int k; for (k = 0; k < 200 && zx * zx + zy * zy < 4; k++) { double t = zx * zx - zy * zy + cx; zy = 2 * zx * zy + cy; zx = t; } if (k == 200) inside++; }
    QueryPerformanceCounter(&t1); printf("mandelbrot(200x100x200): %ld inside in %.1f ms\n", inside, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    /* 3. memory: 16 MB memcpy x 8 + random access */
    QueryPerformanceCounter(&t0); unsigned char *a = malloc(16 << 20), *b = malloc(16 << 20); for (int i = 0; i < (16 << 20); i += 4096) a[i] = (unsigned char)i; for (int r = 0; r < 8; r++) memcpy(b, a, 16 << 20); unsigned sum = 0; for (int i = 0; i < 1000000; i++) sum += b[next() & ((16 << 20) - 1)];
    QueryPerformanceCounter(&t1); printf("memcpy 128MB + 1M random reads: sum=%u in %.1f ms\n", sum, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    return 0;
}
