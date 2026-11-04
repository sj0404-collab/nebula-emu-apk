/* msvcp.c — drives the REAL msvcp140.dll (VC++ redistributable) through its mangled exports: proves that a
 * vendor runtime DLL loads inside the emulator, that its static initialisation (std::cout & friends, locale
 * facets) works on top of our UCRT shims and that output reaches the console.  Built with zig cc (no MSVC needed). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef void *(*fn_ostream_int)(void *self, int v);
typedef void *(*fn_ostream_dbl)(void *self, double v);
typedef void *(*fn_put)(void *self, char ch);
typedef void *(*fn_flush)(void *self);
typedef void *(*fn_ctor_str)(void *self, const char *s);   /* ??0?$basic_string...@QEAA@PEBD@Z is inline — not exported; use _Yarn? no: keep to ostream */
int main(int argc, char **argv) {
  const char *name = argc > 1 ? argv[1] : "msvcp140.dll";
  HMODULE m = LoadLibraryA(name);
  if (!m) { printf("LoadLibrary(%s) failed: %lu\n", name, GetLastError()); return 2; }
  void *cout = (void *)GetProcAddress(m, "?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A");
  fn_ostream_int op_int = (fn_ostream_int)GetProcAddress(m, "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@H@Z");
  fn_ostream_dbl op_dbl = (fn_ostream_dbl)GetProcAddress(m, "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@N@Z");
  fn_put put = (fn_put)GetProcAddress(m, "?put@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@D@Z");
  fn_flush flush = (fn_flush)GetProcAddress(m, "?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ");
  if (!cout || !op_int || !op_dbl || !put || !flush) { printf("exports missing: cout=%p int=%p dbl=%p put=%p flush=%p\n", cout, (void *)op_int, (void *)op_dbl, (void *)put, (void *)flush); return 3; }
  /* std::cout << 42 << ' ' << 3.25 << '\n' << std::flush — exactly what an MSVC-built program does */
  op_int(cout, 42); put(cout, ' '); op_dbl(cout, 3.25); put(cout, '\n'); flush(cout);
  /* the "cout << string" path goes through basic_string, which is header-only; the narrow literal path is
     operator<<(ostream&, const char*) — also a header template in VS2015+, so we spell it with put() */
  const char *msg = "msvcp140 says hello\n"; for (const char *p = msg; *p; p++) put(cout, *p); flush(cout);
  printf("MSVCP OK\n");
  return 0;
}
