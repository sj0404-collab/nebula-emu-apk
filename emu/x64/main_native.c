/* x64run: native test runner.  x64run [--inspect] [--frames N] [--max-ms T] [--key VK@ms ...] file.exe [args...] */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int x64_load(const u8 *bytes, size_t len, const char *name, const char *cmdline);
static int g_apitrace;
int x64_run(u64 budget); void x64_trace(int on); const char *x64_inspect(const u8 *b, size_t n); const char *x64_error(void); u64 x64_error_rip(void); const char *x64_report(void); u64 x64_icount(void); int x64_exit_code(void); const char *x64_error_bytes(void); const char *x64_regs(void); const char *x64_history(void); int x64_mem_copy(u64 addr, unsigned char *dst, unsigned n); void x64_key(int vk, int down); void x64_mouse(int x, int y, int ev); double x64_sleep_ms(void); const char *x64_api_stats(void); int x64_has_window(void);
int x64_vfs_add(const char *path, const u8 *data, size_t n); int x64_vfs_mkdir(const char *path); const char *x64_vfs_list(void); void x64_stdin_write(const u8 *data, size_t n); void x64_stdin_eof(void); int x64_wants_stdin(void);
void host_set_dump(int every, const char *prefix); int host_frames(void); double host_now_ms(void);


#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
/* private copy-on-write mapping of a host file: a 200 MB Bun executable is then backed by the page cache instead of a malloc copy */
static u8 *map_file(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY); if (fd < 0) return 0; struct stat st; if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return 0; }
  size_t n = (size_t)st.st_size; if (!n) { close(fd); *len = 0; return (u8 *)malloc(1); }
  void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); close(fd); if (p == MAP_FAILED) return 0; *len = n; return (u8 *)p; }

static int mount_dir(const char *host, const char *guest, int depth) {   /* copies a host directory tree into the VFS */
  DIR *d = opendir(host); if (!d) return 0; struct dirent *e; int n = 0; x64_vfs_mkdir(guest);
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char hp[2048], gp[2048]; snprintf(hp, sizeof hp, "%s/%s", host, e->d_name); snprintf(gp, sizeof gp, "%s\\%s", guest, e->d_name);
    struct stat st; if (stat(hp, &st)) continue;
    if (S_ISDIR(st.st_mode)) { if (depth < 40) n += mount_dir(hp, gp, depth + 1); continue; }
    size_t got = 0; u8 *buf = map_file(hp, &got); if (!buf) continue;
    x64_vfs_add(gp, buf, got); n++;   /* the VFS references the mapping (copy-on-write pages, clean ones are reclaimable) */
  }
  closedir(d); return n;
}
/* X64_PROF=1: sample rip every 20 K instructions, report the hottest 64-byte code blocks (poor man's profiler for spin loops) */
static int g_prof; static struct { u64 key; u64 n; } g_pf[65536]; static u64 g_pf_total;
static void prof_sample(u64 rip) { u64 k = rip >> 6; u32 h = (u32)(k * 0x9E3779B97F4A7C15ull >> 48); for (int i = 0; i < 65536; i++) { u32 j = (h + i) & 65535; if (!g_pf[j].n || g_pf[j].key == k) { g_pf[j].key = k; g_pf[j].n++; g_pf_total++; return; } } }
static void prof_dump(void) { fprintf(stderr, "[x64run] profile: %llu samples, hottest 64-byte blocks:\n", (unsigned long long)g_pf_total);
  for (int t = 0; t < 30; t++) { int bi = -1; for (int j = 0; j < 65536; j++) if (g_pf[j].n && (bi < 0 || g_pf[j].n > g_pf[bi].n)) bi = j; if (bi < 0) break;
    u64 a = g_pf[bi].key << 6; const char *sn = ""; if (a >= 0x200000ull && a < 0x400000ull) { extern const char *x64_slot_name(u64); sn = x64_slot_name(a); }
    fprintf(stderr, "  %5.1f%%  0x%llx %s\n", 100.0 * (double)g_pf[bi].n / (double)g_pf_total, (unsigned long long)a, sn); g_pf[bi].n = 0; } }
int main(int argc, char **argv) {
  int inspect = 0, dump_every = 0; double max_ms = 60000; const char *prefix = "frame"; int quiet = 0;
  const char *mounts[16][2]; int nmounts = 0; int no_stdin = 0;
  struct { int vk; double at; int down; } keys[64]; int nkeys = 0; int ai = 1; int stats = 0;
  struct { int x, y; double at; int ev; } clicks[64]; int nclicks = 0;   /* --click x,y@ms: move + left down + up (ev 0 move, 1 down, 2 up) */
  for (; ai < argc && argv[ai][0] == '-'; ai++) {
    if (!strcmp(argv[ai], "--inspect")) inspect = 1;
    else if (!strcmp(argv[ai], "--frames") && ai + 1 < argc) dump_every = atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "--prefix") && ai + 1 < argc) prefix = argv[++ai];
    else if (!strcmp(argv[ai], "--max-ms") && ai + 1 < argc) max_ms = atof(argv[++ai]);
    else if (!strcmp(argv[ai], "--quiet")) quiet = 1;
    else if (!strcmp(argv[ai], "--trace")) x64_trace(1); else if (!strcmp(argv[ai], "--trace2")) x64_trace(2); else if (!strcmp(argv[ai], "--apitrace")) { extern void w32_api_trace(int); g_apitrace = 1; }
    else if (!strcmp(argv[ai], "--env") && ai + 1 < argc) { extern void w32_add_env(const char *); w32_add_env(argv[++ai]); }   /* one guest variable, value may contain ';' (PATH=…) */
    else if (!strcmp(argv[ai], "--mount-rw") && ai + 1 < argc) { extern void host_fs_mount_rw(const char *, const char *); const char *spec = argv[++ai]; char hostd[1024]; const char *eq = strchr(spec, '='); const char *guest = "C:\\nebula"; if (eq) { snprintf(hostd, sizeof hostd, "%.*s", (int)(eq - spec), spec); guest = eq + 1; } else snprintf(hostd, sizeof hostd, "%s", spec); char abs_[4096]; if (!realpath(hostd, abs_)) snprintf(abs_, sizeof abs_, "%s", hostd); host_fs_mount_rw(abs_, guest); nmounts++; mounts[nmounts - 1][0] = strdup(abs_); mounts[nmounts - 1][1] = guest; }   /* like --mount, but files the guest writes land on the host (and leave RAM) */
    else if (!strcmp(argv[ai], "--mount") && ai + 1 < argc) { const char *spec = argv[++ai]; char hostd[1024]; const char *eq = strchr(spec, '='); const char *guest = "C:\\nebula"; if (eq) { snprintf(hostd, sizeof hostd, "%.*s", (int)(eq - spec), spec); guest = eq + 1; } else snprintf(hostd, sizeof hostd, "%s", spec); char abs2[4096]; if (!realpath(hostd, abs2)) snprintf(abs2, sizeof abs2, "%s", hostd); nmounts++; mounts[nmounts - 1][0] = strdup(abs2); mounts[nmounts - 1][1] = guest; }
    else if (!strcmp(argv[ai], "--no-stdin")) no_stdin = 1;
    else if (!strcmp(argv[ai], "--stats")) stats = 1;
    else if (!strcmp(argv[ai], "--key") && ai + 1 < argc) { int vk; double at; int dur = 100; if (sscanf(argv[++ai], "%i@%lf:%d", &vk, &at, &dur) >= 2 && nkeys < 62) { keys[nkeys].vk = vk; keys[nkeys].at = at; keys[nkeys].down = 1; nkeys++; keys[nkeys].vk = vk; keys[nkeys].at = at + dur; keys[nkeys].down = 0; nkeys++; } }
    else if (!strcmp(argv[ai], "--click") && ai + 1 < argc) { int x, y; double at; if (sscanf(argv[++ai], "%d,%d@%lf", &x, &y, &at) == 3 && nclicks < 61) { clicks[nclicks].x = x; clicks[nclicks].y = y; clicks[nclicks].at = at; clicks[nclicks].ev = 0; nclicks++; clicks[nclicks] = clicks[nclicks - 1]; clicks[nclicks].ev = 1; clicks[nclicks].at = at + 1; nclicks++; clicks[nclicks] = clicks[nclicks - 1]; clicks[nclicks].ev = 2; clicks[nclicks].at = at + 60; nclicks++; } }
    else if (!strcmp(argv[ai], "--rclick") && ai + 1 < argc) { int x, y; double at; if (sscanf(argv[++ai], "%d,%d@%lf", &x, &y, &at) == 3 && nclicks < 61) { clicks[nclicks].x = x; clicks[nclicks].y = y; clicks[nclicks].at = at; clicks[nclicks].ev = 0; nclicks++; clicks[nclicks] = clicks[nclicks - 1]; clicks[nclicks].ev = 3; clicks[nclicks].at = at + 1; nclicks++; clicks[nclicks] = clicks[nclicks - 1]; clicks[nclicks].ev = 4; clicks[nclicks].at = at + 60; nclicks++; } }   /* --rclick: right button */
    else if (!strcmp(argv[ai], "--move") && ai + 1 < argc) { int x, y; double at; if (sscanf(argv[++ai], "%d,%d@%lf", &x, &y, &at) == 3 && nclicks < 63) { clicks[nclicks].x = x; clicks[nclicks].y = y; clicks[nclicks].at = at; clicks[nclicks].ev = 0; nclicks++; } }   /* --move x,y@ms: mouse move only (hover) */
    else { fprintf(stderr, "unknown option %s\n", argv[ai]); return 2; }
  }
  if (ai >= argc) { fprintf(stderr, "usage: x64run [--inspect] [--frames N] [--max-ms T] [--key VK@ms[:dur]] [--click x,y@ms] [--move x,y@ms] file.exe [args]\n"); return 2; }
  const char *path = argv[ai];
  size_t flen = 0; u8 *bytes = map_file(path, &flen); if (!bytes) { perror(path); return 2; } long len = (long)flen;
  if (inspect) { puts(x64_inspect(bytes, len)); return 0; }
  char cmdline[4096]; const char *base = strrchr(path, '/'); base = base ? base + 1 : path; snprintf(cmdline, sizeof cmdline, "%s", base);
  for (int i = ai + 1; i < argc; i++) {   /* MSVC command-line quoting rules */
    const char *s = argv[i]; size_t o = strlen(cmdline); if (o + 1 < sizeof cmdline) cmdline[o++] = ' ';
    int q = !s[0] || strpbrk(s, " \t\"") != 0; if (q && o + 1 < sizeof cmdline) cmdline[o++] = '"';
    for (const char *p = s; *p && o + 3 < sizeof cmdline; p++) { if (q && *p == '"') cmdline[o++] = '\\'; cmdline[o++] = *p; }
    if (q && o + 1 < sizeof cmdline) cmdline[o++] = '"'; cmdline[o] = 0;
  }
  host_set_dump(dump_every, prefix);
  for (int mi = 0; mi < nmounts; mi++) { int n = mount_dir(mounts[mi][0], mounts[mi][1], 0); if (!quiet) fprintf(stderr, "[x64run] mounted %s -> %s (%d files)\n", mounts[mi][0], mounts[mi][1], n); }   /* before load: DLLs next to the exe */
  if (getenv("X64_TAP")) { extern const char *g_vfs_tap; g_vfs_tap = getenv("X64_TAP"); }
  if (getenv("X64_GUEST_ENV")) { extern void w32_add_env(const char *); char *ge = strdup(getenv("X64_GUEST_ENV")); for (char *t = strtok(ge, ";"); t; t = strtok(0, ";")) w32_add_env(t); }
  char guest_name[4200]; { char abs_[4096]; const char *an = realpath(path, abs_) ? abs_ : path; const char *nm = base;
    for (int mi = 0; mi < nmounts; mi++) { size_t hl = strlen(mounts[mi][0]); if (!strncmp(an, mounts[mi][0], hl) && an[hl] == '/') { snprintf(guest_name, sizeof guest_name, "%s%s", mounts[mi][1], an + hl); for (char *q = guest_name; *q; q++) if (*q == '/') *q = '\\'; nm = guest_name; break; } }
    base = nm; }   /* an exe that lives inside a --mount keeps its real path (process.execPath, GetModuleFileName) */
  int rc = x64_load(bytes, len, base, cmdline);
  if (g_apitrace) { extern void w32_api_trace(int); w32_api_trace(1); }
  if (rc) { fprintf(stderr, "load failed (%d): %s", rc, x64_report()); return 3; }
  g_prof = getenv("X64_PROF") != 0;
  if (getenv("X64_DWATCH")) { extern u64 g_dwatch; g_dwatch = strtoull(getenv("X64_DWATCH"), 0, 16); }
  if (getenv("X64_WATCH")) { extern u64 g_watch[8]; extern int g_watch_n; char *ws = strdup(getenv("X64_WATCH")); for (char *t = strtok(ws, ","); t && g_watch_n < 8; t = strtok(0, ",")) g_watch[g_watch_n++] = strtoull(t, 0, 16); }
  u64 trace_after = getenv("X64_TRACE_AFTER") ? strtoull(getenv("X64_TRACE_AFTER"), 0, 10) : 0; int trace_on = 0;
  double t0 = host_now_ms(); int r; int ki = 0, ci = 0; u64 yields = 0; int stdin_eof = 0;
  for (;;) {
    double el = host_now_ms() - t0;
    if (el > max_ms) { if (!quiet) { fprintf(stderr, "[x64run] time limit reached\n"); if (getenv("X64_HIST")) { extern u64 x64_rip(void); fprintf(stderr, "[x64run] rip=%llx regs: %s\n[x64run] last blocks: %s\n", (unsigned long long)x64_rip(), x64_regs(), x64_history()); unsigned char b[512]; u64 a = (x64_rip() & ~0xfull) - 256; if (x64_mem_copy(a, b, 512)) { FILE *f = fopen("/tmp/rip_tl.bin", "wb"); if (f) { fwrite(b, 1, 512, f); fclose(f); fprintf(stderr, "[x64run] dumped 512 bytes from 0x%llx to /tmp/rip_tl.bin\n", (unsigned long long)a); } } } } r = -1; break; }
    while (ki < nkeys && keys[ki].at <= el) { x64_key(keys[ki].vk, keys[ki].down); ki++; }
    while (ci < nclicks && clicks[ci].at <= el) { x64_mouse(clicks[ci].x, clicks[ci].y, clicks[ci].ev); ci++; }
    if (g_prof) { extern u64 x64_rip(void); r = x64_run(20000); prof_sample(x64_rip()); } else r = x64_run(2000000);
    if (trace_after && !trace_on && x64_icount() >= trace_after) { extern void w32_api_trace(int); w32_api_trace(1); trace_on = 1; fprintf(stderr, "[x64run] apitrace enabled at %llu instructions\n", (unsigned long long)x64_icount()); }
    if (!no_stdin && !stdin_eof) {   /* feed host stdin as raw bytes whenever some arrive (TUIs read single key presses; never block the emulator on the terminal) */
      struct pollfd pf = { 0, POLLIN, 0 }; if (poll(&pf, 1, 0) > 0) { unsigned char buf[4096]; ssize_t got = read(0, buf, sizeof buf); if (got > 0) x64_stdin_write(buf, (size_t)got); else if (got == 0 || (errno != EAGAIN && errno != EINTR)) { stdin_eof = 1; x64_stdin_eof(); } } }
    if (r == RUN_YIELD && x64_wants_stdin()) { if (no_stdin || stdin_eof) { x64_stdin_eof(); continue; } struct pollfd pf = { 0, POLLIN, 0 }; if (poll(&pf, 1, 2) <= 0) { yields++; continue; } continue; }
    if (r == RUN_YIELD) { yields++; double s = x64_sleep_ms(); if (s > 0) { if (s > 16) s = 16; usleep((useconds_t)(s * 1000)); } else if (x64_has_window()) usleep(1000); continue; }
    if (r != RUN_OK) break;
  }
  double dt = (host_now_ms() - t0) / 1000.0; u64 ic = x64_icount();
  if (g_prof) prof_dump();
  if (!quiet) {
    fflush(stdout);
    if (r == RUN_EXIT) fprintf(stderr, "[x64run] exit code %d\n", x64_exit_code());
    else if (r == RUN_FAULT) fprintf(stderr, "[x64run] FAULT at rip=0x%llx: %s  bytes: %s\n", (unsigned long long)x64_error_rip(), x64_error(), x64_error_bytes()), fprintf(stderr, "[x64run] regs: %s\n", x64_regs()), fprintf(stderr, "[x64run] last blocks: %s\n", x64_history());
    if (getenv("X64_WNDDUMP")) { extern void x64_present_all(void); user_dump_windows(); x64_present_all(); }
    if (getenv("X64_DUMP")) { unsigned char b[512]; u64 a = x64_error_rip() & ~0xfull; if (a >= 256) a -= 256; if (x64_mem_copy(a, b, 512)) { FILE *f = fopen(getenv("X64_DUMP"), "wb"); if (f) { fwrite(b, 1, 512, f); fclose(f); fprintf(stderr, "[x64run] dumped 512 bytes from 0x%llx to %s\n", (unsigned long long)a, getenv("X64_DUMP")); } } }
    else if (r == RUN_BREAK) fprintf(stderr, "[x64run] breakpoint at rip=0x%llx\n", (unsigned long long)x64_error_rip());
    fprintf(stderr, "[x64run] %llu instructions in %.3fs = %.1f MIPS, %d frames, %llu yields\n", (unsigned long long)ic, dt, dt > 0 ? ic / dt / 1e6 : 0, host_frames(), (unsigned long long)yields);
#ifdef X64_OPHIST
    { extern void cpu_dump_ophist(void); cpu_dump_ophist(); }
#endif
    if (x64_report()[0]) fprintf(stderr, "[x64run] report:\n%s", x64_report());
    if (stats) fprintf(stderr, "[x64run] api calls: %s\n", x64_api_stats());
  }
  vfs_sync_all();   /* flush files still open / never closed to the write-back mounts */
  return r == RUN_EXIT ? x64_exit_code() : r == -1 ? 0 : 100 + r;
}
