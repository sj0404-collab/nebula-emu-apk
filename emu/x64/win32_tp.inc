/* win32_tp.inc — the Vista+ kernel32 thread-pool API (TP_WORK / TP_TIMER / TP_WAIT / simple callbacks, callback-instance
 * helpers, pools and cleanup groups).  concrt140, msvcp140_atomic_wait, the PPL, C++/WinRT and many game engines use it.
 *
 * Model: every callback invocation runs on its own detached helper thread (like QueueUserWorkItem here), started at the
 * dynamic shim slot nebula_tp_thunk with a *job* block as parameter.  The thunk waits (timer: until due; wait: on the
 * handle), then calls the callback with a per-invocation PTP_CALLBACK_INSTANCE; a CONT_TP continuation runs when the
 * callback returns: *WhenCallbackReturns actions, pending-count bookkeeping (WaitForThreadpool*Callbacks), periodic
 * timers re-arm by starting the next helper.  Not implemented: thread-pool I/O (CreateThreadpoolIo …) — those stop with
 * the API name, never hang silently.
 *
 * object (guest heap, 64 B): [0] magic [4] kind (1 work 2 timer 3 wait 4 simple) [8] callback [16] context [24] pending
 *   [28] cancel_pending [32] env [40] generation [44] armed [48] closed [52] period ms [56] running (callbacks executing now)
 * job (guest heap, 128 B): [0] obj [8] kind [16] generation [24] wait handle [32] due (host ms, double) [40] wait ms
 *   [44] phase [48] callback [56] context [64..112) PTP_CALLBACK_INSTANCE: [64] event [72] semaphore [80] sem count
 *   [88] mutex [96] critical section [104] hmodule   [112] period ms [116] wait result */
enum { CONT_TP = 120 };
#define TP_MAGIC 0x424F5054u
#define TP_WORK 1
#define TP_TIMER 2
#define TP_WAIT 3
#define TP_SIMPLE 4
static u64 tp_obj_of(Cpu *c, u64 a) { if (!a || !mem_ptr(c->mem, a, 64, 0) || rd32(c, a) != TP_MAGIC) return 0; return a; }
static u64 tp_create(Cpu *c, int kind, u64 cb, u64 ctx, u64 env) {
  u64 o = w32_heap_alloc(W, 64, 1); if (!o) { W->last_error = 8; return 0; }
  wr32(c, o, TP_MAGIC); wr32(c, o + 4, (u32)kind); wr64(c, o + 8, cb); wr64(c, o + 16, ctx); wr64(c, o + 32, env); return o;
}
static void tp_obj_release(Cpu *c, u64 o) { if (rd32(c, o + 48) && rd32(c, o + 24) == 0) { wr32(c, o, 0); w32_heap_free(W, o); } }   /* closed and no callbacks pending */
/* start one helper thread for a job; the object's pending count was already incremented by the caller */
static int tp_start_job(Cpu *c, u64 job) {
  u64 thunk = w32_shim_slot(W, "dynamic", "nebula_tp_thunk"); u32 tid = 0;
  u64 th = thread_create(c, thunk, job, 0, 0, &tid); if (!th) return 0;
  { int fd = handle_fd(th); vfs_close(fd); } return 1;
}
static u64 tp_new_job(Cpu *c, u64 o, int kind) {
  u64 j = w32_heap_alloc(W, 128, 1); if (!j) return 0;
  wr64(c, j, o); wr64(c, j + 8, (u64)kind); wr32(c, j + 16, rd32(c, o + 40)); wr64(c, j + 48, rd64(c, o + 8)); wr64(c, j + 56, rd64(c, o + 16)); wr32(c, j + 112, rd32(c, o + 52));
  return j;
}
static void tp_job_done(Cpu *c, u64 job) {   /* a job ends without running (cancelled / stale generation) */
  u64 o = rd64(c, job); if (o && rd32(c, o) == TP_MAGIC) { u32 p = rd32(c, o + 24); if (p) wr32(c, o + 24, p - 1); tp_obj_release(c, o); }
  w32_heap_free(W, job); W->wake_gen++;
}
/* short parking used by the polling waits (WaitForThreadpool*Callbacks): sleep up to `ms`, then the shim is re-run */
static void tp_park(Cpu *c, double ms) { Win32 *w = W; w->sleeping = 1; w->sleep_until_ms = host_now_ms() + ms; w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); }

/* helper-thread body: param = job */
static void s_nebula_tp_thunk(Cpu *c) {
  u64 job = ARG(0); Win32 *w = W; if (!job || !mem_ptr(c->mem, job, 128, 0)) { RET(0); return; }
  u64 o = rd64(c, job); int kind = (int)rd64(c, job + 8); u32 phase = rd32(c, job + 44);
  if (kind == TP_TIMER && phase == 0) {                                   /* sleep until due (re-executed after each wake-up) */
    u64 bits = rd64(c, job + 32); double due; e_memcpy(&due, &bits, 8); double now = host_now_ms();
    if (now < due) { w->sleeping = 1; w->sleep_until_ms = due; w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; }
    w->sleeping = 0; wr32(c, job + 44, 1);
  }
  if (kind == TP_WAIT && phase == 0) {
    do_wait(c, 1, 0, rd64(c, job + 24), 0, rd32(c, job + 40)); if (c->status == RUN_YIELD) return;   /* parked on the handle */
    u32 r = (u32)c->r[RAX]; wr32(c, job + 116, r == 0x102 ? 0x102 : r == 0xffffffffu ? 0xffffffffu : 0); wr32(c, job + 44, 1);
  }
  /* about to run: still wanted? (Close / cancel / a newer Set* on the same object) */
  int stale = !o || rd32(c, o) != TP_MAGIC || (kind != TP_SIMPLE && rd32(c, job + 16) != rd32(c, o + 40)) || (kind == TP_WORK && rd32(c, o + 28));
  if (stale) { tp_job_done(c, job); RET(0); return; }
  if (kind == TP_TIMER || kind == TP_WAIT) wr32(c, o + 44, kind == TP_TIMER && rd32(c, job + 112) ? 1 : 0);   /* one-shot timers and waits disarm when they fire */
  wr32(c, o + 56, rd32(c, o + 56) + 1);
  int i = cont_push(c, CONT_TP); if (i < 0) return; w->cont[i].data[0] = job;
  u64 cb = rd64(c, job + 48), ctx = rd64(c, job + 56), inst = job + 64;
  if (kind == TP_WORK) guest_call(c, cb, inst, ctx, o, 0);
  else if (kind == TP_TIMER) guest_call(c, cb, inst, ctx, o, 0);
  else if (kind == TP_WAIT) guest_call(c, cb, inst, ctx, o, rd32(c, job + 116));
  else guest_call(c, cb, inst, ctx, 0, 0);
}
/* callback returned */
static int tp_cont_resume(Cpu *c, int kind, u64 rax) {
  (void)rax; if (kind != CONT_TP) return 0;
  Win32 *w = W; int i = w->ncont - 1; u64 job = w->cont[i].data[0];
  u64 inst = job + 64;
  if (rd64(c, inst)) call_shim(c, s_SetEvent, rd64(c, inst), 0, 0, 0);
  if (rd64(c, inst + 8)) call_shim(c, s_ReleaseSemaphore, rd64(c, inst + 8), rd64(c, inst + 16), 0, 0);
  if (rd64(c, inst + 24)) call_shim(c, s_ReleaseMutex, rd64(c, inst + 24), 0, 0, 0);
  if (rd64(c, inst + 32)) call_shim(c, s_LeaveCriticalSection, rd64(c, inst + 32), 0, 0, 0);
  u64 o = rd64(c, job); int jk = (int)rd64(c, job + 8); u32 period = rd32(c, job + 112);
  if (o && rd32(c, o) == TP_MAGIC) { u32 r = rd32(c, o + 56); if (r) wr32(c, o + 56, r - 1); }
  if (jk == TP_TIMER && period && o && rd32(c, o) == TP_MAGIC && rd32(c, job + 16) == rd32(c, o + 40) && !rd32(c, o + 48)) {
    /* periodic timer: next helper, due += period (pending stays +1 for the chain) */
    u64 nj = tp_new_job(c, o, TP_TIMER); if (nj) { u64 bits = rd64(c, job + 32); double due; e_memcpy(&due, &bits, 8); due += period; e_memcpy(&bits, &due, 8); wr64(c, nj + 32, bits); wr32(c, nj + 16, rd32(c, job + 16)); if (tp_start_job(c, nj)) { w32_heap_free(w, job); w->wake_gen++; cont_finish(c, 0); return 1; } w32_heap_free(w, nj); }
  }
  tp_job_done(c, job);
  cont_finish(c, 0); return 1;   /* returns into the thread-start continuation → the helper thread exits */
}

/* ---- API ---- */
static void s_CreateThreadpoolWork(Cpu *c) { RET(tp_create(c, TP_WORK, ARG(0), ARG(1), ARG(2))); }
static void s_SubmitThreadpoolWork(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return;
  wr32(c, o + 28, 0); wr32(c, o + 24, rd32(c, o + 24) + 1); u64 j = tp_new_job(c, o, TP_WORK);
  if (!j || !tp_start_job(c, j)) { wr32(c, o + 24, rd32(c, o + 24) - 1); if (j) w32_heap_free(W, j); cpu_fault(c, "SubmitThreadpoolWork: no thread slot"); } }
static void s_TrySubmitThreadpoolCallback(Cpu *c) { u64 o = tp_create(c, TP_SIMPLE, ARG(0), ARG(1), ARG(2)); if (!o) { RET(0); return; }
  wr32(c, o + 48, 1); wr32(c, o + 24, 1); u64 j = tp_new_job(c, o, TP_SIMPLE); if (!j || !tp_start_job(c, j)) { if (j) w32_heap_free(W, j); w32_heap_free(W, o); W->last_error = 8; RET(0); return; } RET(1); }
/* WaitForThreadpool{Work,Timer,Wait}Callbacks(obj, fCancelPendingCallbacks): poll until nothing is pending */
static void s_WaitForThreadpoolWorkCallbacks(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); Win32 *w = W; w->sleeping = 0; if (!o) return;
  if ((u32)ARG(1)) wr32(c, o + 28, 1);
  if (rd32(c, (u32)ARG(1) ? o + 56 : o + 24) == 0) return;   /* cancel: only callbacks already running are waited for */
  tp_park(c, 1); }
static void s_CloseThreadpoolWork(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return; wr32(c, o + 28, 1); wr32(c, o + 40, rd32(c, o + 40) + 1); wr32(c, o + 48, 1); tp_obj_release(c, o); }

static void s_CreateThreadpoolTimer(Cpu *c) { RET(tp_create(c, TP_TIMER, ARG(0), ARG(1), ARG(2))); }
/* SetThreadpoolTimer(timer, PFILETIME due, period ms, window ms): due NULL = disarm; negative FILETIME = relative 100 ns units */
static void s_SetThreadpoolTimer(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return;
  wr32(c, o + 40, rd32(c, o + 40) + 1);                                   /* any previous schedule is stale */
  if (!ARG(1)) { wr32(c, o + 44, 0); return; }
  s64 ft = (s64)rd64(c, ARG(1)); double ms = ft <= 0 ? (double)(-ft) / 10000.0 : ((double)ft - (double)now_100ns()) / 10000.0; if (ms < 0) ms = 0;
  wr32(c, o + 52, (u32)ARG(2)); wr32(c, o + 44, 1); wr32(c, o + 24, rd32(c, o + 24) + 1);
  u64 j = tp_new_job(c, o, TP_TIMER); if (!j) { wr32(c, o + 24, rd32(c, o + 24) - 1); return; }
  double due = host_now_ms() + ms; u64 bits; e_memcpy(&bits, &due, 8); wr64(c, j + 32, bits);
  if (!tp_start_job(c, j)) { wr32(c, o + 24, rd32(c, o + 24) - 1); w32_heap_free(W, j); cpu_fault(c, "SetThreadpoolTimer: no thread slot"); } }
static void s_SetThreadpoolTimerEx(Cpu *c) { s_SetThreadpoolTimer(c); RET(0); }
static void s_IsThreadpoolTimerSet(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); RET(o ? rd32(c, o + 44) : 0); }
static void s_WaitForThreadpoolTimerCallbacks(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); Win32 *w = W; w->sleeping = 0; if (!o) return;
  if ((u32)ARG(1)) { wr32(c, o + 40, rd32(c, o + 40) + 1); wr32(c, o + 44, 0); }   /* cancel pending: scheduled-but-not-fired helpers become stale */
  if (rd32(c, (u32)ARG(1) ? o + 56 : o + 24) == 0) return;   /* cancel: sleeping helpers notice at their due time; only running callbacks are waited for */
  tp_park(c, 1); }
static void s_CloseThreadpoolTimer(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return; wr32(c, o + 40, rd32(c, o + 40) + 1); wr32(c, o + 44, 0); wr32(c, o + 48, 1); tp_obj_release(c, o); }

static void s_CreateThreadpoolWait(Cpu *c) { RET(tp_create(c, TP_WAIT, ARG(0), ARG(1), ARG(2))); }
/* SetThreadpoolWait(wait, handle, PFILETIME timeout): handle NULL = cancel */
static void s_SetThreadpoolWait(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return;
  wr32(c, o + 40, rd32(c, o + 40) + 1);
  if (!ARG(1)) { wr32(c, o + 44, 0); return; }
  int kind, idx; if (!wait_handle(c, ARG(1), &kind, &idx)) { W->last_error = 6; return; }
  u32 ms = 0xffffffffu; if (ARG(2)) { s64 ft = (s64)rd64(c, ARG(2)); double d = ft <= 0 ? (double)(-ft) / 10000.0 : ((double)ft - (double)now_100ns()) / 10000.0; if (ft == 0) ms = 0; else ms = d < 0 ? 0 : d > 4294967294.0 ? 0xfffffffeu : (u32)d; }
  wr32(c, o + 44, 1); wr32(c, o + 24, rd32(c, o + 24) + 1);
  u64 j = tp_new_job(c, o, TP_WAIT); if (!j) { wr32(c, o + 24, rd32(c, o + 24) - 1); return; }
  wr64(c, j + 24, ARG(1)); wr32(c, j + 40, ms);
  if (!tp_start_job(c, j)) { wr32(c, o + 24, rd32(c, o + 24) - 1); w32_heap_free(W, j); cpu_fault(c, "SetThreadpoolWait: no thread slot"); } }
static void s_SetThreadpoolWaitEx(Cpu *c) { s_SetThreadpoolWait(c); RET(1); }
static void s_WaitForThreadpoolWaitCallbacks(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); Win32 *w = W; w->sleeping = 0; if (!o) return;
  if ((u32)ARG(1)) { wr32(c, o + 40, rd32(c, o + 40) + 1); wr32(c, o + 44, 0); }
  if (rd32(c, (u32)ARG(1) ? o + 56 : o + 24) == 0) return;   /* cancel: a parked helper cannot be interrupted until its handle signals; only running callbacks are waited for */
  tp_park(c, 1); }
static void s_CloseThreadpoolWait(Cpu *c) { u64 o = tp_obj_of(c, ARG(0)); if (!o) return; wr32(c, o + 40, rd32(c, o + 40) + 1); wr32(c, o + 44, 0); wr32(c, o + 48, 1); tp_obj_release(c, o); }

/* callback-instance helpers (instance = job + 64) */
static u64 tp_inst(Cpu *c, u64 a) { return a && mem_ptr(c->mem, a, 48, 1) ? a : 0; }
static void s_SetEventWhenCallbackReturns(Cpu *c) { u64 i = tp_inst(c, ARG(0)); if (i) wr64(c, i, ARG(1)); }
static void s_ReleaseSemaphoreWhenCallbackReturns(Cpu *c) { u64 i = tp_inst(c, ARG(0)); if (i) { wr64(c, i + 8, ARG(1)); wr64(c, i + 16, ARG(2)); } }
static void s_ReleaseMutexWhenCallbackReturns(Cpu *c) { u64 i = tp_inst(c, ARG(0)); if (i) wr64(c, i + 24, ARG(1)); }
static void s_LeaveCriticalSectionWhenCallbackReturns(Cpu *c) { u64 i = tp_inst(c, ARG(0)); if (i) wr64(c, i + 32, ARG(1)); }
static void s_FreeLibraryWhenCallbackReturns(Cpu *c) { u64 i = tp_inst(c, ARG(0)); if (i) wr64(c, i + 40, ARG(1)); }   /* FreeLibrary is a no-op here anyway */
static void s_CallbackMayRunLong(Cpu *c) { RET(1); }
static void s_DisassociateCurrentThreadFromCallback(Cpu *c) { (void)c; }

/* pools, environments, cleanup groups: accepted and remembered, all callbacks run on helper threads regardless */
static void s_CreateThreadpool(Cpu *c) { u64 p = w32_heap_alloc(W, 32, 1); wr32(c, p, 0x4C4F4F50u); RET(p); }
static void s_CloseThreadpool(Cpu *c) { if (ARG(0) && mem_ptr(c->mem, ARG(0), 32, 0)) w32_heap_free(W, ARG(0)); }
static void s_SetThreadpoolThreadMaximum(Cpu *c) { (void)c; }
static void s_SetThreadpoolThreadMinimum(Cpu *c) { RET(1); }
static void s_SetThreadpoolStackInformation(Cpu *c) { RET(1); }
static void s_QueryThreadpoolStackInformation(Cpu *c) { if (ARG(1)) { wr64(c, ARG(1), 1 << 20); wr64(c, ARG(1) + 8, 1 << 20); } RET(1); }
static void s_CreateThreadpoolCleanupGroup(Cpu *c) { u64 g = w32_heap_alloc(W, 32, 1); wr32(c, g, 0x50524743u); RET(g); }
static void s_CloseThreadpoolCleanupGroupMembers(Cpu *c) { (void)c; }   /* members are closed individually by the program or leak at exit (documented) */
static void s_CloseThreadpoolCleanupGroup(Cpu *c) { if (ARG(0) && mem_ptr(c->mem, ARG(0), 32, 0)) w32_heap_free(W, ARG(0)); }
/* thread-pool I/O is not implemented: say so instead of hanging */
static void s_CreateThreadpoolIo(Cpu *c) { cpu_fault(c, "CreateThreadpoolIo: thread-pool I/O completion callbacks are not implemented"); }

#define TP_SHIMS \
  S(nebula_tp_thunk), S(CreateThreadpoolWork), S(SubmitThreadpoolWork), S(TrySubmitThreadpoolCallback), S(WaitForThreadpoolWorkCallbacks), S(CloseThreadpoolWork), \
  S(CreateThreadpoolTimer), S(SetThreadpoolTimer), S(SetThreadpoolTimerEx), S(IsThreadpoolTimerSet), S(WaitForThreadpoolTimerCallbacks), S(CloseThreadpoolTimer), \
  S(CreateThreadpoolWait), S(SetThreadpoolWait), S(SetThreadpoolWaitEx), S(WaitForThreadpoolWaitCallbacks), S(CloseThreadpoolWait), \
  S(SetEventWhenCallbackReturns), S(ReleaseSemaphoreWhenCallbackReturns), S(ReleaseMutexWhenCallbackReturns), S(LeaveCriticalSectionWhenCallbackReturns), S(FreeLibraryWhenCallbackReturns), S(CallbackMayRunLong), S(DisassociateCurrentThreadFromCallback), \
  S(CreateThreadpool), S(CloseThreadpool), S(SetThreadpoolThreadMaximum), S(SetThreadpoolThreadMinimum), S(SetThreadpoolStackInformation), S(QueryThreadpoolStackInformation), \
  S(CreateThreadpoolCleanupGroup), S(CloseThreadpoolCleanupGroupMembers), S(CloseThreadpoolCleanupGroup), S(CreateThreadpoolIo),
