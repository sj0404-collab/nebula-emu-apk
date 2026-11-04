/* ---------- file system + stdin shims (included by win32.c; these entries come first in the table) ---------- */
static Win32 *g_fs_w;   /* the process currently executing (set by the scheduler via w32_set_current) */
void w32_set_current(Win32 *w) { g_fs_w = w; }
static int lc_(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
#define H_FILE_BASE 0x1000ull
#define H_FIND_BASE 0x3000ull
#define FILE_MAGIC  0x4E58F11Eu
#define ERRNO_ADDR  (W->ubuf + 0x258)
static void set_errno(Cpu *c, int v) { wr32(c, ERRNO_ADDR, (u32)v); }
static int errno_of(int rc) { return rc == -2 ? 2 : rc == -17 ? 17 : rc == -21 ? 21 : rc == -13 ? 13 : 24; }
static u32 lasterr_of(int rc) { return rc == -2 ? 2 : rc == -17 ? 80 : rc == -21 ? 5 : rc == -13 ? 5 : 4; }
/* HANDLE → fd: -1 stdin, 1/2 console, ≥3 file, 0 invalid */
static int sa_inherit(Cpu *c, u64 sa) { return sa ? rd32(c, sa + (c->mode32 ? 8 : 16)) != 0 : 0; }   /* SECURITY_ATTRIBUTES: nLength, pad, lpSecurityDescriptor, bInheritHandle at +16 (x64) */
static int handle_fd(u64 h) { if (h == 0x11 || h == 0x12) return (int)(h - 0x10); if (h == 0x10) return -1; if (h >= H_FILE_BASE && h < H_FILE_BASE + 0x1000 && vfs_is_open((int)(h - H_FILE_BASE))) return (int)(h - H_FILE_BASE); return 0; }
#define H_OF(fd) (H_FILE_BASE + (u64)(fd))
static void block_on(Cpu *c, int reason, int arg) { W->block = reason; W->nblock_pids = 0; W->block_pipe = -1; if (reason == BLOCK_PROC) { W->block_pids[0] = arg; W->nblock_pids = 1; } if (reason == BLOCK_PIPE_R || reason == BLOCK_PIPE_W) W->block_pipe = arg; retry_and_yield(c, g_cur_slot); }
/* guest FILE* → fd (std streams live in ubuf; file streams are 48-byte heap blocks with a magic) */
static int file_fd(Cpu *c, u64 f) { u64 iob = W->ubuf, st = IOB_STRIDE; if (f == iob + st) return 1; if (f == iob + 2 * st) return 2; if (f == iob) return -1; if (f < 0x10000) return 0; if (rd32(c, f + 32) == FILE_MAGIC) { int fd = (int)rd32(c, f + 28); return vfs_is_open(fd) ? fd : 0; } return 0; }
static u64 make_file(Cpu *c, int fd) { u64 f = w32_heap_alloc(W, 48, 1); wr32(c, f + 28, (u32)fd); wr32(c, f + 32, FILE_MAGIC); wr32(c, f + 24, 1); return f; }
static void put_a(Cpu *c, u64 dst, const char *s, u32 cap) { size_t l = e_strlen(s); if (cap && l + 1 > cap) l = cap - 1; u8 *p = mem_ptr(c->mem, dst, l + 1, 1); if (p) { e_memcpy(p, s, l); p[l] = 0; } }
static void put_w(Cpu *c, u64 dst, const char *s, u32 cap) { size_t l = e_strlen(s); if (cap && l + 1 > cap) l = cap - 1; for (size_t i = 0; i < l; i++) wr16(c, dst + i * 2, (u8)s[i]); wr16(c, dst + l * 2, 0); }
static const char *gstr(Cpu *c, u64 a) { const char *s = rd_str(c, a, 1 << 30); return s ? s : ""; }
static const char *name_a(Cpu *c, u64 a) { const char *s = rd_str(c, a, 1024); return s ? s : ""; }
static char g_wname[1024];
static const char *name_w(Cpu *c, u64 a) { if (!a) return ""; wstr_to_utf8(c, a, g_wname, sizeof g_wname, -1); return g_wname; }
static u64 vfs_filetime(void) { return (unix_time() + 11644473600ull) * 10000000ull; }

/* generic read/write used by ReadFile/_read/fread */
/* -2 = would block (caller must call blocked_io() and return), -3 = broken pipe */
static int g_last_io_fd;
static s64 fd_read(Cpu *c, int fd, u64 buf, u64 n) {
  u8 *p = mem_ptr(c->mem, buf, n, 1); if (!p) return -1;
  fd = vfs_resolve(fd == -1 ? 0 : fd); g_last_io_fd = fd;
  if (fd == -1) { s64 r = vfs_stdin_read(p, n); return r; }
  if (fd >= 3) return vfs_read(fd, p, n);
  return -1;
}
/* after a -2 from fd_read/raw_read/fd_write: park the process on the right object and re-run the shim later */
static void blocked_io(Cpu *c, int for_write) {
  if (g_last_io_fd == -1) block_on(c, BLOCK_STDIN, 0);
  else if (vfs_kind(g_last_io_fd) == FD_PIPE) block_on(c, for_write ? BLOCK_PIPE_W : BLOCK_PIPE_R, vfs_pipe_id(g_last_io_fd));
  else retry_and_yield(c, g_cur_slot);
}
/* write to a resolved fd; pipes may take only part of the data: remember the progress so the retried shim continues
   where it left off (-2 = blocked, caller parks the process; -3 = no reader / EPIPE) */
static s64 resolved_write(Cpu *c, int fd, const u8 *p, u64 n) {
  if (fd == 1 || fd == 2) { con_out(W, fd, (const char *)p, n); return (s64)n; }
  if (fd < 3) return -1;
  if (vfs_kind(fd) != FD_PIPE) return vfs_write(fd, p, n);
  Win32 *w = W; u64 done = 0;
  if (w->pw_active && w->pw_rip == c->rip && w->pw_fd == fd && w->pw_total == n) done = w->pw_done;
  while (done < n) {
    s64 r = vfs_write(fd, p + done, n - done);
    if (r == -2) { w->pw_active = 1; w->pw_rip = c->rip; w->pw_fd = fd; w->pw_total = n; w->pw_done = done; return -2; }
    if (r < 0) { w->pw_active = 0; return done ? (s64)done : r; }
    done += (u64)r;
  }
  w->pw_active = 0; return (s64)n;
}
static s64 fd_write(Cpu *c, int fd, u64 buf, u64 n) {
  const u8 *p = mem_ptr(c->mem, buf, n, 0); if (!p) return -1;
  fd = vfs_resolve(fd == -1 ? 0 : fd); g_last_io_fd = fd;
  return resolved_write(c, fd, p, n);
}
static s64 raw_read(int fd, u8 *p, u64 n) { fd = vfs_resolve(fd == -1 ? 0 : fd); g_last_io_fd = fd; if (fd == -1) return vfs_stdin_read(p, n); if (fd >= 3) return vfs_read(fd, p, n); return -1; }
static s64 raw_write(int fd, const u8 *p, u64 n) { fd = vfs_resolve(fd == -1 ? 0 : fd); g_last_io_fd = fd; return resolved_write(g_fs_w->cpu, fd, p, n); }
/* stdio-style write: 1 done, 0 blocked (process parked, shim will be retried), -1 error (errno set) */
static int out_write(Cpu *c, int fd, const u8 *p, u64 n) { s64 r = raw_write(fd, p, n); if (r == -2) { blocked_io(c, 1); return 0; } if (r == -3) { set_errno(c, 32); return -1; } if (r < 0) { set_errno(c, 9); return -1; } return 1; }
/* ---- KERNEL32 ---- */
/* Rust std's anonymous pipes are named pipes: CreateNamedPipeW("\\\\.\\pipe\\__rust_anonymous_pipe1__…") for "ours" + CreateFileW(same name) for "theirs" */
#define NPIPE_MAX 16
static struct { char name[160]; int fd; int used; } g_npipes[NPIPE_MAX];
/* "\\\\.\\pipe\\name" and "\\\\?\\pipe\\name" (libuv) are the same namespace; names are compared after this prefix */
static int npipe_prefix(const char *n) { static const char pre[] = "\\\\.\\pipe\\"; for (int i = 0; pre[i]; i++) if (!n[i] || (n[i] != pre[i] && !(pre[i] == '\\' && n[i] == '/') && !(i == 2 && n[i] == '?'))) return 0; return 1; }
static int npipe_same(const char *a, const char *b) { return !e_stricmp(a + 4, b + 4); }
static void create_named_pipe(Cpu *c, const char *n) {
  u32 mode = (u32)ARG(1);
  if (!npipe_prefix(n)) { char m[300]; e_snprintf(m, sizeof m, "[x64] CreateNamedPipe: unsupported name '%s'\n", n); host_write(3, m, e_strlen(m)); W->last_error = 123; RET(~0ull); return; }
  for (int k = 0; k < NPIPE_MAX; k++) if (g_npipes[k].used && npipe_same(g_npipes[k].name, n)) { W->last_error = 5; RET(~0ull); return; }   /* FILE_FLAG_FIRST_PIPE_INSTANCE: name in use */
  int r, wfd; if (vfs_pipe(&r, &wfd)) { W->last_error = 8; RET(~0ull); return; }
  int ours = (mode & 3) == 2 ? wfd : r, theirs = ours == r ? wfd : r;   /* PIPE_ACCESS_INBOUND (1): the server reads; OUTBOUND (2): it writes */
  vfs_set_inherit(ours, sa_inherit(c, ARG(7))); vfs_set_inherit(theirs, 0);
  int k; for (k = 0; k < NPIPE_MAX && g_npipes[k].used; k++) { } if (k == NPIPE_MAX) { vfs_close(r); vfs_close(wfd); W->last_error = 8; RET(~0ull); return; }
  g_npipes[k].used = 1; g_npipes[k].fd = theirs; e_snprintf(g_npipes[k].name, sizeof g_npipes[k].name, "%s", n);
  RET(H_OF(ours));
}
static void s_CreateNamedPipeW(Cpu *c) { create_named_pipe(c, name_w(c, ARG(0))); }   /* Rust std */
static void s_CreateNamedPipeA(Cpu *c) { create_named_pipe(c, name_a(c, ARG(0))); }   /* libuv: "\\\\.\\pipe\\uv\\%p-%lu" for every stdio pipe of uv_spawn */
static void s_ConnectNamedPipe(Cpu *c) { RET(1); }
static void create_file(Cpu *c, const char *n) {
  u32 access = (u32)ARG(1), disp = (u32)ARG(4);
  if (!e_stricmp(n, "CONOUT$")) { RET(0x11); return; } if (!e_stricmp(n, "CONIN$")) { RET(0x10); return; }
  if (npipe_prefix(n)) { for (int k = 0; k < NPIPE_MAX; k++) if (g_npipes[k].used && npipe_same(g_npipes[k].name, n)) { int fd = g_npipes[k].fd; g_npipes[k].used = 0; vfs_set_inherit(fd, sa_inherit(c, ARG(3))); RET(H_OF(fd)); return; } W->last_error = 2; RET(~0ull); return; }
  int acc = ((access & 0x80000000u) ? 1 : 0) | ((access & 0x40000004u) ? 2 : 0); if (!acc) acc = 1;
  int fd = vfs_open(n, acc, (int)disp, (access & 4) && !(access & 0x40000000u));
  if (fd < 0) { W->last_error = lasterr_of(fd); RET(~0ull); return; }
  vfs_set_inherit(fd, sa_inherit(c, ARG(3))); if (ARG(5) & 0x04000000u) vfs_delete_on_close(fd, 1);   /* FILE_FLAG_DELETE_ON_CLOSE */
  W->last_error = (disp == 4 || disp == 2) && vfs_tell(fd) == 0 ? 0 : 0; RET(H_FILE_BASE + fd);
}
static void s_CreateFileA(Cpu *c) { create_file(c, name_a(c, ARG(0))); }
static void s_CreateFileW(Cpu *c) { create_file(c, name_w(c, ARG(0))); }
/* synchronous handle + OVERLAPPED: the I/O happens at Offset/OffsetHigh (SQLite, 7-Zip and most "pread"-style code rely on this) */
static void overlapped_seek(Cpu *c, int fd, u64 ov) { if (ov && fd >= 3) { u64 off = rd32(c, OV_OFFSET(ov)) | ((u64)rd32(c, OV_OFFSET(ov) + 4) << 32); vfs_seek(fd, (s64)off, 0); } }
static void s_ReadFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); if (!fd || fd == 1 || fd == 2) { W->last_error = 6; RET(0); return; }
  if (iocp_rw(c, fd, 0, ARG(1), (u32)ARG(2), ARG(3), ARG(4))) return;
  overlapped_seek(c, fd, ARG(4));
  s64 r = fd_read(c, fd, ARG(1), (u32)ARG(2));
  if (r == -2) { blocked_io(c, 0); return; }
  if (r < 0) { W->last_error = 5; RET(0); return; }
  if (ARG(3)) wr32(c, ARG(3), (u32)r); if (r == 0 && vfs_kind(g_last_io_fd) == FD_PIPE) { W->last_error = 109; RET(0); return; } RET(1);
}
/* ReadFileEx/WriteFileEx(handle, buf, len, OVERLAPPED*, completion): the I/O completes synchronously here; the completion routine
   (dwErrorCode, dwBytes, lpOverlapped) is queued as a user APC and runs at the next alertable SleepEx — exactly Rust std's AnonPipe::read */
static void rw_ex(Cpu *c, int write) {
  int fd = handle_fd(ARG(0)); u64 ov = ARG(3), cb = ARG(4); if (!fd || (!write && (fd == 1 || fd == 2))) { W->last_error = 6; RET(0); return; }
  overlapped_seek(c, fd, ov);
  s64 r = write ? fd_write(c, fd, ARG(1), (u32)ARG(2)) : fd_read(c, fd, ARG(1), (u32)ARG(2));
  if (r == -2) { blocked_io(c, write); return; }
  u32 err = 0; if (r == -3) { err = 109; r = 0; } else if (r < 0) { err = 5; r = 0; } else if (!write && r == 0 && (u32)ARG(2) && vfs_kind(g_last_io_fd) == FD_PIPE) err = 109;
  if (ov) { ov_set(c, ov, err ? (err == 109 ? 0xC000014Bu : 0xC0000022u) : 0, (u64)r); }
  if (cb) queue_apc(W, cb, err, (u64)r, ov);
  RET(1);
}
static void s_ReadFileEx(Cpu *c) { rw_ex(c, 0); }
static void s_WriteFileEx(Cpu *c) { rw_ex(c, 1); }
/* ntdll!Nt{Read,Write}File(handle, event, apc, apcctx, IOSB*, buf, len, ByteOffset*, key) — Rust std uses them for File::read/write(_at) */
static void nt_rw(Cpu *c, int write) {
  int fd = handle_fd(ARG(0)); u64 iosb = ARG(4), buf = ARG(5), off = ARG(7); u32 len = (u32)ARG(6);
  if (!fd || (!write && (fd == 1 || fd == 2))) { RET(0xC0000008u); return; }   /* STATUS_INVALID_HANDLE */
  if (off && fd >= 3) { s64 o = (s64)rd64(c, off); if (o >= 0) vfs_seek(fd, o, 0); }
  s64 r = write ? fd_write(c, fd, buf, len) : fd_read(c, fd, buf, len);
  if (r == -2) { blocked_io(c, write); return; }
  if (r == -3) { ov_set(c, iosb, 0xC000014Bu, 0); RET(0xC000014Bu); return; }   /* STATUS_PIPE_BROKEN */
  if (r < 0) { ov_set(c, iosb, 0xC0000022u, 0); RET(0xC0000022u); return; }     /* STATUS_ACCESS_DENIED */
  if (!write && r == 0 && len) { u32 st = vfs_kind(g_last_io_fd) == FD_PIPE ? 0xC000014Bu : 0xC0000011u; ov_set(c, iosb, st, 0); RET(st); return; }   /* STATUS_END_OF_FILE */
  ov_set(c, iosb, 0, (u64)r); RET(0);
}
static void s_NtReadFile(Cpu *c) { nt_rw(c, 0); }
static void s_NtWriteFile(Cpu *c) { nt_rw(c, 1); }
static void s_WriteFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); if (fd <= 0) { W->last_error = 6; RET(0); return; }
  if (iocp_rw(c, fd, 1, ARG(1), (u32)ARG(2), ARG(3), ARG(4))) return;
  overlapped_seek(c, fd, ARG(4));
  s64 r = fd_write(c, fd, ARG(1), (u32)ARG(2)); if (r == -2) { blocked_io(c, 1); return; } if (r == -3) { W->last_error = 232; RET(0); return; } if (r < 0) { W->last_error = 5; RET(0); return; }
  if (ARG(3)) wr32(c, ARG(3), (u32)r); RET(1);
}
static void s_CloseHandle(Cpu *c) { u64 h = ARG(0); if (h >= H_FILE_BASE && h < H_FILE_BASE + 0x1000) { int fd = (int)(h - H_FILE_BASE); int ko = vfs_fd_kobj(fd); if (ko >= 0) { KObj *k = kobj_get(ko); if (k && k->kind == KO_IOCP && k->refs <= 1) iocp_kobj_closed(ko); } else iocp_handle_closed(W->pid, fd); vfs_close(fd); } RET(1); }
static void s_GetFileType(Cpu *c) { int fd = handle_fd(ARG(0)); if (fd && fd < 3) { int r = vfs_resolve(fd < 0 ? 0 : fd); if (r >= 3) fd = r; } if (fd >= 3) { int k = vfs_kind(fd); RET(k == FD_PIPE ? 3 : k == FD_CONSOLE || k == FD_NULL ? 2 : k == FD_FILE ? 1 : 0); return; } RET(fd ? 2 : 0); }
static void s_GetFileSize(Cpu *c) { int fd = handle_fd(ARG(0)); s64 s = fd >= 3 ? vfs_size(fd) : -1; if (s < 0) { W->last_error = 6; RET(0xffffffffu); return; } if (ARG(1)) wr32(c, ARG(1), (u32)((u64)s >> 32)); RET((u32)s); }
static void s_GetFileSizeEx(Cpu *c) { int fd = handle_fd(ARG(0)); s64 s = fd >= 3 ? vfs_size(fd) : -1; if (s < 0) { W->last_error = 6; RET(0); return; } wr64(c, ARG(1), (u64)s); RET(1); }
static void s_SetFilePointer(Cpu *c) { int fd = handle_fd(ARG(0)); s64 off = (s32)(u32)ARG(1); if (ARG(2)) off |= (s64)rd32(c, ARG(2)) << 32; s64 r = fd >= 3 ? vfs_seek(fd, off, (int)ARG(3)) : -1; if (r < 0) { W->last_error = 6; RET(0xffffffffu); return; } if (ARG(2)) wr32(c, ARG(2), (u32)((u64)r >> 32)); RET((u32)r); }
static void s_SetFilePointerEx(Cpu *c) { int fd = handle_fd(ARG(0)); s64 r = fd >= 3 ? vfs_seek(fd, (s64)ARG(1), (int)ARG(3)) : -1; if (r < 0) { W->last_error = 6; RET(0); return; } if (ARG(2)) wr64(c, ARG(2), (u64)r); RET(1); }
static void s_SetEndOfFile(Cpu *c) { int fd = handle_fd(ARG(0)); RET(fd >= 3 && vfs_truncate(fd)); }
static u32 attrs_of(int st) { return st == 2 ? 0x10 : 0x80; }
static void file_attrs(Cpu *c, const char *n) { u64 sz; int st = vfs_stat(n, &sz); if (!st) { W->last_error = 2; RET(0xffffffffu); return; } RET(attrs_of(st)); }
static void s_GetFileAttributesA(Cpu *c) { file_attrs(c, name_a(c, ARG(0))); }
static void s_GetFileAttributesW(Cpu *c) { file_attrs(c, name_w(c, ARG(0))); }
static void file_attrs_ex(Cpu *c, const char *n) { u64 sz; int st = vfs_stat(n, &sz); if (!st) { W->last_error = 2; RET(0); return; } u64 a = ARG(2); u64 ft = vfs_filetime(); wr32(c, a, attrs_of(st)); wr64(c, a + 4, ft); wr64(c, a + 12, ft); wr64(c, a + 20, ft); wr32(c, a + 28, (u32)(sz >> 32)); wr32(c, a + 32, (u32)sz); RET(1); }
static void s_GetFileAttributesExA(Cpu *c) { file_attrs_ex(c, name_a(c, ARG(0))); }
static void s_GetFileAttributesExW(Cpu *c) { file_attrs_ex(c, name_w(c, ARG(0))); }
static void s_GetFileInformationByHandle(Cpu *c) { int fd = handle_fd(ARG(0)); if (fd < 3) { W->last_error = 6; RET(0); return; } u64 a = ARG(1); u64 ft = vfs_filetime(); s64 sz = vfs_size(fd); wr32(c, a, vfs_fd_isdir(fd) ? 0x10 : 0x80); wr64(c, a + 4, ft); wr64(c, a + 12, ft); wr64(c, a + 20, ft); wr32(c, a + 28, 0x4e45); wr32(c, a + 32, (u32)((u64)sz >> 32)); wr32(c, a + 36, (u32)sz); wr32(c, a + 40, 1); wr32(c, a + 44, 0); wr32(c, a + 48, (u32)fd); RET(1); }
/* GetFileInformationByHandleEx(h, class, buf, size): FileBasicInfo 0, FileStandardInfo 1, FileNameInfo 2, FileAttributeTagInfo 9, FileIdInfo 18 */
static void s_GetFileInformationByHandleEx(Cpu *c) {
  int fd = handle_fd(ARG(0)); u32 cls = (u32)ARG(1); u64 a = ARG(2), n = ARG(3); if (fd < 3) { W->last_error = 6; RET(0); return; }
  u64 ft = vfs_filetime(); s64 sz = vfs_size(fd); int dir = vfs_fd_isdir(fd);
  switch (cls) {
  case 0: if (n < 40) { W->last_error = 24; RET(0); return; } wr64(c, a, ft); wr64(c, a + 8, ft); wr64(c, a + 16, ft); wr64(c, a + 24, ft); wr32(c, a + 32, dir ? 0x10 : 0x80); wr32(c, a + 36, 0); RET(1); return;
  case 1: if (n < 24) { W->last_error = 24; RET(0); return; } wr64(c, a, (u64)sz); wr64(c, a + 8, (u64)sz); wr32(c, a + 16, 1); wr8(c, a + 20, 0); wr8(c, a + 21, (u8)dir); RET(1); return;
  case 2: { const char *pn = vfs_fd_path(fd); if (!pn) pn = ""; size_t l = e_strlen(pn); if (n < 4 + l * 2) { W->last_error = 24; RET(0); return; } wr32(c, a, (u32)(l * 2)); put_w(c, a + 4, pn, 0); RET(1); return; }
  case 9: if (n < 8) { W->last_error = 24; RET(0); return; } wr32(c, a, dir ? 0x10 : 0x80); wr32(c, a + 4, 0); RET(1); return;
  case 18: if (n < 24) { W->last_error = 24; RET(0); return; } wr64(c, a, 0x4e45); wr64(c, a + 8, (u64)fd + 1); wr64(c, a + 16, 0); RET(1); return;
  default: W->last_error = 87; RET(0); return;   /* ERROR_INVALID_PARAMETER */
  }
}
static void s_GetFileTime(Cpu *c) { u64 ft = vfs_filetime(); if (ARG(1)) wr64(c, ARG(1), ft); if (ARG(2)) wr64(c, ARG(2), ft); if (ARG(3)) wr64(c, ARG(3), ft); RET(1); }
/* FindFirstFile family: data layout A = 320 bytes, W = 592 bytes */
static void fill_find(Cpu *c, u64 a, const char *name, u64 sz, int dir, int wide) {
  u64 ft = vfs_filetime(); wr32(c, a, dir ? 0x10 : 0x80); wr64(c, a + 4, ft); wr64(c, a + 12, ft); wr64(c, a + 20, ft); wr32(c, a + 28, (u32)(sz >> 32)); wr32(c, a + 32, (u32)sz); wr32(c, a + 36, 0); wr32(c, a + 40, 0);
  if (wide) { put_w(c, a + 44, name, 260); wr16(c, a + 44 + 520, 0); } else { put_a(c, a + 44, name, 260); wr8(c, a + 44 + 260, 0); }
}
static void find_first(Cpu *c, const char *pat, u64 data, int wide) {
  int h = vfs_find_first(pat); char name[260]; u64 sz; int dir;
  if (h < 0 || !vfs_find_next(h, name, sizeof name, &sz, &dir)) { if (h >= 0) vfs_find_close(h); W->last_error = 2; RET(~0ull); return; }
  fill_find(c, data, name, sz, dir, wide); RET(H_FIND_BASE + h);
}
static void s_FindFirstFileA(Cpu *c) { find_first(c, name_a(c, ARG(0)), ARG(1), 0); }
static void s_FindFirstFileW(Cpu *c) { find_first(c, name_w(c, ARG(0)), ARG(1), 1); }
static void s_FindFirstFileExA(Cpu *c) { find_first(c, name_a(c, ARG(0)), ARG(2), 0); }
static void s_FindFirstFileExW(Cpu *c) { find_first(c, name_w(c, ARG(0)), ARG(2), 1); }
static void find_next(Cpu *c, int wide) { u64 h = ARG(0); char name[260]; u64 sz; int dir; if (h < H_FIND_BASE || h >= H_FIND_BASE + 64 || !vfs_find_next((int)(h - H_FIND_BASE), name, sizeof name, &sz, &dir)) { W->last_error = 18; RET(0); return; } fill_find(c, ARG(1), name, sz, dir, wide); RET(1); }
static void s_FindNextFileA(Cpu *c) { find_next(c, 0); }
static void s_FindNextFileW(Cpu *c) { find_next(c, 1); }
static void s_FindClose(Cpu *c) { u64 h = ARG(0); if (h >= H_FIND_BASE && h < H_FIND_BASE + 64) vfs_find_close((int)(h - H_FIND_BASE)); RET(1); }
static void s_DeleteFileA(Cpu *c) { if (vfs_remove(name_a(c, ARG(0)))) RET(1); else { W->last_error = 2; RET(0); } }
static void s_DeleteFileW(Cpu *c) { if (vfs_remove(name_w(c, ARG(0)))) RET(1); else { W->last_error = 2; RET(0); } }
static void s_CreateDirectoryA(Cpu *c) { if (vfs_mkdir(name_a(c, ARG(0)))) RET(1); else { W->last_error = 183; RET(0); } }
static void s_CreateDirectoryW(Cpu *c) { if (vfs_mkdir(name_w(c, ARG(0)))) RET(1); else { W->last_error = 183; RET(0); } }
static void s_RemoveDirectoryA(Cpu *c) { RET(vfs_remove(name_a(c, ARG(0)))); }
static void s_RemoveDirectoryW(Cpu *c) { RET(vfs_remove(name_w(c, ARG(0)))); }
static void s_MoveFileA(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_a(c, ARG(0))); RET(vfs_rename(from, name_a(c, ARG(1)))); }
static void s_MoveFileW(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_w(c, ARG(0))); RET(vfs_rename(from, name_w(c, ARG(1)))); }
static void copy_file(Cpu *c, const char *from, const char *to, int fail_exists) {
  int src = vfs_open(from, 1, 3, 0); if (src < 0) { W->last_error = 2; RET(0); return; }
  int dst = vfs_open(to, 2, fail_exists ? 1 : 2, 0); if (dst < 0) { vfs_close(src); W->last_error = lasterr_of(dst); RET(0); return; }
  u8 buf[4096]; s64 r; while ((r = vfs_read(src, buf, sizeof buf)) > 0) vfs_write(dst, buf, (u64)r);
  vfs_close(src); vfs_close(dst); RET(1);
}
static void s_CopyFileA(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_a(c, ARG(0))); copy_file(c, from, name_a(c, ARG(1)), (int)ARG(2)); }
static void s_CopyFileW(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_w(c, ARG(0))); copy_file(c, from, name_w(c, ARG(1)), (int)ARG(2)); }
static void s_GetCurrentDirectoryA(Cpu *c) { const char *d = vfs_cwd(); size_t l = e_strlen(d); if ((u32)ARG(0) > l) put_a(c, ARG(1), d, 0); RET((u32)ARG(0) > l ? l : l + 1); }
static void s_GetCurrentDirectoryW(Cpu *c) { const char *d = vfs_cwd(); size_t l = e_strlen(d); if ((u32)ARG(0) > l) put_w(c, ARG(1), d, 0); RET((u32)ARG(0) > l ? l : l + 1); }
static void s_SetCurrentDirectoryA(Cpu *c) { if (vfs_chdir(name_a(c, ARG(0)))) RET(1); else { W->last_error = 2; RET(0); } }
static void s_SetCurrentDirectoryW(Cpu *c) { if (vfs_chdir(name_w(c, ARG(0)))) RET(1); else { W->last_error = 2; RET(0); } }
static void full_path(Cpu *c, const char *n, int wide) { char out[1024]; vfs_normalize(n, out, sizeof out); size_t l = e_strlen(out); u32 cap = (u32)ARG(1); if (cap <= l) { RET(l + 1); return; } if (wide) put_w(c, ARG(2), out, 0); else put_a(c, ARG(2), out, 0); if (ARG(3)) { size_t b = l; while (b > 0 && out[b - 1] != '\\') b--; wrp(c, ARG(3), ARG(2) + (wide ? b * 2 : b)); } RET(l); }
/* SearchPath(path, file, ext, cap, buf, &filepart): path = ';'-separated directories, or NULL → directory of the exe, current directory, System32, Windows */
static void search_path(Cpu *c, int wide) { char path[2048], file[520], ext[32]; path[0] = file[0] = ext[0] = 0;
  if (wide) { if (ARG(0)) wstr_to_utf8(c, ARG(0), path, sizeof path, -1); if (ARG(1)) wstr_to_utf8(c, ARG(1), file, sizeof file, -1); if (ARG(2)) wstr_to_utf8(c, ARG(2), ext, sizeof ext, -1); }
  else { const char *q; if (ARG(0) && (q = rd_str(c, ARG(0), 2047))) e_snprintf(path, sizeof path, "%s", q); if (ARG(1) && (q = rd_str(c, ARG(1), 519))) e_snprintf(file, sizeof file, "%s", q); if (ARG(2) && (q = rd_str(c, ARG(2), 31))) e_snprintf(ext, sizeof ext, "%s", q); }
  if (!file[0]) { W->last_error = 87; RET(0); return; } size_t fl = e_strlen(file); int has_dot = 0; for (size_t i = 0; i < fl; i++) if (file[i] == '.') has_dot = 1; else if (file[i] == '\\' || file[i] == '/') has_dot = 0;
  if (ext[0] && !has_dot) e_snprintf(file + fl, sizeof file - fl, "%s", ext);
  char cand[1100], found[1100]; found[0] = 0; u64 sz;
  if (file[1] == ':' || file[0] == '\\' || file[0] == '/') { if (vfs_stat(file, &sz)) vfs_normalize(file, found, sizeof found); }
  else { char dirs[2600]; if (ARG(0)) e_snprintf(dirs, sizeof dirs, "%s", path); else { char ed[600]; e_snprintf(ed, sizeof ed, "%s", W->exe_path); size_t k = e_strlen(ed); while (k && ed[k - 1] != '\\' && ed[k - 1] != '/') k--; ed[k] = 0; e_snprintf(dirs, sizeof dirs, "%s;%s;C:\\Windows\\System32;C:\\Windows", ed, vfs_cwd()); }
    char *d = dirs; while (*d && !found[0]) { char *e = d; while (*e && *e != ';') e++; char save = *e; *e = 0; if (*d) { size_t dl = e_strlen(d); e_snprintf(cand, sizeof cand, "%s%s%s", d, (d[dl - 1] == '\\' || d[dl - 1] == '/') ? "" : "\\", file); if (vfs_stat(cand, &sz)) vfs_normalize(cand, found, sizeof found); } if (!save) break; d = e + 1; } }
  if (!found[0]) { W->last_error = 2; RET(0); return; }
  size_t l = e_strlen(found); u32 cap = (u32)ARG(3); if (cap <= l) { RET(l + 1); return; } if (wide) put_w(c, ARG(4), found, 0); else put_a(c, ARG(4), found, 0);
  if (ARG(5)) { size_t b = l; while (b && found[b - 1] != '\\') b--; wrp(c, ARG(5), ARG(4) + (wide ? b * 2 : b)); } RET(l); }
static void s_SearchPathW(Cpu *c) { search_path(c, 1); }
static void s_SearchPathA(Cpu *c) { search_path(c, 0); }
/* GetFinalPathNameByHandle(h, buf, cap, flags): the normalised path of the open file; VOLUME_NAME_DOS (0) gets the \\?\ prefix */
static void final_path(Cpu *c, int wide) {
  int fd = handle_fd(ARG(0)); if (fd < 3) { W->last_error = 6; RET(0); return; }
  char out[1100]; u32 fl = (u32)ARG(3) & 0xF; e_snprintf(out, sizeof out, "%s%s", fl == 0 ? "\\\\?\\" : "", vfs_fd_path(fd));
  size_t l = e_strlen(out); u32 cap = (u32)ARG(2); if (cap <= l) { RET(l + 1); return; }
  if (wide) put_w(c, ARG(1), out, 0); else put_a(c, ARG(1), out, 0); RET(l);
}
static void s_GetFinalPathNameByHandleA(Cpu *c) { final_path(c, 0); }
static void s_GetFinalPathNameByHandleW(Cpu *c) { final_path(c, 1); }
/* GetLongPathNameW(src, dst, cap): names are already long on the virtual disk → copy */
static void s_GetLongPathNameW(Cpu *c) { char tmp[1024]; e_snprintf(tmp, sizeof tmp, "%s", name_w(c, ARG(0))); size_t l = e_strlen(tmp); if ((u32)ARG(2) <= l) { RET(l + 1); return; } put_w(c, ARG(1), tmp, 0); RET(l); }
static void s_GetLongPathNameA(Cpu *c) { char tmp[1024]; e_snprintf(tmp, sizeof tmp, "%s", name_a(c, ARG(0))); size_t l = e_strlen(tmp); if ((u32)ARG(2) <= l) { RET(l + 1); return; } put_a(c, ARG(1), tmp, 0); RET(l); }
static void s_GetShortPathNameW(Cpu *c) { s_GetLongPathNameW(c); }
/* GetVolumePathNameW(path, out, cap): everything lives on C:\ */
static void s_GetVolumePathNameW(Cpu *c) { if ((u32)ARG(2) < 4) { W->last_error = 122; RET(0); return; } put_w(c, ARG(1), "C:\\", 0); RET(1); }
/* collapse "\.\" and "x\..\" segments in place (keeps the root) */
static void path_collapse(char *p) {
  char out[1100]; size_t o = 0, i = 0, root = 0; size_t n = e_strlen(p);
  if (n >= 2 && p[1] == ':') { out[0] = p[0]; out[1] = ':'; o = 2; i = 2; if (p[2] == '\\') { out[o++] = '\\'; i = 3; } root = o; }
  else if (n >= 2 && p[0] == '\\' && p[1] == '\\') { out[0] = out[1] = '\\'; o = i = 2; root = 2; }
  else if (n >= 1 && p[0] == '\\') { out[0] = '\\'; o = i = 1; root = 1; }
  while (i <= n && o < sizeof out - 2) {
    size_t j = i; while (j < n && p[j] != '\\') j++;
    size_t len = j - i;
    if (len == 1 && p[i] == '.') { }
    else if (len == 2 && p[i] == '.' && p[i + 1] == '.') { if (o > root) { size_t k = o - 1; if (k > root && out[k - 1] == '\\') k--; while (k > root && out[k - 1] != '\\') k--; o = k; } else if (!root) { e_memcpy(out + o, "..\\", 3); o += 3; } }
    else if (len) { e_memcpy(out + o, p + i, len); o += len; out[o++] = '\\'; }
    i = j + 1;
  }
  if (o > root && out[o - 1] == '\\') o--;
  out[o] = 0; e_memcpy(p, out, o + 1);
}
/* PathCchCombineEx(out, cch, in, more, flags) / PathCchCanonicalizeEx(out, cch, in, flags) */
static void s_PathCchCombineEx(Cpu *c) {
  char a[1024], b[1024], r[2100]; e_snprintf(a, sizeof a, "%s", ARG(2) ? name_w(c, ARG(2)) : ""); e_snprintf(b, sizeof b, "%s", ARG(3) ? name_w(c, ARG(3)) : "");
  int b_abs = (b[0] == '\\') || (b[0] && b[1] == ':');
  if (b_abs || !a[0]) { if (b[0] == '\\' && b[1] != '\\' && a[0] && a[1] == ':') e_snprintf(r, sizeof r, "%c:%s", a[0], b); else e_snprintf(r, sizeof r, "%s", b); }
  else if (!b[0]) e_snprintf(r, sizeof r, "%s", a);
  else e_snprintf(r, sizeof r, "%s%s%s", a, a[e_strlen(a) - 1] == '\\' ? "" : "\\", b);
  path_collapse(r); size_t l = e_strlen(r); if (l + 1 > ARG(1)) { RET(0x8007007Au); return; }   /* ERROR_INSUFFICIENT_BUFFER */
  put_w(c, ARG(0), r, 0); RET(0);
}
static void s_PathCchCanonicalizeEx(Cpu *c) { char r[1100]; e_snprintf(r, sizeof r, "%s", ARG(2) ? name_w(c, ARG(2)) : ""); path_collapse(r); size_t l = e_strlen(r); if (l + 1 > ARG(1)) { RET(0x8007007Au); return; } put_w(c, ARG(0), r, 0); RET(0); }
/* PathCchSkipRoot(path, &rest): skip "C:\", "\\?\C:\", "\\server\share\" */
static void s_PathCchSkipRoot(Cpu *c) { u64 p = ARG(0), i = 0; u16 a = rd16(c, p), b = rd16(c, p + 2);
  if (a == '\\' && b == '\\') { u16 q = rd16(c, p + 4); i = 2; int segs = (q == '?' || q == '.') ? 1 : 2; if (segs == 1 && rd16(c, p + 6) == '\\') { i = 4; if (rd16(c, p + 10) == ':') { i = 6; if (rd16(c, p + 12) == '\\') i = 7; } else segs = 2; }
    for (int s2 = 0; s2 < segs; s2++) { while (rd16(c, p + 2 * i) && rd16(c, p + 2 * i) != '\\') i++; if (rd16(c, p + 2 * i) == '\\') i++; } }
  else if (b == ':') { i = 2; if (rd16(c, p + 4) == '\\') i = 3; }
  else if (a == '\\') i = 1;
  else { RET(0x80070057u); return; }
  wrp(c, ARG(1), p + 2 * i); RET(0); }
static void s_GetFullPathNameA(Cpu *c) { full_path(c, name_a(c, ARG(0)), 0); }
static void s_GetFullPathNameW(Cpu *c) { full_path(c, name_w(c, ARG(0)), 1); }
static void s_GetTempPathA(Cpu *c) { const char *d = "C:\\tmp\\"; size_t l = e_strlen(d); if ((u32)ARG(0) > l) put_a(c, ARG(1), d, 0); RET(l); }
static void s_GetTempPathW(Cpu *c) { const char *d = "C:\\tmp\\"; size_t l = e_strlen(d); if ((u32)ARG(0) > l) put_w(c, ARG(1), d, 0); RET(l); }
static u32 g_tmp_seq = 0x1a2b;
static void s_GetTempFileNameA(Cpu *c) { char out[1024]; const char *dir = name_a(c, ARG(0)); char pre[8]; e_snprintf(pre, sizeof pre, "%s", ARG(1) ? name_a(c, ARG(1)) : "tmp"); u32 u = (u32)ARG(2) ? (u32)ARG(2) : ++g_tmp_seq; e_snprintf(out, sizeof out, "%s%s%s%04X.TMP", dir, (dir[0] && dir[e_strlen(dir) - 1] == '\\') ? "" : "\\", pre, u & 0xffff); if (!ARG(2)) { u8 z = 0; vfs_add(out, &z, 0); } put_a(c, ARG(3), out, 260); RET(u & 0xffff); }
static void s_GetTempFileNameW(Cpu *c) { char dir[1024]; e_snprintf(dir, sizeof dir, "%s", name_w(c, ARG(0))); char pre[8]; e_snprintf(pre, sizeof pre, "%s", ARG(1) ? name_w(c, ARG(1)) : "tmp"); u32 u = (u32)ARG(2) ? (u32)ARG(2) : ++g_tmp_seq; char out[1024]; e_snprintf(out, sizeof out, "%s%s%s%04X.TMP", dir, (dir[0] && dir[e_strlen(dir) - 1] == '\\') ? "" : "\\", pre, u & 0xffff); if (!ARG(2)) { u8 z = 0; vfs_add(out, &z, 0); } put_w(c, ARG(3), out, 260); RET(u & 0xffff); }
/* file mappings: MapViewOfFile copies the file into guest memory (read-only semantics) */
/* file mappings are kernel buffers; a view is a copy in guest memory, written back on Unmap / before spawning a child */
static void s_CreateFileMappingA(Cpu *c) { u64 h = ARG(0); u64 size = ((u64)(u32)ARG(3) << 32) | (u32)ARG(4); int src = -1; if (h != ~0ull && h != 0) { src = handle_fd(h); if (src < 3) { W->last_error = 6; RET(0); return; } } int fd = vfs_map_create(src, size); if (fd < 0) { W->last_error = 8; RET(0); return; } vfs_set_inherit(fd, sa_inherit(c, ARG(1))); RET(H_OF(fd)); }
static void s_CreateFileMappingW(Cpu *c) { s_CreateFileMappingA(c); }
static void s_MapViewOfFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); if (fd < 3 || vfs_kind(fd) != FD_MAP) { W->last_error = 6; RET(0); return; }
  u64 sz; u8 *src = vfs_map_data(fd, &sz); if (!src) { RET(0); return; }
  u64 off = ((u64)(u32)ARG(2) << 32) | (u32)ARG(3); if (off > sz) off = sz; u64 n = ARG(4) ? ARG(4) : sz - off; if (off + n > sz) n = sz - off;
  u64 a = w32_heap_alloc(W, n ? n : 1, 1); u8 *p = mem_ptr(c->mem, a, n, 1); if (p && n) e_memcpy(p, src + off, (size_t)n);
  ProcFs *P = vfs_P; if (P->nviews < 32) { P->views[P->nviews].addr = a; P->views[P->nviews].size = n; P->views[P->nviews].fd = vfs_dup(fd); P->nviews++; }
  RET(a);
}
static void s_MapViewOfFileEx(Cpu *c) { s_MapViewOfFile(c); }
static void view_flush(Cpu *c, int i) { ProcFs *P = vfs_P; const u8 *p = mem_ptr(c->mem, P->views[i].addr, P->views[i].size, 0); if (p) vfs_map_write_back(P->views[i].fd, p, P->views[i].size); }
static void views_flush_all(Cpu *c) { for (int i = 0; i < vfs_P->nviews; i++) view_flush(c, i); }
static void s_UnmapViewOfFile(Cpu *c) { ProcFs *P = vfs_P; for (int i = 0; i < P->nviews; i++) if (P->views[i].addr == ARG(0)) { view_flush(c, i); vfs_close(P->views[i].fd); P->views[i] = P->views[--P->nviews]; break; } w32_heap_free(W, ARG(0)); RET(1); }
static void s_FlushViewOfFile(Cpu *c) { ProcFs *P = vfs_P; for (int i = 0; i < P->nviews; i++) if (ARG(0) >= P->views[i].addr && ARG(0) < P->views[i].addr + P->views[i].size) view_flush(c, i); RET(1); }
/* MinGW inlines getc/putc: getc = (--f->_cnt >= 0 ? *f->_ptr++ : _filbuf(f)); so FILE needs a real guest buffer:
   _ptr +0, _cnt +8, _base +16, _flag +24, _file +28.  file_sync() discards an unread buffer (seeking back) before any
   operation that bypasses it. */
#define FBUF 4096
static void file_sync(Cpu *c, u64 f, int fd) { if (!f || f < 0x10000) return; s32 cnt = (s32)rd32(c, f + 8); if (cnt > 0 && fd >= 3) vfs_seek(fd, -cnt, 1); if (cnt > 0 && fd == -1) return; wr32(c, f + 8, 0); wr64(c, f, rd64(c, f + 16)); }
static u64 file_base(Cpu *c, u64 f) { u64 b = rd64(c, f + 16); if (!b) { b = w32_heap_alloc(W, FBUF, 0); wr64(c, f + 16, b); } return b; }
/* take up to n bytes buffered in a FILE (used so fgets/fread see what getc left) */
static u64 file_take(Cpu *c, u64 f, u64 dst, u64 n, int stop_nl) { if (!f || f < 0x10000) return 0; s32 cnt = (s32)rd32(c, f + 8); if (cnt <= 0) return 0; u64 p = rd64(c, f); u64 o = 0; while (o < n && cnt > 0) { u8 ch = rd8(c, p++); wr8(c, dst + o, ch); o++; cnt--; if (stop_nl && ch == '\n') break; } wr32(c, f + 8, (u32)cnt); wr64(c, f, p); return o; }
static void file_set_eof(Cpu *c, u64 f) { if (f >= 0x10000) wr32(c, f + 24, rd32(c, f + 24) | 0x10); }
static void s_filbuf(Cpu *c) {
  u64 f = ARG(0); int fd = file_fd(c, f); if (!fd || fd == 1 || fd == 2) { RET(~0ull); return; }
  u64 b = file_base(c, f); u8 *p = mem_ptr(c->mem, b, FBUF, 1); if (!p) { RET(~0ull); return; }
  s64 r = raw_read(fd, p, FBUF);
  if (r == -2) { blocked_io(c, 0); return; }
  if (r <= 0) { wr32(c, f + 8, 0); wr32(c, f + 24, rd32(c, f + 24) | 0x10); RET(~0ull); return; }
  wr64(c, f, b + 1); wr32(c, f + 8, (u32)(r - 1)); RET(p[0]);
}
static void s_flsbuf(Cpu *c) { int fd = file_fd(c, ARG(1)); u8 ch = (u8)ARG(0); if (!fd) { RET(~0ull); return; } int r = out_write(c, fd, &ch, 1); if (!r) return; if (r < 0) { RET(~0ull); return; } RET(ch); }
static void s_fdopen(Cpu *c) { int fd = (int)ARG(0); if (fd == 0) { RET(W->ubuf); return; } if (fd == 1 || fd == 2) { RET(W->ubuf + IOB_STRIDE * fd); return; } if (!vfs_is_open(fd)) { set_errno(c, 9); RET(0); return; } RET(make_file(c, fd)); }
/* ---- CRT ---- */
static int parse_mode(const char *m, int *acc, int *disp, int *append) {
  *append = 0; char k = m[0]; int plus = e_strchr(m, '+') != 0;
  if (k == 'r') { *acc = plus ? 3 : 1; *disp = 3; } else if (k == 'w') { *acc = plus ? 3 : 2; *disp = 2; } else if (k == 'a') { *acc = plus ? 3 : 2; *disp = 4; *append = 1; } else return 0;
  return 1;
}
static u64 crt_fopen(Cpu *c, const char *n, const char *m) { int acc, disp, app; if (!parse_mode(m, &acc, &disp, &app)) { set_errno(c, 22); return 0; } int fd = vfs_open(n, acc, disp, app); if (fd < 0) { set_errno(c, errno_of(fd)); return 0; } return make_file(c, fd); }
static void s_fopen(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_a(c, ARG(0))); RET(crt_fopen(c, n, name_a(c, ARG(1)))); }
static void s_wfopen(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_w(c, ARG(0))); RET(crt_fopen(c, n, name_w(c, ARG(1)))); }
static void s_fopen_s(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_a(c, ARG(1))); u64 f = crt_fopen(c, n, name_a(c, ARG(2))); wr64(c, ARG(0), f); RET(f ? 0 : rd32(c, ERRNO_ADDR)); }
static void s_wfopen_s(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_w(c, ARG(1))); u64 f = crt_fopen(c, n, name_w(c, ARG(2))); wr64(c, ARG(0), f); RET(f ? 0 : rd32(c, ERRNO_ADDR)); }
static void s_fsopen(Cpu *c) { s_fopen(c); }
static void s_freopen(Cpu *c) { /* redirect only supported for files → new FILE*, std streams unchanged */ int old = file_fd(c, ARG(2)); if (old >= 3) vfs_close(old); char n[1024]; e_snprintf(n, sizeof n, "%s", name_a(c, ARG(0))); int acc, disp, app; if (!parse_mode(name_a(c, ARG(1)), &acc, &disp, &app)) { RET(0); return; } int fd = vfs_open(n, acc, disp, app); if (fd < 0) { set_errno(c, errno_of(fd)); RET(0); return; } if (old >= 3) { wr32(c, ARG(2) + 28, (u32)fd); RET(ARG(2)); } else RET(make_file(c, fd)); }
static void s_fclose(Cpu *c) { int fd = file_fd(c, ARG(0)); if (fd >= 3) { vfs_close(fd); w32_heap_free(W, ARG(0)); RET(0); } else RET(fd ? 0 : ~0ull); }
static void s_fread(Cpu *c) { int fd = file_fd(c, ARG(3)); u64 sz = ARG(1), cnt = ARG(2); if (!fd || !sz) { RET(0); return; } u64 pre = file_take(c, ARG(3), ARG(0), sz * cnt, 0); if (pre == sz * cnt) { RET(cnt); return; } s64 r = fd_read(c, fd, ARG(0) + pre, sz * cnt - pre); if (r == -2 && pre) { RET(pre / sz); return; } if (r >= 0) r += (s64)pre; if (r == -2) { blocked_io(c, 0); return; } if (r < 0) r = 0; if ((u64)r < sz * cnt) file_set_eof(c, ARG(3)); RET((u64)r / sz); }
static void s_fwrite(Cpu *c) { int fd = file_fd(c, ARG(3)); u64 sz = ARG(1), cnt = ARG(2); if (fd <= 0 || !sz) { RET(0); return; } s64 r = fd_write(c, fd, ARG(0), sz * cnt); if (r == -2) { blocked_io(c, 1); return; } if (r < 0) r = 0; RET((u64)r / sz); }
static void s_fgets(Cpu *c) {
  int fd = file_fd(c, ARG(2)); u64 buf = ARG(0); int n = (int)ARG(1); if (!fd || n <= 1) { RET(0); return; }
  { u64 pre = file_take(c, ARG(2), buf, (u64)(n - 1), 1); if (pre) { if (rd8(c, buf + pre - 1) != '\n' && (int)pre < n - 1 && fd >= 3) { int i = (int)pre; u8 ch; while (i < n - 1) { s64 r = vfs_read(fd, &ch, 1); if (r <= 0) break; wr8(c, buf + i, ch); i++; if (ch == '\n') break; } pre = (u64)i; } wr8(c, buf + pre, 0); RET(buf); return; } }
  if (fd == -1 && vfs_resolve(0) == -1) { u8 tmp[4096]; g_last_io_fd = -1; s64 r = vfs_stdin_read(tmp, (u64)(n - 1) < sizeof tmp ? (u64)(n - 1) : sizeof tmp); if (r == -2) { blocked_io(c, 0); return; } if (r <= 0) { file_set_eof(c, ARG(2)); RET(0); return; } u8 *p = mem_ptr(c->mem, buf, (u64)r + 1, 1); if (p) { e_memcpy(p, tmp, (size_t)r); p[r] = 0; } RET(buf); return; }
  int i = 0; u8 ch; while (i < n - 1) { s64 r = raw_read(fd, &ch, 1); if (r == -2) { if (!i) { blocked_io(c, 0); return; } break; }   /* pipe: wait for the writer, or hand back a partial line */ if (r <= 0) break; wr8(c, buf + i, ch); i++; if (ch == '\n') break; }
  if (!i) { file_set_eof(c, ARG(2)); RET(0); return; } wr8(c, buf + i, 0); RET(buf);
}
static void s_fgetc(Cpu *c) { int fd = file_fd(c, ARG(0)); u8 ch; if (!fd) { RET(~0ull); return; } if (file_take(c, ARG(0), W->ubuf + 0x7f0, 1, 0)) { RET(rd8(c, W->ubuf + 0x7f0)); return; } s64 r = raw_read(fd, &ch, 1); if (r == -2) { blocked_io(c, 0); return; } if (r != 1) file_set_eof(c, ARG(0)); RET(r == 1 ? ch : ~0ull); }
static void s_getchar(Cpu *c) { u8 ch; s64 r = raw_read(0, &ch, 1); if (r == -2) { blocked_io(c, 0); return; } RET(r == 1 ? ch : ~0ull); }
static void s_ungetc(Cpu *c) { int fd = file_fd(c, ARG(1)); RET(fd >= 3 ? vfs_ungetc(fd, (int)ARG(0)) : ~0ull); }
static void s_fseek(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); wr32(c, ARG(0) + 24, rd32(c, ARG(0) + 24) & ~0x10u); RET(fd >= 3 && vfs_seek(fd, (s64)(s32)(u32)ARG(1), (int)ARG(2)) >= 0 ? 0 : ~0ull); }
static void s_fseeki64(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); wr32(c, ARG(0) + 24, rd32(c, ARG(0) + 24) & ~0x10u); RET(fd >= 3 && vfs_seek(fd, (s64)ARG(1), (int)ARG(2)) >= 0 ? 0 : ~0ull); }
static void s_fgetpos(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); if (fd < 3) { RET(~0ull); return; } wr64(c, ARG(1), (u64)vfs_tell(fd)); RET(0); }
static void s_fsetpos(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); wr32(c, ARG(0) + 24, rd32(c, ARG(0) + 24) & ~0x10u); RET(fd >= 3 && vfs_seek(fd, (s64)rd64(c, ARG(1)), 0) >= 0 ? 0 : ~0ull); }
static void s_ftell(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); RET(fd >= 3 ? (u64)vfs_tell(fd) : ~0ull); }
static void s_rewind(Cpu *c) { int fd = file_fd(c, ARG(0)); file_sync(c, ARG(0), fd); wr32(c, ARG(0) + 24, rd32(c, ARG(0) + 24) & ~0x10u); if (fd >= 3) vfs_seek(fd, 0, 0); }
static void s_feof(Cpu *c) { int fd = file_fd(c, ARG(0)); if (ARG(0) >= 0x10000 && (rd32(c, ARG(0) + 24) & 0x10)) { RET(1); return; } RET(fd >= 3 ? vfs_eof(fd) && (s32)rd32(c, ARG(0) + 8) <= 0 : 0); }
static void s_ferror(Cpu *c) { RET(0); }
static void s_clearerr(Cpu *c) { int fd = file_fd(c, ARG(0)); if (fd >= 3) vfs_clearerr(fd); if (ARG(0) >= 0x10000) wr32(c, ARG(0) + 24, rd32(c, ARG(0) + 24) & ~0x30u); }
static void s_fileno(Cpu *c) { int fd = file_fd(c, ARG(0)); RET(fd == -1 ? 0 : fd > 0 ? fd : ~0ull); }
static void s_filelengthi64(Cpu *c) { s64 s = vfs_size((int)ARG(0)); RET(s < 0 ? ~0ull : (u64)s); }
static void s_isatty(Cpu *c) { int r = vfs_resolve((int)ARG(0)); RET(r == -1 || r == 1 || r == 2); }
static void s_pipe(Cpu *c) { int r, wfd; if (vfs_pipe(&r, &wfd)) { set_errno(c, 24); RET(~0ull); return; } if ((u32)ARG(2) & 0x80) { vfs_set_inherit(r, 0); vfs_set_inherit(wfd, 0); } r = vfs_place_low(r); wfd = vfs_place_low(wfd); wr32(c, ARG(0), (u32)r); wr32(c, ARG(0) + 4, (u32)wfd); RET(0); }
static void s_get_osfhandle(Cpu *c) { int fd = (int)ARG(0); if (fd >= 0 && fd <= 2) { int r = vfs_resolve(fd); if (r >= 3) { RET(H_OF(r)); return; } RET(0x10 + (u64)fd); return; } RET(vfs_is_open(fd) ? H_OF(fd) : ~0ull); }
static void s_open_osfhandle(Cpu *c) { int fd = handle_fd(ARG(0)); RET(fd == -1 ? 0 : fd > 0 ? fd : ~0ull); }
static int oflag_to_disp(u32 o) { if ((o & 0x100) && (o & 0x400)) return 1; if ((o & 0x100) && (o & 0x200)) return 2; if (o & 0x100) return 4; if (o & 0x200) return 5; return 3; }
static u64 crt_open(Cpu *c, const char *n, u32 oflag) { int acc = (oflag & 3) == 0 ? 1 : (oflag & 3) == 1 ? 2 : 3; int fd = vfs_open(n, acc, oflag_to_disp(oflag), (oflag & 8) != 0); if (fd < 0) { set_errno(c, errno_of(fd)); return ~0ull; } if (oflag & 0x80) vfs_set_inherit(fd, 0); return (u64)vfs_place_low(fd); }
static void s_open(Cpu *c) { RET(crt_open(c, name_a(c, ARG(0)), (u32)ARG(1))); }
static void s_wopen(Cpu *c) { RET(crt_open(c, name_w(c, ARG(0)), (u32)ARG(1))); }
static void s_sopen_s(Cpu *c) { u64 fd = crt_open(c, name_a(c, ARG(1)), (u32)ARG(2)); wr32(c, ARG(0), (u32)fd); RET(fd == ~0ull ? rd32(c, ERRNO_ADDR) : 0); }
static void s_wsopen_s(Cpu *c) { u64 fd = crt_open(c, name_w(c, ARG(1)), (u32)ARG(2)); wr32(c, ARG(0), (u32)fd); RET(fd == ~0ull ? rd32(c, ERRNO_ADDR) : 0); }
static void s_sopen(Cpu *c) { s_open(c); }
static void s_close(Cpu *c) { RET(vfs_close((int)ARG(0)) ? 0 : ~0ull); }
static void s_read(Cpu *c) { int fd = (int)ARG(0); if (fd == 0) fd = -1; s64 r = fd_read(c, fd, ARG(1), (u32)ARG(2)); if (r == -2) { blocked_io(c, 0); return; } if (r < 0) { set_errno(c, 9); RET(~0ull); return; } RET((u64)r); }
static void s_write(Cpu *c) { s64 r = fd_write(c, (int)ARG(0), ARG(1), (u32)ARG(2)); if (r == -2) { blocked_io(c, 1); return; } if (r == -3) { set_errno(c, 32); RET(~0ull); return; } if (r < 0) { set_errno(c, 9); RET(~0ull); return; } RET((u64)r); }
static void s_lseek(Cpu *c) { s64 r = vfs_seek((int)ARG(0), (s64)(s32)(u32)ARG(1), (int)ARG(2)); RET(r < 0 ? ~0ull : (u64)r); }
static void s_lseeki64(Cpu *c) { s64 r = vfs_seek((int)ARG(0), (s64)ARG(1), (int)ARG(2)); RET(r < 0 ? ~0ull : (u64)r); }
static void s_tell(Cpu *c) { s64 r = vfs_tell((int)ARG(0)); RET(r < 0 ? ~0ull : (u64)r); }
static void s_eof(Cpu *c) { int fd = (int)ARG(0); s64 p = vfs_tell(fd), s = vfs_size(fd); RET(p < 0 ? ~0ull : p >= s); }
static void s_chsize(Cpu *c) { int fd = (int)ARG(0); s64 keep = vfs_tell(fd); if (keep < 0) { RET(~0ull); return; } vfs_seek(fd, (s64)ARG(1), 0); vfs_truncate(fd); vfs_seek(fd, keep, 0); RET(0); }
static void s_dup(Cpu *c) { int d = vfs_dup((int)ARG(0)); if (d < 0) set_errno(c, 9); else d = vfs_place_low(d); RET(d < 0 ? ~0ull : (u64)d); }
static void s_dup2(Cpu *c) { int d = vfs_dup2((int)ARG(0), (int)ARG(1)); if (d < 0) set_errno(c, 9); RET(d < 0 ? ~0ull : 0); }
static void s_setbuf(Cpu *c) { }
/* Toolhelp: a single process (ourselves) so shells can find their parent */
static void s_CreateToolhelp32Snapshot(Cpu *c) { RET(0x5000); }
static int snap_cursor;
static void proc_entry(Cpu *c, int idx, int wide) { int pid, ppid; const char *name; if (!proc_enum(idx, &pid, &ppid, &name)) { W->last_error = 18; RET(0); return; } u64 a = ARG(1); /* PROCESSENTRY32 (x64): +8 pid, +16 heapid, +24 moduleid, +28 threads, +32 ppid, +36 pri, +40 flags, +44 szExeFile */ wr32(c, a + 4, 0); wr32(c, a + 8, (u32)pid); wr64(c, a + 16, 0); wr32(c, a + 24, 0); wr32(c, a + 28, 1); wr32(c, a + 32, (u32)ppid); wr32(c, a + 36, 8); wr32(c, a + 40, 0); if (wide) put_w(c, a + 44, name, 260); else put_a(c, a + 44, name, 260); snap_cursor = idx + 1; RET(1); }
static void s_Process32First(Cpu *c) { proc_entry(c, 0, 0); }
static void s_Process32Next(Cpu *c) { proc_entry(c, snap_cursor, 0); }
static void s_Process32FirstW(Cpu *c) { proc_entry(c, 0, 1); }
static void s_Process32NextW(Cpu *c) { proc_entry(c, snap_cursor, 1); }
/* ---- processes ---- */
/* STARTUPINFOA/W: cb +0, dwFlags +60, hStdInput +80, hStdOutput +88, hStdError +96 */
static int std_from_handle(u64 h) { if (h == 0x10 || h == 0x11 || h == 0x12) return -2;   /* the parent's own standard handle (possibly redirected) → inherit it */ int fd = handle_fd(h); return fd >= 3 ? fd : -1; }
static const char *env_get(const char *name);
#define CMD_EXE_PATH "C:\\Windows\\System32\\cmd.exe"
/* find a program the way CreateProcess/cmd do: as given, +.exe, then (bare names) the app dir, System32 and every %PATH% entry; finally our own image */
static int resolve_exe(Cpu *c, const char *exe, char *cand, size_t cap) {
  u64 sz; int has_sep = 0; for (const char *q = exe; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') has_sep = 1;
  const char *bn = exe; for (const char *q = exe; *q; q++) if (*q == '\\' || *q == '/') bn = q + 1; int has_ext = e_strchr(bn, '.') != 0;
  for (int i = 0; i < 2; i++) { if (i == 1 && has_ext && e_strlen(bn) > 4 && !e_stricmp(bn + e_strlen(bn) - 4, ".exe")) break; e_snprintf(cand, cap, i ? "%s.exe" : "%s", exe); if (vfs_stat(cand, &sz) == 1) return 1; }
  if (!has_sep) {
    char path[8192]; const char *pv = env_get("PATH"); e_snprintf(path, sizeof path, "C:\\nebula;C:\\Windows\\System32;%s", pv ? pv : "");
    for (char *p = path; *p;) { char *e = p; while (*e && *e != ';') e++; char save = *e; *e = 0; if (*p) for (int i = 0; i < 2; i++) { e_snprintf(cand, cap, i ? "%s\\%s.exe" : "%s\\%s", p, exe); if (vfs_stat(cand, &sz) == 1) return 1; } if (!save) break; p = e + 1; }
  }
  if (!e_stricmp(bn, W->exe_name)) { e_snprintf(cand, cap, "C:\\nebula\\%s", W->exe_name); return vfs_stat(cand, &sz) == 1; }
  return 0;
}
static void create_process(Cpu *c, const char *app, const char *cmd, const char *cwd) {
  char exe[1024]; exe[0] = 0;
  if (app && app[0]) e_snprintf(exe, sizeof exe, "%s", app);
  else { /* first token of the command line */ const char *p = cmd; size_t o = 0; if (*p == '"') { p++; while (*p && *p != '"' && o + 1 < sizeof exe) exe[o++] = *p++; } else while (*p && *p != ' ' && o + 1 < sizeof exe) exe[o++] = *p++; exe[o] = 0; }
  if (!exe[0]) { W->last_error = 2; RET(0); return; }
  char cand[1100];
  if (!resolve_exe(c, exe, cand, sizeof cand)) { W->last_error = 2; RET(0); return; }
  u64 si = ARG(8); int std[3] = { -2, -2, -2 };   /* -2 = inherit */
  if (si && (rd32(c, si + 60) & 0x100)) { std[0] = std_from_handle(rd64(c, si + 80)); std[1] = std_from_handle(rd64(c, si + 88)); std[2] = std_from_handle(rd64(c, si + 96)); }
  views_flush_all(c);   /* the child must see what the parent wrote into shared mappings */
  /* lpEnvironment: a block of NAME=value strings (UTF-16 when CREATE_UNICODE_ENVIRONMENT) → the child's whole environment */
  const char *envv[512]; int nenv = 0; char *envbuf = 0;
  if (ARG(6)) {
    int wide = (ARG(5) & 0x400) != 0; u64 a = ARG(6); size_t cap = 1 << 20; envbuf = emu_alloc(cap); size_t o = 0;
    for (int guard = 0; guard < 4096 && nenv < 512; guard++) {
      if (wide) { if (!rd16(c, a)) break; size_t room = cap - o; if (room < 8) break; wstr_to_utf8(c, a, envbuf + o, room > 32768 ? 32768 : room, -1); while (rd16(c, a)) a += 2; a += 2; }
      else { if (!rd8(c, a)) break; size_t k = 0; while (rd8(c, a + k) && o + k + 1 < cap) { envbuf[o + k] = (char)rd8(c, a + k); k++; } envbuf[o + k] = 0; a += k + 1; }
      envv[nenv++] = envbuf + o; o += e_strlen(envbuf + o) + 1;
    }
    g_spawn_env = envv; g_spawn_nenv = nenv;
  }
  g_spawn_cwd = cwd && cwd[0] ? cwd : 0;
  int pid = proc_spawn(W, cand, cmd && cmd[0] ? cmd : exe, std);
  g_spawn_env = 0; g_spawn_nenv = 0; g_spawn_cwd = 0; if (envbuf) emu_free(envbuf);
  { char m[900]; e_snprintf(m, sizeof m, "[x64] CreateProcess(%s | %s) flags=0x%x std=%d,%d,%d cwd=%s -> pid %d\n", cand, cmd ? cmd : "", (u32)ARG(5), std[0], std[1], std[2], cwd && cwd[0] ? cwd : "(inherit)", pid); host_write(3, m, e_strlen(m)); }
  if (pid < 0) { W->last_error = pid == -8 ? 8 : 2; RET(0); return; }
  u64 pi = ARG(9); if (pi) { int h = vfs_proc_handle(pid); wr64(c, pi, h >= 0 ? H_OF(h) : 0); { int tk = proc_main_thread_kobj(pid); int tf = tk >= 0 ? vfs_kobj_handle(tk) : -1; wr64(c, pi + 8, tf >= 0 ? H_OF(tf) : 0); } wr32(c, pi + 16, (u32)pid); wr32(c, pi + 20, (u32)pid + 1); }
  RET(1);
}
static void s_CreateProcessA(Cpu *c) { char app[1024]; e_snprintf(app, sizeof app, "%s", ARG(0) ? name_a(c, ARG(0)) : ""); char cmd[4096]; e_snprintf(cmd, sizeof cmd, "%s", ARG(1) ? name_a(c, ARG(1)) : ""); char cwd[1024]; e_snprintf(cwd, sizeof cwd, "%s", ARG(7) ? name_a(c, ARG(7)) : ""); create_process(c, app, cmd, cwd); }
static void s_CreateProcessW(Cpu *c) { char app[1024]; e_snprintf(app, sizeof app, "%s", ARG(0) ? name_w(c, ARG(0)) : ""); char cmd[4096]; if (ARG(1)) wstr_to_utf8(c, ARG(1), cmd, sizeof cmd, -1); else cmd[0] = 0; char cwd[1024]; e_snprintf(cwd, sizeof cwd, "%s", ARG(7) ? name_w(c, ARG(7)) : ""); create_process(c, app, cmd, cwd); }
static int wait_proc_handle(u64 h, int *pid_out) { int fd = handle_fd(h); if (fd < 3) return 0; int pid = vfs_fd_pid(fd); if (pid < 0) return 0; *pid_out = pid; return 1; }
static void s_GetExitCodeProcess(Cpu *c) { int pid; if (!wait_proc_handle(ARG(0), &pid)) { if (ARG(0) == ~0ull) { wr32(c, ARG(1), 259); RET(1); return; } W->last_error = 6; RET(0); return; } int code; int st = proc_state(pid, &code); wr32(c, ARG(1), st == 2 ? (u32)code : 259); RET(1); }
static void s_GetProcessId(Cpu *c) { int pid; if (ARG(0) == ~0ull) { RET((u64)W->pid); return; } RET(wait_proc_handle(ARG(0), &pid) ? (u64)pid : 0); }
static void s_TerminateProcess(Cpu *c) { int pid; if (ARG(0) == ~0ull || !wait_proc_handle(ARG(0), &pid)) { do_exit(c, (int)ARG(1)); return; } proc_kill(pid, (int)ARG(1)); RET(1); }
static void s_OpenProcess(Cpu *c) { int pid = (int)ARG(2); int code; if (!proc_state(pid, &code)) { W->last_error = 87; RET(0); return; } int h = vfs_proc_handle(pid); RET(h >= 0 ? H_OF(h) : 0); }
static void s_CreatePipe(Cpu *c) { int r, wfd; if (vfs_pipe(&r, &wfd)) { W->last_error = 8; RET(0); return; } int inh = sa_inherit(c, ARG(2)); vfs_set_inherit(r, inh); vfs_set_inherit(wfd, inh); wr64(c, ARG(0), H_OF(r)); wr64(c, ARG(1), H_OF(wfd)); RET(1); }
static void s_PeekNamedPipe(Cpu *c) { int fd = handle_fd(ARG(0)); int avail = fd >= 3 ? vfs_pipe_avail(fd) : -1; if (avail < 0) { W->last_error = 6; RET(0); return; } if (ARG(3)) wr32(c, ARG(3), 0); if (ARG(4)) wr32(c, ARG(4), (u32)avail); if (ARG(5)) wr32(c, ARG(5), (u32)avail); RET(1); }
static void s_DuplicateHandle(Cpu *c) { u64 h = ARG(1); int fd = handle_fd(h); u64 out; if (h == ~1ull) { int tf = vfs_kobj_handle(W->th[W->cur_th].kobj); out = tf >= 0 ? H_OF(tf) : 0; } else if (h == ~0ull || h == 0x10 || h == 0x11 || h == 0x12 || fd < 3) out = h; else { int d = vfs_dup(fd); if (d >= 0) vfs_set_inherit(d, (u32)ARG(5) != 0);   /* bInheritHandle is the 6th argument */ out = d >= 0 ? H_OF(d) : 0; } if (ARG(3)) wr64(c, ARG(3), out); if (ARG(6) & 1) vfs_close(fd); RET(out != 0); }
static void s_GetCurrentProcessId(Cpu *c) { RET((u64)W->pid); }
static void s_getpid(Cpu *c) { RET((u64)W->pid); }
static void env_put(const char *kv);
/* msvcrt _spawn* family: argv -> quoted command line, optional envp, modes P_WAIT 0 / P_NOWAIT 1 / P_OVERLAY 2 / P_NOWAITO 3 / P_DETACH 4 */
static void argv_to_cmdline(Cpu *c, u64 argv, char *out, size_t cap) {   /* msvcrt joins argv with spaces and does NOT quote (callers quote themselves) */
  size_t o = 0; for (int i = 0; i < 512; i++) { u64 sp = rd64(c, argv + (u64)i * 8); if (!sp) break; const char *s = rd_str(c, sp, 4096); if (!s) break;
    if (o && o + 1 < cap) out[o++] = ' ';
    for (const char *q = s; *q && o + 1 < cap; q++) out[o++] = *q; }
  out[o] = 0;
}
static void spawn_common(Cpu *c, int mode, u64 path, u64 argv, u64 envp) {
  if (W->spawn_wait_pid) {   /* resumed after the child we waited for exited */
    int code; int st = proc_state(W->spawn_wait_pid, &code); if (st == 1) { block_on(c, BLOCK_PROC, W->spawn_wait_pid); return; }
    int pid = W->spawn_wait_pid; int m = W->spawn_wait_mode; W->spawn_wait_pid = 0; proc_handle_ref(pid, -1);
    if (m == 2) { do_exit(c, st == 2 ? code : 0); return; }
    RET(st == 2 ? (u64)(u32)code : 0); return;
  }
  const char *p = path ? name_a(c, path) : 0; if (!p || !p[0]) { set_errno(c, 2); RET(~0ull); return; }
  char exe[1024]; e_snprintf(exe, sizeof exe, "%s", p);
  char cand[1100]; if (!resolve_exe(c, exe, cand, sizeof cand)) { set_errno(c, 2); RET(~0ull); return; }
  static char cmd[8192]; argv_to_cmdline(c, argv, cmd, sizeof cmd);
  int std[3] = { -2, -2, -2 }; views_flush_all(c);
  int pid = proc_spawn(W, cand, cmd[0] ? cmd : cand, std);
  if (pid < 0) { set_errno(c, pid == -8 ? 12 : 2); RET(~0ull); return; }
  if (envp) { ProcFs *child = proc_fs(pid), *save = vfs_P; if (child) { vfs_use(child); vfs_P->nenv = 0; for (int i = 0; i < 4096; i++) { u64 sp = rdp(c, envp + (u64)i * PSZ); if (!sp) break; const char *s = rd_str(c, sp, 32767); if (s && e_strchr(s, '=')) env_put(s); } vfs_use(save); } }
  int h = vfs_proc_handle(pid);
  if (mode == 0 || mode == 2) { W->spawn_wait_pid = pid; W->spawn_wait_mode = mode; proc_handle_ref(pid, 1); if (h >= 0) { vfs_close(h); } block_on(c, BLOCK_PROC, pid); return; }
  if (mode == 3 || mode == 4) { if (h >= 0) vfs_close(h); RET(0); return; }
  RET(h >= 0 ? H_OF(h) : ~0ull);
}
static void s_spawnve(Cpu *c) { spawn_common(c, (int)ARG(0), ARG(1), ARG(2), ARG(3)); }
/* system()/_popen(): run `cmd.exe /c command` through the embedded C:\Windows\System32\cmd.exe */
static int wait_child_resume(Cpu *c, u64 *code_out) {   /* 1 = still blocked (call returned), 0 = finished (code in *code_out) */
  int code; int st = proc_state(W->spawn_wait_pid, &code); if (st == 1) { block_on(c, BLOCK_PROC, W->spawn_wait_pid); return 1; }
  int pid = W->spawn_wait_pid; W->spawn_wait_pid = 0; proc_handle_ref(pid, -1); *code_out = st == 2 ? (u64)(u32)code : 0; return 0;
}
static void run_shell(Cpu *c, const char *cmd) {
  if (W->spawn_wait_pid) { u64 code; if (wait_child_resume(c, &code)) return; RET(code); return; }
  if (!cmd) { u64 sz; RET(vfs_stat(CMD_EXE_PATH, &sz) == 1); return; }   /* system(NULL): is a shell available? */
  static char cl[8192]; e_snprintf(cl, sizeof cl, "cmd.exe /c %s", cmd);
  int std[3] = { -2, -2, -2 }; views_flush_all(c);
  int pid = proc_spawn(W, CMD_EXE_PATH, cl, std);
  if (pid < 0) { set_errno(c, pid == -8 ? 12 : 2); RET(~0ull); return; }
  W->spawn_wait_pid = pid; W->spawn_wait_mode = 0; proc_handle_ref(pid, 1); block_on(c, BLOCK_PROC, pid);
}
static void s_system(Cpu *c) { run_shell(c, ARG(0) ? name_a(c, ARG(0)) : 0); }
static void s_wsystem(Cpu *c) { run_shell(c, ARG(0) ? name_w(c, ARG(0)) : 0); }
static void popen_common(Cpu *c, const char *cmd, const char *mode) {
  int r, wfd; if (vfs_pipe(&r, &wfd)) { set_errno(c, 24); RET(0); return; }
  int reading = !(mode && mode[0] == 'w');
  int std[3] = { -2, -2, -2 }; if (reading) std[1] = wfd; else std[0] = r;
  int mine = reading ? r : wfd; vfs_set_inherit(mine, 0);   /* our end must not leak into the child, or it would never see EOF */
  static char cl[8192]; e_snprintf(cl, sizeof cl, "cmd.exe /c %s", cmd); views_flush_all(c);
  int pid = proc_spawn(W, CMD_EXE_PATH, cl, std);
  vfs_close(reading ? wfd : r);   /* the child's end stays open only in the child */
  if (pid < 0) { vfs_close(reading ? r : wfd); set_errno(c, 2); RET(0); return; }
  u64 f = make_file(c, mine); wr32(c, f + 40, (u32)pid); proc_handle_ref(pid, 1);   /* FILE +40: child pid for _pclose */
  RET(f);
}
static void s_popen(Cpu *c) { popen_common(c, name_a(c, ARG(0)), name_a(c, ARG(1))); }
static void s_wpopen(Cpu *c) { char cmd[8192]; wstr_to_utf8(c, ARG(0), cmd, sizeof cmd, -1); popen_common(c, cmd, name_w(c, ARG(1))); }
static void s_pclose(Cpu *c) {
  if (W->spawn_wait_pid) { u64 code; if (wait_child_resume(c, &code)) return; RET(code); return; }
  int fd = file_fd(c, ARG(0)); if (!fd) { set_errno(c, 9); RET(~0ull); return; }
  int pid = (int)rd32(c, ARG(0) + 40); if (fd >= 3) vfs_close(fd); w32_heap_free(W, ARG(0));
  if (pid <= 0) { RET(~0ull); return; }
  W->spawn_wait_pid = pid; W->spawn_wait_mode = 0; block_on(c, BLOCK_PROC, pid);
}
static void s_spawnv(Cpu *c) { spawn_common(c, (int)ARG(0), ARG(1), ARG(2), 0); }
static void s_execve(Cpu *c) { spawn_common(c, 2, ARG(0), ARG(1), ARG(2)); }
static void s_execv(Cpu *c) { spawn_common(c, 2, ARG(0), ARG(1), 0); }
static void s_cwait(Cpu *c) { int pid; if (!wait_proc_handle(ARG(1), &pid)) { set_errno(c, 10); RET(~0ull); return; } int code; int st = proc_state(pid, &code); if (!st) { set_errno(c, 10); RET(~0ull); return; } if (st == 1) { block_on(c, BLOCK_PROC, pid); return; } if (ARG(0)) wr32(c, ARG(0), (u32)code); RET((u64)pid); }
/* psapi / remote-process introspection (busybox ps reads its own strings out of sibling processes) */
static int pid_of_handle(Cpu *c, u64 h) { int pid; if (h == ~0ull) return W->pid; return wait_proc_handle(h, &pid) ? pid : -1; }
static void proc_image_path(int pid, char *out, size_t cap, int devform) { const char *name = 0; proc_image_base(pid, &name); e_snprintf(out, cap, devform ? "\\Device\\HarddiskVolume1\\nebula\\%s" : "C:\\nebula\\%s", name ? name : "unknown.exe"); }
static void s_GetProcessImageFileNameA(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, 1); u32 cap = (u32)ARG(2); size_t l = e_strlen(p); if (l + 1 > cap) { W->last_error = 122; RET(0); return; } put_a(c, ARG(1), p, cap); RET(l); }
static void s_GetProcessImageFileNameW(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, 1); u32 cap = (u32)ARG(2); size_t l = e_strlen(p); if (l + 1 > cap) { W->last_error = 122; RET(0); return; } put_w(c, ARG(1), p, cap); RET(l); }
static void s_QueryFullProcessImageNameA(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, (u32)ARG(1) & 1); u32 cap = rd32(c, ARG(3)); size_t l = e_strlen(p); if (l + 1 > cap) { W->last_error = 122; RET(0); return; } put_a(c, ARG(2), p, cap); wr32(c, ARG(3), (u32)l); RET(1); }
static void s_QueryFullProcessImageNameW(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, (u32)ARG(1) & 1); u32 cap = rd32(c, ARG(3)); size_t l = e_strlen(p); if (l + 1 > cap) { W->last_error = 122; RET(0); return; } put_w(c, ARG(2), p, cap); wr32(c, ARG(3), (u32)l); RET(1); }
static void s_EnumProcessModules(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } u64 base = proc_image_base(pid, 0); if ((u32)ARG(2) >= 8 && ARG(1)) wr64(c, ARG(1), base); if (ARG(3)) wr32(c, ARG(3), 8); RET(1); }
static void s_EnumProcessModulesEx(Cpu *c) { s_EnumProcessModules(c); }
static void s_GetModuleFileNameExA(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, 0); u32 cap = (u32)ARG(3); size_t l = e_strlen(p); if (l + 1 > cap) l = cap ? cap - 1 : 0; put_a(c, ARG(2), p, cap); RET(l); }
static void s_GetModuleFileNameExW(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } char p[320]; proc_image_path(pid, p, sizeof p, 0); u32 cap = (u32)ARG(3); size_t l = e_strlen(p); if (l + 1 > cap) l = cap ? cap - 1 : 0; put_w(c, ARG(2), p, cap); RET(l); }
static void s_GetModuleBaseNameA(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); const char *name = 0; if (pid < 0 || !proc_image_base(pid, &name)) { W->last_error = 6; RET(0); return; } u32 cap = (u32)ARG(3); size_t l = e_strlen(name); if (l + 1 > cap) l = cap ? cap - 1 : 0; put_a(c, ARG(2), name, cap); RET(l); }
static void s_ReadProcessMemory(Cpu *c) {
  int pid = pid_of_handle(c, ARG(0)); u64 n = ARG(3); if (pid < 0 || n > (1u << 24)) { W->last_error = 6; RET(0); return; }
  u8 *dst = mem_ptr(c->mem, ARG(2), n, 1); if (!dst) { W->last_error = 998; RET(0); return; }
  if (pid == W->pid) { const u8 *s = mem_ptr(c->mem, ARG(1), n, 0); if (!s) { W->last_error = 299; RET(0); return; } e_memcpy(dst, s, (size_t)n); }
  else if (!proc_read_mem(pid, ARG(1), dst, n)) { W->last_error = 299; if (ARG(4)) wr64(c, ARG(4), 0); RET(0); return; }
  if (ARG(4)) wr64(c, ARG(4), n); RET(1);
}
/* the emulated Windows is 64-bit: a 32-bit (PE32) process runs "under WOW64", so 32-bit setup stubs that
   probe for a 64-bit OS (7-Zip x64 setup, many NSIS/Inno installers) take their 64-bit path instead of refusing */
static void s_IsWow64Process(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), W->mode32 ? 1 : 0); RET(1); }
static void s_IsWow64Process2(Cpu *c) { if (ARG(1)) wr16(c, ARG(1), W->mode32 ? 0x14c : 0); if (ARG(2)) wr16(c, ARG(2), 0x8664); RET(1); }
/* CreateRemoteThread(ExitProcess, code) is how busybox kill delivers signals; anything else is refused */
static void s_CreateRemoteThread(Cpu *c) {
  int pid = pid_of_handle(c, ARG(0)); u64 start = ARG(3); if (pid < 0) { W->last_error = 6; RET(0); return; }
  const char *fn = 0; if (start >= G_SHIM_BASE && start < G_SHIM_BASE + (u64)MAX_SHIMS * SHIM_SLOT_SIZE) fn = W->slots[(start - G_SHIM_BASE) / SHIM_SLOT_SIZE].name;
  if (fn && (!e_strcmp(fn, "ExitProcess") || !e_strcmp(fn, "TerminateProcess"))) { if (pid == W->pid) { do_exit(c, (int)ARG(4)); return; } proc_kill(pid, (int)ARG(4)); if (ARG(6)) wr32(c, ARG(6), (u32)pid + 1); RET(0x7100 + (u64)pid); return; }
  if (pid == W->pid) { extern u64 w32_thread_create_api(Cpu *c, u64 start, u64 param, u64 stack, int suspended, u32 *tid); u32 tid = 0; u64 h = w32_thread_create_api(c, start, ARG(4), ARG(2), (u32)ARG(5) & 4, &tid); if (h && ARG(6)) wr32(c, ARG(6), tid); RET(h); return; }
  W->last_error = 5; RET(0);
}
static void s_GetProcessTimes(Cpu *c) { int pid = pid_of_handle(c, ARG(0)); if (pid < 0) { W->last_error = 6; RET(0); return; } double now = host_now_ms(); u64 t = now_100ns(); u64 created = t - (u64)((now - proc_start_ms(pid)) * 10000.0); wr64(c, ARG(1), created); wr64(c, ARG(2), 0); wr64(c, ARG(3), 0); wr64(c, ARG(4), (u64)((now - proc_start_ms(pid)) * 10000)); RET(1); }
static void s_SetHandleInformation(Cpu *c) { int fd = handle_fd(ARG(0)); if (fd < 3) { RET(1); return; } if ((u32)ARG(1) & 1) vfs_set_inherit(fd, (u32)ARG(2) & 1); RET(1); }
static void s_GetHandleInformation(Cpu *c) { int fd = handle_fd(ARG(0)); if (!fd) { W->last_error = 6; RET(0); return; } int inh = fd < 3 ? 1 : vfs_set_inherit(fd, -1); if (fd >= 3) vfs_set_inherit(fd, inh); if (ARG(1)) wr32(c, ARG(1), inh > 0 ? 1 : 0); RET(1); }
static void s_commit(Cpu *c) { RET(0); }
/* stat family */
static void fill_stat(Cpu *c, u64 st, int kind, u64 size, int big) {
  u64 t = unix_time(); wr32(c, st, 0); wr16(c, st + 4, 0); wr16(c, st + 6, (u16)((kind == 2 ? 0x4000 | 0x1ff : kind == 3 ? 0x2000 | 0x1b6 : kind == 4 ? 0x1000 | 0x1b6 : 0x8000 | 0x1b6))); wr16(c, st + 8, 1); wr16(c, st + 10, 0); wr16(c, st + 12, 0); wr32(c, st + 16, 0);
  if (big) { wr64(c, st + 24, size); wr64(c, st + 32, t); wr64(c, st + 40, t); wr64(c, st + 48, t); } else { wr32(c, st + 20, (u32)size); wr64(c, st + 24, t); wr64(c, st + 32, t); wr64(c, st + 40, t); }
}
static void stat_path(Cpu *c, const char *n, u64 st, int big) { u64 sz; int k = vfs_stat(n, &sz); if (!k) { set_errno(c, 2); RET(~0ull); return; } fill_stat(c, st, k, sz, big); RET(0); }
static void s_stat64(Cpu *c) { stat_path(c, name_a(c, ARG(0)), ARG(1), 1); }
static void s_stat64i32(Cpu *c) { stat_path(c, name_a(c, ARG(0)), ARG(1), 0); }
static void s_wstat64(Cpu *c) { stat_path(c, name_w(c, ARG(0)), ARG(1), 1); }
static void s_wstat64i32(Cpu *c) { stat_path(c, name_w(c, ARG(0)), ARG(1), 0); }
static void fstat_fd(Cpu *c, int fd, u64 st, int big) { int r = vfs_resolve(fd); if (r == -1 || r == 1 || r == 2) { fill_stat(c, st, 3, 0, big); RET(0); return; } if (r >= 3 && vfs_kind(r) == FD_NULL) { fill_stat(c, st, 3, 0, big); RET(0); return; } if (r >= 3 && vfs_kind(r) == FD_PIPE) { fill_stat(c, st, 4, 0, big); RET(0); return; } fd = r; s64 s = vfs_size(fd); if (s < 0) { set_errno(c, 9); RET(~0ull); return; } fill_stat(c, st, vfs_fd_isdir(fd) ? 2 : 1, (u64)s, big); RET(0); }
static void s_fstat64(Cpu *c) { fstat_fd(c, (int)ARG(0), ARG(1), 1); }
static void s_fstat64i32(Cpu *c) { fstat_fd(c, (int)ARG(0), ARG(1), 0); }
static void s_access(Cpu *c) { u64 sz; if (vfs_stat(name_a(c, ARG(0)), &sz)) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_waccess(Cpu *c) { u64 sz; if (vfs_stat(name_w(c, ARG(0)), &sz)) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_access_s(Cpu *c) { u64 sz; RET(vfs_stat(name_a(c, ARG(0)), &sz) ? 0 : 2); }
static void s_unlink(Cpu *c) { if (vfs_remove(name_a(c, ARG(0)))) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_wunlink(Cpu *c) { if (vfs_remove(name_w(c, ARG(0)))) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_mkdir(Cpu *c) { if (vfs_mkdir(name_a(c, ARG(0)))) RET(0); else { set_errno(c, 17); RET(~0ull); } }
static void s_wmkdir(Cpu *c) { if (vfs_mkdir(name_w(c, ARG(0)))) RET(0); else { set_errno(c, 17); RET(~0ull); } }
static void s_rmdir(Cpu *c) { RET(vfs_remove(name_a(c, ARG(0))) ? 0 : ~0ull); }
static void s_rename(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_a(c, ARG(0))); RET(vfs_rename(from, name_a(c, ARG(1))) ? 0 : ~0ull); }
static void s_wrename(Cpu *c) { char from[1024]; e_snprintf(from, sizeof from, "%s", name_w(c, ARG(0))); RET(vfs_rename(from, name_w(c, ARG(1))) ? 0 : ~0ull); }
static void s_getcwd(Cpu *c) { const char *d = vfs_cwd(); size_t l = e_strlen(d); u64 buf = ARG(0); u32 n = (u32)ARG(1); if (!buf) { buf = w32_heap_alloc(W, l + 1, 0); n = (u32)l + 1; } if (n <= l) { set_errno(c, 34); RET(0); return; } put_a(c, buf, d, 0); RET(buf); }
static void s_wgetcwd(Cpu *c) { const char *d = vfs_cwd(); size_t l = e_strlen(d); u64 buf = ARG(0); u32 n = (u32)ARG(1); if (!buf) { buf = w32_heap_alloc(W, (l + 1) * 2, 0); n = (u32)l + 1; } if (n <= l) { set_errno(c, 34); RET(0); return; } put_w(c, buf, d, 0); RET(buf); }
static void s_chdir(Cpu *c) { if (vfs_chdir(name_a(c, ARG(0)))) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_wchdir(Cpu *c) { if (vfs_chdir(name_w(c, ARG(0)))) RET(0); else { set_errno(c, 2); RET(~0ull); } }
static void s_fullpath(Cpu *c) { char out[1024]; vfs_normalize(ARG(1) ? name_a(c, ARG(1)) : ".", out, sizeof out); size_t l = e_strlen(out) + 1; u64 dst = ARG(0); if (!dst) dst = w32_heap_alloc(W, l, 0); else if (ARG(2) < l) { RET(0); return; } put_a(c, dst, out, 0); RET(dst); }
static void s_wfullpath(Cpu *c) { char out[1024]; vfs_normalize(ARG(1) ? name_w(c, ARG(1)) : ".", out, sizeof out); size_t l = e_strlen(out) + 1; u64 dst = ARG(0); if (!dst) dst = w32_heap_alloc(W, l * 2, 0); else if (ARG(2) < l) { RET(0); return; } put_w(c, dst, out, 0); RET(dst); }
/* _findfirst64 family (struct _finddata64_t: attrib(4) pad(4) ctime(8) atime(8) wtime(8) size(8) name[260]) */
static void s_findfirst64(Cpu *c) { int h = vfs_find_first(name_a(c, ARG(0))); char nm[260]; u64 sz; int dir; if (h < 0 || !vfs_find_next(h, nm, sizeof nm, &sz, &dir)) { if (h >= 0) vfs_find_close(h); set_errno(c, 2); RET(~0ull); return; } u64 a = ARG(1); u64 t = unix_time(); wr32(c, a, dir ? 0x10 : 0x80); wr64(c, a + 8, t); wr64(c, a + 16, t); wr64(c, a + 24, t); wr64(c, a + 32, sz); put_a(c, a + 40, nm, 260); RET(H_FIND_BASE + h); }
static void s_findnext64(Cpu *c) { u64 h = ARG(0); char nm[260]; u64 sz; int dir; if (h < H_FIND_BASE || !vfs_find_next((int)(h - H_FIND_BASE), nm, sizeof nm, &sz, &dir)) { set_errno(c, 2); RET(~0ull); return; } u64 a = ARG(1); u64 t = unix_time(); wr32(c, a, dir ? 0x10 : 0x80); wr64(c, a + 8, t); wr64(c, a + 16, t); wr64(c, a + 24, t); wr64(c, a + 32, sz); put_a(c, a + 40, nm, 260); RET(0); }
static void s_findclose(Cpu *c) { u64 h = ARG(0); if (h >= H_FIND_BASE) vfs_find_close((int)(h - H_FIND_BASE)); RET(0); }
/* console input helpers */
static void s_ReadConsoleA(Cpu *c) { s64 r = fd_read(c, -1, ARG(1), (u32)ARG(2)); if (r == -2) { blocked_io(c, 0); return; } if (ARG(3)) wr32(c, ARG(3), r < 0 ? 0 : (u32)r); RET(1); }
/* UTF-8 bytes → UTF-16 units (surrogate pairs for astral code points); returns the unit count */
static size_t utf8_to_w16(const u8 *s, size_t n, u16 *out, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n && o < cap) {
    u32 b = s[i], cp = b; int extra = 0;
    if (b >= 0xF0) { cp = b & 7; extra = 3; } else if (b >= 0xE0) { cp = b & 15; extra = 2; } else if (b >= 0xC0) { cp = b & 31; extra = 1; }
    i++; for (int k = 0; k < extra; k++) { if (i < n && (s[i] & 0xC0) == 0x80) cp = (cp << 6) | (s[i] & 0x3F), i++; else { cp = 0xFFFD; break; } }
    if (cp >= 0x10000) { if (o + 2 > cap) break; cp -= 0x10000; out[o++] = (u16)(0xD800 + (cp >> 10)); out[o++] = (u16)(0xDC00 + (cp & 0x3FF)); } else out[o++] = (u16)cp;
  }
  return o;
}
static void s_ReadConsoleW(Cpu *c) { u8 tmp[2048]; u16 wtmp[2048]; u32 want = (u32)ARG(2); if (want > 2048) want = 2048; g_last_io_fd = -1; s64 r = vfs_stdin_read(tmp, want); if (r == -2) { blocked_io(c, 0); return; } if (r < 0) r = 0;
  size_t n = utf8_to_w16(tmp, (size_t)r, wtmp, want); if (!n && want >= 3) { wtmp[0] = 0x1a; wtmp[1] = 13; wtmp[2] = 10; n = 3; }   /* EOF shows up as Ctrl-Z, exactly like a real console */
  W->last_error = 0; for (size_t i = 0; i < n; i++) wr16(c, ARG(1) + i * 2, wtmp[i]); if (ARG(3)) wr32(c, ARG(3), (u32)n); RET(1); }
static void s_GetNumberOfConsoleInputEvents(Cpu *c) { int fd = fd_of_handle(ARG(0)); if (fd != -1 && fd != 0) { W->last_error = 6; RET(0); return; }   /* only the input buffer answers: Python tells console in/out apart this way */
  if (ARG(1)) wr32(c, ARG(1), vfs_stdin_available() ? 1 : 0); RET(1); }
static void s_kbhit(Cpu *c) { RET(vfs_stdin_available() ? 1 : 0); }
static void s_getch(Cpu *c) { u8 ch; g_last_io_fd = -1; s64 r = vfs_stdin_read(&ch, 1); if (r == -2) { blocked_io(c, 0); return; } RET(r == 1 ? ch : ~0ull); }
static void s_gets_s(Cpu *c) { /* reuse fgets: (buf, n, stdin) */ u64 buf = ARG(0); int n = (int)ARG(1); u8 tmp[4096]; g_last_io_fd = -1; s64 r = vfs_stdin_read(tmp, (u64)(n - 1) < sizeof tmp ? (u64)(n - 1) : sizeof tmp); if (r == -2) { blocked_io(c, 0); return; } if (r <= 0) { RET(0); return; } while (r > 0 && (tmp[r - 1] == '\n' || tmp[r - 1] == '\r')) r--; u8 *p = mem_ptr(c->mem, buf, (u64)r + 1, 1); if (p) { e_memcpy(p, tmp, (size_t)r); p[r] = 0; } RET(buf); }


/* console input records (INPUT_RECORD = 20 bytes): one KEY_EVENT (key down) per stdin byte; '\n' → VK_RETURN '\r' */
static void put_key_record(Cpu *c, u64 a, u16 ch) {
  u16 vk = ch == '\n' ? 0x0D : ch == '\t' ? 0x09 : ch == 8 ? 0x08 : ch == 27 ? 0x1B : (ch >= 'a' && ch <= 'z') ? (u16)(ch - 32) : (ch >= 'A' && ch <= 'Z') ? ch : (ch >= '0' && ch <= '9') ? ch : 0;
  if (ch == '\n') ch = '\r';
  wr16(c, a, 1); wr16(c, a + 2, 0); wr32(c, a + 4, 1); wr16(c, a + 8, 1); wr16(c, a + 10, vk); wr16(c, a + 12, 0); wr16(c, a + 14, ch); wr32(c, a + 16, 0);
}
/* stdin bytes are UTF-8; key records carry UTF-16 code units (surrogate pairs for astral characters, like the real console with VT input) */
static u8 g_kin_pend[8]; static int g_kin_npend;
static void s_ReadConsoleInputW(Cpu *c) {
  u32 cap = (u32)ARG(2); if (!cap) { if (ARG(3)) wr32(c, ARG(3), 0); RET(1); return; }
  u8 tmp[264]; g_last_io_fd = -1; u32 want = cap < 256 ? cap : 256;
  e_memcpy(tmp, g_kin_pend, (size_t)g_kin_npend);
  s64 r = vfs_stdin_read(tmp + g_kin_npend, want);
  if (r == -2) { blocked_io(c, 0); return; }
  if (r <= 0) { /* EOF: deliver Ctrl-Z Enter so line readers terminate */ g_kin_npend = 0; put_key_record(c, ARG(1), 26); if (ARG(3)) wr32(c, ARG(3), 1); RET(1); return; }
  s64 n = r + g_kin_npend; g_kin_npend = 0; u32 out = 0; s64 i = 0;
  while (i < n && out < cap) {
    u8 b = tmp[i]; int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1; u32 cp = b;
    if (len > 1 && i + len > n) { if (n - i <= 4) { g_kin_npend = (int)(n - i); e_memcpy(g_kin_pend, tmp + i, (size_t)g_kin_npend); } break; }   /* incomplete sequence: keep for the next call */
    if (len == 2) cp = ((b & 0x1F) << 6) | (tmp[i + 1] & 0x3F); else if (len == 3) cp = ((b & 0x0F) << 12) | ((tmp[i + 1] & 0x3F) << 6) | (tmp[i + 2] & 0x3F); else if (len == 4) cp = ((b & 0x07) << 18) | ((tmp[i + 1] & 0x3F) << 12) | ((tmp[i + 2] & 0x3F) << 6) | (tmp[i + 3] & 0x3F);
    i += len;
    if (cp >= 0x10000) { if (out + 2 > cap) { i -= len; break; } cp -= 0x10000; put_key_record(c, ARG(1) + (u64)out * 20, (u16)(0xD800 + (cp >> 10))); out++; put_key_record(c, ARG(1) + (u64)out * 20, (u16)(0xDC00 + (cp & 0x3FF))); out++; }
    else { put_key_record(c, ARG(1) + (u64)out * 20, (u16)cp); out++; }
  }
  if (i < n && !g_kin_npend) { vfs_stdin_unread(tmp + i, (int)(n - i)); }
  if (ARG(3)) wr32(c, ARG(3), out); RET(1);
}
static void s_PeekConsoleInputW(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), vfs_stdin_available() ? 1 : 0); if (vfs_stdin_available() && ARG(2)) put_key_record(c, ARG(1), '\n'); RET(1); }
static void s_FlushConsoleInputBuffer(Cpu *c) { RET(1); }
static void s_PeekConsoleInputA(Cpu *c) { s_PeekConsoleInputW(c); }

/* ---- process environment (dynamic): host table, mirrored into guest memory on demand ---- */
#define ENV_MAX 512
#define g_env (vfs_P->env)
#define g_nenv (vfs_P->nenv)
#define g_env_dirty (vfs_P->env_dirty)
static int env_find(const char *name, size_t nl) { for (int i = 0; i < g_nenv; i++) { const char *e = g_env[i]; size_t k = 0; while (k < nl && e[k] && lc_(e[k]) == lc_(name[k])) k++; if (k == nl && e[k] == '=') return i; } return -1; }
static const char *env_get(const char *name) { int i = env_find(name, e_strlen(name)); return i < 0 ? 0 : g_env[i] + e_strlen(name) + 1; }
static void env_put(const char *kv) {   /* "NAME=value" sets, "NAME=" removes (msvcrt semantics) */
  const char *eq = e_strchr(kv, '='); if (!eq || eq == kv) return; size_t nl = (size_t)(eq - kv); int i = env_find(kv, nl);
  if (!eq[1]) { if (i >= 0) { g_env[i] = g_env[--g_nenv]; g_env_dirty = 1; } return; }
  size_t l = e_strlen(kv) + 1; char *copy = emu_alloc(l); e_memcpy(copy, kv, l);
  if (i >= 0) g_env[i] = copy; else if (g_nenv < ENV_MAX) g_env[g_nenv++] = copy; g_env_dirty = 1;
}
static void env_set(const char *name, const char *val) { char kv[4096]; e_snprintf(kv, sizeof kv, "%s=%s", name, val ? val : ""); env_put(kv); }
void w32_env_replace(const char *const *kv, int n) { g_nenv = 0; for (int i = 0; i < n; i++) env_put(kv[i]); g_env_dirty = 1; }
void w32_env_init(const char **envs) { if (g_nenv) { g_env_dirty = 1; return; } g_env_dirty = 1; for (int i = 0; envs[i]; i++) env_put(envs[i]); }
static void env_sync(Cpu *c) {   /* rebuild guest envp[], narrow block and wide block */
  if (!g_env_dirty) return; g_env_dirty = 0; Win32 *w = W;
  size_t total = 1; for (int i = 0; i < g_nenv; i++) total += e_strlen(g_env[i]) + 1;
  u64 envp = w32_heap_alloc(w, 8 * (size_t)(g_nenv + 1), 1), wenvp = w32_heap_alloc(w, 8 * (size_t)(g_nenv + 1), 1), blk = w32_heap_alloc(w, total, 1), wblk = w32_heap_alloc(w, total * 2, 1); u64 o = 0;
  for (int i = 0; i < g_nenv; i++) { size_t l = e_strlen(g_env[i]) + 1; put_a(c, blk + o, g_env[i], 0); for (size_t k = 0; k < l; k++) wr16(c, wblk + (o + k) * 2, (u8)g_env[i][k]); wrp(c, envp + i * PSZ, blk + o); wrp(c, wenvp + i * PSZ, wblk + o * 2); o += l; }
  wr8(c, blk + o, 0); wr16(c, wblk + o * 2, 0); wrp(c, envp + g_nenv * PSZ, 0); wrp(c, wenvp + g_nenv * PSZ, 0);
  w->env_a = blk; w->env_w = wblk; wr64(c, w->ubuf + 0x228, envp); wr64(c, w->ubuf + 0x230, wenvp);   /* _environ and _wenviron */
}
static void s_getenv(Cpu *c) { const char *v = env_get(gstr(c, ARG(0))); if (!v) { RET(0); return; } env_sync(c); /* return pointer inside the guest block so it stays valid */ u64 blk = W->env_a; const char *n = gstr(c, ARG(0)); size_t nl = e_strlen(n); u64 p = blk; for (int i = 0; i < g_nenv; i++) { size_t l = e_strlen(g_env[i]) + 1; if (env_find(n, nl) == i) { RET(p + nl + 1); return; } p += l; } RET(0); }
static void s_wgetenv(Cpu *c) { const char *v = env_get(name_w(c, ARG(0))); if (!v) { RET(0); return; } u64 p = w32_heap_alloc(W, (e_strlen(v) + 1) * 2, 0); put_w(c, p, v, 0); RET(p); }
static void s_getenv_s(Cpu *c) { const char *v = env_get(gstr(c, ARG(3))); size_t l = v ? e_strlen(v) + 1 : 0; if (ARG(0)) wr64(c, ARG(0), l); if (!v) { RET(0); return; } if (ARG(2) < l) { RET(34); return; } put_a(c, ARG(1), v, 0); RET(0); }
static void s_dupenv_s(Cpu *c) { const char *v = env_get(gstr(c, ARG(2))); if (!v) { wr64(c, ARG(0), 0); if (ARG(1)) wr64(c, ARG(1), 0); RET(0); return; } wr64(c, ARG(0), w32_guest_strdup(W, v)); if (ARG(1)) wr64(c, ARG(1), e_strlen(v) + 1); RET(0); }
static void s_putenv(Cpu *c) { env_put(gstr(c, ARG(0))); env_sync(c); RET(0); }   /* msvcrt rebuilds _environ at once; clearenv() loops on environ[0] */
static void s_wputenv(Cpu *c) { env_put(name_w(c, ARG(0))); env_sync(c); RET(0); }
static void s_wputenv_s(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_w(c, ARG(0))); env_set(n, name_w(c, ARG(1))); env_sync(c); RET(0); }
static void s_putenv_s(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", gstr(c, ARG(0))); env_set(n, gstr(c, ARG(1))); env_sync(c); RET(0); }
static void s_GetEnvironmentVariableA(Cpu *c) { const char *v = env_get(gstr(c, ARG(0))); if (!v) { W->last_error = 203; RET(0); return; } size_t l = e_strlen(v); if ((u32)ARG(2) <= l) { RET(l + 1); return; } put_a(c, ARG(1), v, 0); RET(l); }
static void s_GetEnvironmentVariableW(Cpu *c) { const char *v = env_get(name_w(c, ARG(0))); if (!v) { W->last_error = 203; RET(0); return; } size_t l = e_strlen(v); if ((u32)ARG(2) <= l) { RET(l + 1); return; } put_w(c, ARG(1), v, 0); RET(l); }
static void s_SetEnvironmentVariableA(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", gstr(c, ARG(0))); if (ARG(1)) env_set(n, gstr(c, ARG(1))); else env_set(n, ""); RET(1); }
static void s_SetEnvironmentVariableW(Cpu *c) { char n[1024]; e_snprintf(n, sizeof n, "%s", name_w(c, ARG(0))); if (ARG(1)) env_set(n, name_w(c, ARG(1))); else env_set(n, ""); RET(1); }
static size_t expand_env(const char *s, char *out, size_t cap) { size_t o = 0; while (*s) { if (*s == '%') { const char *e = e_strchr(s + 1, '%'); if (e) { char n[256]; size_t nl = (size_t)(e - s - 1); if (nl < sizeof n) { e_memcpy(n, s + 1, nl); n[nl] = 0; const char *v = env_get(n); if (v) { size_t vl = e_strlen(v); if (o + vl < cap) e_memcpy(out + o, v, vl); o += vl; s = e + 1; continue; } } } } if (o + 1 < cap) out[o] = *s; o++; s++; } if (o < cap) out[o] = 0; else out[cap - 1] = 0; return o + 1; }
static void s_ExpandEnvironmentStringsA(Cpu *c) { char out[4096]; size_t n = expand_env(gstr(c, ARG(0)), out, sizeof out); if (ARG(1) && (u32)ARG(2) >= n) put_a(c, ARG(1), out, 0); RET(n); }
static void s_ExpandEnvironmentStringsW(Cpu *c) { char out[4096]; size_t n = expand_env(name_w(c, ARG(0)), out, sizeof out); if (ARG(1) && (u32)ARG(2) >= n) put_w(c, ARG(1), out, 0); RET(n); }
static void s_GetEnvironmentStrings(Cpu *c) { env_sync(c); RET(W->env_a); }
static void s_GetEnvironmentStringsW(Cpu *c) { env_sync(c); RET(W->env_w); }
static void s_p__environ(Cpu *c) { env_sync(c); RET(W->ubuf + 0x228); }
static void s_p__wenviron(Cpu *c) { env_sync(c); RET(W->ubuf + 0x230); }
static void s_get_initial_wide_environment(Cpu *c) { env_sync(c); RET(rd64(c, W->ubuf + 0x230)); }
static void s_get_initial_narrow_environment(Cpu *c) { env_sync(c); RET(rd64(c, W->ubuf + 0x228)); }
static void s_getmainargs(Cpu *c) { env_sync(c); wr32(c, ARG(0), rd32(c, W->ubuf + 0x200)); wrp(c, ARG(1), rd64(c, W->ubuf + 0x208)); wrp(c, ARG(2), rd64(c, W->ubuf + 0x228)); RET(0); }
/* ---- string helpers that were ret0 stubs ---- */
static int in_set(const char *set, char ch) { for (; *set; set++) if (*set == ch) return 1; return 0; }
static void s_strspn(Cpu *c) { const char *s = gstr(c, ARG(0)); const char *set = gstr(c, ARG(1)); u64 n = 0; while (s[n] && in_set(set, s[n])) n++; RET(n); }
static void s_strcspn(Cpu *c) { const char *s = gstr(c, ARG(0)); const char *set = gstr(c, ARG(1)); u64 n = 0; while (s[n] && !in_set(set, s[n])) n++; RET(n); }
static void s_strpbrk(Cpu *c) { const char *s = gstr(c, ARG(0)); const char *set = gstr(c, ARG(1)); for (u64 n = 0; s[n]; n++) if (in_set(set, s[n])) { RET(ARG(0) + n); return; } RET(0); }
#define g_strtok_next (vfs_P->strtok_next)
static void strtok_impl(Cpu *c, u64 str, u64 delim, u64 *state) {
  if (!str) str = *state; if (!str) { RET(0); return; }
  const char *d = gstr(c, delim); const char *s = gstr(c, str); u64 i = 0; while (s[i] && in_set(d, s[i])) i++;
  if (!s[i]) { *state = 0; RET(0); return; }
  u64 start = i; while (s[i] && !in_set(d, s[i])) i++;
  if (s[i]) { wr8(c, str + i, 0); *state = str + i + 1; } else *state = 0;
  RET(str + start);
}
static void s_strtok(Cpu *c) { strtok_impl(c, ARG(0), ARG(1), &g_strtok_next); }
static void s_strtok_s(Cpu *c) { u64 st = rd64(c, ARG(2)); strtok_impl(c, ARG(0), ARG(1), &st); wr64(c, ARG(2), st); }
static void s_strlwr(Cpu *c) { u64 a = ARG(0); for (u64 i = 0;; i++) { u8 ch = rd8(c, a + i); if (!ch) break; if (ch >= 'A' && ch <= 'Z') wr8(c, a + i, ch + 32); } RET(a); }
static void s_strupr(Cpu *c) { u64 a = ARG(0); for (u64 i = 0;; i++) { u8 ch = rd8(c, a + i); if (!ch) break; if (ch >= 'a' && ch <= 'z') wr8(c, a + i, ch - 32); } RET(a); }
static void s_strrev(Cpu *c) { u64 a = ARG(0); u64 l = e_strlen(gstr(c, a)); for (u64 i = 0; i + 1 < l - i && l; i++) { u8 x = rd8(c, a + i), y = rd8(c, a + l - 1 - i); wr8(c, a + i, y); wr8(c, a + l - 1 - i, x); } RET(a); }
static void itoa_impl(Cpu *c, s64 v, int is_signed, u64 dst, int radix) {
  char buf[72]; int n = 0; u64 u = (u64)v; int neg = 0; if (radix < 2 || radix > 36) radix = 10;
  if (is_signed && radix == 10 && v < 0) { neg = 1; u = (u64)(-v); }
  do { int d = (int)(u % (u64)radix); buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (u64)radix; } while (u);
  if (neg) buf[n++] = '-';
  for (int i = 0; i < n; i++) wr8(c, dst + i, (u8)buf[n - 1 - i]); wr8(c, dst + n, 0); RET(dst);
}
static void s_itoa(Cpu *c) { itoa_impl(c, (s32)(u32)ARG(0), 1, ARG(1), (int)ARG(2)); }
static void s_ultoa(Cpu *c) { itoa_impl(c, (s64)(u32)ARG(0), 0, ARG(1), (int)ARG(2)); }
static void s_i64toa(Cpu *c) { itoa_impl(c, (s64)ARG(0), 1, ARG(1), (int)ARG(2)); }
static void s_ui64toa(Cpu *c) { itoa_impl(c, (s64)ARG(0), 0, ARG(1), (int)ARG(2)); }
static const char *errstr(int e) {
  switch (e) { case 0: return "No error"; case 1: return "Operation not permitted"; case 2: return "No such file or directory"; case 4: return "Interrupted function call"; case 5: return "Input/output error"; case 6: return "No such device or address"; case 7: return "Arg list too long"; case 8: return "Exec format error"; case 9: return "Bad file descriptor"; case 10: return "No child processes"; case 11: return "Resource temporarily unavailable"; case 12: return "Not enough memory"; case 13: return "Permission denied"; case 16: return "Resource device"; case 17: return "File exists"; case 18: return "Improper link"; case 19: return "No such device"; case 20: return "Not a directory"; case 21: return "Is a directory"; case 22: return "Invalid argument"; case 23: return "Too many open files in system"; case 24: return "Too many open files"; case 25: return "Inappropriate I/O control operation"; case 27: return "File too large"; case 28: return "No space left on device"; case 29: return "Invalid seek"; case 30: return "Read-only file system"; case 31: return "Too many links"; case 32: return "Broken pipe"; case 33: return "Math argument"; case 34: return "Result too large"; case 36: return "Resource deadlock avoided"; case 38: return "File name too long"; case 39: return "Directory not empty"; case 41: return "Not a directory"; default: return "Unknown error"; }
}
static void s_strerror(Cpu *c) { u64 a = W->ubuf + 0x7d0; put_a(c, a, errstr((int)ARG(0)), 0x30); RET(a); }
/* __sys_nerr() / __sys_errlist(): the UCRT's errno message table (43 entries) */
static void s___sys_nerr(Cpu *c) { Win32 *w = W; if (!w->sys_nerr) { w->sys_nerr = shim_bump(w, 16); wr32(c, w->sys_nerr, 43); } RET(w->sys_nerr); }
static void s___sys_errlist(Cpu *c) { Win32 *w = W; if (!w->sys_errlist) { w->sys_errlist = w32_heap_alloc(w, 43 * 8, 1); for (int i = 0; i < 43; i++) wr64(c, w->sys_errlist + 8 * i, w32_guest_strdup(w, errstr(i))); } RET(w->sys_errlist); }
static void s_strerror_s(Cpu *c) { put_a(c, ARG(0), errstr((int)ARG(2)), (u32)ARG(1)); RET(0); }
static void s_strcpy_s(Cpu *c) { const char *s = gstr(c, ARG(2)); size_t l = e_strlen(s); if (l + 1 > ARG(1)) { if (ARG(1)) wr8(c, ARG(0), 0); RET(34); return; } put_a(c, ARG(0), s, 0); RET(0); }
static void s_strcat_s(Cpu *c) { const char *d = gstr(c, ARG(0)); size_t dl = e_strlen(d); const char *s = gstr(c, ARG(2)); size_t l = e_strlen(s); if (dl + l + 1 > ARG(1)) { RET(34); return; } char tmp[4096]; if (l >= sizeof tmp) { RET(34); return; } e_memcpy(tmp, s, l + 1); put_a(c, ARG(0) + dl, tmp, 0); RET(0); }
static void s_strncpy_s(Cpu *c) { const char *s = gstr(c, ARG(2)); size_t l = e_strlen(s); u64 n = ARG(3); if (n == ~0ull) n = l; if (l > n) l = n; if (l + 1 > ARG(1)) { if (ARG(1)) wr8(c, ARG(0), 0); RET(34); return; } char tmp[4096]; if (l >= sizeof tmp) { RET(34); return; } e_memcpy(tmp, s, l); tmp[l] = 0; put_a(c, ARG(0), tmp, 0); RET(0); }
static void s_memcpy_s(Cpu *c) { u64 n = ARG(3); if (n > ARG(1)) { RET(34); return; } u8 *d = mem_ptr(c->mem, ARG(0), n, 1); const u8 *s = mem_ptr(c->mem, ARG(2), n, 0); if (d && s && n) e_memmove(d, s, (size_t)n); RET(0); }
static void s_mktemp(Cpu *c) { u64 a = ARG(0); u64 l = e_strlen(gstr(c, a)); u32 u = ++g_tmp_seq; for (u64 i = l; i > 0 && rd8(c, a + i - 1) == 'X'; i--) { wr8(c, a + i - 1, (u8)('a' + u % 26)); u /= 26; u += 7; } RET(a); }

#define FS_SHIMS \
  S(CreateFileA), S(CreateFileW), SA("CreateFile2", CreateFileW), S(ReadFile), S(WriteFile), S(CloseHandle), S(GetFileType), S(GetFileSize), S(GetFileSizeEx), S(SetFilePointer), S(SetFilePointerEx), S(SetEndOfFile), \
  S(GetFileAttributesA), S(GetFileAttributesW), S(GetFileAttributesExA), S(GetFileAttributesExW), S(GetFileInformationByHandle), S(GetFileTime), \
  S(FindFirstFileA), S(FindFirstFileW), S(FindFirstFileExA), S(FindFirstFileExW), S(FindNextFileA), S(FindNextFileW), S(FindClose), \
  S(DeleteFileA), S(DeleteFileW), S(CreateDirectoryA), S(CreateDirectoryW), S(RemoveDirectoryA), S(RemoveDirectoryW), S(MoveFileA), S(MoveFileW), SA("MoveFileExA", MoveFileA), SA("MoveFileExW", MoveFileW), S(CopyFileA), S(CopyFileW), \
  S(GetCurrentDirectoryA), S(GetCurrentDirectoryW), S(SetCurrentDirectoryA), S(SetCurrentDirectoryW), S(GetFullPathNameA), S(GetFullPathNameW), S(GetTempPathA), S(GetTempPathW), SA("GetTempPath2A", GetTempPathA), SA("GetTempPath2W", GetTempPathW), S(GetTempFileNameA), S(GetTempFileNameW), \
  S(CreateFileMappingA), S(CreateFileMappingW), S(MapViewOfFile), S(MapViewOfFileEx), S(UnmapViewOfFile), \
  S(ReadConsoleA), S(ReadConsoleW), S(ReadConsoleInputW), SA("ReadConsoleInputA", ReadConsoleInputW), S(PeekConsoleInputW), S(PeekConsoleInputA), S(FlushConsoleInputBuffer), S(GetNumberOfConsoleInputEvents), \
  SA("_filbuf", filbuf), SA("_flsbuf", flsbuf), SA("_fdopen", fdopen), SA("fdopen", fdopen), S(setvbuf), S(fopen), SA("_wfopen", wfopen), S(fopen_s), SA("_wfopen_s", wfopen_s), SA("_fsopen", fsopen), SA("_wfsopen", wfopen), S(freopen), S(fclose), S(fread), SA("fread_s", fread), S(fwrite), S(fgets), S(fgetc), SA("getc", fgetc), S(getchar), S(ungetc), \
  S(fseek), SA("_fseeki64", fseeki64), S(ftell), S(fgetpos), S(fsetpos), SA("_ftelli64", ftell), S(rewind), S(feof), S(ferror), S(clearerr), SA("_fileno", fileno), SA("fileno", fileno), SA("_filelengthi64", filelengthi64), SA("_filelength", filelengthi64), SA("_isatty", isatty), SA("isatty", isatty), SA("_get_osfhandle", get_osfhandle), SA("_open_osfhandle", open_osfhandle), \
  SA("_open", open), SA("open", open), SA("_wopen", wopen), SA("_sopen_s", sopen_s), SA("_wsopen_s", wsopen_s), SA("_sopen", sopen), SA("_close", close), SA("close", close), SA("_read", read), SA("read", read), SA("_write", write), SA("write", write), \
  S(SetHandleInformation), S(GetHandleInformation), S(GetProcessImageFileNameA), S(GetProcessImageFileNameW), S(QueryFullProcessImageNameA), S(QueryFullProcessImageNameW), S(EnumProcessModules), S(EnumProcessModulesEx), S(GetModuleFileNameExA), S(GetModuleFileNameExW), S(GetModuleBaseNameA), S(ReadProcessMemory), S(IsWow64Process), S(IsWow64Process2), S(CreateRemoteThread), S(GetProcessTimes), SA("_spawnve", spawnve), SA("_spawnvpe", spawnve), SA("_spawnv", spawnv), SA("_spawnvp", spawnv), SA("_execve", execve), SA("_execvpe", execve), SA("_execv", execv), SA("_execvp", execv), SA("_wspawnve", spawnve), SA("_lseek", lseek), SA("lseek", lseek), SA("_lseeki64", lseeki64), SA("_tell", tell), SA("_telli64", tell), SA("_eof", eof), SA("_chsize", chsize), SA("_chsize_s", chsize), SA("_dup", dup), SA("dup", dup), SA("_dup2", dup2), S(setbuf), S(CreateToolhelp32Snapshot), S(Process32First), S(Process32Next), S(Process32FirstW), S(Process32NextW), S(CreateProcessA), S(CreateProcessW), S(WaitForSingleObject), S(WaitForSingleObjectEx), S(WaitForMultipleObjects), SA("WaitForMultipleObjectsEx", WaitForMultipleObjects), S(GetExitCodeProcess), S(GetProcessId), S(TerminateProcess), S(OpenProcess), S(CreatePipe), S(PeekNamedPipe), S(DuplicateHandle), S(GetCurrentProcessId), SA("_getpid", getpid), SA("getpid", getpid), SA("_cwait", cwait), SA("_pipe", pipe), S(FlushViewOfFile), SA("_commit", commit), \
  SA("_stat64", stat64), SA("_stat64i32", stat64i32), SA("_stat32", stat64i32), SA("_stat", stat64i32), SA("stat", stat64i32), SA("_wstat64", wstat64), SA("_wstat64i32", wstat64i32), SA("_fstat64", fstat64), SA("_fstat64i32", fstat64i32), SA("_fstat32", fstat64i32), SA("_fstat", fstat64i32), SA("fstat", fstat64i32), \
  SA("_access", access), SA("access", access), SA("_waccess", waccess), SA("_access_s", access_s), SA("_unlink", unlink), SA("unlink", unlink), SA("remove", unlink), SA("_wunlink", wunlink), SA("_wremove", wunlink), SA("_mkdir", mkdir), SA("mkdir", mkdir), SA("_wmkdir", wmkdir), SA("_rmdir", rmdir), SA("rmdir", rmdir), SA("_wrmdir", rmdir), \
  S(rename), SA("_wrename", wrename), SA("_getcwd", getcwd), SA("getcwd", getcwd), SA("_wgetcwd", wgetcwd), SA("_chdir", chdir), SA("chdir", chdir), SA("_wchdir", wchdir), SA("_fullpath", fullpath), SA("_wfullpath", wfullpath), \
  SA("_findfirst64", findfirst64), SA("_findfirst64i32", findfirst64), SA("_findnext64", findnext64), SA("_findnext64i32", findnext64), SA("_findclose", findclose), SA("_kbhit", kbhit), SA("_getch", getch), SA("_getche", getch), SA("gets_s", gets_s), \
  S(getenv), SA("__p___initenv", p__environ), SA("__p___winitenv", p__wenviron), SA("_wgetenv", wgetenv), S(getenv_s), SA("_dupenv_s", dupenv_s), SA("_putenv", putenv), SA("putenv", putenv), SA("_wputenv", wputenv), SA("_putenv_s", putenv_s), SA("_wputenv_s", wputenv_s), S(GetEnvironmentVariableA), S(GetEnvironmentVariableW), S(SetEnvironmentVariableA), S(SetEnvironmentVariableW), S(ExpandEnvironmentStringsA), S(ExpandEnvironmentStringsW), S(GetEnvironmentStrings), SA("GetEnvironmentStringsA", GetEnvironmentStrings), S(GetEnvironmentStringsW), SA("__p__environ", p__environ), SA("_get_initial_narrow_environment", get_initial_narrow_environment), SA("__getmainargs", getmainargs), \
  S(strspn), S(strcspn), S(strpbrk), S(strtok), S(strtok_s), SA("_strlwr", strlwr), SA("strlwr", strlwr), SA("_strupr", strupr), SA("strupr", strupr), SA("_strrev", strrev), SA("_itoa", itoa), SA("itoa", itoa), SA("_ltoa", itoa), SA("_ultoa", ultoa), SA("_i64toa", i64toa), SA("_ui64toa", ui64toa), S(strerror), S(__sys_nerr), S(__sys_errlist), SA("_strerror", strerror), S(strerror_s), S(strcpy_s), S(strcat_s), S(strncpy_s), S(memcpy_s), SA("memmove_s", memcpy_s), SA("_mktemp", mktemp),
