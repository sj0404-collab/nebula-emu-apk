#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static DWORD WINAPI thr(LPVOID p) { printf("thread %u param=%d\n", (unsigned)GetCurrentThreadId() != 0, *(int *)p); return 7; }
int main(int argc, char **argv) {
  printf("hello from 32-bit x86! argc=%d sizeof(void*)=%u\n", argc, (unsigned)sizeof(void *));
  for (int i = 0; i < argc; i++) printf("  argv[%d]=%s\n", i, argv[i]);
  int a[8] = { 5, 3, 9, 1, 7, 2, 8, 4 }; qsort(a, 8, sizeof a[0], cmp);
  printf("sorted:"); for (int i = 0; i < 8; i++) printf(" %d", a[i]); printf("\n");
  char *p = malloc(100); strcpy(p, "heap ok"); printf("%s %p\n", p, (void *)(p != 0)); free(p);
  double d = sin(1.0) + pow(2.0, 10.0) + sqrt(2.0); printf("math %.6f %lld %.3f\n", d, 1234567890123LL * 3, floor(2.7));
  long long q = 1234567890123LL; printf("ll div %lld mul %lld\n", q / 7, q * 11);
  char buf[64]; snprintf(buf, sizeof buf, "%s-%d-%x", "fmt", 42, 255); puts(buf);
  int v = 33; HANDLE h = CreateThread(0, 0, thr, &v, 0, 0); WaitForSingleObject(h, INFINITE); DWORD ec = 0; GetExitCodeThread(h, &ec); printf("thread exit %lu\n", ec);
  SYSTEM_INFO si; GetSystemInfo(&si); printf("arch=%u pagesize=%lu\n", si.wProcessorArchitecture, si.dwPageSize);
  char path[MAX_PATH]; GetModuleFileNameA(0, path, sizeof path); printf("exe=%s\n", path);
  DWORD tid = GetCurrentThreadId(); printf("tid ok=%d tick ok=%d\n", tid != 0, GetTickCount() > 0);
  FILE *f = fopen("C:\\tmp\\t32.txt", "w"); if (f) { fprintf(f, "line %d\n", 1); fclose(f); f = fopen("C:\\tmp\\t32.txt", "r"); if (f) { fgets(buf, sizeof buf, f); printf("file: %s", buf); fclose(f); } }
  return 42;
}
