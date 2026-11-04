#include <windows.h>
#include <stdio.h>
int main(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD r = WaitForSingleObject(in, 50); printf("wait(50ms) before input -> %lu\n", r);
  r = WaitForSingleObject(in, 5000); printf("wait(5s) -> %lu (0=signaled)\n", r);
  INPUT_RECORD rec[16]; DWORD n = 0; if (ReadConsoleInputW(in, rec, 16, &n)) { printf("ReadConsoleInput: %lu records", n); for (DWORD i = 0; i < n; i++) if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown) printf(" '%lc'", rec[i].Event.KeyEvent.uChar.UnicodeChar); printf("\n"); }
  return 0;
}
