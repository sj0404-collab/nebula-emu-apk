#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(int argc, char **argv) {
    printf("Hello from x86-64 Windows! argc=%d argv0=%s\n", argc, argv[0]);
    for (int i = 1; i < argc; i++) printf("  arg[%d]=%s\n", i, argv[i]);
    double x = 2.0; printf("sqrt(2)=%.6f sin(1)=%.4f pow(2,10)=%.0f %e %g\n", sqrt(x), sin(1.0), pow(2, 10), 123456.789, 0.00001234);
    printf("int: %d %5d|%-5d|%05d %x %X %o %u %lld %llu %zu %p\n", -42, 42, 42, 42, 255, 255, 8, 3000000000u, -1234567890123LL, 18446744073709551615ULL, sizeof(void*), (void*)0x1234);
    char buf[64]; snprintf(buf, sizeof buf, "%s-%c-%.2f", "snprintf", 'Z', 3.14159); puts(buf);
    int arr[] = { 5, 3, 9, 1, 7, 2, 8 }; qsort(arr, 7, sizeof arr[0], cmp); for (int i = 0; i < 7; i++) printf("%d ", arr[i]); printf("\n");
    char *m = malloc(100); strcpy(m, "heap string"); strcat(m, " ok"); printf("%s (%zu)\n", m, strlen(m)); free(m);
    unsigned long long h = 1469598103934665603ULL; for (int i = 0; i < 1000000; i++) { h ^= (unsigned char)i; h *= 1099511628211ULL; } printf("fnv=%016llx\n", h);
    float f = 0; for (int i = 1; i <= 1000; i++) f += 1.0f / (float)i; printf("harmonic=%.5f\n", f);
    LARGE_INTEGER q; QueryPerformanceCounter(&q); printf("qpc ok=%d tick=%u\n", q.QuadPart != 0, GetTickCount() != 0);
    char tmp[MAX_PATH]; GetModuleFileNameA(NULL, tmp, MAX_PATH); printf("module=%s cmdline=%s\n", tmp, GetCommandLineA());
    fprintf(stderr, "stderr works\n");
    return 7;
}
