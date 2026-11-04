#include <windows.h>
#include <stdio.h>
__declspec(dllimport) int bump(int);
static DWORD WINAPI th(LPVOID p) { int v = 0; for (int i = 0; i < 5; i++) v = bump((int)(INT_PTR)p); printf("thread %d: tls=%d\n", (int)(INT_PTR)p, v); return 0; }
int main(void) {
  printf("main first bump: %d\n", bump(1));
  HANDLE h[2]; h[0] = CreateThread(0, 0, th, (LPVOID)10, 0, 0); h[1] = CreateThread(0, 0, th, (LPVOID)100, 0, 0);
  WaitForMultipleObjects(2, h, TRUE, INFINITE);
  printf("main after threads: %d (expect 9)\n", bump(1));
  return 0;
}
