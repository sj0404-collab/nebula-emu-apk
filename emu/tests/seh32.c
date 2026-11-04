/* x86 (32-bit) structured exception handling through the fs:[0] registration chain, the way MSVC/Delphi/Inno/NSIS binaries use it:
 * hand-built EXCEPTION_REGISTRATION frames, RaiseException → handler → RtlUnwind (+ unwind notifications to inner frames) → longjmp,
 * access-violation fix-up by rewriting the CONTEXT (ExceptionContinueExecution), a vectored handler patching Eip/Eax, and the
 * top-level SetUnhandledExceptionFilter callback.
 * build: zig cc -target x86-windows-gnu -O1 -o seh32.exe seh32.c */
#include <windows.h>
#include <stdio.h>
#include <setjmp.h>

typedef struct _REG { struct _REG *prev; void *handler; jmp_buf *jb; int *counter; } REG;
#define PUSH_FRAME(r) __asm__ volatile("movl %%fs:0, %%eax\n\tmovl %%eax, (%0)\n\tmovl %0, %%fs:0" :: "r"(&(r)) : "eax", "memory")
#define POP_FRAME(r)  __asm__ volatile("movl (%0), %%eax\n\tmovl %%eax, %%fs:0" :: "r"(&(r)) : "eax", "memory")

static int checks = 0, failed = 0;
#define CHECK(cond, name) do { checks++; if (cond) printf("ok   %s\n", name); else { failed++; printf("FAIL %s\n", name); } fflush(stdout); } while (0)

static int unwind_notices = 0, inner_calls = 0;
static EXCEPTION_DISPOSITION __cdecl h_inner(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp) {
  (void)frame; (void)ctx; (void)disp;
  if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) { unwind_notices++; return ExceptionContinueSearch; }
  inner_calls++; return ExceptionContinueSearch;   /* not ours: keep searching */
}
static EXCEPTION_DISPOSITION __cdecl h_unwind(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp) {
  REG *r = (REG *)frame; (void)ctx; (void)disp;
  if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
  if (rec->ExceptionCode == 0xE0001234 && rec->NumberParameters == 2 && rec->ExceptionInformation[1] == 77) {
    RtlUnwind(frame, 0, rec, 0);   /* unwinds (and unlinks) the inner frame, leaves ours as the chain head */
    (*r->counter)++;
    longjmp(*r->jb, 7);
  }
  return ExceptionContinueSearch;
}
/* AV fix-up: resume at the setjmp point by rewriting the context from the jmp_buf (_JUMP_BUFFER: Ebp Ebx Edi Esi Esp Eip) */
static EXCEPTION_DISPOSITION __cdecl h_fix(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp) {
  REG *r = (REG *)frame; (void)disp;
  if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && rec->ExceptionInformation[0] == 1 && rec->ExceptionInformation[1] == 0x10 && rec->ExceptionAddress == (void *)ctx->Eip) {
    DWORD *jb = (DWORD *)*r->jb;
    ctx->Ebp = jb[0]; ctx->Ebx = jb[1]; ctx->Edi = jb[2]; ctx->Esi = jb[3]; ctx->Esp = jb[4]; ctx->Eip = jb[5]; ctx->Eax = 5;
    return ExceptionContinueExecution;
  }
  return ExceptionContinueSearch;
}
static volatile int *g_bad = (volatile int *)0x10;
static volatile int g_zero = 0;
__attribute__((noinline)) static void do_write(volatile int *p, int v) { *p = v; }
__attribute__((noinline)) static int do_div(int a, int b) { int q; __asm__ volatile("cdq\n\tidivl %2" : "=a"(q) : "a"(a), "r"(b) : "edx"); return q; }

static int veh_calls = 0;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
  veh_calls++;
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    unsigned char *ip = (unsigned char *)ep->ContextRecord->Eip;
    if (ip[0] == 0xF7 && (ip[1] & 0xF8) == 0xF8) { ep->ContextRecord->Eip += 2; ep->ContextRecord->Eax = 42; ep->ContextRecord->Edx = 0; return EXCEPTION_CONTINUE_EXECUTION; }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep) {
  printf("top-level filter: code 0x%08lx\n", ep->ExceptionRecord->ExceptionCode); fflush(stdout);
  printf("seh32: %d checks, %d failed\n", checks + 1, failed); fflush(stdout);
  ExitProcess(failed ? 1 : 0);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
  jmp_buf jb; int counter = 0; void *head0; __asm__ volatile("movl %%fs:0, %0" : "=r"(head0));
  /* 1. RaiseException → outer handler unwinds the inner frame and longjmps out */
  REG outer = { 0, (void *)h_unwind, &jb, &counter }, inner = { 0, (void *)h_inner, 0, 0 };
  int v = setjmp(jb);
  if (v == 0) {
    PUSH_FRAME(outer); PUSH_FRAME(inner);
    ULONG_PTR args[2] = { 1, 77 };
    RaiseException(0xE0001234, 0, 2, args);
    printf("FAIL RaiseException returned\n"); failed++;
  }
  CHECK(v == 7, "RaiseException -> handler -> RtlUnwind -> longjmp");
  CHECK(inner_calls == 1, "inner frame searched first");
  CHECK(unwind_notices == 1, "inner frame got the unwind notification");
  { void *head; __asm__ volatile("movl %%fs:0, %0" : "=r"(head)); CHECK(head == head0, "longjmp restored the chain head of the setjmp time (frames above it unlinked)"); }
  /* 2. access violation: handler rewrites the CONTEXT and continues execution */
  REG fix = { 0, (void *)h_fix, &jb, 0 };
  v = setjmp(jb);
  if (v == 0) { PUSH_FRAME(fix); do_write(g_bad, 1); printf("FAIL write to 0x10 succeeded\n"); failed++; }
  POP_FRAME(fix);
  CHECK(v == 5, "AV: handler modified Eip/Esp/Eax, ExceptionContinueExecution");
  /* 3. vectored handler: divide by zero patched to 42 */
  PVOID vh = AddVectoredExceptionHandler(1, veh);
  int q = do_div(10, g_zero);
  CHECK(q == 42 && veh_calls == 1, "vectored handler fixed the divide by zero");
  RemoveVectoredExceptionHandler(vh);
  /* 4. longjmp across frames (pure msvcrt longjmp, no exception) */
  REG dummy = { 0, (void *)h_inner, 0, 0 };
  v = setjmp(jb);
  if (v == 0) { PUSH_FRAME(dummy); longjmp(jb, 3); }
  CHECK(v == 3, "longjmp");
  { void *head; __asm__ volatile("movl %%fs:0, %0" : "=r"(head)); CHECK(head != &dummy, "longjmp unwound the frame registered after setjmp"); }
  /* 5. unhandled exception → top-level filter (prints the summary and exits) */
  SetUnhandledExceptionFilter(top_filter);
  RaiseException(0xE000BEEF, 0, 0, 0);
  printf("FAIL unhandled exception returned\n");
  return 9;
}
