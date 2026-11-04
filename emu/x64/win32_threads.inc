/* ---------- threads, kernel objects and synchronisation (included by win32.c after win32_fs.inc) ----------
   Model: every guest thread has its own Cpu, TEB, stack and static-TLS block; the interpreter runs ONE thread at a
   time and the scheduler (api.c) switches between (process, thread) pairs. A blocking call parks the thread
   (ctx.block = BLOCK_*) and is re-executed when the condition may hold, so waits are "retry until satisfied";
   a shim recognises its own retry through wait_active/wait_rip. Kernel objects (event/mutex/semaphore/thread)
   live in a global refcounted table (vfs.c) and are referenced from the per-process fd table (FD_KOBJ), so
   CloseHandle, DuplicateHandle and handle inheritance work exactly like for files and pipes. */
enum { CONT_TSTART = 100, CONT_TEXIT, CONT_ONCE };
#define TEB_SIZE 0x2000
#define THREAD_STACK_DEFAULT (1ull << 20)
#define WAIT_TIMEOUT_ 0x102
#define STILL_ACTIVE_ 259
static int g_next_tid = 8192;

static Thread *cur_thread(Win32 *w) { return &w->th[w->cur_th]; }
static u32 cur_tid(Win32 *w) { return (u32)w->th[w->cur_th].tid; }
static u64 gm_rd(Win32 *w, u64 a, int n) { const u8 *p = mem_ptr(w->mem, a, (u64)n, 0); if (!p) return 0; u64 v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* ---- switching ---- */
void w32_thread_sync(Win32 *w) { w->th[w->cur_th].ctx = w->tctx; }
static void thread_load(Win32 *w, int i) { w->tctx = w->th[i].ctx; w->cpu = w->th[i].cpu; w->cpu->w = w; w->cur_th = i; }
int w32_thread_count(Win32 *w) { int n = 0; for (int i = 0; i < THREAD_MAX; i++) if (w->th[i].used && w->th[i].state == 1) n++; return n; }

/* ---- wait bookkeeping (timed waits survive shim retries) ---- */
static double wait_begin(Cpu *c, u32 ms) { Win32 *w = W; if (w->wait_active && w->wait_rip == c->rip) return w->wait_deadline; w->wait_active = 1; w->wait_rip = c->rip; w->wait_deadline = ms == 0xffffffffu ? -1 : host_now_ms() + (double)ms; return w->wait_deadline; }
static void wait_end(Win32 *w) { w->wait_active = 0; w->block = BLOCK_NONE; }
static int deadline_passed(double dl) { return dl >= 0 && host_now_ms() >= dl; }
static void park(Cpu *c, int block) { W->block = block; retry_and_yield(c, g_cur_slot); }

/* ---- kernel objects ---- */
static int obj_signaled(int pid, int tid, int kind, int idx, int consume) {
  if (kind == 1) { int code; return proc_state(idx, &code) != 1; }
  if (kind == 3) return vfs_stdin_available();
  KObj *k = kobj_get(idx); if (!k) return 1;   /* object vanished: never hang on it */
  switch (k->kind) {
    case KO_THREAD: return k->state == 2;
    case KO_EVENT: if (!k->signaled) return 0; if (consume && !k->manual) k->signaled = 0; return 1;
    case KO_MUTEX: if (k->owner_tid && !(k->owner_pid == pid && k->owner_tid == tid)) return 0; if (consume) { k->owner_pid = pid; k->owner_tid = tid; k->recursion++; k->abandoned = 0; } return 1;
    case KO_SEM: if (k->count <= 0) return 0; if (consume) k->count--; return 1;
    case KO_IOCP: return iocp_has_packets(idx);
    case KO_TIMER: if (k->due_ms < 0 || host_now_ms() < k->due_ms) return 0; if (consume && !k->manual) { if (k->period_ms > 0) k->due_ms += k->period_ms; else k->due_ms = -1; } return 1;
  }
  return 1;
}
static int wait_check(int pid, int tid, const int *kinds, const int *idxs, int n, int all, int consume, int *which) {
  if (all) { for (int i = 0; i < n; i++) if (!obj_signaled(pid, tid, kinds[i], idxs[i], 0)) return 0; if (consume) for (int i = 0; i < n; i++) obj_signaled(pid, tid, kinds[i], idxs[i], 1); *which = 0; return 1; }
  for (int i = 0; i < n; i++) if (obj_signaled(pid, tid, kinds[i], idxs[i], consume)) { *which = i; return 1; }
  return 0;
}
/* HANDLE → waitable object: kind 1 = process (idx pid), kind 2 = kernel object (idx) */
static int wait_handle(Cpu *c, u64 h, int *kind, int *idx) {
  if (h == ~1ull) { *kind = 2; *idx = cur_thread(W)->kobj; return 1; }
  if (h == ~0ull) { *kind = 1; *idx = W->pid; return 1; }
  int fd = handle_fd(h); if (h == 0x10 || (fd >= 3 && vfs_resolve(fd) == -1)) { *kind = 3; *idx = 0; return 1; }   /* console input handle: signaled while input is pending (libuv's raw tty reader waits on it) */
  if (fd < 3) return 0;
  int pid = vfs_fd_pid(fd); if (pid >= 0) { *kind = 1; *idx = pid; return 1; }
  int k = vfs_fd_kobj(fd); if (k >= 0) { *kind = 2; *idx = k; return 1; }
  return 0;
}
static KObj *kobj_of_handle(Cpu *c, u64 h, int want_kind) { int kind, idx; if (!wait_handle(c, h, &kind, &idx) || kind != 2) return 0; KObj *k = kobj_get(idx); if (!k || (want_kind && k->kind != want_kind)) return 0; return k; }
static u64 new_kobj_handle(Cpu *c, int kind, const char *name, u64 sa, int *existed, KObj **out) {
  int ex = 0; int idx = kobj_new(kind, name, &ex); if (idx < 0) { W->last_error = 8; return 0; }
  int fd = vfs_kobj_handle(idx); if (fd < 0) { if (!ex) kobj_ref(idx, 0); W->last_error = 4; return 0; }
  vfs_set_inherit(fd, sa_inherit(c, sa)); W->last_error = ex ? 183 : 0; if (existed) *existed = ex; if (out) *out = kobj_get(idx);
  return H_OF(fd);
}
static u64 open_kobj_handle(Cpu *c, int kind, const char *name, int inherit) {
  if (!name || !name[0]) { W->last_error = 87; return 0; }
  for (int i = 0; i < KOBJ_MAX; i++) { KObj *k = kobj_get(i); if (k && k->kind == kind && !e_strcmp(k->name, name)) { int fd = vfs_kobj_handle(i); if (fd < 0) { W->last_error = 4; return 0; } vfs_set_inherit(fd, inherit); return H_OF(fd); } }
  W->last_error = 2; return 0;
}

static int net_select_ready(ThreadCtx *x);   /* win32_net.inc: any socket of a parked select()/WSAPoll() ready? */
/* ---- is a (possibly non-current) thread runnable? clears nothing; the retried shim completes the wait ---- */
static int ctx_runnable(Win32 *w, ThreadCtx *x, int tid) {
  switch (x->block) {
    case BLOCK_SELECT: return deadline_passed(x->wait_deadline) || net_select_ready(x);
    case BLOCK_NONE: return 1;
    case BLOCK_STDIN: return vfs_stdin_available() != 0;
    case BLOCK_PROC: { if (!x->nblock_pids) return 1; for (int i = 0; i < x->nblock_pids; i++) { int code; if (proc_state(x->block_pids[i], &code) != 1) return 1; } return 0; }
    case BLOCK_PIPE_R: return !vfs_pipe_would_block(x->block_pipe, 0);
    case BLOCK_PIPE_W: return !vfs_pipe_would_block(x->block_pipe, 1);
    case BLOCK_SLEEP: return !(x->sleeping && x->sleep_until_ms > host_now_ms());
    case BLOCK_OBJ: { if (x->wait_granted || deadline_passed(x->wait_deadline)) return 1; int which; return wait_check(w->pid, tid, x->wobj_kind, x->wobj_idx, x->nwobj, x->wait_all, 0, &which); }
    case BLOCK_CS: { if (deadline_passed(x->wait_deadline)) return 1; u64 owner = w->mode32 ? gm_rd(w, x->block_addr + 12, 4) : gm_rd(w, x->block_addr + 16, 8); return owner == 0 || owner == (u64)(u32)tid; }
    case BLOCK_SRW: { if (deadline_passed(x->wait_deadline)) return 1; u64 v = gm_rd(w, x->block_addr, w->mode32 ? 4 : 8); return x->block_flag ? v == 0 : !(v & 1); }
    case BLOCK_CV: return deadline_passed(x->wait_deadline) || gm_rd(w, x->block_addr, w->mode32 ? 4 : 8) != x->block_val;
    case BLOCK_ADDR: return deadline_passed(x->wait_deadline) || gm_rd(w, x->block_addr, x->block_flag) != x->block_val || w->wake_gen != x->wake_snap;
  }
  return 1;
}
int w32_thread_pick(Win32 *w) {
  w32_thread_sync(w);
  for (int k = 1; k <= THREAD_MAX; k++) {
    int i = (w->cur_th + k) % THREAD_MAX; Thread *t = &w->th[i];
    if (!t->used || t->state != 1 || t->suspend > 0) continue;
    if (ctx_runnable(w, &t->ctx, t->tid)) { t->ctx.block = BLOCK_NONE; thread_load(w, i); return 1; }
  }
  return 0;
}
void w32_threads_summary(Win32 *w, int *stdin_wait, int *sleeping, double *best_ms) {
  double now = host_now_ms();
  for (int i = 0; i < THREAD_MAX; i++) {
    Thread *t = &w->th[i]; if (!t->used || t->state != 1 || t->suspend > 0) continue;
    ThreadCtx *x = i == w->cur_th ? &w->tctx : &t->ctx; double rem = -1;
    if (x->block == BLOCK_NONE) rem = 0;
    else if (x->block == BLOCK_STDIN) *stdin_wait = 1;
    else if (x->block == BLOCK_SLEEP) { if (x->sleeping) { rem = x->sleep_until_ms - now; *sleeping = 1; } else rem = 0; }
    else if (x->block == BLOCK_SELECT) { rem = 2; if (x->wait_deadline >= 0 && x->wait_deadline - now < rem) rem = x->wait_deadline - now; *sleeping = 1; }   /* socket readiness comes from the host: poll every 2 ms */
    else if (x->block >= BLOCK_OBJ) {
      if (x->wait_deadline >= 0) { rem = x->wait_deadline - now; *sleeping = 1; }
      if (x->block == BLOCK_OBJ) for (int j = 0; j < x->nwobj; j++) { if (x->wobj_kind[j] == 3) *stdin_wait = 1; if (x->wobj_kind[j] != 2) continue; KObj *k = kobj_get(x->wobj_idx[j]); if (k && k->kind == KO_TIMER && k->due_ms >= 0) { double r2 = k->due_ms - now; if (r2 < 0) r2 = 0; if (rem < 0 || r2 < rem) rem = r2; *sleeping = 1; } }
    }
    if (rem >= 0) { if (rem < 0) rem = 0; if (*best_ms < 0 || rem < *best_ms) *best_ms = rem; }
  }
}

/* ---- thread life cycle ---- */
static u64 thread_create(Cpu *c, u64 start, u64 param, u64 stack_size, int suspended, u32 *tid_out) {
  Win32 *w = W; int k = -1; for (int i = 1; i < THREAD_MAX; i++) if (!w->th[i].used) { k = i; break; }
  if (k < 0) { W->last_error = 8; w32_report(w, "CreateThread: more than %d threads", THREAD_MAX - 1); return 0; }
  Thread *t = &w->th[k];
  u64 sz = THREAD_STACK_DEFAULT; if (stack_size > sz) { sz = (stack_size + 0xffff) & ~0xffffull; if (sz > (16ull << 20)) sz = 16ull << 20; }
  if (t->stack && t->stack_size != sz) { w32_heap_free(w, t->stack); t->stack = 0; }
  if (!t->stack) { t->stack = w32_heap_alloc(w, sz, 1); t->stack_size = sz; if (!t->stack) { W->last_error = 8; return 0; } }
  if (!t->teb) t->teb = shim_bump(w, TEB_SIZE);
  if (!t->teb) { W->last_error = 8; return 0; }
  { u8 *p = mem_ptr(w->mem, t->teb, TEB_SIZE, 1); if (p) e_memset(p, 0, TEB_SIZE); }
  int tid = g_next_tid; g_next_tid += 4;
  /* static TLS: a private copy of the template block, reachable through TEB+0x58 → array[_tls_index = 0] */
  u64 arr = w32_heap_alloc(w, 8 * MAX_TLS_IMAGES, 1); e_memset(t->tls_blk, 0, sizeof t->tls_blk);
  teb_init(w, c, t->teb, t->stack, t->stack + sz, (u32)w->pid, (u32)tid, arr); t->tls_arr = arr; pe_tls_thread_init(w, k);
  Cpu *tc = &w->tcpu[k]; cpu_reset(tc); tc->mem = w->mem; tc->w = w; tc->fs_base = tc->gs_base = t->teb; tc->trace = c->trace; tc->mode32 = (u8)w->mode32;
  tc->r[RSP] = t->stack + sz - 0x100; tc->rip = SLOT_ADDR(SLOT_STARTUP); cpu_push(tc, 0);
  t->cpu = tc; t->used = 1; t->state = 1; t->tid = tid; t->suspend = suspended ? 1 : 0; t->exit_code = STILL_ACTIVE_; t->start = start; t->param = param;
  e_memset(&t->ctx, 0, sizeof t->ctx); t->ctx.wait_deadline = -1;
  int idx = kobj_new(KO_THREAD, 0, 0); if (idx < 0) { t->used = 0; W->last_error = 8; return 0; }
  KObj *ko = kobj_get(idx); ko->pid = w->pid; ko->tid = tid; ko->state = 1; ko->exit_code = STILL_ACTIVE_; ko->refs = 1;   /* the thread's own reference */
  t->kobj = idx;
  int fd = vfs_kobj_handle(idx); if (fd < 0) { W->last_error = 4; return 0; }
  if (tid_out) *tid_out = (u32)tid;
  { char m[200]; e_snprintf(m, sizeof m, "[x64] thread %d created by %u: start=0x%llx param=0x%llx\n", tid, (unsigned)cur_tid(w), (unsigned long long)start, (unsigned long long)param); host_write(3, m, e_strlen(m)); }
  return H_OF(fd);
}
/* a thread is gone (returned, ExitThread, TerminateThread, process exit): publish its exit code, abandon its mutexes */
static void thread_mark_exited(Win32 *w, int k, u32 code) {
  Thread *t = &w->th[k]; if (!t->used || t->state == 2) return;
  t->state = 2; t->exit_code = code;
  KObj *ko = kobj_get(t->kobj); if (ko) { ko->state = 2; ko->exit_code = code; }
  for (int i = 0; i < KOBJ_MAX; i++) { KObj *m = kobj_get(i); if (m && m->kind == KO_MUTEX && m->owner_pid == w->pid && m->owner_tid == t->tid) { m->owner_tid = 0; m->owner_pid = 0; m->recursion = 0; m->abandoned = 1; } }
  if (k) for (int i = 0; i < MAX_TLS_IMAGES; i++) if (t->tls_blk[i]) { w32_heap_free(w, t->tls_blk[i]); t->tls_blk[i] = 0; }
  if (t->kobj >= 0) { kobj_ref(t->kobj, -1); t->kobj = -1; }
  if (k) t->used = 0;   /* slot (and its cached stack/TEB) reusable; the main thread keeps its slot */
}
static void thread_finish(Cpu *c, u32 code) {
  Win32 *w = W; int k = w->cur_th; thread_mark_exited(w, k, code);
  int alive = 0; for (int i = 0; i < THREAD_MAX; i++) if (w->th[i].used && w->th[i].state == 1) alive++;
  if (!alive) { do_exit(c, (int)code); return; }   /* last thread: the process ends with its exit code */
  w->block = BLOCK_NONE; c->status = RUN_YIELD;    /* the scheduler notices the current thread is dead and picks another */
}
void w32_proc_teardown(Win32 *w) { for (int i = 0; i < THREAD_MAX; i++) if (w->th[i].used && w->th[i].state == 1) thread_mark_exited(w, i, (u32)w->cpu->exit_code); if (w->ppid) user_proc_exit(w); }   /* child processes: drop their windows/classes (the root process keeps its screen until the emulator stops) */
int w32_thread_alive(Win32 *w) { Thread *t = cur_thread(w); return t->used && t->state == 1 && t->suspend <= 0; }
/* ExitThread / _endthreadex: DLL_THREAD_DETACH callbacks first, then finish */
static void thread_exit(Cpu *c, u32 code) {
  Win32 *w = W; if (!w->n_tls_cb) { thread_finish(c, code); return; }
  int i = cont_push(c, CONT_TEXIT); if (i < 0) return; w->cont[i].data[0] = code; w32_cont_resume(c);
}
/* called from w32_cont_resume for the thread-specific continuation kinds; returns 1 if handled */
static int thread_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int i = w->ncont - 1;
  if (kind == CONT_TSTART) {
    if (w->cont[i].idx < w->n_tls_cb) { u64 fn = w->tls_callbacks[w->cont[i].idx++]; guest_call(c, fn, w->image_base, 2 /* DLL_THREAD_ATTACH */, 0, 0); return 1; }
    if (w->cont[i].idx == w->n_tls_cb) { w->cont[i].idx++; guest_call(c, w->cont[i].data[0], w->cont[i].data[1], 0, 0, 0); return 1; }
    w->ncont--; thread_exit(c, (u32)rax); return 1;
  }
  if (kind == CONT_TEXIT) {
    if (w->cont[i].idx < w->n_tls_cb) { u64 fn = w->tls_callbacks[w->cont[i].idx++]; guest_call(c, fn, w->image_base, 3 /* DLL_THREAD_DETACH */, 0, 0); return 1; }
    w->ncont--; thread_finish(c, (u32)w->cont[i].data[0]); return 1;
  }
  if (kind == CONT_ONCE) { wr64(c, w->cont[i].data[0], (u32)rax ? 1 : 0); w->wake_gen++; cont_finish(c, rax); return 1; }
  return 0;
}
/* first instruction of a new thread lands in the start-up slot: run TLS callbacks, then start(param) */
static void thread_startup(Cpu *c) { Win32 *w = W; Thread *t = cur_thread(w); int i = cont_push(c, CONT_TSTART); if (i < 0) return; w->cont[i].data[0] = t->start; w->cont[i].data[1] = t->param; w32_cont_resume(c); }
static Thread *thread_of_handle(Cpu *c, u64 h) { KObj *k = kobj_of_handle(c, h, KO_THREAD); if (!k || k->pid != W->pid) return 0; for (int i = 0; i < THREAD_MAX; i++) if (W->th[i].used && W->th[i].tid == k->tid) return &W->th[i]; return 0; }

u64 w32_thread_create_api(Cpu *c, u64 start, u64 param, u64 stack, int suspended, u32 *tid) { return thread_create(c, start, param, stack, suspended, tid); }

/* ================= shims ================= */
static void s_GetCurrentThread(Cpu *c) { RET(~1ull); }
static void s_GetCurrentThreadId(Cpu *c) { RET(cur_tid(W)); }
static void s_CreateThread(Cpu *c) { u32 tid = 0; u64 h = thread_create(c, ARG(2), ARG(3), ARG(1), (u32)ARG(4) & 4, &tid); if (h && ARG(5)) wr32(c, ARG(5), tid); if (h) { int fd = handle_fd(h); vfs_set_inherit(fd, sa_inherit(c, ARG(0))); } RET(h); }
static void s_beginthreadex(Cpu *c) { u32 tid = 0; u64 h = thread_create(c, ARG(2), ARG(3), ARG(1), (u32)ARG(4) & 4, &tid); if (h && ARG(5)) wr32(c, ARG(5), tid); if (!h) set_errno(c, 11); RET(h); }
static void s_beginthread(Cpu *c) { u64 h = thread_create(c, ARG(0), ARG(2), ARG(1), 0, 0); if (!h) { set_errno(c, 11); RET(~0ull); return; } RET(h); }
static void s_QueueUserWorkItem(Cpu *c) { u32 tid = 0; u64 h = thread_create(c, ARG(0), ARG(1), 0, 0, &tid); if (h) { int fd = handle_fd(h); vfs_close(fd); } RET(h ? 1 : 0); }   /* thread-pool work item = a detached thread */
static void s_ExitThread(Cpu *c) { thread_exit(c, (u32)ARG(0)); }
static void s_FreeLibraryAndExitThread(Cpu *c) { thread_exit(c, (u32)ARG(1)); }
static void s_endthreadex(Cpu *c) { thread_exit(c, (u32)ARG(0)); }
static void s_endthread(Cpu *c) { thread_exit(c, 0); }
static void s_GetExitCodeThread(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_THREAD); if (!k) { W->last_error = 6; RET(0); return; } wr32(c, ARG(1), k->state == 2 ? k->exit_code : STILL_ACTIVE_); RET(1); }
static void s_GetThreadId(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_THREAD); if (!k) { W->last_error = 6; RET(0); return; } RET((u32)k->tid); }
static void s_GetProcessIdOfThread(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_THREAD); if (!k) { W->last_error = 6; RET(0); return; } RET((u32)k->pid); }
static void s_OpenThread(Cpu *c) { u32 tid = (u32)ARG(2); for (int i = 0; i < KOBJ_MAX; i++) { KObj *k = kobj_get(i); if (k && k->kind == KO_THREAD && (u32)k->tid == tid) { int fd = vfs_kobj_handle(i); if (fd < 0) break; vfs_set_inherit(fd, (u32)ARG(1) != 0); RET(H_OF(fd)); return; } } W->last_error = 87; RET(0); }
static void s_ResumeThread(Cpu *c) { Thread *t = thread_of_handle(c, ARG(0)); if (!t) { W->last_error = 6; RET(~0ull); return; } int prev = t->suspend; if (t->suspend > 0) t->suspend--; RET((u32)prev); }
static void s_SuspendThread(Cpu *c) { Thread *t = ARG(0) == ~1ull ? cur_thread(W) : thread_of_handle(c, ARG(0)); if (!t) { W->last_error = 6; RET(~0ull); return; } int prev = t->suspend++; RET((u32)prev); if (t == cur_thread(W)) { W->block = BLOCK_NONE; c->status = RUN_YIELD; } }
static void s_TerminateThread(Cpu *c) { Thread *t = ARG(0) == ~1ull ? cur_thread(W) : thread_of_handle(c, ARG(0)); if (!t) { W->last_error = 6; RET(0); return; } if (t == cur_thread(W)) { thread_finish(c, (u32)ARG(1)); return; } thread_mark_exited(W, (int)(t - W->th), (u32)ARG(1)); RET(1); }
static void s_SwitchToThread(Cpu *c) { call_shim(c, s_Sleep, 0, 0, 0, 0); if (c->status != RUN_YIELD) RET(1); }
static void s_SetThreadDescription(Cpu *c) { RET(0); }
static void s_GetThreadDescription(Cpu *c) { RET(0x80070057u); }
static void s_GetCurrentThreadStackLimits(Cpu *c) { Thread *t = cur_thread(W); u64 lo = t->stack ? t->stack : G_STACK_BASE, hi = t->stack ? t->stack + t->stack_size : G_STACK_BASE + G_STACK_SIZE; if (ARG(0)) wr64(c, ARG(0), lo); if (ARG(1)) wr64(c, ARG(1), hi); }
static void s_SetThreadStackGuarantee(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 0); RET(1); }

/* TLS: slot bitmap is per process, values live in each thread's TEB (TlsSlots at +0x1480) */
static void s_TlsAlloc(Cpu *c) { Win32 *w = W; for (u32 i = 1; i < 64; i++) if (!w->tls_slots[i]) { w->tls_slots[i] = 1; for (int t = 0; t < THREAD_MAX; t++) if (w->th[t].used) wrp(c, tls_slot_addr(c, w->th[t].teb, i), 0); RET(i); return; } W->last_error = 8; RET(0xffffffffu); }
static void s_TlsFree(Cpu *c) { u32 i = (u32)ARG(0); if (i < 64) W->tls_slots[i] = 0; RET(1); }
static void s_TlsGetValue(Cpu *c) { u32 i = (u32)ARG(0); W->last_error = 0; RET(i < 64 ? rdp(c, tls_slot_addr(c, c->gs_base, i)) : 0); }
static void s_TlsSetValue(Cpu *c) { u32 i = (u32)ARG(0); if (i >= 64) { W->last_error = 87; RET(0); return; } wrp(c, tls_slot_addr(c, c->gs_base, i), ARG(1)); RET(1); }
static void s_FlsAlloc(Cpu *c) { s_TlsAlloc(c); }
static void s_FlsFree(Cpu *c) { s_TlsFree(c); }
static void s_FlsGetValue(Cpu *c) { s_TlsGetValue(c); }
static void s_FlsSetValue(Cpu *c) { s_TlsSetValue(c); }

/* CRITICAL_SECTION on the real RTL_CRITICAL_SECTION layout: LockCount +8 (-1 free), RecursionCount +12, OwningThread +16, SpinCount +32 */
/* CRITICAL_SECTION field offsets: x64 LockCount@8 RecursionCount@12 OwningThread@16 SpinCount@32 (40 bytes); 32-bit @4 @8 @12 @20 (24 bytes) */
#define CS_LOCK (c->mode32 ? 4 : 8)
#define CS_REC (c->mode32 ? 8 : 12)
#define CS_OWNER (c->mode32 ? 12 : 16)
#define CS_SPIN (c->mode32 ? 20 : 32)
static void cs_init(Cpu *c, u64 a) { u32 sz = c->mode32 ? 24 : 40; u8 *p = mem_ptr(c->mem, a, sz, 1); if (p) { e_memset(p, 0, sz); wr32(c, a + CS_LOCK, (u32)-1); } }
static void s_InitializeCriticalSection(Cpu *c) { cs_init(c, ARG(0)); }
static void s_InitializeCriticalSectionAndSpinCount(Cpu *c) { cs_init(c, ARG(0)); wrp(c, ARG(0) + CS_SPIN, ARG(1)); RET(1); }
static void s_InitializeCriticalSectionEx(Cpu *c) { cs_init(c, ARG(0)); wrp(c, ARG(0) + CS_SPIN, ARG(1)); RET(1); }
static void s_DeleteCriticalSection(Cpu *c) { cs_init(c, ARG(0)); }
static void s_SetCriticalSectionSpinCount(Cpu *c) { u64 a = ARG(0); u64 prev = rdp(c, a + CS_SPIN); wrp(c, a + CS_SPIN, ARG(1)); RET((u32)prev); }
static int cs_try(Cpu *c, u64 a) {
  u64 owner = rdp(c, a + CS_OWNER); u32 me = cur_tid(W);
  if (owner == me) { wr32(c, a + CS_REC, rd32(c, a + CS_REC) + 1); wr32(c, a + CS_LOCK, rd32(c, a + CS_LOCK) + 1); return 1; }
  if (owner == 0) { wrp(c, a + CS_OWNER, me); wr32(c, a + CS_REC, 1); wr32(c, a + CS_LOCK, 0); return 1; }
  return 0;
}
static void s_EnterCriticalSection(Cpu *c) { u64 a = ARG(0); if (cs_try(c, a)) { wait_end(W); return; } W->block_addr = a; W->wait_deadline = -1; park(c, BLOCK_CS); }
static void s_TryEnterCriticalSection(Cpu *c) { RET(cs_try(c, ARG(0)) ? 1 : 0); }
static void s_LeaveCriticalSection(Cpu *c) { u64 a = ARG(0); if (rdp(c, a + CS_OWNER) != cur_tid(W)) return; int rec = (int)rd32(c, a + CS_REC) - 1; if (rec <= 0) { wr32(c, a + CS_REC, 0); wrp(c, a + CS_OWNER, 0); wr32(c, a + CS_LOCK, (u32)-1); } else { wr32(c, a + CS_REC, (u32)rec); wr32(c, a + CS_LOCK, rd32(c, a + CS_LOCK) - 1); } }

/* SRW lock: one 64-bit word — bit 0 exclusive, shared holders counted from bit 4 */
static void s_InitializeSRWLock(Cpu *c) { wrp(c, ARG(0), 0); }
static void s_AcquireSRWLockExclusive(Cpu *c) { u64 a = ARG(0); if (rdp(c, a) == 0) { wrp(c, a, 1); wait_end(W); return; } W->block_addr = a; W->block_flag = 1; W->wait_deadline = -1; park(c, BLOCK_SRW); }
static void s_AcquireSRWLockShared(Cpu *c) { u64 a = ARG(0); u64 v = rdp(c, a); if (!(v & 1)) { wrp(c, a, v + 16); wait_end(W); return; } W->block_addr = a; W->block_flag = 0; W->wait_deadline = -1; park(c, BLOCK_SRW); }
static void s_ReleaseSRWLockExclusive(Cpu *c) { wrp(c, ARG(0), 0); }
static void s_ReleaseSRWLockShared(Cpu *c) { u64 a = ARG(0); u64 v = rdp(c, a); wrp(c, a, v >= 16 ? v - 16 : 0); }
static void s_TryAcquireSRWLockExclusive(Cpu *c) { u64 a = ARG(0); if (rdp(c, a) == 0) { wrp(c, a, 1); RET(1); } else RET(0); }
static void s_TryAcquireSRWLockShared(Cpu *c) { u64 a = ARG(0); u64 v = rdp(c, a); if (!(v & 1)) { wrp(c, a, v + 16); RET(1); } else RET(0); }

/* condition variables: the word is a generation counter; sleepers wake when it changes (spurious wake-ups are allowed by the API) */
static void s_InitializeConditionVariable(Cpu *c) { wrp(c, ARG(0), 0); }
static void s_WakeConditionVariable(Cpu *c) { u64 a = ARG(0); wrp(c, a, rdp(c, a) + 1); }
static void s_WakeAllConditionVariable(Cpu *c) { s_WakeConditionVariable(c); }
/* lock_kind 0: critical section, 1: SRW exclusive, 2: SRW shared */
static void cv_sleep(Cpu *c, u64 cv, u64 lock, u32 ms, int lock_kind) {
  Win32 *w = W; int retry = w->wait_active && w->wait_rip == c->rip;
  if (!retry) {
    w->block_val = rdp(c, cv); w->block_addr = cv; wait_begin(c, ms); w->wake_snap = 0;   /* wake_snap: 0 sleeping, 1 woken, 2 timed out */
    if (lock_kind == 0) { wr32(c, lock + CS_REC, 0); wrp(c, lock + CS_OWNER, 0); wr32(c, lock + CS_LOCK, (u32)-1); }
    else if (lock_kind == 1) wrp(c, lock, 0); else { u64 v = rdp(c, lock); wrp(c, lock, v >= 16 ? v - 16 : 0); }
    park(c, BLOCK_CV); return;
  }
  if (w->wake_snap == 0) {
    int woken = rdp(c, cv) != w->block_val, expired = deadline_passed(w->wait_deadline);
    if (!woken && !expired) { w->block_addr = cv; park(c, BLOCK_CV); return; }
    w->wake_snap = woken ? 1 : 2; w->wait_deadline = -1;
  }
  /* re-acquire the lock before returning (may park again on the lock; the retry comes back here) */
  if (lock_kind == 0) { if (!cs_try(c, lock)) { w->block_addr = lock; w->block_flag = 0; park(c, BLOCK_CS); return; } }
  else if (lock_kind == 1) { if (rdp(c, lock) != 0) { w->block_addr = lock; w->block_flag = 1; park(c, BLOCK_SRW); return; } wrp(c, lock, 1); }
  else { u64 v = rdp(c, lock); if (v & 1) { w->block_addr = lock; w->block_flag = 0; park(c, BLOCK_SRW); return; } wrp(c, lock, v + 16); }
  int woken = w->wake_snap == 1; wait_end(w);
  if (woken) RET(1); else { w->last_error = 258; RET(0); }
}
static void s_SleepConditionVariableCS(Cpu *c) { cv_sleep(c, ARG(0), ARG(1), (u32)ARG(2), 0); }
static void s_SleepConditionVariableSRW(Cpu *c) { cv_sleep(c, ARG(0), ARG(1), (u32)ARG(2), (ARG(3) & 1) ? 2 : 1); }

/* one-time initialisation: 0 untouched, 2 in progress (others wait), 1 done */
static void s_InitOnceExecuteOnce(Cpu *c) {
  u64 once = ARG(0); u64 v = rdp(c, once);
  if (v == 1) { RET(1); return; }
  if (v == 2) { W->block_addr = once; W->block_val = 2; W->block_flag = PSZ; W->wake_snap = W->wake_gen; W->wait_deadline = -1; park(c, BLOCK_ADDR); return; }
  wrp(c, once, 2); int i = cont_push(c, CONT_ONCE); if (i < 0) return; W->cont[i].data[0] = once; guest_call(c, ARG(1), once, ARG(2), ARG(3), 0);
}
static void s_InitOnceBeginInitialize(Cpu *c) {
  u64 once = ARG(0); u64 v = rdp(c, once); u32 flags = (u32)ARG(1);
  if (v == 1) { if (ARG(2)) wr32(c, ARG(2), 0); RET(1); return; }
  if (flags & 1) { RET(0); return; }   /* INIT_ONCE_CHECK_ONLY */
  if (v == 2) { W->block_addr = once; W->block_val = 2; W->block_flag = PSZ; W->wake_snap = W->wake_gen; W->wait_deadline = -1; park(c, BLOCK_ADDR); return; }
  wrp(c, once, 2); if (ARG(2)) wr32(c, ARG(2), 1); RET(1);
}
static void s_InitOnceComplete(Cpu *c) { wrp(c, ARG(0), ((u32)ARG(1) & 4) ? 0 : 1); W->wake_gen++; RET(1); }
static void s_InitOnceInitialize(Cpu *c) { wrp(c, ARG(0), 0); }

/* WaitOnAddress / WakeByAddress */
static void s_WaitOnAddress(Cpu *c) {
  Win32 *w = W; u64 addr = ARG(0), cmp = ARG(1); int n = (int)ARG(2); u32 ms = (u32)ARG(3); if (n != 1 && n != 2 && n != 4 && n != 8) { w->last_error = 87; RET(0); return; }
  u64 cur = gm_rd(w, addr, n), want = gm_rd(w, cmp, n);
  int retry = w->wait_active && w->wait_rip == c->rip;
  if (cur != want || (retry && w->wake_gen != w->wake_snap)) { wait_end(w); RET(1); return; }
  double dl = wait_begin(c, ms);
  if (deadline_passed(dl)) { wait_end(w); w->last_error = 1460; RET(0); return; }
  if (!retry) w->wake_snap = w->wake_gen;
  w->block_addr = addr; w->block_val = cur; w->block_flag = n; park(c, BLOCK_ADDR);
}
static void s_WakeByAddressSingle(Cpu *c) { W->wake_gen++; }
static void s_WakeByAddressAll(Cpu *c) { W->wake_gen++; }

/* events / mutexes / semaphores */
static void s_CreateEventA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, ARG(3) ? name_a(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = (u32)ARG(1) != 0; k->signaled = (u32)ARG(2) != 0; } RET(h); }
static void s_CreateEventW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, ARG(3) ? name_w(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = (u32)ARG(1) != 0; k->signaled = (u32)ARG(2) != 0; } RET(h); }
static void s_CreateEventExW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, ARG(1) ? name_w(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = ((u32)ARG(2) & 1) != 0; k->signaled = ((u32)ARG(2) & 2) != 0; } RET(h); }
static void s_CreateEventExA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, ARG(1) ? name_a(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = ((u32)ARG(2) & 1) != 0; k->signaled = ((u32)ARG(2) & 2) != 0; } RET(h); }
/* waitable timers: CreateWaitableTimer(Ex)(sa, name, flags/manual, access); SetWaitableTimer(h, due(100ns, <0 relative), period ms, apc, ctx, resume) */
static void s_CreateWaitableTimerW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_TIMER, ARG(2) ? name_w(c, ARG(2)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = (u32)ARG(1) != 0; k->due_ms = -1; k->period_ms = 0; } RET(h); }
static void s_CreateWaitableTimerA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_TIMER, ARG(2) ? name_a(c, ARG(2)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = (u32)ARG(1) != 0; k->due_ms = -1; k->period_ms = 0; } RET(h); }
static void s_CreateWaitableTimerExW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_TIMER, ARG(1) ? name_w(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = ((u32)ARG(2) & 1) != 0; k->due_ms = -1; k->period_ms = 0; } RET(h); }
static void s_CreateWaitableTimerExA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_TIMER, ARG(1) ? name_a(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->manual = ((u32)ARG(2) & 1) != 0; k->due_ms = -1; k->period_ms = 0; } RET(h); }
static void s_SetWaitableTimer(Cpu *c) {
  KObj *k = kobj_of_handle(c, ARG(0), KO_TIMER); if (!k) { W->last_error = 6; RET(0); return; }
  s64 due = (s64)rd64(c, ARG(1)); double now = host_now_ms();
  if (due <= 0) k->due_ms = now + (double)(-due) / 10000.0; else { double ft_now = (double)(u64)vfs_filetime(); k->due_ms = now + (((double)due - ft_now) / 10000.0 > 0 ? ((double)due - ft_now) / 10000.0 : 0); }
  k->period_ms = (int)(u32)ARG(2); RET(1);
}
static void s_SetWaitableTimerEx(Cpu *c) { s_SetWaitableTimer(c); }
static void s_CancelWaitableTimer(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_TIMER); if (!k) { W->last_error = 6; RET(0); return; } k->due_ms = -1; RET(1); }
static void s_OpenEventA(Cpu *c) { RET(open_kobj_handle(c, KO_EVENT, name_a(c, ARG(2)), (u32)ARG(1) != 0)); }
static void s_OpenEventW(Cpu *c) { RET(open_kobj_handle(c, KO_EVENT, name_w(c, ARG(2)), (u32)ARG(1) != 0)); }
/* auto-reset event with waiters: hand the signal straight to one wait-any waiter (Windows wakes synchronously, so two
   back-to-back SetEvents release two waiters); returns the number of waiters served (0 or 1, or all for a pulse) */
static int event_deliver(Win32 *self, int idx, int all) {
  int served = 0;
  for (int pi = 0; ; pi++) {
    Win32 *w = proc_w32_at(pi); if (!w) break; if (w == (Win32 *)1) continue;
    for (int i = 0; i < THREAD_MAX; i++) {
      Thread *t = &w->th[i]; if (!t->used || t->state != 1) continue;
      if (w == self && i == w->cur_th) continue;
      ThreadCtx *x = i == w->cur_th ? &w->tctx : &t->ctx;
      if (x->block != BLOCK_OBJ || x->wait_all || x->wait_granted) continue;
      for (int j = 0; j < x->nwobj; j++) if (x->wobj_kind[j] == 2 && x->wobj_idx[j] == idx) { x->wait_granted = j + 1; served++; if (!all) return served; break; }
    }
  }
  return served;
}
static void s_SetEvent(Cpu *c) { int kind, idx; if (!wait_handle(c, ARG(0), &kind, &idx) || kind != 2) { W->last_error = 6; RET(0); return; } KObj *k = kobj_get(idx); if (!k || k->kind != KO_EVENT) { W->last_error = 6; RET(0); return; } if (k->manual || !event_deliver(W, idx, 0)) k->signaled = 1; RET(1); }
static void s_PulseEvent(Cpu *c) { int kind, idx; if (!wait_handle(c, ARG(0), &kind, &idx) || kind != 2) { W->last_error = 6; RET(0); return; } KObj *k = kobj_get(idx); if (!k || k->kind != KO_EVENT) { W->last_error = 6; RET(0); return; } event_deliver(W, idx, k->manual); k->signaled = 0; RET(1); }
static void s_ResetEvent(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_EVENT); if (!k) { W->last_error = 6; RET(0); return; } k->signaled = 0; RET(1); }
static void s_CreateMutexA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_MUTEX, ARG(2) ? name_a(c, ARG(2)) : 0, ARG(0), &ex, &k); if (h && k && !ex && (u32)ARG(1)) { k->owner_pid = W->pid; k->owner_tid = (int)cur_tid(W); k->recursion = 1; } RET(h); }
static void s_CreateMutexW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_MUTEX, ARG(2) ? name_w(c, ARG(2)) : 0, ARG(0), &ex, &k); if (h && k && !ex && (u32)ARG(1)) { k->owner_pid = W->pid; k->owner_tid = (int)cur_tid(W); k->recursion = 1; } RET(h); }
static void s_CreateMutexExW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_MUTEX, ARG(1) ? name_w(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex && ((u32)ARG(2) & 1)) { k->owner_pid = W->pid; k->owner_tid = (int)cur_tid(W); k->recursion = 1; } RET(h); }
static void s_CreateMutexExA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_MUTEX, ARG(1) ? name_a(c, ARG(1)) : 0, ARG(0), &ex, &k); if (h && k && !ex && ((u32)ARG(2) & 1)) { k->owner_pid = W->pid; k->owner_tid = (int)cur_tid(W); k->recursion = 1; } RET(h); }
static void s_OpenMutexA(Cpu *c) { RET(open_kobj_handle(c, KO_MUTEX, name_a(c, ARG(2)), (u32)ARG(1) != 0)); }
static void s_OpenMutexW(Cpu *c) { RET(open_kobj_handle(c, KO_MUTEX, name_w(c, ARG(2)), (u32)ARG(1) != 0)); }
static void s_ReleaseMutex(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_MUTEX); if (!k) { W->last_error = 6; RET(0); return; } if (!(k->owner_pid == W->pid && k->owner_tid == (int)cur_tid(W))) { W->last_error = 288; RET(0); return; } if (--k->recursion <= 0) { k->recursion = 0; k->owner_tid = 0; k->owner_pid = 0; } RET(1); }
static void s_CreateSemaphoreA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_SEM, ARG(3) ? name_a(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->count = (int)(s32)ARG(1); k->max = (int)(s32)ARG(2); } RET(h); }
static void s_CreateSemaphoreW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_SEM, ARG(3) ? name_w(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->count = (int)(s32)ARG(1); k->max = (int)(s32)ARG(2); } RET(h); }
static void s_CreateSemaphoreExW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_SEM, ARG(3) ? name_w(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->count = (int)(s32)ARG(1); k->max = (int)(s32)ARG(2); } RET(h); }
static void s_CreateSemaphoreExA(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_SEM, ARG(3) ? name_a(c, ARG(3)) : 0, ARG(0), &ex, &k); if (h && k && !ex) { k->count = (int)(s32)ARG(1); k->max = (int)(s32)ARG(2); } RET(h); }
static void s_OpenSemaphoreA(Cpu *c) { RET(open_kobj_handle(c, KO_SEM, name_a(c, ARG(2)), (u32)ARG(1) != 0)); }
static void s_OpenSemaphoreW(Cpu *c) { RET(open_kobj_handle(c, KO_SEM, name_w(c, ARG(2)), (u32)ARG(1) != 0)); }
static void s_ReleaseSemaphore(Cpu *c) { KObj *k = kobj_of_handle(c, ARG(0), KO_SEM); if (!k) { W->last_error = 6; RET(0); return; } int n = (int)(s32)ARG(1); if (n <= 0 || k->count + n > k->max) { W->last_error = 298; RET(0); return; } if (ARG(2)) wr32(c, ARG(2), (u32)k->count); k->count += n; RET(1); }

/* waits */
static void do_wait(Cpu *c, u32 n, u64 arr, u64 single, int all, u32 ms) {
  Win32 *w = W; int kinds[64], idxs[64]; if (n > 64) n = 64; if (n == 0) { w->last_error = 87; wait_end(w); RET(0xffffffffu); return; }
  for (u32 i = 0; i < n; i++) { u64 h = arr ? rdp(c, arr + i * PSZ_OF(c)) : single; if (!wait_handle(c, h, &kinds[i], &idxs[i])) { w->last_error = 6; wait_end(w); RET(0xffffffffu); return; } }
  int which = 0; int tid = (int)cur_tid(w);
  if (w->wait_active && w->wait_rip == c->rip && w->wait_granted) { which = w->wait_granted - 1; w->wait_granted = 0; wait_end(w); RET((u32)which); return; }
  w->wait_granted = 0;
  if (wait_check(w->pid, tid, kinds, idxs, (int)n, all, 1, &which)) { wait_end(w); RET((u32)which); return; }
  double dl = wait_begin(c, ms);
  if (deadline_passed(dl)) { wait_end(w); RET(WAIT_TIMEOUT_); return; }
  w->nwobj = (int)n; for (u32 i = 0; i < n; i++) { w->wobj_kind[i] = kinds[i]; w->wobj_idx[i] = idxs[i]; } w->wait_all = all;
  park(c, BLOCK_OBJ);
}
int w32_wait_poll(Cpu *c, u32 n, u64 arr, int all, int *which) { Win32 *w = W; int kinds[64], idxs[64]; if (n > 64) n = 64; if (!n || !arr) return 0;
  for (u32 i = 0; i < n; i++) if (!wait_handle(c, rdp(c, arr + i * PSZ_OF(c)), &kinds[i], &idxs[i])) return -1;
  return wait_check(w->pid, (int)cur_tid(w), kinds, idxs, (int)n, all, 1, which) ? 1 : 0; }
static void s_WaitForSingleObject(Cpu *c) { do_wait(c, 1, 0, ARG(0), 0, (u32)ARG(1)); }
static void s_WaitForSingleObjectEx(Cpu *c) { do_wait(c, 1, 0, ARG(0), 0, (u32)ARG(1)); }
static void s_WaitForMultipleObjects(Cpu *c) { do_wait(c, (u32)ARG(0), ARG(1), 0, (u32)ARG(2) != 0, (u32)ARG(3)); }
static void s_WaitForMultipleObjectsEx(Cpu *c) { do_wait(c, (u32)ARG(0), ARG(1), 0, (u32)ARG(2) != 0, (u32)ARG(3)); }
static void s_SignalObjectAndWait(Cpu *c) {
  Win32 *w = W; int retry = w->wait_active && w->wait_rip == c->rip;
  if (!retry) { KObj *k = kobj_of_handle(c, ARG(0), 0); if (!k) { w->last_error = 6; RET(0xffffffffu); return; }
    if (k->kind == KO_EVENT) k->signaled = 1; else if (k->kind == KO_SEM) { if (k->count < k->max) k->count++; } else if (k->kind == KO_MUTEX && k->owner_pid == w->pid && k->owner_tid == (int)cur_tid(w) && --k->recursion <= 0) { k->recursion = 0; k->owner_tid = 0; k->owner_pid = 0; } }
  do_wait(c, 1, 0, ARG(1), 0, (u32)ARG(2));
}

/* interlocked helpers that are not inlined by every compiler */
static void s_InterlockedCompareExchange64(Cpu *c) { u64 old = rd64(c, ARG(0)); if (old == ARG(2)) wr64(c, ARG(0), ARG(1)); RET(old); }
static void s_InterlockedExchange64(Cpu *c) { u64 old = rd64(c, ARG(0)); wr64(c, ARG(0), ARG(1)); RET(old); }
static void s_InterlockedExchangeAdd64(Cpu *c) { u64 old = rd64(c, ARG(0)); wr64(c, ARG(0), old + ARG(1)); RET(old); }
static void s_InterlockedIncrement64(Cpu *c) { u64 v = rd64(c, ARG(0)) + 1; wr64(c, ARG(0), v); RET(v); }
static void s_InterlockedDecrement64(Cpu *c) { u64 v = rd64(c, ARG(0)) - 1; wr64(c, ARG(0), v); RET(v); }
static void s_InterlockedAnd(Cpu *c) { u32 old = rd32(c, ARG(0)); wr32(c, ARG(0), old & (u32)ARG(1)); RET(old); }
static void s_InterlockedOr(Cpu *c) { u32 old = rd32(c, ARG(0)); wr32(c, ARG(0), old | (u32)ARG(1)); RET(old); }
static void s_InterlockedXor(Cpu *c) { u32 old = rd32(c, ARG(0)); wr32(c, ARG(0), old ^ (u32)ARG(1)); RET(old); }

#define THREAD_SHIMS \
  S(GetCurrentThread), S(GetCurrentThreadId), S(CreateThread), S(QueueUserWorkItem), SA("_beginthreadex", beginthreadex), SA("_beginthread", beginthread), S(ExitThread), S(FreeLibraryAndExitThread), SA("_endthreadex", endthreadex), SA("_endthread", endthread), \
  S(GetExitCodeThread), S(GetThreadId), S(GetProcessIdOfThread), S(OpenThread), S(ResumeThread), S(SuspendThread), S(TerminateThread), S(SwitchToThread), S(GetThreadContext), S(SetThreadContext), \
  S(SetThreadDescription), S(GetThreadDescription), S(GetCurrentThreadStackLimits), S(SetThreadStackGuarantee), \
  S(TlsAlloc), S(TlsFree), S(TlsGetValue), S(TlsSetValue), S(FlsAlloc), S(FlsFree), S(FlsGetValue), S(FlsSetValue), \
  S(InitializeCriticalSection), S(InitializeCriticalSectionAndSpinCount), S(InitializeCriticalSectionEx), S(DeleteCriticalSection), S(SetCriticalSectionSpinCount), S(EnterCriticalSection), S(TryEnterCriticalSection), S(LeaveCriticalSection), \
  S(InitializeSRWLock), S(AcquireSRWLockExclusive), S(AcquireSRWLockShared), S(ReleaseSRWLockExclusive), S(ReleaseSRWLockShared), S(TryAcquireSRWLockExclusive), S(TryAcquireSRWLockShared), \
  S(InitializeConditionVariable), S(WakeConditionVariable), S(WakeAllConditionVariable), S(SleepConditionVariableCS), S(SleepConditionVariableSRW), \
  S(InitOnceExecuteOnce), S(InitOnceBeginInitialize), S(InitOnceComplete), S(InitOnceInitialize), S(WaitOnAddress), S(WakeByAddressSingle), S(WakeByAddressAll), \
  S(CreateEventA), S(CreateEventW), S(CreateEventExW), S(CreateEventExA), S(OpenEventA), S(OpenEventW), S(SetEvent), S(ResetEvent), S(PulseEvent), \
  S(CreateMutexA), S(CreateMutexW), S(CreateMutexExW), S(CreateMutexExA), S(OpenMutexA), S(OpenMutexW), S(ReleaseMutex), \
  S(CreateSemaphoreA), S(CreateSemaphoreW), S(CreateSemaphoreExW), S(CreateSemaphoreExA), S(OpenSemaphoreA), S(OpenSemaphoreW), S(ReleaseSemaphore), \
  S(WaitForSingleObject), S(WaitForSingleObjectEx), S(WaitForMultipleObjects), S(WaitForMultipleObjectsEx), S(SignalObjectAndWait), \
  S(InterlockedCompareExchange64), S(InterlockedExchange64), S(InterlockedExchangeAdd64), S(InterlockedIncrement64), S(InterlockedDecrement64), S(InterlockedAnd), S(InterlockedOr), S(InterlockedXor), \
  SA("InterlockedCompareExchangePointer", InterlockedCompareExchange64), SA("InterlockedExchangePointer", InterlockedExchange64),
