#include <windows.h>
#include <stdio.h>
#include <excpt.h>
static int filter(DWORD code, EXCEPTION_POINTERS *p) { printf("  filter: code 0x%08lx at %p\n", code, p->ExceptionRecord->ExceptionAddress); return code == 0xE0001234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
static void inner(void) { __try { puts("  inner: raising"); RaiseException(0xE0001234, 0, 2, (ULONG_PTR[]){ 11, 22 }); puts("  not reached"); } __finally { puts("  inner __finally"); } }
int main(void) {
  __try { inner(); puts("not reached"); } __except (filter(GetExceptionCode(), GetExceptionInformation())) { printf("__except: code 0x%08lx\n", GetExceptionCode()); }
  __try { puts("second try"); } __finally { puts("second finally (normal exit)"); }
  int depth = 0;
  __try { __try { RaiseException(0xE0000001, 0, 0, 0); } __except (EXCEPTION_CONTINUE_SEARCH) { puts("wrong"); } } __except (EXCEPTION_EXECUTE_HANDLER) { depth = GetExceptionCode() == 0xE0000001; }
  printf("nested except: %d\n", depth);
  puts("done");
  return 0;
}
