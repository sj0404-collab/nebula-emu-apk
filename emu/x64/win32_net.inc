/* win32_net.inc — Winsock 2 over the host's TCP stack (IPv4 stream sockets: what node/npm, curl-like tools and HTTP clients use).
 *
 *  A SOCKET is an ordinary handle (H_FILE_BASE + fd, fd kind FD_SOCK) referencing g_socks[]; the host socket is created lazily at
 *  connect()/listen(). Blocking semantics follow libuv's expectations:
 *    socket/WSASocketW, closesocket, bind, listen, connect, ioctlsocket(FIONBIO/FIONREAD), setsockopt, getsockopt(SO_PROTOCOL_INFOW/SO_ERROR/SO_TYPE),
 *    getsockname/getpeername, shutdown, send/recv, WSASend/WSARecv (overlapped → IOCP packets, honouring FILE_SKIP_COMPLETION_PORT_ON_SUCCESS),
 *    WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER → ConnectEx/AcceptEx/GetAcceptExSockaddrs/DisconnectEx, SIO_BASE_HANDLE),
 *    ConnectEx/AcceptEx (always complete through the port), GetAddrInfoW/getaddrinfo (host resolver, A records), WSAGetLastError.
 *  Asynchronous operations are parked in g_spend[] and finished by net_poll() (called from w32_io_poll before every scheduler pick).
 *  Closing a socket with parked operations completes them with STATUS_CANCELLED (what CancelIo does on Windows) so libuv can finish closing.
 *  The wasm host has no raw TCP: every host_net_* call fails there with WSAENETDOWN, so programs see "network down" instead of hanging. */

#define SOCK_MAX 256
typedef struct { int used, refs, host, state, err, nonblock, bound; u8 lip[4]; int lport;
  u8 *rx; u32 rxlen, rxoff, rxcap; int rxeof, rxerr;            /* rx*: host-side read-ahead buffer (see sock_pump) */
  u8 *tx; u32 txlen, txoff, txcap; int txerr, txshut; u64 tx_total, tx_sent; } Sock;   /* tx*: send queue flushed by the scheduler (see sock_write); state 5 = closed by the guest, draining */   /* state: 0 new 1 connecting 2 connected 3 listening 4 failed */
static Sock g_socks[SOCK_MAX];
static int sock_ready_mask(int si);
#define SPEND_MAX 256
typedef struct { int used, kind, sock, pid, port, acc; Win32 *w; u64 key, ov, buf, target; u32 len; } SockOp;   /* kind: 1 zero-read 2 read 3 connect 4 accept 5 send (done when tx_sent >= target) */
static SockOp g_spend[SPEND_MAX];
static int g_net_pending;
static void spend_finish(SockOp *o, u32 status, u32 bytes, u32 err);
static void sock_destroy(int idx) { Sock *s = &g_socks[idx]; if (s->host >= 0) host_net_close(s->host); if (s->rx) emu_free(s->rx); if (s->tx) emu_free(s->tx); s->rx = s->tx = 0; s->host = -1; s->used = 0; }

void w32_sock_ref(int idx, int delta) {
  if (idx < 0 || idx >= SOCK_MAX || !g_socks[idx].used) return;
  g_socks[idx].refs += delta;
  if (g_socks[idx].refs <= 0) {
    for (int k = 0; k < SPEND_MAX; k++) if (g_spend[k].used && g_spend[k].sock == idx) spend_finish(&g_spend[k], 0xC0000120u, 0, 995);   /* STATUS_CANCELLED / ERROR_OPERATION_ABORTED */
    Sock *s = &g_socks[idx];
    if (s->host >= 0 && s->state == 2 && !s->txerr && s->txlen > s->txoff) { s->state = 5; return; }   /* unsent bytes: keep the host socket until the scheduler has flushed them */
    sock_destroy(idx); }
}
int w32_net_pending(void) { return g_net_pending; }
static int sock_new(void) { for (int i = 0; i < SOCK_MAX; i++) if (!g_socks[i].used) { e_memset(&g_socks[i], 0, sizeof(Sock)); g_socks[i].used = 1; g_socks[i].host = -1; return i; } return -1; }
static int sock_of(Cpu *c, u64 h) { int fd = handle_fd(h); if (fd < 3 || vfs_kind(fd) != FD_SOCK) return -1; int i = vfs_fd_obj(fd); return (i >= 0 && i < SOCK_MAX && g_socks[i].used) ? i : -1; }
#define SOCK_ARG0(var) int var = sock_of(c, ARG(0)); if (var < 0) { W->last_error = 10038; RET(~0ull); return; }
/* ---- read-ahead: the emulated program digests network data 50-100x slower than a native one; if the bytes stayed in the kernel
 * socket buffer the TCP window would close for minutes and servers/proxies drop the connection (npm saw ECONNRESET on big tarballs).
 * sock_pump() therefore moves everything the host socket has into a per-socket buffer (up to RX_MAX) whenever the scheduler runs,
 * so the peer always sees an open window; guest reads are served from the buffer first. */
#define RX_MAX (256u << 20)
static void sock_pump(Sock *s) {
  if (s->state != 2 || s->host < 0 || s->rxeof || s->rxerr) return;
  for (;;) {
    if (s->rxoff == s->rxlen) s->rxoff = s->rxlen = 0;
    u32 live = s->rxlen - s->rxoff; if (live >= RX_MAX) return;
    if (s->rxcap - s->rxlen < 65536) {   /* compact / grow so that at least 64 KB are free */
      u32 nc = s->rxcap ? s->rxcap : 262144; while (nc - live < 65536) nc *= 2;
      if (nc != s->rxcap) { u8 *nb = (u8 *)emu_alloc(nc); if (!nb) return; if (live) e_memcpy(nb, s->rx + s->rxoff, live); if (s->rx) emu_free(s->rx); s->rx = nb; s->rxcap = nc; }
      else if (s->rxoff) e_memmove(s->rx, s->rx + s->rxoff, live);
      s->rxoff = 0; s->rxlen = live; }
    s64 r = host_net_read(s->host, s->rx + s->rxlen, s->rxcap - s->rxlen);
    if (r > 0) { s->rxlen += (u32)r; continue; }
    if (r == -2) return;
    if (r == 0) s->rxeof = 1; else { s->rxerr = 1; e_log("[x64] socket %d: host read error (reset) after %u buffered bytes\n", (int)(s - g_socks), live); }
    return; }
}
static s64 sock_read(Sock *s, u8 *p, u32 len) {   /* >0 bytes, 0 EOF, -2 would block, -3 error */
  if (s->host < 0) return -3;
  sock_pump(s); u32 have = s->rxlen - s->rxoff;
  if (have) { if (len > have) len = have; e_memcpy(p, s->rx + s->rxoff, len); s->rxoff += len; return (s64)len; }
  if (s->rxerr) return -3; if (s->rxeof) return 0;
  return s->state == 2 ? -2 : host_net_read(s->host, p, len); }
static int sock_readable(Sock *s) { if (s->host < 0) return 1; sock_pump(s); if (s->rxlen > s->rxoff || s->rxeof || s->rxerr) return 1; return s->state == 2 ? 0 : host_net_readable(s->host); }
static int sock_avail(Sock *s) { if (s->host < 0) return 0; sock_pump(s); return (int)(s->rxlen - s->rxoff) + host_net_avail(s->host); }
/* ---- send queue: Winsock promises that an overlapped WSASend completes only when all bytes were accepted, and a blocking send() returns
 * the full count. The host socket is non-blocking, so whatever does not fit right now is copied into a per-socket queue that the
 * scheduler flushes (sock_flush); a parked kind-5 op completes once tx_sent reaches its target. (Before this, a 5 MB write on a
 * loopback connection returned a short count that libuv took as "all written" and the receiver waited forever.) */
static int sock_flush(Sock *s) {   /* 1 = queue empty, 0 = bytes still pending, -1 = error */
  while (s->txoff < s->txlen) { s64 r = host_net_write(s->host, s->tx + s->txoff, s->txlen - s->txoff); if (r < 0) { s->txerr = 1; return -1; } if (r == 0) return 0; s->txoff += (u32)r; s->tx_sent += (u64)r; }
  s->txoff = s->txlen = 0; if (s->txshut) { host_net_shutdown(s->host, s->txshut - 1); s->txshut = 0; } return 1; }
static int sock_queue(Sock *s, const u8 *p, u32 len) {
  if (s->txoff == s->txlen) s->txoff = s->txlen = 0;
  if (s->txcap - s->txlen < len) { u32 live = s->txlen - s->txoff; u32 nc = s->txcap ? s->txcap : 65536; while (nc - live < len) nc *= 2;
    u8 *nb = (u8 *)emu_alloc(nc); if (!nb) return 0; if (live) e_memcpy(nb, s->tx + s->txoff, live); if (s->tx) emu_free(s->tx); s->tx = nb; s->txcap = nc; s->txoff = 0; s->txlen = live; }
  e_memcpy(s->tx + s->txlen, p, len); s->txlen += len; s->tx_total += len; return 1; }
static s64 sock_write(Sock *s, const u8 *p, u32 len) {   /* accepts everything: returns len, or -3 on a dead connection */
  if (s->host < 0 || s->state != 2 || s->txerr) return -3; u32 want = len;
  if (s->txoff == s->txlen) { s64 r = host_net_write(s->host, p, len); if (r < 0) { s->txerr = 1; return -3; } s->tx_total += (u64)r; s->tx_sent += (u64)r; if ((u32)r == len) return (s64)want; p += r; len -= (u32)r; }
  if (!sock_queue(s, p, len)) return -3;
  return (s64)want; }
/* blocking sockets (default mode): the call parks the thread on the socket's readiness mask (same mechanism as select()) and is
 * re-executed when the scheduler sees the socket ready — non-blocking sockets (FIONBIO) keep returning WSAEWOULDBLOCK */
static void sock_block(Cpu *c, int si, int mask) { Win32 *w = W; wait_begin(c, 0xffffffffu); w->nwobj = 1; w->wobj_idx[0] = si; w->wobj_kind[0] = mask; park(c, BLOCK_SELECT); }
static double g_pump_ms;
static void net_pump_all(void) {   /* called before every scheduler pick; at most every 5 ms */
  double now = host_now_ms(); if (now - g_pump_ms < 5) return; g_pump_ms = now;
  for (int i = 0; i < SOCK_MAX; i++) { Sock *s = &g_socks[i]; if (!s->used || s->host < 0) continue;
    if (s->state == 2) { sock_pump(s); if (s->txlen > s->txoff) sock_flush(s); }
    else if (s->state == 5) { if (sock_flush(s) != 0) sock_destroy(i); } } }
static u32 wsa_to_ntstatus(int e) {
  switch (e) { case 0: return 0; case 10061: return 0xC0000236u; case 10060: return 0xC00000B5u; case 10065: return 0xC000023Du; case 10051: return 0xC000023Cu; case 10054: return 0xC000020Du;
    case 10053: return 0xC0000241u; case 10058: return 0xC000020Cu; case 10050: return 0xC00000BEu; default: return 0xC0000001u; } }
static void write_sockaddr(Cpu *c, u64 a, const u8 *ip, int port) { wr16(c, a, 2); wr8(c, a + 2, (u8)(port >> 8)); wr8(c, a + 3, (u8)port); for (int i = 0; i < 4; i++) wr8(c, a + 4 + i, ip[i]); for (int i = 0; i < 8; i++) wr8(c, a + 8 + i, 0); }
static int read_sockaddr(Cpu *c, u64 a, u8 *ip, int *port) { if (rd16(c, a) != 2) return 0; *port = (rd8(c, a + 2) << 8) | rd8(c, a + 3); for (int i = 0; i < 4; i++) ip[i] = rd8(c, a + 4 + i); return 1; }
static int sock_assoc(Cpu *c, int si, int *port, u64 *key, int *skip) {
  (void)si; int fd = handle_fd(ARG(0)); int ai = fd > 0 ? iocp_assoc_find(W->pid, fd) : -1; if (ai < 0) return 0; *port = g_iocp_assoc[ai].port; *key = g_iocp_assoc[ai].key; *skip = g_iocp_assoc[ai].skip; return 1; }
static int spend_add(Cpu *c, int kind, int si, int port, u64 key, u64 ov, u64 buf, u32 len, int acc) {
  int k; for (k = 0; k < SPEND_MAX && g_spend[k].used; k++); if (k == SPEND_MAX) return -1;
  g_spend[k].used = 1; g_spend[k].kind = kind; g_spend[k].sock = si; g_spend[k].pid = W->pid; g_spend[k].port = port; g_spend[k].w = W; g_spend[k].key = key; g_spend[k].ov = ov; g_spend[k].buf = buf; g_spend[k].len = len; g_spend[k].acc = acc;
  if (ov) { ov_set(c, ov, 0x103, 0); } g_net_pending++; return k; }

static void s_socket(Cpu *c) {
  int af = (int)ARG(0), type = (int)ARG(1);
  if (af != 2) { W->last_error = 10047; RET(~0ull); return; }                      /* WSAEAFNOSUPPORT: IPv4 only */
  if (type != 1 && type != 0) { W->last_error = 10044; RET(~0ull); return; }       /* WSAESOCKTNOSUPPORT: no UDP yet */
  int si = sock_new(); if (si < 0) { W->last_error = 10024; RET(~0ull); return; }
  int fd = vfs_sock_handle(si); if (fd < 0) { g_socks[si].used = 0; W->last_error = 10024; RET(~0ull); return; }
  vfs_set_inherit(fd, 0); RET(H_FILE_BASE + (u64)fd); }
static void s_closesocket(Cpu *c) { int si = sock_of(c, ARG(0)); if (si < 0) { W->last_error = 10038; RET(~0ull); return; } int fd = handle_fd(ARG(0)); iocp_handle_closed(W->pid, fd); vfs_close(fd); RET(0); }
static void s_ioctlsocket(Cpu *c) { SOCK_ARG0(si); u32 cmd = (u32)ARG(1);
  if (cmd == 0x8004667Eu) { g_socks[si].nonblock = rd32(c, ARG(2)) != 0; RET(0); return; }                                   /* FIONBIO */
  if (cmd == 0x4004667Fu) { wr32(c, ARG(2), (u32)sock_avail(&g_socks[si])); RET(0); return; }   /* FIONREAD */
  RET(0); }
static void s_setsockopt(Cpu *c) { SOCK_ARG0(si); (void)si; RET(0); }
/* getsockopt(s, level, opt, buf, &len) */
static void s_getsockopt(Cpu *c) { SOCK_ARG0(si); int level = (int)ARG(1), opt = (int)ARG(2); u64 buf = ARG(3), lenp = ARG(4); u32 len = lenp ? rd32(c, lenp) : 0;
  if (level == 0xffff && opt == 0x2005) {   /* SO_PROTOCOL_INFOW: 628 B; dwServiceFlags1 must advertise XP1_IFS_HANDLES */
    if (len < 628) { W->last_error = 10014; RET(~0ull); return; }
    u8 *p = mem_ptr(c->mem, buf, 628, 1); if (!p) { W->last_error = 10014; RET(~0ull); return; } e_memset(p, 0, 628);
    wr32(c, buf, 0x20066); wr32(c, buf + 16, 0x8); wr32(c, buf + 36, 1001); wr32(c, buf + 40, 1); wr32(c, buf + 72, 2); wr32(c, buf + 76, 2); wr32(c, buf + 80, 16); wr32(c, buf + 84, 16); wr32(c, buf + 88, 1); wr32(c, buf + 92, 6); wr32(c, buf + 100, 1);
    static const char nm[] = "MSAFD Tcpip [TCP/IP]"; for (int i = 0; nm[i]; i++) wr16(c, buf + 116 + i * 2, (u8)nm[i]); wr32(c, lenp, 628); RET(0); return; }
  if (level == 0xffff && opt == 0x1007) { if (len >= 4) wr32(c, buf, (u32)g_socks[si].err); wr32(c, lenp, 4); RET(0); return; }   /* SO_ERROR */
  if (level == 0xffff && opt == 0x1008) { if (len >= 4) wr32(c, buf, 1); wr32(c, lenp, 4); RET(0); return; }                     /* SO_TYPE */
  if (len >= 4) wr32(c, buf, 0); RET(0); }
static void s_bind(Cpu *c) { SOCK_ARG0(si); u8 ip[4]; int port = 0; if (!read_sockaddr(c, ARG(1), ip, &port)) { W->last_error = 10047; RET(~0ull); return; } e_memcpy(g_socks[si].lip, ip, 4); g_socks[si].lport = port; g_socks[si].bound = 1; RET(0); }
static void s_listen(Cpu *c) { SOCK_ARG0(si); Sock *s = &g_socks[si]; if (s->state == 3) { RET(0); return; }
  if (!s->bound) { W->last_error = 10022; RET(~0ull); return; }
  int err = 0; s->host = host_net_listen(s->lip, s->lport, (int)ARG(1), &err); if (s->host < 0) { s->state = 4; s->err = err; W->last_error = err; RET(~0ull); return; }
  host_net_name(s->host, 0, s->lip, &s->lport); s->state = 3; RET(0); }
static int sock_start_connect(Cpu *c, int si, u64 addr) {
  Sock *s = &g_socks[si]; u8 ip[4]; int port = 0; if (!read_sockaddr(c, addr, ip, &port)) return 10047;
  if (s->state == 1) return 10036; if (s->state == 2) return 10056;
  int err = 0; s->host = host_net_connect(ip, port, &err); if (s->host < 0) { s->state = 4; s->err = err; return err; }
  s->state = 1; return 0; }
/* classic connect(): starts the connect and reports WSAEWOULDBLOCK unless it finished at once (callers then poll/select) */
static void s_connect(Cpu *c) { SOCK_ARG0(si); Sock *s = &g_socks[si];
  if (s->state == 1 && W->wait_active && W->wait_rip == c->rip) {   /* re-executed after parking: report the outcome */
    int m = sock_ready_mask(si); if (!(m & 6)) { sock_block(c, si, 2 | 4); return; } wait_end(W); if (s->state == 2) { RET(0); return; } W->last_error = s->err ? (u32)s->err : 10061; RET(~0ull); return; }
  int e = sock_start_connect(c, si, ARG(1)); if (e) { wait_end(W); W->last_error = e; RET(~0ull); return; }
  int st = host_net_status(s->host); if (st > 0) { wait_end(W); s->state = 2; RET(0); return; }
  if (st < 0) { wait_end(W); s->state = 4; s->err = -st; W->last_error = (u32)-st; RET(~0ull); return; }
  if (!s->nonblock) { sock_block(c, si, 2 | 4); return; } wait_end(W); W->last_error = 10035; RET(~0ull); }
/* ConnectEx(s, addr, len, sendbuf, sendlen, &bytes, ov): always completes through the port */
static void s_ConnectEx(Cpu *c) { int si = sock_of(c, ARG(0)); if (si < 0) { W->last_error = 10038; RET(0); return; } u64 ov = ARG(6);
  int port = -1, skip = 0; u64 key = 0; if (!sock_assoc(c, si, &port, &key, &skip) || !ov) { W->last_error = 10045; RET(0); return; }
  int e = sock_start_connect(c, si, ARG(1)); if (e) { W->last_error = e; RET(0); return; }
  if (spend_add(c, 3, si, port, key, ov, 0, 0, 0) < 0) { W->last_error = 10055; RET(0); return; } W->last_error = 997; RET(0); }
static void sock_shutdown(Sock *s, int how) { if (s->host < 0) return; if (how != 0 && s->txlen > s->txoff && !s->txerr) { s->txshut = how + 1; if (how == 2) host_net_shutdown(s->host, 0); return; } host_net_shutdown(s->host, how); }
static void s_shutdown(Cpu *c) { SOCK_ARG0(si); sock_shutdown(&g_socks[si], (int)ARG(1)); RET(0); }
static void s_getsockname(Cpu *c) { SOCK_ARG0(si); Sock *s = &g_socks[si]; u8 ip[4] = {0,0,0,0}; int port = s->lport; e_memcpy(ip, s->lip, 4);
  if (s->host < 0 && s->bound && s->lport == 0 && s->state == 0) { int err = 0; s->host = host_net_listen(s->lip, 0, 16, &err); if (s->host >= 0) { host_net_name(s->host, 0, s->lip, &s->lport); s->state = 3; port = s->lport; e_memcpy(ip, s->lip, 4); } }   /* bind(0) + getsockname: Windows reports the chosen port before listen() */
  if (s->host >= 0) host_net_name(s->host, 0, ip, &port); if (ARG(2) && rd32(c, ARG(2)) >= 16) { write_sockaddr(c, ARG(1), ip, port); wr32(c, ARG(2), 16); RET(0); } else { W->last_error = 10014; RET(~0ull); } }
static void s_getpeername(Cpu *c) { SOCK_ARG0(si); Sock *s = &g_socks[si]; u8 ip[4]; int port;
  if (s->host < 0 || !host_net_name(s->host, 1, ip, &port)) { W->last_error = 10057; RET(~0ull); return; } if (ARG(2) && rd32(c, ARG(2)) >= 16) { write_sockaddr(c, ARG(1), ip, port); wr32(c, ARG(2), 16); RET(0); } else { W->last_error = 10014; RET(~0ull); } }
/* gather WSABUF[] { u32 len; pad; u64 buf } */
#define WSABUF_SZ (2 * PSZ_OF(c))   /* WSABUF { ULONG len; CHAR *buf; } */
static u32 wsabuf_total(Cpu *c, u64 bufs, u32 n) { u64 t = 0; for (u32 i = 0; i < n; i++) t += rd32(c, bufs + i * WSABUF_SZ); return t > 0x7fffffff ? 0x7fffffff : (u32)t; }
static s64 sock_recv_into(Cpu *c, int si, u64 bufs, u32 n) {   /* non-blocking read into the first buffer with room; >0 bytes, 0 EOF, -2 would block, -3 error */
  Sock *s = &g_socks[si]; if (s->host < 0) return -3;
  for (u32 i = 0; i < n; i++) { u32 len = rd32(c, bufs + i * WSABUF_SZ); u64 b = rdp(c, bufs + i * WSABUF_SZ + PSZ_OF(c)); if (!len) continue; u8 *p = mem_ptr(c->mem, b, len, 1); if (!p) return -3; return sock_read(s, p, len); }
  return 0; }
static s64 sock_send_from(Cpu *c, int si, u64 bufs, u32 n) {
  Sock *s = &g_socks[si]; if (s->host < 0 || s->state != 2) return -3; u64 total = 0;
  for (u32 i = 0; i < n; i++) { u32 len = rd32(c, bufs + i * WSABUF_SZ); u64 b = rdp(c, bufs + i * WSABUF_SZ + PSZ_OF(c)); if (!len) continue; const u8 *p = mem_ptr(c->mem, b, len, 0); if (!p) return -3;
    s64 r = sock_write(s, p, len); if (r < 0) return total ? (s64)total : -3; total += (u64)r; }
  return (s64)total; }
/* WSARecv(s, bufs, count, &bytes, &flags, ov, routine) */
static void s_WSARecv(Cpu *c) { SOCK_ARG0(si); u64 bufs = ARG(1); u32 n = (u32)ARG(2); u64 bytesp = ARG(3), ov = ARG(5);
  Sock *s = &g_socks[si];
  if (!ov) { s64 r = sock_recv_into(c, si, bufs, n); if (r == -2) { if (!s->nonblock && s->state == 2) { sock_block(c, si, 1 | 4); return; } wait_end(W); W->last_error = 10035; RET(~0ull); return; }
    wait_end(W); if (r == -3) { W->last_error = 10054; RET(~0ull); return; } if (bytesp) wr32(c, bytesp, (u32)r); RET(0); return; }
  int port = -1, skip = 0; u64 key = 0; int assoc = sock_assoc(c, si, &port, &key, &skip);
  u32 total = wsabuf_total(c, bufs, n);
  if (s->host < 0) { W->last_error = 10057; RET(~0ull); return; }
  if (total == 0) {   /* zero-byte read: readiness notification */
    if (sock_readable(s)) { ov_set(c, ov, 0, 0); if (bytesp) wr32(c, bytesp, 0); if (assoc && !(skip & 1)) iocp_post(port, key, ov, 0, 0); RET(0); return; }
    if (spend_add(c, 1, si, port, key, ov, 0, 0, 0) < 0) { W->last_error = 10055; RET(~0ull); return; } W->last_error = 997; RET(~0ull); return; }
  s64 r = sock_recv_into(c, si, bufs, n);
  if (r == -2) {   /* capture the first non-empty buffer NOW: the WSABUF array itself is often a temporary (.NET pins the data, not the array) */
    u64 b = 0; u32 len = 0; for (u32 i = 0; i < n; i++) { len = rd32(c, bufs + i * WSABUF_SZ); b = rdp(c, bufs + i * WSABUF_SZ + PSZ_OF(c)); if (len) break; }
    if (spend_add(c, 2, si, port, key, ov, b, len, 0) < 0) { W->last_error = 10055; RET(~0ull); return; } W->last_error = 997; RET(~0ull); return; }
  if (r == -3) { ov_set(c, ov, 0xC000020Dull, 0); W->last_error = 10054; RET(~0ull); return; }
  ov_set(c, ov, 0, (u64)r); if (bytesp) wr32(c, bytesp, (u32)r); if (assoc && !(skip & 1)) iocp_post(port, key, ov, (u32)r, 0); RET(0); }
/* WSASend(s, bufs, count, &bytes, flags, ov, routine): writes synchronously (host waits for socket room), completes at once */
static void s_WSASend(Cpu *c) { SOCK_ARG0(si); u64 bufs = ARG(1); u32 n = (u32)ARG(2); u64 bytesp = ARG(3), ov = ARG(5);
  s64 r = sock_send_from(c, si, bufs, n);
  if (r < 0) { if (ov) { ov_set(c, ov, 0xC000020Dull, 0); } W->last_error = g_socks[si].state == 2 ? 10054 : 10057; RET(~0ull); return; }
  Sock *s = &g_socks[si];
  if (ov && s->txlen > s->txoff) {   /* not everything reached the host yet: complete asynchronously when the queue has drained this far */
    int port = -1, skip = 0; u64 key = 0; sock_assoc(c, si, &port, &key, &skip);
    int k = spend_add(c, 5, si, port, key, ov, 0, (u32)r, 0); if (k < 0) { W->last_error = 10055; RET(~0ull); return; } g_spend[k].target = s->tx_total; W->last_error = 997; RET(~0ull); return; }
  if (bytesp) wr32(c, bytesp, (u32)r);
  if (ov) { ov_set(c, ov, 0, (u64)r); int port = -1, skip = 0; u64 key = 0; if (sock_assoc(c, si, &port, &key, &skip) && !(skip & 1)) iocp_post(port, key, ov, (u32)r, 0); }
  RET(0); }
static void s_recv(Cpu *c) { SOCK_ARG0(si); u32 len = (u32)ARG(2);
  if (len == 0) { int m = sock_ready_mask(si); if (g_socks[si].state == 2) { RET(0); return; } W->last_error = (m & 4) ? (g_socks[si].err ? (u32)g_socks[si].err : 10061) : 10057; RET(~0ull); return; }   /* recv(s, NULL, 0): Bun probes connection state this way */
  u8 *p = mem_ptr(c->mem, ARG(1), len, 1); if (!p || g_socks[si].host < 0) { wait_end(W); W->last_error = 10014; RET(~0ull); return; }
  s64 r = sock_read(&g_socks[si], p, len); if (r == -2) { if (!g_socks[si].nonblock && g_socks[si].state == 2) { sock_block(c, si, 1 | 4); return; } wait_end(W); W->last_error = 10035; RET(~0ull); return; }
  wait_end(W); if (r == -3) { W->last_error = 10054; RET(~0ull); return; } RET((u64)r); }
static void s_send(Cpu *c) { SOCK_ARG0(si); u32 len = (u32)ARG(2); if (len == 0) { RET(0); return; }
  const u8 *p = mem_ptr(c->mem, ARG(1), len, 0); if (!p || g_socks[si].host < 0) { W->last_error = 10014; RET(~0ull); return; }
  s64 r = sock_write(&g_socks[si], p, len); if (r < 0) { W->last_error = 10054; RET(~0ull); return; } RET((u64)r); }
/* accept(): non-blocking; AcceptEx(listen, accept, buf, 0, local_len, remote_len, &bytes, ov) → completes through the port, buffer gets the two sockaddrs */
static void s_accept(Cpu *c) { SOCK_ARG0(si); Sock *s = &g_socks[si]; if (s->state != 3) { W->last_error = 10022; RET(~0ull); return; }
  u8 ip[4]; int port = 0; int h = host_net_accept(s->host, ip, &port); if (h == -2) { if (!s->nonblock) { sock_block(c, si, 1); return; } wait_end(W); W->last_error = 10035; RET(~0ull); return; } wait_end(W); if (h < 0) { W->last_error = 10054; RET(~0ull); return; }
  int ni = sock_new(); if (ni < 0) { host_net_close(h); W->last_error = 10024; RET(~0ull); return; } g_socks[ni].host = h; g_socks[ni].state = 2; g_socks[ni].nonblock = s->nonblock;   /* inherits the blocking mode */
  int fd = vfs_sock_handle(ni); if (fd < 0) { g_socks[ni].used = 0; host_net_close(h); W->last_error = 10024; RET(~0ull); return; }
  if (ARG(1) && ARG(2) && rd32(c, ARG(2)) >= 16) { write_sockaddr(c, ARG(1), ip, port); wr32(c, ARG(2), 16); } RET(H_FILE_BASE + (u64)fd); }
static void s_AcceptEx(Cpu *c) { int si = sock_of(c, ARG(0)), ai = sock_of(c, ARG(1)); u64 ov = ARG(7);
  if (si < 0 || ai < 0 || !ov) { W->last_error = 10038; RET(0); return; }
  int port = -1, skip = 0; u64 key = 0; if (!sock_assoc(c, si, &port, &key, &skip)) { W->last_error = 10045; RET(0); return; }
  if (g_socks[si].state != 3) { W->last_error = 10022; RET(0); return; }
  if (spend_add(c, 4, si, port, key, ov, ARG(2), (u32)ARG(4) + (u32)ARG(5), ai) < 0) { W->last_error = 10055; RET(0); return; } W->last_error = 997; RET(0); }
/* GetAcceptExSockaddrs(buf, datalen, local_len, remote_len, &local, &local_len, &remote, &remote_len) */
static void s_GetAcceptExSockaddrs(Cpu *c) { u64 buf = ARG(0) + ARG(1); u32 ll = (u32)ARG(2); if (ARG(4)) wrp(c, ARG(4), buf); if (ARG(5)) wr32(c, ARG(5), 16); if (ARG(6)) wrp(c, ARG(6), buf + ll); if (ARG(7)) wr32(c, ARG(7), 16); RET(0); }
static void s_DisconnectEx(Cpu *c) { int si = sock_of(c, ARG(0)); if (si >= 0) sock_shutdown(&g_socks[si], 2); RET(1); }
/* WSAIoctl(s, code, in, inlen, out, outlen, &ret, ov, routine) */
static void s_WSAIoctl(Cpu *c) { SOCK_ARG0(si); u32 code = (u32)ARG(1); u64 in = ARG(2), out = ARG(4); u64 retp = ARG(6);
  if (code == 0xC8000006u) {   /* SIO_GET_EXTENSION_FUNCTION_POINTER: GUID Data1 picks the function */
    u32 d1 = rd32(c, in); const char *fn = 0;
    if (d1 == 0x25a207b9) fn = "ConnectEx"; else if (d1 == 0xb5367df1) fn = "AcceptEx"; else if (d1 == 0xb5367df2) fn = "GetAcceptExSockaddrs"; else if (d1 == 0x7fda2e11) fn = "DisconnectEx"; else if (d1 == 0xb5367df0) fn = "TransmitFile";
    if (!fn) { W->last_error = 10045; RET(~0ull); return; }
    wrp(c, out, w32_shim_slot(W, "mswsock", fn)); if (retp) wr32(c, retp, PSZ_OF(c)); RET(0); return; }
  if (code == 0x48000022u) { wrp(c, out, ARG(0)); if (retp) wr32(c, retp, PSZ_OF(c)); RET(0); return; }   /* SIO_BASE_HANDLE */
  if (code == 0x98000004u || code == 0x9800000Cu || code == 0x9800000Fu) { if (retp) wr32(c, retp, 0); RET(0); return; }   /* SIO_KEEPALIVE_VALS, SIO_UDP_CONNRESET, TCP_INITIAL_RTO */
  (void)si; W->last_error = 10045; RET(~0ull); }
static void s_WSAGetLastError(Cpu *c) { RET(W->last_error); }
static void s_WSASetLastError(Cpu *c) { W->last_error = (u32)ARG(0); RET(0); }
static u32 ntstatus_to_wsa(u32 st) { switch (st) { case 0xC0000236u: return 10061; case 0xC00000B5u: return 10060; case 0xC000023Du: return 10065; case 0xC000023Cu: return 10051; case 0xC000020Du: return 10054; case 0xC0000241u: return 10053; case 0xC0000120u: return 995; default: return 10054; } }
/* WSAGetOverlappedResult(s, ov, &bytes, fWait, &flags): like GetOverlappedResult; fWait re-schedules the thread until the op completed */
static void s_WSAGetOverlappedResult(Cpu *c) { Win32 *w = W; u64 ov = ARG(1); if (!ov) { w->sleeping = 0; w->last_error = 10014; RET(0); return; } u64 st = rdp(c, ov);
  if (st == 0x103) { if (ARG(3)) { w->sleeping = 1; w->sleep_until_ms = host_now_ms() + 2; w->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; } w->sleeping = 0; w->last_error = 996; RET(0); return; }
  w->sleeping = 0; if (ARG(2)) wr32(c, ARG(2), (u32)rdp(c, OV_HIGH(ov))); if (ARG(4)) wr32(c, ARG(4), 0);
  if ((u32)st && ((u32)st & 0xC0000000u) == 0xC0000000u) { w->last_error = ntstatus_to_wsa((u32)st); RET(0); return; } RET(1); }

/* ---- name resolution: ADDRINFO(W) { int flags, family, socktype, protocol; size_t addrlen; canonname*; addr*; next* } 48 B + sockaddr_in 16 B ---- */
static int service_port(const char *svc) { if (!svc || !*svc) return 0; if (svc[0] >= '0' && svc[0] <= '9') { int p = 0; while (*svc >= '0' && *svc <= '9') p = p * 10 + (*svc++ - '0'); return p; }
  if (!e_stricmp(svc, "http") || !e_stricmp(svc, "www")) return 80; if (!e_stricmp(svc, "https")) return 443; if (!e_stricmp(svc, "ftp")) return 21; if (!e_stricmp(svc, "ssh")) return 22; if (!e_stricmp(svc, "domain")) return 53; return 0; }
static int parse_ip4(const char *s, u8 *ip) { int part = 0, v = -1; for (;; s++) { if (*s >= '0' && *s <= '9') { v = (v < 0 ? 0 : v * 10) + (*s - '0'); if (v > 255) return 0; } else if (*s == '.' || !*s) { if (v < 0 || part > 3) return 0; ip[part++] = (u8)v; v = -1; if (!*s) break; } else return 0; } return part == 4; }
static int resolve_build(Cpu *c, const char *node, const char *svc, u64 hints, u64 outp, int wide) {
  int flags = hints ? (int)rd32(c, hints) : 0, family = hints ? (int)rd32(c, hints + 4) : 0, socktype = hints ? (int)rd32(c, hints + 8) : 0, proto = hints ? (int)rd32(c, hints + 12) : 0;
  if (family != 0 && family != 2) return 10047;
  int port = service_port(svc); u8 ips[16 * 4]; int n = 0;
  if (!node || !*node) { u8 lo[4] = {127,0,0,1}, any[4] = {0,0,0,0}; e_memcpy(ips, (flags & 1) ? any : lo, 4); n = 1; }
  else if (parse_ip4(node, ips)) n = 1;
  else if (!e_stricmp(node, "localhost")) { u8 lo[4] = {127,0,0,1}; e_memcpy(ips, lo, 4); n = 1; }
  else if (flags & 4) return 11001;   /* AI_NUMERICHOST with a name */
  else { n = host_net_resolve(node, ips, 16); if (n < 0) return 11002; if (n == 0) return 11001; }   /* WSATRY_AGAIN / WSAHOST_NOT_FOUND */
  int types = 0; if (socktype == 1 || (socktype == 0 && proto == 6)) types = 1; else if (socktype == 2 || (socktype == 0 && proto == 17)) types = 2; else types = 1;   /* one entry per address (stream) */
  (void)wide; u64 first = 0, prev = 0;
  for (int i = 0; i < n; i++) { u64 a = w32_heap_alloc(W, 64, 1); if (!a) break;
    wr32(c, a, 0); wr32(c, a + 4, 2); wr32(c, a + 8, types == 2 ? 2 : 1); wr32(c, a + 12, types == 2 ? 17 : 6);
    if (c->mode32) { wr32(c, a + 16, 16); wr32(c, a + 20, 0); wr32(c, a + 24, (u32)a + 32); wr32(c, a + 28, 0); write_sockaddr(c, a + 32, ips + i * 4, port); if (prev) wr32(c, prev + 28, (u32)a); }
    else { wr64(c, a + 16, 16); wr64(c, a + 24, 0); wr64(c, a + 32, a + 48); wr64(c, a + 40, 0); write_sockaddr(c, a + 48, ips + i * 4, port); if (prev) wr64(c, prev + 40, a); }
    if (!prev) first = a; prev = a; }
  if (!first) return 10055; wrp(c, outp, first); return 0; }
static void s_GetAddrInfoW(Cpu *c) { char node[256], svc[64]; const char *np = 0, *sp = 0;
  if (ARG(0)) { wstr_to_utf8(c, ARG(0), node, sizeof node, -1); np = node; } if (ARG(1)) { wstr_to_utf8(c, ARG(1), svc, sizeof svc, -1); sp = svc; }
  RET(resolve_build(c, np, sp, ARG(2), ARG(3), 1)); }
static void s_getaddrinfo(Cpu *c) { char node[256], svc[64]; const char *np = 0, *sp = 0;
  if (ARG(0)) { const char *t = name_a(c, ARG(0)); e_snprintf(node, sizeof node, "%s", t); np = node; } if (ARG(1)) { const char *t = name_a(c, ARG(1)); e_snprintf(svc, sizeof svc, "%s", t); sp = svc; }
  RET(resolve_build(c, np, sp, ARG(2), ARG(3), 0)); }
#define ADDRINFO_NEXT (c->mode32 ? 28 : 40)
static void s_FreeAddrInfoW(Cpu *c) { u64 a = ARG(0); int guard = 0; while (a && guard++ < 64) { u64 nx = rdp(c, a + ADDRINFO_NEXT); w32_heap_free(W, a); a = nx; } RET(0); }
/* GetNameInfoW(sa, salen, host, hostlen, serv, servlen, flags): numeric forms only */
static void s_GetNameInfoW(Cpu *c) { u8 ip[4]; int port; if (!read_sockaddr(c, ARG(0), ip, &port)) { RET(10047); return; }
  char h[32]; e_snprintf(h, sizeof h, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); if (ARG(2) && ARG(3) > 16) put_w(c, ARG(2), h, 0);
  if (ARG(4) && ARG(5) > 6) { e_snprintf(h, sizeof h, "%d", port); put_w(c, ARG(4), h, 0); } RET(0); }
static void s_inet_addr(Cpu *c) { const char *s = name_a(c, ARG(0)); u8 ip[4]; if (!parse_ip4(s, ip)) { RET(0xffffffffu); return; } RET((u32)ip[0] | ((u32)ip[1] << 8) | ((u32)ip[2] << 16) | ((u32)ip[3] << 24)); }
static void s_inet_pton(Cpu *c) { if ((int)ARG(0) != 2) { RET(0); return; } const char *s = name_a(c, ARG(1)); u8 ip[4]; if (!parse_ip4(s, ip)) { RET(0); return; } for (int i = 0; i < 4; i++) wr8(c, ARG(2) + i, ip[i]); RET(1); }
static void s_inet_ntop(Cpu *c) { if ((int)ARG(0) != 2) { RET(0); return; } char h[32]; e_snprintf(h, sizeof h, "%d.%d.%d.%d", rd8(c, ARG(1)), rd8(c, ARG(1) + 1), rd8(c, ARG(1) + 2), rd8(c, ARG(1) + 3)); if (ARG(3) <= e_strlen(h)) { RET(0); return; } put_a(c, ARG(2), h, 0); RET(ARG(2)); }

/* ---- scheduler hook: finish parked socket operations ---- */
static void spend_finish(SockOp *o, u32 status, u32 bytes, u32 err) {
  u8 *ovp = o->ov ? mem_ptr(o->w->mem, o->ov, 16, 1) : 0;
  if (ovp) { int ps = o->w->mode32 ? 4 : 8; u64 st = status, by = bytes; for (int i = 0; i < ps; i++) { ovp[i] = (u8)(st >> (8 * i)); ovp[ps + i] = (u8)(by >> (8 * i)); } }   /* OVERLAPPED.Internal / InternalHigh are pointer-sized */
  iocp_post(o->port, o->key, o->ov, bytes, err); o->used = 0; g_net_pending--; }
static void net_poll(void) {
  net_pump_all();
  if (!g_net_pending) return;
  for (int k = 0; k < SPEND_MAX; k++) {
    SockOp *o = &g_spend[k]; if (!o->used) continue;
    Sock *s = &g_socks[o->sock]; if (!s->used || s->host < 0) { spend_finish(o, 0xC0000241u, 0, 10053); continue; }
    if (o->kind == 3) { int st = host_net_status(s->host); if (st == 0) continue; if (st > 0) { s->state = 2; spend_finish(o, 0, 0, 0); } else { s->state = 4; s->err = -st; spend_finish(o, wsa_to_ntstatus(-st), 0, (u32)-st); } continue; }
    if (o->kind == 5) { int f = s->txlen > s->txoff ? sock_flush(s) : 1; if (f < 0) spend_finish(o, 0xC000020Du, 0, 10054); else if (s->tx_sent >= o->target) spend_finish(o, 0, o->len, 0); continue; }
    if (o->kind == 1) { if (!sock_readable(s)) continue; spend_finish(o, 0, 0, 0); continue; }
    if (o->kind == 2) { if (!sock_readable(s)) continue;
      Mem *m = o->w->mem; s64 r = -3;
      { u8 *p = o->len ? mem_ptr(m, o->buf, o->len, 1) : 0; if (p) r = sock_read(s, p, o->len); }   /* o->buf/o->len: the data buffer captured at WSARecv time */
      if (r == -2) continue; if (r < 0) spend_finish(o, 0xC000020Du, 0, 10054); else spend_finish(o, 0, (u32)r, 0); continue; }
    if (o->kind == 4) { u8 ip[4]; int port = 0; int h = host_net_accept(s->host, ip, &port); if (h == -2) continue; if (h < 0) { spend_finish(o, 0xC0000241u, 0, 10053); continue; }
      Sock *a = &g_socks[o->acc]; if (!a->used) { host_net_close(h); spend_finish(o, 0xC0000241u, 0, 10053); continue; }
      if (a->host >= 0) host_net_close(a->host); a->host = h; a->state = 2; a->nonblock = 1;
      Mem *m = o->w->mem; u8 *buf = o->buf ? mem_ptr(m, o->buf, o->len, 1) : 0;
      if (buf && o->len >= 32) { u8 lip[4] = {0,0,0,0}; int lport = 0; host_net_name(h, 0, lip, &lport); u32 half = o->len / 2;
        e_memset(buf, 0, o->len); buf[0] = 2; buf[2] = (u8)(lport >> 8); buf[3] = (u8)lport; e_memcpy(buf + 4, lip, 4); buf[half] = 2; buf[half + 2] = (u8)(port >> 8); buf[half + 3] = (u8)port; e_memcpy(buf + half + 4, ip, 4); }
      spend_finish(o, 0, 0, 0); continue; }
  }
}
/* ---- select() / WSAPoll(): readiness polling (libuv's slow-poll thread uses select on providers without AFD fast poll; Bun's uSockets sits on uv_poll) ----
 * mask bits: 1 readable (data / EOF / accept pending), 2 writable (connected), 4 exceptional (connect failed) */
static int sock_ready_mask(int si) {
  Sock *s = &g_socks[si]; if (!s->used) return 4; int m = 0;
  if (s->state == 1 && s->host >= 0) { int st = host_net_status(s->host); if (st > 0) s->state = 2; else if (st < 0) { s->state = 4; s->err = -st; } }
  if (s->state == 4) m |= 4 | 1;
  if (s->state == 2) { m |= 2; if (s->host >= 0 && sock_readable(s)) m |= 1; }
  if (s->state == 3 && s->host >= 0 && host_net_readable(s->host)) m |= 1;
  return m;
}
static int net_select_ready(ThreadCtx *x) { for (int i = 0; i < x->nwobj; i++) if (sock_ready_mask(x->wobj_idx[i]) & x->wobj_kind[i]) return 1; return 0; }
/* fd_set: u32 fd_count; SOCKET fd_array[64] (x64: array at +8, x86: at +4) */
#define FDSET_OFF (PSZ_OF(c))
static void sel_collect(Cpu *c, u64 set, int bit, int *n, int *socks, int *masks) {
  if (!set) return; u32 cnt = rd32(c, set); if (cnt > 64) cnt = 64;
  for (u32 i = 0; i < cnt; i++) { int si = sock_of(c, rdp(c, set + FDSET_OFF + PSZ_OF(c) * i)); if (si < 0) continue; int k; for (k = 0; k < *n; k++) if (socks[k] == si) break; if (k == *n) { if (*n >= 64) return; socks[k] = si; masks[k] = 0; (*n)++; } masks[k] |= bit; }
}
static int sel_writeback(Cpu *c, u64 set, int bit) {
  if (!set) return 0; u32 cnt = rd32(c, set); if (cnt > 64) cnt = 64; u32 out = 0;
  for (u32 i = 0; i < cnt; i++) { u64 h = rdp(c, set + FDSET_OFF + PSZ_OF(c) * i); int si = sock_of(c, h); if (si >= 0 && (sock_ready_mask(si) & bit)) wrp(c, set + FDSET_OFF + PSZ_OF(c) * out++, h); }
  wr32(c, set, out); return (int)out;
}
/* select(nfds, readfds, writefds, exceptfds, timeout) */
static void s_select(Cpu *c) {
  Win32 *w = W; u64 rs = ARG(1), ws = ARG(2), es = ARG(3), tv = ARG(4);
  int n = 0, socks[64], masks[64]; sel_collect(c, rs, 1, &n, socks, masks); sel_collect(c, ws, 2, &n, socks, masks); sel_collect(c, es, 4, &n, socks, masks);
  if (!n) { w->last_error = 10022; wait_end(w); RET(~0ull); return; }
  u32 ms = 0xffffffffu; if (tv) { s64 sec = (s32)rd32(c, tv), us = (s32)rd32(c, tv + 4); s64 t = sec * 1000 + us / 1000; ms = t < 0 ? 0 : t > 0xfffffffell ? 0xfffffffeu : (u32)t; }
  int any = 0; for (int i = 0; i < n; i++) if (sock_ready_mask(socks[i]) & masks[i]) any = 1;
  double dl = wait_begin(c, ms);
  if (!any && !deadline_passed(dl)) { w->nwobj = n; for (int i = 0; i < n; i++) { w->wobj_idx[i] = socks[i]; w->wobj_kind[i] = masks[i]; } park(c, BLOCK_SELECT); return; }
  wait_end(w); int total = sel_writeback(c, rs, 1) + sel_writeback(c, ws, 2) + sel_writeback(c, es, 4); RET((u64)total);
}
static void s___WSAFDIsSet(Cpu *c) { u64 set = ARG(1); if (!set) { RET(0); return; } u32 cnt = rd32(c, set); if (cnt > 64) cnt = 64; for (u32 i = 0; i < cnt; i++) if (rdp(c, set + FDSET_OFF + PSZ_OF(c) * i) == ARG(0)) { RET(1); return; } RET(0); }
/* WSAPoll(fds, n, timeout_ms): WSAPOLLFD { SOCKET fd; short events; short revents } = 16 B; POLLRDNORM 0x100 POLLWRNORM 0x10 POLLERR 1 POLLHUP 2 POLLNVAL 4 */
#define POLLFD_SZ (2 * PSZ_OF(c))
static void s_WSAPoll(Cpu *c) {
  Win32 *w = W; u64 fds = ARG(0); u32 nf = (u32)ARG(1); s32 to = (s32)ARG(2); if (!fds || !nf) { w->last_error = 10022; wait_end(w); RET(~0ull); return; }
  int n = 0, socks[64], masks[64], any = 0;
  for (u32 i = 0; i < nf && n < 64; i++) { u64 h = rdp(c, fds + POLLFD_SZ * i); u16 ev = rd16(c, fds + POLLFD_SZ * i + PSZ_OF(c)); int si = sock_of(c, h); if (si < 0) { any = 1; continue; } int m = 4; if (ev & 0x300) m |= 1; if (ev & 0x10) m |= 2; socks[n] = si; masks[n] = m; n++; if (sock_ready_mask(si) & m) any = 1; }
  double dl = wait_begin(c, to < 0 ? 0xffffffffu : (u32)to);
  if (!any && n && !deadline_passed(dl)) { w->nwobj = n; for (int i = 0; i < n; i++) { w->wobj_idx[i] = socks[i]; w->wobj_kind[i] = masks[i]; } park(c, BLOCK_SELECT); return; }
  wait_end(w); u32 total = 0;
  for (u32 i = 0; i < nf; i++) { u64 h = rdp(c, fds + POLLFD_SZ * i); u16 ev = rd16(c, fds + POLLFD_SZ * i + PSZ_OF(c)), rev = 0; int si = sock_of(c, h);
    if (si < 0) rev = 4; else { int m = sock_ready_mask(si); if ((m & 1) && (ev & 0x300)) rev |= (u16)(ev & 0x300); if ((m & 2) && (ev & 0x10)) rev |= 0x10; if (m & 4) rev |= 1; }
    wr16(c, fds + POLLFD_SZ * i + PSZ_OF(c) + 2, rev); if (rev) total++; }
  RET(total);
}
static void net_drop(Win32 *w) { for (int k = 0; k < SPEND_MAX; k++) if (g_spend[k].used && g_spend[k].w == w) { g_spend[k].used = 0; g_net_pending--; } }
static void sock_cancel_io(Cpu *c, u64 h) { int si = sock_of(c, h); if (si < 0) return; for (int k = 0; k < SPEND_MAX; k++) if (g_spend[k].used && g_spend[k].sock == si && g_spend[k].pid == W->pid) spend_finish(&g_spend[k], 0xC0000120u, 0, 995); }

#define NET_SHIMS \
  S(WSAStartup), SA("WSACleanup", ret0), S(WSAGetLastError), S(WSASetLastError), S(gethostname), S(htons), SA("ntohs", htons), S(htonl), SA("ntohl", htonl), \
  S(socket), SA("WSASocketW", socket), SA("WSASocketA", socket), S(closesocket), S(bind), S(listen), S(connect), S(accept), S(send), S(recv), S(shutdown), S(ioctlsocket), S(setsockopt), S(getsockopt), \
  S(getsockname), S(getpeername), S(WSAIoctl), S(WSASend), S(WSARecv), S(WSAGetOverlappedResult), S(ConnectEx), S(AcceptEx), S(GetAcceptExSockaddrs), S(DisconnectEx), SA("TransmitFile", sock_err), \
  S(GetAddrInfoW), S(FreeAddrInfoW), SA("getaddrinfo", getaddrinfo), SA("freeaddrinfo", FreeAddrInfoW), S(GetNameInfoW), SA("getnameinfo", sock_err), S(inet_addr), SA("inet_ntoa", ret0), S(inet_pton), S(inet_ntop), \
  SA("sendto", sock_err), SA("recvfrom", sock_err), S(select), SA("gethostbyname", ret0), SA("gethostbyaddr", ret0), S(__WSAFDIsSet), SA("WSAEventSelect", sock_err), \
  SA("WSAWaitForMultipleEvents", sock_err), SA("WSAEnumNetworkEvents", sock_err), SA("WSACreateEvent", ret0), SA("WSACloseEvent", ret1), SA("WSAResetEvent", ret1), S(WSAPoll), \
  SA("getservbyname", ret0), SA("getprotobyname", ret0), SA("WSADuplicateSocketW", sock_err), SA("WSAAddressToStringW", sock_err), SA("WSAStringToAddressW", sock_err), SA("WSARecvFrom", sock_err), SA("WSASendTo", sock_err),
