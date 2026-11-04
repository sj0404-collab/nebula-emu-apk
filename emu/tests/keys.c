#include <windows.h>
#include <stdio.h>
int main(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE); INPUT_RECORD rec[64]; DWORD n = 0; int total = 0;
  while (ReadConsoleInputW(in, rec, 64, &n) && n) { int done = 0; for (DWORD i = 0; i < n; i++) if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown) { WCHAR ch = rec[i].Event.KeyEvent.uChar.UnicodeChar; if (ch == 26) { done = 1; break; } printf("%04x ", ch); total++; } if (done) break; }
  printf("\ntotal=%d\n", total); return 0;
}
