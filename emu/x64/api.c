/* Entry points shared by the wasm64 build (exported to JS) and the native test runner. */
#include "emu.h"
#include "cmd_exe.h"
#include "pwsh_exe.h"

/* ---------- process table ---------- */
#define PROC_MAX 32
typedef struct { int used, pid, ppid, state, exit_code, handles; Mem mem; Cpu cpu; Win32 w32; char name[64]; double wake_ms; int lastc[MAX_SHIMS]; } Proc;
static Proc *g_procs[PROC_MAX]; static int g_nprocs; static int g_cur; static int g_next_pid = 4242;
static Proc *g_root; static int g_loaded;
#define g_cpu (g_root->cpu)
#define g_w32 (g_root->w32)
#define g_mem (g_root->mem)
static char g_inspect[16384];
extern Win32 *g_root_w32;

static Proc *proc_find(int pid) { for (int i = 0; i < PROC_MAX; i++) if (g_procs[i] && g_procs[i]->used && g_procs[i]->pid == pid) return g_procs[i]; return 0; }
static void proc_free_slot(Proc *p) { p->used = 0; if (p == g_root) return; }
static Proc *proc_alloc(void) {
  for (int i = 0; i < PROC_MAX; i++) if (g_procs[i] && !g_procs[i]->used) { Proc *p = g_procs[i]; ProcFs *fs = p->w32.fs; e_memset(&p->mem, 0, sizeof p->mem); p->w32.fs = fs; p->used = 1; return p; }
  for (int i = 0; i < PROC_MAX; i++) if (!g_procs[i]) { Proc *p = emu_alloc(sizeof(Proc)); if (!p) return 0; g_procs[i] = p; p->used = 1; if (i >= g_nprocs) g_nprocs = i + 1; return p; }
  /* table full: reclaim the oldest zombie nobody holds a handle to */
  for (int i = 0; i < PROC_MAX; i++) if (g_procs[i]->state == 2 && g_procs[i]->handles <= 0 && g_procs[i] != g_root) { Proc *p = g_procs[i]; ProcFs *fs = p->w32.fs; e_memset(&p->mem, 0, sizeof p->mem); p->w32.fs = fs; p->used = 1; return p; }
  return 0;
}
static void proc_exit(Proc *p, int code) {
  if (p->state == 2) return;
  p->state = 2; p->exit_code = code;
  p->w32.cpu->exit_code = code; w32_io_drop(&p->w32); w32_proc_teardown(&p->w32);   /* every thread dies with the process; thread objects become signalled */
  vfs_release_proc(p->w32.fs);
  if (p != g_root) mem_release(&p->mem);
}
int proc_state(int pid, int *exit_code) { Proc *p = proc_find(pid); if (!p) return 0; if (exit_code) *exit_code = p->exit_code; return p->state == 2 ? 2 : 1; }
void proc_handle_ref(int pid, int delta) { Proc *p = proc_find(pid); if (p) p->handles += delta; }
ProcFs *proc_fs(int pid) { Proc *p = proc_find(pid); return p ? p->w32.fs : 0; }
int proc_read_mem(int pid, u64 addr, u8 *buf, u64 n) { Proc *p = proc_find(pid); if (!p || p->state != 1) return 0; const u8 *s = mem_ptr(&p->mem, addr, n, 0); if (!s) return 0; e_memcpy(buf, s, (size_t)n); return 1; }
u64 proc_image_base(int pid, const char **name) { Proc *p = proc_find(pid); if (!p) return 0; if (name) *name = p->name; return p->w32.image_base; }
double proc_start_ms(int pid) { Proc *p = proc_find(pid); return p ? p->w32.start_ms : 0; }
void proc_kill(int pid, int code) { Proc *p = proc_find(pid); if (p) proc_exit(p, code); }
Win32 *proc_w32_at(int i) { if (i < 0 || i >= PROC_MAX) return 0; Proc *p = g_procs[i]; return p && p->used && p->state == 1 ? &p->w32 : (Win32 *)1; }
int proc_main_thread_kobj(int pid) { Proc *p = proc_find(pid); return p ? p->w32.th[0].kobj : -1; }
/* idx 0 is a synthetic parent for the root process ("explorer.exe", pid 1000): busybox treats getppid()==1 as "orphan, do not wait" */
#define HOST_PID 1000
int proc_enum(int idx, int *pid, int *ppid, const char **name) { if (idx == 0) { *pid = HOST_PID; *ppid = 4; *name = "explorer.exe"; return 1; } idx--; int k = 0; for (int i = 0; i < PROC_MAX; i++) { Proc *p = g_procs[i]; if (!p || !p->used || p->state == 2) continue; if (k++ == idx) { *pid = p->pid; *ppid = p->ppid; *name = p->name; return 1; } } return 0; }
static const char *basename_of(const char *p) { const char *b = p; for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1; return b; }
/* ---------- .NET: managed exes/dlls are started through the official runtime (the ".NET pack" at C:\Program Files\dotnet) ----------
   `app.exe a b`  →  "C:\Program Files\dotnet\dotnet.exe" exec [--runtimeconfig "C:\tmp\app.runtimeconfig.json"] "C:\dir\app.exe" a b
   A runtimeconfig is generated when the app has none next to it (classic .NET Framework exes): net10.0, roll forward to any
   installed major, invariant globalization, BinaryFormatter allowed — the compatibility mode in which net48 console apps run. */
#define DOTNET_EXE "C:\\Program Files\\dotnet\\dotnet.exe"
static const char *cmd_skip_first(const char *cl) {   /* command line without its first (possibly quoted) token */
  const char *p = cl; while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; } else while (*p && *p != ' ' && *p != '\t') p++;
  while (*p == ' ' || *p == '\t') p++; return p;
}
static char g_dotnet_cmd[4096], g_dotnet_cfg[2048];
/* returns the dotnet.exe VFS node (>= 0) and rewrites *cmdline, or -1 (not managed) / -2 (managed but no runtime pack) */
static int dotnet_redirect(Win32 *w, const char *app_path, const u8 *bytes, size_t len, const char **cmdline) {
  if (pe_managed_kind(bytes, len) != 1) return -1;
  int dn = vfs_find_node(DOTNET_EXE); VNodeInfo di;
  if (dn < 0 || !vfs_node_info(dn, &di) || di.is_dir) { w32_report(w, "%s is a .NET assembly: install the .NET runtime pack (Microsoft .NET Runtime 10, win-x64) into C:\\Program Files\\dotnet first", basename_of(app_path)); return -2; }
  /* <dir>\<base>.runtimeconfig.json next to the app? otherwise generate one in C:\tmp */
  char cfg[600]; e_snprintf(cfg, sizeof cfg, "%s", app_path); size_t l = e_strlen(cfg); if (l > 4 && (cfg[l - 4] == '.')) cfg[l - 4] = 0;
  size_t cl = e_strlen(cfg); if (cl + 20 < sizeof cfg) e_memcpy(cfg + cl, ".runtimeconfig.json", 20);
  const char *rc_opt = "";
  if (vfs_find_node(cfg) < 0) {
    const char *bn = basename_of(app_path); char base[260]; e_snprintf(base, sizeof base, "%s", bn); size_t bl = e_strlen(base); if (bl > 4 && base[bl - 4] == '.') base[bl - 4] = 0;
    static char gen[600]; e_snprintf(gen, sizeof gen, "C:\\tmp\\%s.runtimeconfig.json", base);
    static const char json[] = "{\"runtimeOptions\":{\"tfm\":\"net10.0\",\"rollForward\":\"LatestMajor\",\"framework\":{\"name\":\"Microsoft.NETCore.App\",\"version\":\"10.0.0\"},"
      "\"configProperties\":{\"System.Globalization.Invariant\":true,\"System.Globalization.PredefinedCulturesOnly\":false,\"System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization\":true,\"System.Reflection.Metadata.MetadataUpdater.IsSupported\":false}}}";
    vfs_mkdir("C:\\tmp"); vfs_add(gen, (const u8 *)json, sizeof json - 1);
    e_snprintf(g_dotnet_cfg, sizeof g_dotnet_cfg, " --runtimeconfig \"%s\"", gen); rc_opt = g_dotnet_cfg;
  }
  e_snprintf(g_dotnet_cmd, sizeof g_dotnet_cmd, "\"%s\" exec%s \"%s\" %s", DOTNET_EXE, rc_opt, app_path, *cmdline ? cmd_skip_first(*cmdline) : "");
  { char msg[700]; e_snprintf(msg, sizeof msg, "[x64] .NET assembly %s -> %s\n", basename_of(app_path), g_dotnet_cmd); host_write(3, msg, e_strlen(msg)); }
  *cmdline = g_dotnet_cmd; return dn;
}
/* CreateProcess: load an exe from the VFS into a fresh address space; std_fd[k]: -2 inherit, -1 console, >=3 parent's table entry */
const char *const *g_spawn_env; int g_spawn_nenv; const char *g_spawn_cwd;
int proc_spawn(Win32 *parent, const char *exe_path, const char *cmdline, const int std_fd[3]) {
  int node = vfs_find_node(exe_path); VNodeInfo in; if (node < 0 || !vfs_node_info(node, &in) || in.is_dir) return -2;
  { const u8 *img0 = vfs_node_data(node); if (img0) { int dn = dotnet_redirect(parent, in.path, img0, (size_t)in.size, &cmdline); if (dn == -2) return -2; if (dn >= 0) { node = dn; if (!vfs_node_info(node, &in)) return -2; } } }
  Proc *p = proc_alloc(); if (!p) return -8;
  p->pid = ++g_next_pid; p->ppid = parent->pid; p->state = 1; p->exit_code = 0; p->handles = 0; p->wake_ms = 0;
  e_snprintf(p->name, sizeof p->name, "%s", basename_of(in.path));
  ProcFs *save = vfs_P;
  cpu_reset(&p->cpu); w32_init(&p->w32, &p->cpu, &p->mem); p->w32.pid = p->pid; p->w32.ppid = p->ppid; p->w32.api_trace = parent->api_trace;   /* --apitrace follows child processes */
  vfs_init_proc(p->w32.fs, parent->fs);
  for (int k = 0; k < 3; k++) if (std_fd[k] != -2) { ProcFs *f = p->w32.fs; if (f->redir[k] != -1) { vfs_use(f); vfs_close(f->redir[k]); } f->redir[k] = -1; if (std_fd[k] >= 3 && f->fd[std_fd[k] - 3].kind) { vfs_use(f); f->redir[k] = vfs_dup(std_fd[k]); } }
  vfs_use(p->w32.fs);
  if (g_spawn_env) w32_env_replace(g_spawn_env, g_spawn_nenv);
  const u8 *img = vfs_node_data(node); if (!img) return -2;
  int rc = pe_load(&p->w32, img, (size_t)in.size, p->name); e_snprintf(p->w32.exe_path, sizeof p->w32.exe_path, "%s", in.path);
  if (rc) { vfs_use(save); proc_exit(p, 127); p->used = 0; return -2; }
  w32_start(&p->w32, cmdline);
  vfs_use(save); w32_set_current(parent);
  return p->pid;
}
/* does this process have a runnable thread? (switches the process to it) */
static int proc_runnable(Proc *p) {
  if (p->state != 1) return 0;
  return w32_thread_pick(&p->w32);
}
static void switch_to(Proc *p) { vfs_use(p->w32.fs); w32_set_current(&p->w32); }
static int g_want_stdin; static int g_trace;

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#else
#define EXPORT(name)
#endif

EXPORT(x64_alloc) void *x64_alloc(size_t n) { return emu_alloc(n); }
EXPORT(x64_reserve) int x64_reserve(size_t n) { return emu_reserve(n); }   /* grow the wasm arena once before a batch of x64_alloc/x64_vfs_add */
EXPORT(x64_inspect) const char *x64_inspect(const u8 *bytes, size_t len) { pe_inspect(bytes, len, g_inspect, sizeof g_inspect); return g_inspect; }
EXPORT(x64_load) int x64_load(const u8 *bytes, size_t len, const char *name, const char *cmdline) {
  /* tear down a previous session: every process dies, kernel objects are dropped, the VFS files stay */
  for (int i = 0; i < PROC_MAX; i++) if (g_procs[i] && g_procs[i]->used) { proc_exit(g_procs[i], 0); g_procs[i]->used = 0; }
  g_loaded = 0; g_want_stdin = 0; vfs_close_all(); vfs_mkdir("C:\\tmp"); vfs_mkdir("C:\\nebula");
  vfs_mkdir("C:\\Windows"); vfs_mkdir("C:\\Windows\\System32"); vfs_add("C:\\Windows\\System32\\cmd.exe", cmd_exe_bytes, sizeof cmd_exe_bytes);   /* the embedded shell (emu/cmd/cmd.c) */
  vfs_mkdir("C:\\Windows\\System32\\WindowsPowerShell"); vfs_mkdir("C:\\Windows\\System32\\WindowsPowerShell\\v1.0"); vfs_add("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", pwsh_exe_bytes, sizeof pwsh_exe_bytes);   /* the embedded PowerShell subset (emu/pwsh/pwsh.c) */
  Proc *p = proc_alloc(); if (!p) return 8; g_root = p; g_cur = 0; g_root_w32 = &p->w32;
  p->pid = 4242; p->ppid = HOST_PID; p->state = 1; p->exit_code = 0; p->handles = 0; g_next_pid = 4242;
  e_snprintf(p->name, sizeof p->name, "%s", name ? basename_of(name) : "app.exe");
  mem_release(&p->mem); cpu_reset(&p->cpu);
  w32_init(&p->w32, &p->cpu, &p->mem); p->w32.pid = p->pid; p->w32.ppid = HOST_PID;
  vfs_init_proc(p->w32.fs, 0); vfs_use(p->w32.fs);
  if (name && e_strchr(name, '\\')) e_snprintf(p->w32.exe_path, sizeof p->w32.exe_path, "%s", name); else e_snprintf(p->w32.exe_path, sizeof p->w32.exe_path, "C:\\nebula\\%s", p->name);
  if (vfs_find_node(p->w32.exe_path) < 0) vfs_add_ref(p->w32.exe_path, (u8 *)bytes, len);   /* on the virtual disk before loading: a managed exe is passed to dotnet.exe by path */
  const char *cl = cmdline ? cmdline : name ? name : "app.exe";
  { int dn = dotnet_redirect(&p->w32, p->w32.exe_path, bytes, len, &cl);
    if (dn == -2) return -4;
    if (dn >= 0) { VNodeInfo di; vfs_node_info(dn, &di); bytes = vfs_node_data(dn); len = (size_t)di.size; e_snprintf(p->w32.exe_path, sizeof p->w32.exe_path, "%s", DOTNET_EXE); e_snprintf(p->name, sizeof p->name, "dotnet.exe"); } }
  int rc = pe_load(&p->w32, bytes, len, p->name);
  if (rc) return rc;
  w32_start(&p->w32, cl);
  g_loaded = 1; return 0;
}
/* scheduler: run runnable processes round-robin; the host sees the root process' lifecycle */
static int run_one(Proc *p, u64 budget) {
  switch_to(p); Cpu *tc = p->w32.cpu; u64 ic0 = tc->icount; int r = cpu_run(tc, budget);
  if (g_trace) { char msg[256]; e_snprintf(msg, sizeof msg, "[sched] pid %d tid %d %.40s ran %llu insns -> r=%d block=%d rip=0x%llx\n", p->pid, p->w32.th[p->w32.cur_th].tid, p->w32.cmdline, (unsigned long long)(tc->icount - ic0), r, p->w32.block, (unsigned long long)tc->rip); host_write(3, msg, e_strlen(msg));
    if (g_trace > 1) { int *lastc = p->lastc; for (int i = 2; i < p->w32.nslots; i++) if (p->w32.slots[i].calls != lastc[i]) { e_snprintf(msg, sizeof msg, "   %s!%s +%d\n", p->w32.slots[i].dll, p->w32.slots[i].name, p->w32.slots[i].calls - lastc[i]); host_write(3, msg, e_strlen(msg)); lastc[i] = p->w32.slots[i].calls; } } }
  if (p->w32.dirty && p->w32.screen) { host_present(p->w32.screen, p->w32.screen_w, p->w32.screen_h); p->w32.dirty = 0; }
  /* a fault/exit in a worker thread is reported through the main thread's cpu (what the host inspects) */
  if ((r == RUN_FAULT || r == RUN_BREAK || r == RUN_EXIT) && tc != &p->cpu) { Cpu *m = &p->cpu; Cpu saved = *m; *m = *tc; m->mem = saved.mem; m->w = saved.w; p->w32.th[0].cpu = m; }
  return r;
}
EXPORT(x64_run) int x64_run(u64 budget) {
  if (!g_loaded) return RUN_FAULT;
  g_want_stdin = 0;
  for (int attempt = 0; attempt < PROC_MAX * 2; attempt++) {
    w32_io_poll(); w32_audio_pump();
    if (w32_audio_cb_pending()) for (int i = 0; i < g_nprocs; i++) { Proc *q = g_procs[i]; if (q && q->used && q->state == 1) { switch_to(q); w32_audio_deliver(&q->w32); } }
    /* pick the next runnable process after g_cur */
    Proc *p = 0; int start = g_cur;
    for (int k = 0; k < g_nprocs; k++) { int i = (start + 1 + k) % g_nprocs; Proc *q = g_procs[i]; if (q && q->used && proc_runnable(q)) { p = q; g_cur = i; break; } }
    if (!p) {
      /* nothing runnable: waiting on the host (stdin / time) or deadlocked */
      int stdin_wait = 0, sleeping = 0; double best = -1;
      for (int i = 0; i < g_nprocs; i++) { Proc *q = g_procs[i]; if (!q || !q->used || q->state != 1) continue; w32_threads_summary(&q->w32, &stdin_wait, &sleeping, &best); }
      if (g_root->state == 2) return RUN_EXIT;
      if (stdin_wait) { g_want_stdin = 1; return RUN_YIELD; }
      if (sleeping || w32_net_pending() || w32_audio_pending()) return RUN_YIELD;   /* network completions / audio periods arrive from the host clock: keep polling */
      cpu_fault(&g_root->cpu, "deadlock: every thread of every process is blocked (wait on an object, lock, pipe or process that nobody will signal)"); return RUN_FAULT;
    }
    int r = run_one(p, budget);
    if (r == RUN_EXIT) {
      proc_exit(p, p->cpu.exit_code);
      if (p == g_root) { for (int i = 0; i < g_nprocs; i++) if (g_procs[i] && g_procs[i]->used && g_procs[i] != g_root) proc_exit(g_procs[i], 0); return RUN_EXIT; }
      continue;
    }
    if (r == RUN_FAULT || r == RUN_BREAK) {
      if (p == g_root) return r;
      { char msg[512]; e_snprintf(msg, sizeof msg, "[x64] child %s (pid %d) %s at rip=0x%llx: %s\n", p->name, p->pid, r == RUN_FAULT ? "faulted" : "hit a breakpoint", (unsigned long long)p->cpu.err_rip, p->cpu.err); host_write(3, msg, e_strlen(msg)); }
      proc_exit(p, 0xC0000005); continue;
    }
    if (r == RUN_YIELD) { if (p->w32.block != BLOCK_NONE || !w32_thread_alive(&p->w32)) continue; return RUN_YIELD; }
    return RUN_OK;   /* budget used up; next call continues with the following process */
  }
  return RUN_YIELD;
}
/* ---- virtual file system / stdin (host side) ---- */
EXPORT(x64_vfs_add) int x64_vfs_add(const char *path, const u8 *data, size_t n) { return vfs_add_ref(path, (u8 *)data, n) >= 0; }   /* host-owned buffer, referenced not copied */
EXPORT(x64_vfs_mkdir) int x64_vfs_mkdir(const char *path) { return vfs_mkdir(path); }
EXPORT(x64_vfs_remove) int x64_vfs_remove(const char *path) { return vfs_remove(path); }
EXPORT(x64_vfs_size) s64 x64_vfs_size(const char *path) { u64 sz; int k = vfs_stat(path, &sz); return k == 1 ? (s64)sz : -1; }
EXPORT(x64_vfs_data) const u8 *x64_vfs_data(const char *path) { int i = vfs_find_node(path); VNodeInfo in; if (i < 0 || !vfs_node_info(i, &in) || in.is_dir) return 0; return vfs_node_data(i); }
static char g_vfs_json[65536];
EXPORT(x64_vfs_list) const char *x64_vfs_list(void) {
  size_t o = 0; g_vfs_json[o++] = '['; int first = 1; VNodeInfo in;
  for (int i = 0; i < vfs_node_count(); i++) {
    if (!vfs_node_info(i, &in)) continue;
    char esc[1100]; size_t e = 0; for (const char *p = in.path; *p && e + 3 < sizeof esc; p++) { if (*p == '\\' || *p == '"') esc[e++] = '\\'; esc[e++] = *p; } esc[e] = 0;
    int n = e_snprintf(g_vfs_json + o, sizeof g_vfs_json - o, "%s{\"path\":\"%s\",\"size\":%llu,\"dir\":%d,\"written\":%d}", first ? "" : ",", esc, (unsigned long long)in.size, in.is_dir, in.written);
    if (n < 0 || o + (size_t)n + 2 >= sizeof g_vfs_json) break; o += (size_t)n; first = 0;
  }
  g_vfs_json[o++] = ']'; g_vfs_json[o] = 0; return g_vfs_json;
}
EXPORT(x64_stdin_write) void x64_stdin_write(const u8 *data, size_t n) { vfs_stdin_push(data, n); }
EXPORT(x64_stdin_eof) void x64_stdin_eof(void) { vfs_stdin_eof(); }
EXPORT(x64_trace) void x64_trace(int on) { g_trace = on; }
EXPORT(x64_wants_stdin) int x64_wants_stdin(void) { return g_want_stdin; }
EXPORT(x64_key) void x64_key(int vk, int down) { w32_key(&g_w32, vk, down); }
EXPORT(x64_mouse) void x64_mouse(int x, int y, int ev) { w32_mouse(&g_w32, x, y, ev); }
EXPORT(x64_icount) u64 x64_icount(void) { u64 n = 0; for (int i = 0; i < g_nprocs; i++) if (g_procs[i]) { n += g_procs[i]->cpu.icount; for (int t = 1; t < THREAD_MAX; t++) n += g_procs[i]->w32.tcpu[t].icount; } return n; }
EXPORT(x64_exit_code) int x64_exit_code(void) { return g_cpu.exit_code; }
EXPORT(x64_error) const char *x64_error(void) { return g_cpu.err; }
EXPORT(x64_error_rip) u64 x64_error_rip(void) { return g_cpu.err_rip; }
static char g_regs[512];
static char g_hist[4096 * 12];
/* the last executed instruction addresses, oldest first, runs of sequential code collapsed to "start..end" */
EXPORT(x64_history) const char *x64_history(void) { int o = 0; u64 run0 = 0, last = 0; int n = 0;
  for (u32 i = 0; i < 4096; i++) { u64 a = g_cpu.hist[(g_cpu.hist_i + i) & 4095]; if (!a) continue;
    if (run0 && a > last && a - last < 16) { last = a; continue; }
    if (run0) { o += e_snprintf(g_hist + o, sizeof g_hist - o, run0 == last ? "%llx " : "%llx..%llx ", (unsigned long long)run0, (unsigned long long)last); n++; }
    run0 = last = a; if (o > (int)sizeof g_hist - 48) break; }
  if (run0) o += e_snprintf(g_hist + o, sizeof g_hist - o, run0 == last ? "%llx " : "%llx..%llx ", (unsigned long long)run0, (unsigned long long)last);
  (void)n; return g_hist; }
EXPORT(x64_mem_copy) int x64_mem_copy(u64 addr, u8 *dst, u32 n) { const u8 *p = mem_ptr(&g_mem, addr, n, 0); if (!p) return 0; e_memcpy(dst, p, n); return 1; }
EXPORT(x64_regs) const char *x64_regs(void) { static const char *nm[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"}; int o = 0; for (int i = 0; i < 16; i++) o += e_snprintf(g_regs + o, sizeof g_regs - o, "%s=%llx ", nm[i], (unsigned long long)g_cpu.r[i]); u64 sp = g_cpu.r[4]; const u8 *st = mem_ptr(&g_mem, sp, 32, 0); if (st) for (int k = 0; k < 4; k++) { u64 v = 0; for (int b = 7; b >= 0; b--) v = (v << 8) | st[k * 8 + b]; o += e_snprintf(g_regs + o, sizeof g_regs - o, "[rsp+%d]=%llx ", k * 8, (unsigned long long)v); } return g_regs; }
EXPORT(x64_rip) u64 x64_rip(void) { return g_cpu.rip; }
EXPORT(x64_slot_name) const char *x64_slot_name(u64 addr) { static char nm[160]; u64 off = addr - G_SHIM_BASE; if (off >= G_SHIM_SIZE) return ""; int i = (int)(off / SHIM_SLOT_SIZE); if (i < 0 || i >= g_w32.nslots) return "?"; e_snprintf(nm, sizeof nm, "%s!%s", g_w32.slots[i].dll, g_w32.slots[i].name); return nm; }
EXPORT(x64_report) const char *x64_report(void) { return g_w32.report; }
EXPORT(x64_console) const char *x64_console(void) { return g_w32.con; }
EXPORT(x64_present_all) void x64_present_all(void) { for (int i = 0; i < g_nprocs; i++) { Proc *p = g_procs[i]; if (p && p->used && p->w32.screen) { e_log("[screen] pid %d %dx%d\n", p->pid, p->w32.screen_w, p->w32.screen_h); host_present(p->w32.screen, p->w32.screen_w, p->w32.screen_h); } } }   /* debug: dump every process' framebuffer */
EXPORT(x64_screen_w) int x64_screen_w(void) { return g_w32.screen ? g_w32.screen_w : 0; }
EXPORT(x64_screen_h) int x64_screen_h(void) { return g_w32.screen ? g_w32.screen_h : 0; }
EXPORT(x64_screen) const u32 *x64_screen(void) { return g_w32.screen; }
EXPORT(x64_title) const char *x64_title(void) { return g_w32.win.title; }
EXPORT(x64_subsystem) int x64_subsystem(void) { return g_w32.subsystem; }
EXPORT(x64_sleep_ms) double x64_sleep_ms(void) { double best = -1; int si = 0, sl = 0; for (int i = 0; i < g_nprocs; i++) { Proc *q = g_procs[i]; if (!q || !q->used || q->state != 1) continue; w32_threads_summary(&q->w32, &si, &sl, &best); } if (w32_net_pending() && (best < 0 || best > 2)) best = 2; if (w32_audio_pending() && (best < 0 || best > 5)) best = 5; return best < 0 ? 0 : best; }
EXPORT(x64_audio_frames) u64 x64_audio_frames(void) { return w32_audio_frames(); }
EXPORT(x64_thread_count) int x64_thread_count(void) { int n = 0; for (int i = 0; i < g_nprocs; i++) if (g_procs[i] && g_procs[i]->used && g_procs[i]->state == 1) n += w32_thread_count(&g_procs[i]->w32); return n; }
EXPORT(x64_proc_count) int x64_proc_count(void) { int n = 0; for (int i = 0; i < g_nprocs; i++) if (g_procs[i] && g_procs[i]->used && g_procs[i]->state == 1) n++; return n; }
EXPORT(x64_mem_used) u64 x64_mem_used(void) { return emu_alloc_used(); }
EXPORT(x64_has_window) int x64_has_window(void) { return g_w32.win.created; }
EXPORT(x64_api_stats) const char *x64_api_stats(void) {
  static char buf[8192]; int p = 0; buf[0] = 0;
  for (int i = 2; i < g_w32.nslots && p < (int)sizeof buf - 128; i++) if (g_w32.slots[i].calls) p += e_snprintf(buf + p, sizeof buf - p, "%s%s!%s:%d", p ? "," : "", g_w32.slots[i].dll, g_w32.slots[i].name, g_w32.slots[i].calls);
  return buf;
}
/* bytes of the error instruction for the report */
EXPORT(x64_error_bytes) const char *x64_error_bytes(void) {
  static char buf[64]; int p = 0; for (int i = 0; i < 12; i++) { u8 *b = mem_ptr(&g_mem, g_cpu.err_rip + i, 1, 0); if (!b) break; p += e_snprintf(buf + p, sizeof buf - p, "%02X ", *b); } return buf;
}
