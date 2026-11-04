/* win32_iocp.inc — I/O completion ports (libuv's event loop on Windows: node.exe, and every program built on it).
 *
 *  CreateIoCompletionPort      creates a port (a KO_IOCP kernel object) or associates a file/pipe handle with one
 *  PostQueuedCompletionStatus  queues a packet
 *  GetQueuedCompletionStatus / GetQueuedCompletionStatusEx   dequeue (blocking through the normal wait machinery)
 *  SetFileCompletionNotificationModes   FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (1) / FILE_SKIP_SET_EVENT_ON_HANDLE (2)
 *
 *  ReadFile/WriteFile with an OVERLAPPED on an associated pipe become asynchronous: if the pipe has data (room) the call
 *  completes at once and a packet is queued (unless skip-on-success); otherwise it returns ERROR_IO_PENDING and the operation
 *  is parked in g_pend[] — w32_io_poll() (called by the scheduler before every pick) finishes it when the other end has
 *  written/read, writes OVERLAPPED.Internal (NTSTATUS) + InternalHigh (bytes) and queues the packet.
 *  Included from win32.c after win32_nt.inc; obj_signaled() in win32_threads.inc asks iocp_has_packets(). */

typedef struct { u64 key, ov; u32 bytes, err; } IocpPkt;
#define IOCP_MAX 16
#define IOCP_Q   1024
static struct { int used, kobj; IocpPkt q[IOCP_Q]; int head, len; } g_iocp[IOCP_MAX];
#define IOCP_ASSOC_MAX 512
static struct { int used, pid, fd, port; u64 key; int skip; } g_iocp_assoc[IOCP_ASSOC_MAX];
#define PEND_MAX 256
typedef struct { int used, write, fd, port, pid; Win32 *w; u64 key, buf, ov; u32 len; } PendOp;
static PendOp g_pend[PEND_MAX];
static void net_poll(void); static void net_drop(Win32 *w);   /* win32_net.inc */

static int iocp_by_kobj(int kobj) { for (int i = 0; i < IOCP_MAX; i++) if (g_iocp[i].used && g_iocp[i].kobj == kobj) return i; return -1; }
static int iocp_has_packets(int kobj) { int i = iocp_by_kobj(kobj); return i >= 0 && g_iocp[i].len > 0; }
static void iocp_post(int port, u64 key, u64 ov, u32 bytes, u32 err) {
  if (port < 0 || !g_iocp[port].used) return; if (g_iocp[port].len >= IOCP_Q) return;   /* overflow: drop (never happens in practice) */
  IocpPkt *p = &g_iocp[port].q[(g_iocp[port].head + g_iocp[port].len) % IOCP_Q]; p->key = key; p->ov = ov; p->bytes = bytes; p->err = err; g_iocp[port].len++; }
static int iocp_pop(int port, IocpPkt *out) { if (port < 0 || !g_iocp[port].len) return 0; *out = g_iocp[port].q[g_iocp[port].head]; g_iocp[port].head = (g_iocp[port].head + 1) % IOCP_Q; g_iocp[port].len--; return 1; }
static int iocp_assoc_find(int pid, int fd) { for (int i = 0; i < IOCP_ASSOC_MAX; i++) if (g_iocp_assoc[i].used && g_iocp_assoc[i].pid == pid && g_iocp_assoc[i].fd == fd) return i; return -1; }
static void iocp_assoc_drop(int pid, int fd) { int i = iocp_assoc_find(pid, fd); if (i >= 0) g_iocp_assoc[i].used = 0; }
static int iocp_port_of_handle(Cpu *c, u64 h) { KObj *k = kobj_of_handle(c, h, KO_IOCP); if (!k) return -1; int kind = 0, idx = -1; wait_handle(c, h, &kind, &idx); return iocp_by_kobj(idx); }

static void s_CreateIoCompletionPort(Cpu *c) {
  u64 file = ARG(0), porth = ARG(1), key = ARG(2); int port = -1;
  if (porth) { port = iocp_port_of_handle(c, porth); if (port < 0) { W->last_error = 6; RET(0); return; } }
  else {
    int i; for (i = 0; i < IOCP_MAX && g_iocp[i].used; i++); if (i == IOCP_MAX) { W->last_error = 8; RET(0); return; }
    KObj *k = 0; int ex = 0; porth = new_kobj_handle(c, KO_IOCP, 0, 0, &ex, &k); if (!porth) { W->last_error = 8; RET(0); return; }
    int kind = 0, idx = -1; wait_handle(c, porth, &kind, &idx); g_iocp[i].used = 1; g_iocp[i].kobj = idx; g_iocp[i].head = g_iocp[i].len = 0; port = i;
  }
  if (file && file != ~0ull) {
    int fd = handle_fd(file); if (fd <= 0 && file >= 0x10 && file <= 0x12) fd = vfs_resolve(file == 0x10 ? 0 : (int)(file - 0x10)); if (fd <= 0) { W->last_error = 6; RET(0); return; }
    int i = iocp_assoc_find(W->pid, fd); if (i < 0) { for (i = 0; i < IOCP_ASSOC_MAX && g_iocp_assoc[i].used; i++); if (i == IOCP_ASSOC_MAX) { W->last_error = 8; RET(0); return; } }
    g_iocp_assoc[i].used = 1; g_iocp_assoc[i].pid = W->pid; g_iocp_assoc[i].fd = fd; g_iocp_assoc[i].port = port; g_iocp_assoc[i].key = key; g_iocp_assoc[i].skip = 0;
  }
  RET(porth); }
static void s_SetFileCompletionNotificationModes(Cpu *c) { int fd = handle_fd(ARG(0)); int i = fd > 0 ? iocp_assoc_find(W->pid, fd) : -1; if (i >= 0) g_iocp_assoc[i].skip = (int)(ARG(1) & 3); RET(1); }
static void s_PostQueuedCompletionStatus(Cpu *c) { int port = iocp_port_of_handle(c, ARG(0)); if (port < 0) { W->last_error = 6; RET(0); return; } iocp_post(port, ARG(2), ARG(3), (u32)ARG(1), 0); RET(1); }

/* shared blocking tail: 1 = a packet is available now (or the wait ended), 0 = parked (caller returns) */
static int iocp_wait(Cpu *c, int port, u32 ms, int *timed_out) {
  Win32 *w = W; *timed_out = 0;
  if (g_iocp[port].len) { wait_end(w); return 1; }
  if (ms == 0) { wait_end(w); *timed_out = 1; return 1; }
  double dl = wait_begin(c, ms);
  if (deadline_passed(dl)) { wait_end(w); *timed_out = 1; return 1; }
  w->nwobj = 1; w->wobj_kind[0] = 2; w->wobj_idx[0] = g_iocp[port].kobj; w->wait_all = 0; w->wait_granted = 0;
  park(c, BLOCK_OBJ); return 0; }
/* GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, ms) */
static void s_GetQueuedCompletionStatus(Cpu *c) {
  int port = iocp_port_of_handle(c, ARG(0)); if (port < 0) { W->last_error = 6; if (ARG(3)) wr64(c, ARG(3), 0); RET(0); return; }
  int to; if (!iocp_wait(c, port, (u32)ARG(4), &to)) return;
  if (to) { W->last_error = 258; if (ARG(3)) wr64(c, ARG(3), 0); RET(0); return; }
  IocpPkt p; iocp_pop(port, &p); if (ARG(1)) wr32(c, ARG(1), p.bytes); if (ARG(2)) wr64(c, ARG(2), p.key); if (ARG(3)) wr64(c, ARG(3), p.ov);
  if (p.err) { W->last_error = p.err; RET(0); return; } RET(1); }
/* GetQueuedCompletionStatusEx(port, OVERLAPPED_ENTRY *entries, count, &removed, ms, alertable); entry = { key, ov, Internal, bytes } 32 B */
static void s_GetQueuedCompletionStatusEx(Cpu *c) {
  int port = iocp_port_of_handle(c, ARG(0)); if (port < 0) { W->last_error = 6; RET(0); return; }
  int to; if (!iocp_wait(c, port, (u32)ARG(4), &to)) return;
  if (to) { W->last_error = 258; if (ARG(3)) wr32(c, ARG(3), 0); RET(0); return; }
  u64 arr = ARG(1); u32 cap = (u32)ARG(2), n = 0; IocpPkt p;
  while (n < cap && iocp_pop(port, &p)) { u64 e = arr + (u64)n * 32; wr64(c, e, p.key); wr64(c, e + 8, p.ov); wr64(c, e + 16, p.ov ? rd64(c, p.ov) : 0); wr32(c, e + 24, p.bytes); wr32(c, e + 28, 0); n++; }
  if (ARG(3)) wr32(c, ARG(3), n); RET(1); }

/* called by ReadFile/WriteFile: returns 1 when it handled the call (the handle is associated with a port and an OVERLAPPED was given) */
static int iocp_rw(Cpu *c, int fd, int write, u64 buf, u32 len, u64 bytes_out, u64 ov) {
  if (!ov) return 0; int rfd = vfs_resolve(fd == -1 ? 0 : fd); if (rfd < 3 || vfs_kind(rfd) != FD_PIPE) return 0;
  int ai = iocp_assoc_find(W->pid, rfd); if (ai < 0) return 0;
  int pi = vfs_pipe_id(rfd);
  if (vfs_pipe_would_block(pi, write)) {
    int k; for (k = 0; k < PEND_MAX && g_pend[k].used; k++); if (k == PEND_MAX) { W->last_error = 8; RET(0); return 1; }
    g_pend[k].used = 1; g_pend[k].write = write; g_pend[k].fd = rfd; g_pend[k].port = g_iocp_assoc[ai].port; g_pend[k].pid = W->pid; g_pend[k].w = W; g_pend[k].key = g_iocp_assoc[ai].key; g_pend[k].buf = buf; g_pend[k].ov = ov; g_pend[k].len = len;
    ov_set(c, ov, 0x103, 0); W->last_error = 997; RET(0); return 1;
  }
  int quiet = (rdp(c, OV_EVENT(ov)) & 1) != 0;   /* OVERLAPPED.hEvent with the low bit set = "do not queue a completion packet" (libuv relies on this) */
  if (!write && len == 0) {   /* libuv's zero-byte read: "data is ready" → immediate success, bytes stay in the pipe; writer gone + empty → EOF */
    if (vfs_pipe_avail(rfd) > 0) { ov_set(c, ov, 0, 0); if (bytes_out) wr32(c, bytes_out, 0);
      if (!(g_iocp_assoc[ai].skip & 1) && !quiet) iocp_post(g_iocp_assoc[ai].port, g_iocp_assoc[ai].key, ov, 0, 0); RET(1); return 1; }
    ov_set(c, ov, 0xC000014Bull, 0); W->last_error = 109; RET(0); return 1; }
  s64 r; if (write) { const u8 *p = mem_ptr(c->mem, buf, len, 0); r = p ? vfs_write(rfd, p, len) : -1; } else { u8 *p = mem_ptr(c->mem, buf, len, 1); r = p ? vfs_read(rfd, p, len) : -1; }
  if (r == -2) { /* raced: park it */ int k; for (k = 0; k < PEND_MAX && g_pend[k].used; k++); if (k < PEND_MAX) { g_pend[k].used = 1; g_pend[k].write = write; g_pend[k].fd = rfd; g_pend[k].port = g_iocp_assoc[ai].port; g_pend[k].pid = W->pid; g_pend[k].w = W; g_pend[k].key = g_iocp_assoc[ai].key; g_pend[k].buf = buf; g_pend[k].ov = ov; g_pend[k].len = len; wr64(c, ov, 0x103); W->last_error = 997; RET(0); return 1; } r = -1; }
  if (r < 0 || (r == 0 && !write)) { u32 err = (r == -3 || r == 0) ? (write ? 232 : 109) : 5; ov_set(c, ov, r == 0 || r == -3 ? 0xC000014Bull : 0xC0000001ull, 0); W->last_error = err; RET(0); return 1; }
  ov_set(c, ov, 0, (u64)r); if (bytes_out) wr32(c, bytes_out, (u32)r);
  if (!(g_iocp_assoc[ai].skip & 1) && !quiet) iocp_post(g_iocp_assoc[ai].port, g_iocp_assoc[ai].key, ov, (u32)r, 0);
  RET(1); return 1; }

/* scheduler hook: finish parked operations whose pipe became ready */
void w32_io_poll(void) {
  for (int k = 0; k < PEND_MAX; k++) {
    PendOp *o = &g_pend[k]; if (!o->used) continue;
    int pi; { vfs_use(o->w->fs); pi = vfs_pipe_id(o->fd); }
    if (pi < 0) { o->used = 0; continue; }
    if (vfs_pipe_would_block(pi, o->write)) continue;
    Mem *m = o->w->mem; s64 r;
    if (!o->write && o->len == 0) {   /* zero-byte read: data ready → success; writer closed with nothing left → EOF (109) */
      u8 *ovp = mem_ptr(m, o->ov, 16, 1); if (vfs_pipe_avail(o->fd) > 0) { if (ovp) e_memset(ovp, 0, 16); iocp_post(o->port, o->key, o->ov, 0, 0); }
      else { if (ovp) { e_memset(ovp, 0, 16); ovp[0] = 0x4B; ovp[1] = 0x01; ovp[2] = 0x00; ovp[3] = 0xC0; } iocp_post(o->port, o->key, o->ov, 0, 109); }
      o->used = 0; continue; }
    if (o->write) { const u8 *p = mem_ptr(m, o->buf, o->len, 0); r = p ? vfs_write(o->fd, p, o->len) : -1; } else { u8 *p = mem_ptr(m, o->buf, o->len, 1); r = p ? vfs_read(o->fd, p, o->len) : -1; }
    if (r == -2) continue;
    u8 *ovp = mem_ptr(m, o->ov, 16, 1);
    u64 status = 0, bytes = 0; u32 err = 0;
    if (r < 0 || (r == 0 && !o->write)) { status = (r == -3 || r == 0) ? 0xC000014Bull : 0xC0000001ull; err = r == -3 || r == 0 ? (o->write ? 232 : 109) : 5; } else bytes = (u64)r;
    if (ovp) { for (int i = 0; i < 8; i++) { ovp[i] = (u8)(status >> (8 * i)); ovp[8 + i] = (u8)(bytes >> (8 * i)); } }
    iocp_post(o->port, o->key, o->ov, (u32)bytes, err); o->used = 0;
  }
  net_poll();
}
void w32_io_drop(Win32 *w) {
  net_drop(w);
  for (int k = 0; k < PEND_MAX; k++) if (g_pend[k].used && g_pend[k].w == w) g_pend[k].used = 0;
  for (int i = 0; i < IOCP_ASSOC_MAX; i++) if (g_iocp_assoc[i].used && g_iocp_assoc[i].pid == w->pid) g_iocp_assoc[i].used = 0;
}
static void iocp_handle_closed(int pid, int fd) { iocp_assoc_drop(pid, fd); for (int k = 0; k < PEND_MAX; k++) if (g_pend[k].used && g_pend[k].pid == pid && g_pend[k].fd == fd) g_pend[k].used = 0; }
static void iocp_kobj_closed(int kobj) { int i = iocp_by_kobj(kobj); if (i >= 0) g_iocp[i].used = 0; }

#define IOCP_SHIMS \
  S(CreateIoCompletionPort), S(PostQueuedCompletionStatus), S(GetQueuedCompletionStatus), S(GetQueuedCompletionStatusEx), S(SetFileCompletionNotificationModes),
