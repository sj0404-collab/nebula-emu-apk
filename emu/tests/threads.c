/* Win32 threads + synchronisation conformance test for the Nebula x64 emulator.
   Build: zig cc -target x86_64-windows-gnu -O2 -o threads.exe threads.c
   Every check prints "ok N name" or "FAIL N name"; exit code = number of failures. */
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(cond, name) do { g_n++; if (cond) printf("ok %d %s\n", g_n, name); else { g_fail++; printf("FAIL %d %s\n", g_n, name); } fflush(stdout); } while (0)

/* 1. basic thread, argument, exit code, join */
static DWORD WINAPI t_basic(LPVOID p) { *(int *)p += 41; return 7; }
static void test_basic(void) {
  int v = 1; DWORD tid = 0;
  HANDLE h = CreateThread(NULL, 0, t_basic, &v, 0, &tid);
  CHECK(h != NULL, "CreateThread returns a handle");
  CHECK(tid != 0 && tid != GetCurrentThreadId(), "new thread id differs from main");
  CHECK(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0, "join via WaitForSingleObject");
  DWORD code = 0; GetExitCodeThread(h, &code);
  CHECK(code == 7, "GetExitCodeThread sees the return value");
  CHECK(v == 42, "thread wrote through its argument");
  CHECK(CloseHandle(h), "CloseHandle(thread)");
}

/* 2. many threads + unprotected counter vs Interlocked counter */
static volatile LONG g_atomic; static volatile long g_plain;
static DWORD WINAPI t_count(LPVOID p) { for (int i = 0; i < 20000; i++) { InterlockedIncrement(&g_atomic); g_plain++; if ((i & 1023) == 0) Sleep(0); } return (DWORD)(size_t)p; }
static void test_many(void) {
  HANDLE h[8]; g_atomic = 0; g_plain = 0;
  for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, t_count, (LPVOID)(size_t)(100 + i), 0, NULL);
  DWORD r = WaitForMultipleObjects(8, h, TRUE, INFINITE);
  CHECK(r == WAIT_OBJECT_0, "WaitForMultipleObjects(wait-all) over 8 threads");
  CHECK(g_atomic == 160000, "InterlockedIncrement from 8 threads == 160000");
  int codes_ok = 1; for (int i = 0; i < 8; i++) { DWORD c; GetExitCodeThread(h[i], &c); if (c != (DWORD)(100 + i)) codes_ok = 0; CloseHandle(h[i]); }
  CHECK(codes_ok, "each thread keeps its own exit code");
  printf("# plain counter = %ld (no ordering guarantee)\n", g_plain);
}

/* 3. critical section: mutual exclusion, recursion count, TryEnter from another thread */
static CRITICAL_SECTION g_cs; static int g_shared, g_max_inside, g_inside;
static DWORD WINAPI t_cs(LPVOID p) {
  for (int i = 0; i < 500; i++) { EnterCriticalSection(&g_cs); g_inside++; if (g_inside > g_max_inside) g_max_inside = g_inside; int s = g_shared; Sleep(0); g_shared = s + 1; g_inside--; LeaveCriticalSection(&g_cs); }
  return 0;
}
static volatile int g_try_result = -1;
static DWORD WINAPI t_try(LPVOID p) { g_try_result = TryEnterCriticalSection(&g_cs) ? 1 : 0; if (g_try_result) LeaveCriticalSection(&g_cs); return 0; }
static void test_cs(void) {
  InitializeCriticalSectionAndSpinCount(&g_cs, 4000);
  HANDLE h[4]; for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, t_cs, NULL, 0, NULL);
  WaitForMultipleObjects(4, h, TRUE, INFINITE); for (int i = 0; i < 4; i++) CloseHandle(h[i]);
  CHECK(g_shared == 2000 && g_max_inside == 1, "CRITICAL_SECTION gives mutual exclusion across Sleep(0)");
  EnterCriticalSection(&g_cs); EnterCriticalSection(&g_cs);
  CHECK(g_cs.RecursionCount == 2 && g_cs.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId(), "recursive EnterCriticalSection updates RecursionCount/OwningThread");
  HANDLE t = CreateThread(NULL, 0, t_try, NULL, 0, NULL); WaitForSingleObject(t, INFINITE); CloseHandle(t);
  CHECK(g_try_result == 0, "TryEnterCriticalSection fails while another thread owns it");
  LeaveCriticalSection(&g_cs); LeaveCriticalSection(&g_cs);
  CHECK(g_cs.LockCount == -1 && g_cs.OwningThread == 0, "fully released CS has LockCount -1");
  DeleteCriticalSection(&g_cs);
}

/* 4. events: manual-reset (set before wait), auto-reset single wake, timeout */
static HANDLE g_auto, g_manual; static volatile LONG g_woken;
static DWORD WINAPI t_waiter(LPVOID p) { if (WaitForSingleObject(g_auto, INFINITE) == WAIT_OBJECT_0) InterlockedIncrement(&g_woken); return 0; }
static void test_events(void) {
  g_manual = CreateEventA(NULL, TRUE, FALSE, NULL); g_auto = CreateEventA(NULL, FALSE, FALSE, NULL);
  CHECK(WaitForSingleObject(g_manual, 0) == WAIT_TIMEOUT, "unsignalled event: WaitForSingleObject(0) == WAIT_TIMEOUT");
  DWORD t0 = GetTickCount(); DWORD r = WaitForSingleObject(g_manual, 60); DWORD dt = GetTickCount() - t0;
  CHECK(r == WAIT_TIMEOUT && dt >= 40, "timed wait really waits (>= 40 ms) and times out");
  SetEvent(g_manual);
  CHECK(WaitForSingleObject(g_manual, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_manual, 0) == WAIT_OBJECT_0, "manual-reset event stays signalled");
  ResetEvent(g_manual);
  CHECK(WaitForSingleObject(g_manual, 0) == WAIT_TIMEOUT, "ResetEvent clears it");
  HANDLE h[3]; g_woken = 0; for (int i = 0; i < 3; i++) h[i] = CreateThread(NULL, 0, t_waiter, NULL, 0, NULL);
  Sleep(30);
  SetEvent(g_auto); Sleep(30);
  CHECK(g_woken == 1, "auto-reset event wakes exactly one of three waiters");
  SetEvent(g_auto); SetEvent(g_auto);
  CHECK(WaitForMultipleObjects(3, h, TRUE, 2000) == WAIT_OBJECT_0 && g_woken == 3, "remaining waiters wake on later SetEvents");
  for (int i = 0; i < 3; i++) CloseHandle(h[i]);
  CHECK(CloseHandle(g_auto) && CloseHandle(g_manual), "CloseHandle(event)");
}

/* 5. mutex + semaphore + WaitForMultipleObjects(wait-any) */
static HANDLE g_mtx, g_sem; static int g_mtx_shared;
static DWORD WINAPI t_mtx(LPVOID p) { for (int i = 0; i < 300; i++) { WaitForSingleObject(g_mtx, INFINITE); int s = g_mtx_shared; Sleep(0); g_mtx_shared = s + 1; ReleaseMutex(g_mtx); } return 0; }
static DWORD WINAPI t_sem(LPVOID p) { Sleep(20); ReleaseSemaphore(g_sem, 2, NULL); return 0; }
static void test_mutex_sem(void) {
  g_mtx = CreateMutexA(NULL, FALSE, NULL); g_sem = CreateSemaphoreA(NULL, 0, 10, NULL);
  HANDLE h[3]; for (int i = 0; i < 3; i++) h[i] = CreateThread(NULL, 0, t_mtx, NULL, 0, NULL);
  WaitForMultipleObjects(3, h, TRUE, INFINITE); for (int i = 0; i < 3; i++) CloseHandle(h[i]);
  CHECK(g_mtx_shared == 900, "mutex serialises 3 threads");
  WaitForSingleObject(g_mtx, INFINITE); WaitForSingleObject(g_mtx, INFINITE);
  CHECK(ReleaseMutex(g_mtx) && ReleaseMutex(g_mtx) && !ReleaseMutex(g_mtx), "mutex is recursive; extra ReleaseMutex fails");
  CHECK(WaitForSingleObject(g_sem, 0) == WAIT_TIMEOUT, "semaphore count 0 blocks");
  HANDLE t = CreateThread(NULL, 0, t_sem, NULL, 0, NULL);
  HANDLE both[2] = { g_sem, g_manual = CreateEventA(NULL, TRUE, FALSE, NULL) };
  DWORD r = WaitForMultipleObjects(2, both, FALSE, 2000);
  CHECK(r == WAIT_OBJECT_0, "wait-any returns the index of the semaphore released by another thread");
  CHECK(WaitForSingleObject(g_sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_sem, 0) == WAIT_TIMEOUT, "ReleaseSemaphore(2) leaves exactly one more count");
  WaitForSingleObject(t, INFINITE); CloseHandle(t); CloseHandle(g_mtx); CloseHandle(g_sem); CloseHandle(g_manual);
}

/* 6. TLS: dynamic (TlsAlloc) and static (__thread) isolation */
static DWORD g_tls; static __thread int g_static_tls = 5;
static DWORD WINAPI t_tls(LPVOID p) {
  int me = (int)(size_t)p; TlsSetValue(g_tls, (LPVOID)(size_t)(me * 10)); g_static_tls += me;
  for (int i = 0; i < 50; i++) Sleep(0);
  int a = (int)(size_t)TlsGetValue(g_tls) == me * 10, b = g_static_tls == 5 + me;
  return (DWORD)(a && b ? 1 : 0);
}
static void test_tls(void) {
  g_tls = TlsAlloc(); TlsSetValue(g_tls, (LPVOID)999); g_static_tls = 77;
  HANDLE h[6]; for (int i = 0; i < 6; i++) h[i] = CreateThread(NULL, 0, t_tls, (LPVOID)(size_t)(i + 1), 0, NULL);
  WaitForMultipleObjects(6, h, TRUE, INFINITE);
  int ok = 1; for (int i = 0; i < 6; i++) { DWORD c; GetExitCodeThread(h[i], &c); if (c != 1) ok = 0; CloseHandle(h[i]); }
  CHECK(ok, "TlsGetValue and __thread are per-thread");
  CHECK((size_t)TlsGetValue(g_tls) == 999 && g_static_tls == 77, "main thread TLS untouched");
  TlsFree(g_tls);
}

/* 7. SRW lock + condition variable producer/consumer */
static SRWLOCK g_srw = SRWLOCK_INIT; static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT; static int g_queue[64], g_qn, g_consumed, g_done_producing;
static DWORD WINAPI t_producer(LPVOID p) { for (int i = 1; i <= 200; i++) { AcquireSRWLockExclusive(&g_srw); while (g_qn == 64) { ReleaseSRWLockExclusive(&g_srw); Sleep(0); AcquireSRWLockExclusive(&g_srw); } g_queue[g_qn++] = i; ReleaseSRWLockExclusive(&g_srw); WakeConditionVariable(&g_cv); } AcquireSRWLockExclusive(&g_srw); g_done_producing = 1; ReleaseSRWLockExclusive(&g_srw); WakeAllConditionVariable(&g_cv); return 0; }
static DWORD WINAPI t_consumer(LPVOID p) { int sum = 0; for (;;) { AcquireSRWLockExclusive(&g_srw); while (g_qn == 0 && !g_done_producing) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0); if (g_qn == 0) { ReleaseSRWLockExclusive(&g_srw); break; } sum += g_queue[--g_qn]; g_consumed++; ReleaseSRWLockExclusive(&g_srw); } return (DWORD)sum; }
static DWORD WINAPI t_reader(LPVOID p) { AcquireSRWLockShared(&g_srw); InterlockedIncrement((LONG *)p); Sleep(20); ReleaseSRWLockShared(&g_srw); return 0; }
static void test_srw_cv(void) {
  HANDLE c1 = CreateThread(NULL, 0, t_consumer, NULL, 0, NULL), c2 = CreateThread(NULL, 0, t_consumer, NULL, 0, NULL);
  HANDLE pr = CreateThread(NULL, 0, t_producer, NULL, 0, NULL);
  HANDLE all[3] = { c1, c2, pr }; DWORD r = WaitForMultipleObjects(3, all, TRUE, 10000);
  DWORD s1, s2; GetExitCodeThread(c1, &s1); GetExitCodeThread(c2, &s2);
  CHECK(r == WAIT_OBJECT_0 && g_consumed == 200 && s1 + s2 == 200 * 201 / 2, "SRW + condition variable producer/consumer delivers every item once");
  CloseHandle(c1); CloseHandle(c2); CloseHandle(pr);
  LONG inside = 0; HANDLE rd[3]; for (int i = 0; i < 3; i++) rd[i] = CreateThread(NULL, 0, t_reader, &inside, 0, NULL);
  Sleep(5); AcquireSRWLockExclusive(&g_srw);
  CHECK(inside == 3, "exclusive acquire waits until all 3 shared holders release");
  ReleaseSRWLockExclusive(&g_srw); WaitForMultipleObjects(3, rd, TRUE, INFINITE); for (int i = 0; i < 3; i++) CloseHandle(rd[i]);
  CHECK(TryAcquireSRWLockExclusive(&g_srw) && !TryAcquireSRWLockShared(&g_srw), "TryAcquireSRWLockShared fails while held exclusively");
  ReleaseSRWLockExclusive(&g_srw);
}

/* 8. suspended start, ResumeThread, SuspendThread counts, thread handle wait with timeout */
static volatile int g_started;
static DWORD WINAPI t_suspended(LPVOID p) { g_started = 1; Sleep(80); return 3; }
static void test_suspend(void) {
  HANDLE h = CreateThread(NULL, 0, t_suspended, NULL, CREATE_SUSPENDED, NULL);
  Sleep(20);
  CHECK(g_started == 0, "CREATE_SUSPENDED thread does not run");
  CHECK(ResumeThread(h) == 1, "ResumeThread returns previous suspend count 1");
  CHECK(WaitForSingleObject(h, 10) == WAIT_TIMEOUT, "waiting on a running thread with a short timeout times out");
  DWORD code = 0; GetExitCodeThread(h, &code);
  CHECK(code == STILL_ACTIVE, "GetExitCodeThread on a running thread == STILL_ACTIVE");
  CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && g_started == 1, "thread finishes after resume");
  CloseHandle(h);
}

/* 9. _beginthreadex / _endthreadex, ExitThread from a nested call, GetThreadId */
static unsigned __stdcall t_crt(void *p) { if (p) _endthreadex(11); return 12; }
static void nested_exit(void) { ExitThread(13); }
static DWORD WINAPI t_exit(LPVOID p) { nested_exit(); return 99; }
static void test_crt(void) {
  unsigned tid1 = 0; HANDLE h1 = (HANDLE)_beginthreadex(NULL, 0, t_crt, (void *)1, 0, &tid1);
  HANDLE h2 = (HANDLE)_beginthreadex(NULL, 0, t_crt, NULL, 0, NULL);
  HANDLE h3 = CreateThread(NULL, 0, t_exit, NULL, 0, NULL);
  CHECK(h1 && h2 && h3 && GetThreadId(h1) == tid1, "_beginthreadex handles + GetThreadId");
  HANDLE hs[3] = { h1, h2, h3 }; WaitForMultipleObjects(3, hs, TRUE, INFINITE);
  DWORD c1, c2, c3; GetExitCodeThread(h1, &c1); GetExitCodeThread(h2, &c2); GetExitCodeThread(h3, &c3);
  CHECK(c1 == 11 && c2 == 12 && c3 == 13, "_endthreadex / return / ExitThread exit codes");
  CloseHandle(h1); CloseHandle(h2); CloseHandle(h3);
}

/* 10. WaitOnAddress / WakeByAddress, InterlockedCompareExchange hand-off */
static volatile LONG g_flag;
static DWORD WINAPI t_wake(LPVOID p) { Sleep(20); InterlockedExchange(&g_flag, 1); WakeByAddressAll((PVOID)&g_flag); return 0; }
static void test_waitonaddress(void) {
  g_flag = 0; HANDLE t = CreateThread(NULL, 0, t_wake, NULL, 0, NULL);
  LONG cmp = 0; DWORD t0 = GetTickCount(); while (g_flag == 0) WaitOnAddress(&g_flag, &cmp, sizeof cmp, INFINITE);
  CHECK(g_flag == 1 && GetTickCount() - t0 >= 10, "WaitOnAddress blocks until WakeByAddressAll");
  WaitForSingleObject(t, INFINITE); CloseHandle(t);
  LONG v = 5; CHECK(InterlockedCompareExchange(&v, 6, 5) == 5 && v == 6 && InterlockedCompareExchange(&v, 7, 5) == 6 && v == 6, "InterlockedCompareExchange semantics");
}

/* 11. process exit while worker threads are still running must terminate them */
static DWORD WINAPI t_forever(LPVOID p) { for (;;) Sleep(10); }

int main(int argc, char **argv) {
  printf("threads test, main tid=%lu\n", GetCurrentThreadId());
  test_basic(); test_many(); test_cs(); test_events(); test_mutex_sem(); test_tls(); test_srw_cv(); test_suspend(); test_crt(); test_waitonaddress();
  CreateThread(NULL, 0, t_forever, NULL, 0, NULL);
  printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail;   /* returning from main with a live worker thread: process must still exit */
}
