/* RtlWaitOnAddress regression (v1.20.1): Bun/Zig call ntdll!RtlWaitOnAddress with a LARGE_INTEGER* timeout.
   The shim parks the thread and is re-executed on wake-up — it must not re-read the timeout pointer from R9 then. */
#include <windows.h>
#include <stdio.h>
typedef LONG (WINAPI *pfn_t)(volatile void *, void *, SIZE_T, LARGE_INTEGER *);
static volatile LONG flag = 0;
static DWORD WINAPI waker(LPVOID p) { (void)p; Sleep(150); InterlockedExchange(&flag, 1); ((void (WINAPI *)(PVOID))GetProcAddress(GetModuleHandleA("kernel32.dll"), "WakeByAddressAll"))((PVOID)&flag); return 0; }
int main(void) {
  pfn_t f = (pfn_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlWaitOnAddress");
  if (!f) { printf("no RtlWaitOnAddress\n"); return 1; }
  LONG cmp = 0; LARGE_INTEGER t; t.QuadPart = -1000000LL;   /* 100 ms relative */
  DWORD t0 = GetTickCount(); LONG r1 = f(&flag, &cmp, 4, &t);   /* nobody wakes us: must time out (0x102) after parking */
  DWORD d1 = GetTickCount() - t0;
  HANDLE h = CreateThread(0, 0, waker, 0, 0, 0); t.QuadPart = -50000000LL;   /* 5 s */
  t0 = GetTickCount(); LONG r2 = 0; while (flag == 0) r2 = f(&flag, &cmp, 4, &t);   /* parked, then woken by the other thread */
  DWORD d2 = GetTickCount() - t0; WaitForSingleObject(h, 5000);
  printf("timeout -> 0x%lx after %s; woken -> 0x%lx flag=%ld after %s\n", r1, d1 >= 80 && d1 < 2000 ? "~100ms" : "bad time", r2, flag, d2 >= 100 && d2 < 4000 ? "~150ms" : "bad time");
  return 0;
}
