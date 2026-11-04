/* tpool.c — kernel32 thread-pool API: work items, periodic + one-shot timers, waits, simple callbacks, instance helpers */
#include <windows.h>
#include <stdio.h>
static volatile LONG work_runs, timer_ticks, simple_runs; static volatile LONG wait_result = -1; static volatile LONG wait_runs;
static volatile LONG sum;
static void CALLBACK work_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work) {
  (void)inst; (void)work; LONG v = (LONG)(ULONG_PTR)ctx; for (int i = 0; i < 1000; i++) InterlockedAdd(&sum, v); InterlockedIncrement(&work_runs);
}
static void CALLBACK timer_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_TIMER t) { (void)inst; (void)t; InterlockedIncrement((volatile LONG *)ctx); }
static void CALLBACK wait_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WAIT w, TP_WAIT_RESULT r) { (void)inst; (void)w; InterlockedExchange(&wait_result, (LONG)r); InterlockedIncrement(&wait_runs); SetEventWhenCallbackReturns(inst, (HANDLE)ctx); }
static void CALLBACK simple_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx) { (void)inst; InterlockedIncrement(&simple_runs); SetEventWhenCallbackReturns(inst, (HANDLE)ctx); }
int main(void) {
  int ok = 0, total = 0;
#define CHECK(cond, name) do { total++; if (cond) { ok++; printf("  ok   %s\n", name); } else printf("  FAIL %s\n", name); } while (0)
  /* 1. work: 8 submissions of 3 work objects, wait for all */
  PTP_WORK w1 = CreateThreadpoolWork(work_cb, (PVOID)1, NULL), w2 = CreateThreadpoolWork(work_cb, (PVOID)2, NULL), w3 = CreateThreadpoolWork(work_cb, (PVOID)3, NULL);
  CHECK(w1 && w2 && w3, "CreateThreadpoolWork");
  for (int i = 0; i < 8; i++) { SubmitThreadpoolWork(w1); SubmitThreadpoolWork(w2); SubmitThreadpoolWork(w3); }
  WaitForThreadpoolWorkCallbacks(w1, FALSE); WaitForThreadpoolWorkCallbacks(w2, FALSE); WaitForThreadpoolWorkCallbacks(w3, FALSE);
  printf("work runs=%ld sum=%ld\n", work_runs, sum);
  CHECK(work_runs == 24 && sum == 8 * 1000 * (1 + 2 + 3), "24 work callbacks ran, sum 48000");
  CloseThreadpoolWork(w1); CloseThreadpoolWork(w2); CloseThreadpoolWork(w3);
  /* 2. periodic timer: 20 ms period, run ~130 ms, expect >= 4 ticks; then cancel and verify it stops */
  volatile LONG ticks = 0; PTP_TIMER t = CreateThreadpoolTimer(timer_cb, (PVOID)&ticks, NULL);
  FILETIME due; LARGE_INTEGER li; li.QuadPart = -200000LL; /* 20 ms relative */ due.dwLowDateTime = li.LowPart; due.dwHighDateTime = (DWORD)li.HighPart;
  SetThreadpoolTimer(t, &due, 20, 0);
  CHECK(IsThreadpoolTimerSet(t), "IsThreadpoolTimerSet after SetThreadpoolTimer");
  Sleep(130);
  LONG before = ticks; printf("timer ticks after 130 ms = %ld\n", before);
  CHECK(before >= 4 && before <= 8, "periodic timer fired 4..8 times in 130 ms");
  SetThreadpoolTimer(t, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(t, TRUE);
  LONG at_cancel = ticks; Sleep(80);
  CHECK(ticks == at_cancel, "no ticks after cancel");
  CHECK(!IsThreadpoolTimerSet(t), "timer not set after cancel");
  /* one-shot timer */
  LONG one = 0; PTP_TIMER t2 = CreateThreadpoolTimer(timer_cb, (PVOID)&one, NULL); li.QuadPart = -100000LL; due.dwLowDateTime = li.LowPart; due.dwHighDateTime = (DWORD)li.HighPart;
  SetThreadpoolTimer(t2, &due, 0, 0); Sleep(60); WaitForThreadpoolTimerCallbacks(t2, FALSE);
  CHECK(one == 1, "one-shot timer fired exactly once");
  CloseThreadpoolTimer(t); CloseThreadpoolTimer(t2);
  /* 3. wait: event signalled after 30 ms → callback with WAIT_OBJECT_0; SetEventWhenCallbackReturns signals a second event */
  HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL), done = CreateEventW(NULL, TRUE, FALSE, NULL);
  PTP_WAIT pw = CreateThreadpoolWait(wait_cb, (PVOID)done, NULL); SetThreadpoolWait(pw, ev, NULL);
  Sleep(30); SetEvent(ev);
  DWORD r = WaitForSingleObject(done, 2000);
  CHECK(r == WAIT_OBJECT_0 && wait_result == WAIT_OBJECT_0 && wait_runs == 1, "wait callback ran with WAIT_OBJECT_0 and SetEventWhenCallbackReturns fired");
  /* wait with timeout */
  HANDLE ev2 = CreateEventW(NULL, TRUE, FALSE, NULL); ResetEvent(done); wait_result = -1;
  li.QuadPart = -300000LL; due.dwLowDateTime = li.LowPart; due.dwHighDateTime = (DWORD)li.HighPart;
  SetThreadpoolWait(pw, ev2, &due); r = WaitForSingleObject(done, 2000);
  CHECK(r == WAIT_OBJECT_0 && wait_result == WAIT_TIMEOUT, "wait callback got WAIT_TIMEOUT after 30 ms");
  WaitForThreadpoolWaitCallbacks(pw, FALSE); CloseThreadpoolWait(pw);
  /* 4. simple callback */
  ResetEvent(done); CHECK(TrySubmitThreadpoolCallback(simple_cb, (PVOID)done, NULL), "TrySubmitThreadpoolCallback");
  r = WaitForSingleObject(done, 2000); CHECK(r == WAIT_OBJECT_0 && simple_runs == 1, "simple callback ran");
  /* 5. pool + cleanup group objects */
  PTP_POOL pool = CreateThreadpool(NULL); CHECK(pool != NULL, "CreateThreadpool"); SetThreadpoolThreadMaximum(pool, 4); CHECK(SetThreadpoolThreadMinimum(pool, 1), "SetThreadpoolThreadMinimum");
  PTP_CLEANUP_GROUP cg = CreateThreadpoolCleanupGroup(); CHECK(cg != NULL, "CreateThreadpoolCleanupGroup"); CloseThreadpoolCleanupGroupMembers(cg, FALSE, NULL); CloseThreadpoolCleanupGroup(cg); CloseThreadpool(pool);
  printf("threadpool %d/%d\n", ok, total);
  return ok == total ? 0 : 1;
}
