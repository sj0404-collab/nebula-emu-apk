/* shelltest.c — system(), _popen("r"), _popen("w") through the embedded cmd.exe (zig cc -target x86_64-windows-gnu -O2 -o shelltest.exe shelltest.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  printf("shell available: %d\n", system(NULL)); fflush(stdout);
  int rc = system("echo hello from system & exit 5");
  printf("system rc=%d\n", rc); fflush(stdout);
  FILE *p = _popen("dir /b *.lua & echo done", "r"); char line[256]; int n = 0;
  while (p && fgets(line, sizeof line, p)) { line[strcspn(line, "\r\n")] = 0; printf("popen: [%s]\n", line); n++; }
  int prc = p ? _pclose(p) : -1; printf("popen lines=%d rc=%d\n", n, prc); fflush(stdout);
  FILE *w = _popen("sort", "w"); if (w) { fputs("pear\napple\nmango\n", w); int wrc = _pclose(w); printf("popen-w rc=%d\n", wrc); }
  return 0;
}
