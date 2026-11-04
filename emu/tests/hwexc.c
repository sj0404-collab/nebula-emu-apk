/* hardware exceptions as SEH: access violation / divide by zero / ud2 caught by __except, vectored handlers, continue execution, top-level filter.
 * build: zig cc -target x86_64-windows-gnu -O1 -fms-extensions -o hwexc.exe hwexc.c
 * (clang only puts calls inside __try scopes without -fasync-exceptions, so the faulting accesses live in noinline helpers — like real programs where the fault is deep in a callee) */
#include <windows.h>
#include <stdio.h>

static volatile int *g_bad = (volatile int *)0x10;
static volatile int g_zero = 0;
static int veh_calls = 0, veh_last_code = 0;
__attribute__((noinline)) static void do_write(volatile int *p, int v) { *p = v; }
__attribute__((noinline)) static int do_div(int a, int b) { return a / b; }
__attribute__((noinline)) static void do_trap(void) { __builtin_trap(); }

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
  veh_calls++; veh_last_code = (int)ep->ExceptionRecord->ExceptionCode;
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO && ep->ContextRecord->Rip) {
    /* fix up: skip the faulting idiv (3 bytes: F7 /7 with register operand) and pretend the quotient was 42 */
    unsigned char *ip = (unsigned char *)ep->ContextRecord->Rip; int len = 0;
    if ((ip[0] & 0xF0) == 0x40) len++;   /* REX prefix */
    if (ip[len] == 0xF7 && (ip[len + 1] & 0xF8) == 0xF8) { ep->ContextRecord->Rip += len + 2; ep->ContextRecord->Rax = 42; ep->ContextRecord->Rdx = 0; return EXCEPTION_CONTINUE_EXECUTION; }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static int filter(unsigned code, EXCEPTION_POINTERS *ep, int *addr_ok) {
  EXCEPTION_RECORD *r = ep->ExceptionRecord;
  if (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) *addr_ok = r->ExceptionInformation[1] == 0x10 && r->ExceptionInformation[0] == 1 /* write */;
  if (code == EXCEPTION_ACCESS_VIOLATION && r->ExceptionAddress == (void *)ep->ContextRecord->Rip) *addr_ok += 2;
  return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep) {
  printf("top-level filter: code 0x%08lx\n", ep->ExceptionRecord->ExceptionCode); fflush(stdout);
  ExitProcess(77);
}

int main(void) {
  int addr_ok = 0, caught = 0;
  __try { do_write(g_bad, 1); printf("not reached\n"); }
  __except (filter(GetExceptionCode(), GetExceptionInformation(), &addr_ok)) { caught = 1; }
  printf("AV caught=%d addr_ok=%d\n", caught, addr_ok);

  caught = 0;
  __try { int q = do_div(100, g_zero); printf("not reached %d\n", q); }
  __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 2; }
  printf("div0 caught=%d\n", caught);

  caught = 0;
  __try { do_trap(); }
  __except (GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 3; }
  printf("ud2 caught=%d\n", caught);

  /* nested: inner __finally must run while unwinding to the outer __except */
  int fin = 0; caught = 0;
  __try { __try { do_write(g_bad, 2); } __finally { fin = 1; } }
  __except (EXCEPTION_EXECUTE_HANDLER) { caught = 4; }
  printf("nested AV caught=%d finally=%d\n", caught, fin);

  /* vectored handler fixes a divide by zero and continues execution */
  void *h = AddVectoredExceptionHandler(1, veh);
  volatile int r = do_div(7, g_zero);
  printf("veh calls=%d code=0x%08x result=%d\n", veh_calls, veh_last_code, r);
  RemoveVectoredExceptionHandler(h);

  /* 2000 faults in a loop (handler cost / stack hygiene) */
  int n = 0;
  for (int i = 0; i < 2000; i++) { __try { do_write(g_bad, i); } __except (EXCEPTION_EXECUTE_HANDLER) { n++; } }
  printf("loop caught=%d\n", n);

  SetUnhandledExceptionFilter(top_filter);
  do_write(g_bad, 3);   /* unhandled → top-level filter → ExitProcess(77) */
  printf("not reached\n");
  return 0;
}
