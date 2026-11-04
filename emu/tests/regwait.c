#include <windows.h>
#include <stdio.h>
static HANDLE ev;
static void CALLBACK cb(PVOID ctx, BOOLEAN timedout) { printf("callback ctx=%p timedout=%d\n", ctx, timedout); fflush(stdout); SetEvent(ev); }
int main(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), w = NULL; ev = CreateEventW(NULL, TRUE, FALSE, NULL);
  BOOL ok = RegisterWaitForSingleObject(&w, in, cb, (PVOID)0x1234, INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
  printf("register -> %d wait=%p\n", ok, w); fflush(stdout);
  DWORD r = WaitForSingleObject(ev, 8000); printf("event wait -> %lu\n", r);
  UnregisterWait(w);
  INPUT_RECORD rec[16]; DWORD n = 0; if (ReadConsoleInputW(in, rec, 16, &n)) printf("records=%lu\n", n);
  return 0;
}
