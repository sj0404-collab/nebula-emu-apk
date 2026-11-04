#include <windows.h>
#include <stdio.h>
__declspec(dllimport) int add3(int, int, int);
__declspec(dllimport) const char *dll_name(void);
typedef double (*hyp_t)(double, double);
int main(void) {
  printf("static import: add3=%d name=%s\n", add3(1, 2, 3), dll_name());
  HMODULE h = LoadLibraryA("mydll.dll"); printf("LoadLibrary=%p same=%d\n", (void *)h, h == GetModuleHandleA("mydll"));
  hyp_t f = (hyp_t)GetProcAddress(h, "dll_hyp"); printf("GetProcAddress dll_hyp -> %s, f(3,4)=%.1f\n", f ? "ok" : "NULL", f ? f(3, 4) : -1.0);
  printf("missing=%p err=%lu\n", (void *)GetProcAddress(h, "nope"), GetLastError());
  char path[260]; GetModuleFileNameA(h, path, sizeof path); printf("path=%s\n", path);
  HMODULE k = LoadLibraryA("kernel32.dll"); printf("kernel32 GetTickCount=%s\n", GetProcAddress(k, "GetTickCount") ? "ok" : "NULL");
  return add3(0, 0, 0);
}
