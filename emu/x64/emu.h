/*
 * Nebula x64 — a user-mode x86-64 emulator with a Win32 personality, written to be compiled
 * to wasm64 (and natively for tests).
 *
 *   emu.h     shared types: guest memory regions, CPU state, Win32 shim table, host interface
 *   mem.c     guest address space (flat 64-bit, region table, bump allocator)
 *   cpu.c     instruction decoder + interpreter (integer ISA, SSE/SSE2 scalar+packed subset, x87 control NOPs)
 *   pe.c      PE32+ loader (sections, base relocations, imports → shim slots, TLS callbacks)
 *   win32.c   kernel32 / msvcrt / user32 / gdi32 shims (enough for mingw-w64 CRT start-up, console and GDI apps)
 *   api.c     exported entry points used by the JS host (wasm64) and by the native test runner
 */
#ifndef NEBULA_EMU_H
#define NEBULA_EMU_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   s8;  typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;

/* ---------- host interface (implemented in host_native.c or imported from JS in wasm) ---------- */
void   host_write(int fd, const char *p, size_t n);        /* console output (1 stdout, 2 stderr, 3 emulator log) */
double host_now_ms(void); double host_wall_ms(void);   /* wall clock, ms since the Unix epoch (UTC) */                                  /* monotonic milliseconds */
void   host_present(const u32 *bgra, int w, int h);        /* framebuffer of the GUI window changed */
int    host_key(int vk);                                   /* 1 if virtual key currently pressed */
void   host_exit(int code);                                /* guest called ExitProcess */
void   host_mem_discard(void *p, size_t n);
/* write-back mounts (native runner --mount-rw): the VFS asks the host to persist / reload / mirror directory operations.
   op 1 WRITE(a, data, size) → 1 if the file now lives on the host (the VFS may drop its copy);  2 LOAD(a) → data/size (mapped, read-only-ish);
   3 MKDIR(a);  4 REMOVE(a, *size = is_dir);  5 RENAME(a, b);  6 UNLOAD(*data, *size) release a LOAD/mount mapping.  Returns 0 when a path is not under a write-back mount. */
int    host_fs_op(int op, const char *a, const char *b, u8 **data, u64 *size);
void   vfs_sync_all(void);                /* MEM_DECOMMIT / MEM_RESET: give the host pages back (contents become zero) */
double host_math(int op, double a, double b);              /* 0 sin 1 cos 2 tan 3 exp 4 log 5 pow 6 atan2 7 asin 8 acos 9 atan 10 sinh 11 cosh 12 tanh 13 log10 14 log2 15 cbrt */
/* TCP networking (IPv4). Native host: Linux sockets; wasm host: stubs that fail (no raw TCP in a browser) until a relay exists. */
int    host_net_resolve(const char *name, u8 *ip4s, int max);          /* A records; returns count, 0 = not found, -1 = no network */
int    host_net_connect(const u8 *ip4, int port, int *wsa_err);        /* starts a non-blocking connect; host socket id >= 0 or -1 (+ winsock error) */
int    host_net_listen(const u8 *ip4, int port, int backlog, int *wsa_err);   /* bound+listening host socket, or -1 */
int    host_net_accept(int s, u8 *peer_ip4, int *peer_port);           /* new host socket, -2 would block, -3 error */
int    host_net_status(int s);                                         /* connect progress: 0 in progress, 1 connected, <0 = -winsock error */
s64    host_net_read(int s, u8 *buf, u64 n);                           /* >0 bytes, 0 EOF, -2 would block, -3 error (reset) */
s64    host_net_write(int s, const u8 *buf, u64 n);                    /* bytes written now (0 = no room, never blocks), -3 error */
int    host_net_readable(int s);                                       /* 1 when a read would not block (data, EOF or error) */
int    host_net_avail(int s);                                          /* bytes available without blocking */
void   host_net_shutdown(int s, int how);
void   host_net_close(int s);
int    host_net_name(int s, int peer, u8 *ip4, int *port);             /* local (0) / peer (1) address; 1 ok */

/* host audio output (browser: Web Audio → the device's audio stack; native runner: silent clock + optional WAV dump) */
int    host_audio_open(int rate);                                      /* returns the actual output rate (stereo float32 interleaved), 0 = no output */
void   host_audio_write(const float *lr, int frames);
int    host_audio_queued(void);                                        /* frames written but not yet played */
void   w32_audio_pump(void);                                           /* win32_audio.inc: mix + feed the host (scheduler, before every pick) */
int    w32_audio_pending(void);                                        /* a stream is playing: keep polling (timers/events come from the mixer) */
int    w32_audio_cb_pending(void);                                     /* waveOut callbacks waiting for their owner process */
u64    w32_audio_frames(void);
/* GPU back end for the D3D11 layer (browser: WebGL2 in src/lib/x64/gfx.ts; native runner: none — returns 0 from host_gfx_init). */
int    host_gfx_init(void);                                            /* 1 = draws are possible */
int    host_gfx_shader(int kind, const char *glsl);                    /* kind 0 fragment / 1 vertex; host shader id >= 0 or -1 (error logged on fd 3) */
void   host_gfx_buffer(int id, const u8 *data, u32 len, u32 bind);     /* (re)upload a buffer object; bind = D3D11_BIND_* flags (index buffers need their own GL target) */
void   host_gfx_texture(int id, int w, int h, int fmt, const u8 *px, u32 pitch);   /* (re)upload a texture; px NULL → allocate (render target / depth) */
void   host_gfx_clear(int tex_id, const float *rgba);                  /* clear a colour target */
void   host_gfx_clear_depth(int tex_id, float depth);                  /* clear the depth buffer of a target */
void   host_gfx_draw(const u32 *cmd);                                  /* one draw: GFX_CMD_WORDS words, layout in gfx_cmd.h */
void   host_gfx_readback(int tex_id, u32 *bgra);                       /* GPU target → BGRA rows, top row first */
void   host_gfx_free(int id);

/* ---------- guest memory ---------- */
#define MAX_REGIONS 4096   /* V8 reserves hundreds of 1 MB chunks; each large reservation is its own region (lazy host pages) */
typedef struct { u64 base, size; u8 *host; const char *name; u32 prot; } Region;
int emu_free(void *p);                 /* 1 = memory returned to the host (native), 0 = not freeable (wasm arena) */

#define TLB_BITS 12
#define TLB_N (1 << TLB_BITS)
typedef struct { u64 tag; u8 *host; } Tlb;   /* tag = page number + 1 (0 = empty); host = host address of that 4 KB page */
typedef struct {
  Region  r[MAX_REGIONS];
  int     n;
  int     last;                       /* last-hit cache */
  u64     fault_addr;                 /* set on failed translation */
  int     fault_write;
  Tlb     tlb[TLB_N];                 /* direct-mapped page translation cache (code / stack / heap no longer thrash `last`) */
  u32     gen;                        /* bumped on every TLB flush (unmap/release): host pointers cached elsewhere (decode cache) expire */
} Mem;
int mem_unmap(Mem *m, u64 base);       /* drop a mem_map region (large VirtualAlloc reservation released) */

u8  *mem_map(Mem *m, u64 base, u64 size, const char *name);   /* add region (host memory allocated) */
u8  *mem_ptr(Mem *m, u64 addr, u64 size, int write);          /* NULL on fault */
/* inline fast path: hit in the last-used region (the common case inside a loop), otherwise the full lookup */
static inline u8 *mem_ptr_fast(Mem *m, u64 addr, u64 size, int write) {
  u64 pg = addr >> 12; Tlb *t = &m->tlb[pg & (TLB_N - 1)];
  if (t->tag == pg + 1) { u64 o = addr & 4095; if (o + size <= 4096) return t->host + o; }
  return mem_ptr(m, addr, size, write);
}
extern u32 g_mem_gen;   /* global generation: a Mem gets a fresh, never-reused value on every flush and on first use */
static inline u32 mem_gen(Mem *m) { if (!m->gen) m->gen = ++g_mem_gen; return m->gen; }
static inline void mem_tlb_flush(Mem *m) { for (int i = 0; i < TLB_N; i++) m->tlb[i].tag = 0; m->gen = ++g_mem_gen; }
Region *mem_region(Mem *m, u64 addr);
void mem_release(Mem *m);                                     /* give all regions back to the pool */
void *emu_alloc(size_t n); int emu_reserve(size_t n);                                    /* zeroed host memory, never freed (bump) */
size_t emu_alloc_used(void);

/* ---------- CPU ---------- */
typedef union { u64 q[2]; u32 d[4]; u16 w[8]; u8 b[16]; double f64[2]; float f32[4]; } Xmm;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7, F_DF = 1u << 10, F_OF = 1u << 11 };

/* why cpu_run returned */
enum {
  RUN_OK = 0,            /* budget exhausted, call again */
  RUN_EXIT = 1,          /* process exited (cpu->exit_code) */
  RUN_FAULT = 2,         /* memory fault / unsupported instruction / unknown import — see cpu->err */
  RUN_YIELD = 3,         /* guest blocked in a wait (Sleep / PeekMessage with no work) — host may present a frame */
  RUN_BREAK = 4,         /* int3 / debugger break */
};

typedef struct Cpu Cpu;
typedef struct Win32 Win32;
void   w32_audio_deliver(Win32 *w);                                    /* win32_audio.inc: run in the owner's context — helper threads / messages for completed waveOut buffers */

struct Cpu {
  u64  r[16];
  u64  rip;
  u64  rflags;
  Xmm  x[16];
  u32  mxcsr;
  u64  fs_base, gs_base;
  u8   mode32;                        /* 1 = IA-32 compatibility mode (PE32 process): 32-bit operand/address/stack size, no REX, fs: = TEB32 */
  u16  fpu_cw;                        /* x87 control word */
  u16  fsw; u8 ftop, ftag;            /* x87 status word (C0-C3), stack top, tag bits (1 = valid) */
  double fp[8];                       /* x87 stack as doubles (53-bit precision, see x87.inc) */
  /* run control */
  u64  icount;                        /* instructions executed */
  int  status;                        /* RUN_* */
  int  exit_code;
  char err[256];
  u64  err_rip;
  u64  hist[4096]; u32 hist_i;             /* ring of the last executed instruction addresses (fault diagnostics) */
  u32  exc_code; u64 exc_addr; u64 exc_info;   /* hardware exception behind the last fault (0 = not dispatchable) */
  Mem *mem;
  Win32 *w;
  /* tracing / stats */
  int  trace;
  u32  unknown_op[4];                 /* first unsupported opcode bytes */
};

void cpu_reset(Cpu *c);
int  cpu_run(Cpu *c, u64 max_insns);  /* returns RUN_* */
void cpu_fault(Cpu *c, const char *fmt, ...);
int w32_hw_exception(Cpu *c);   /* dispatch c->exc_code as an SEH exception at c->rip; 1 = dispatched (keep running) */
/* stack helpers (used by the loader and shims) */
void cpu_push(Cpu *c, u64 v);
u64  cpu_pop(Cpu *c);

/* memory accessors with fault handling (return 0 / set cpu->status on fault) */
u8  rd8 (Cpu *c, u64 a); u16 rd16(Cpu *c, u64 a); u32 rd32(Cpu *c, u64 a); u64 rd64(Cpu *c, u64 a);
/* pointer-sized guest accesses (8 bytes, 4 in a 32-bit process) for pointer arrays and pointer fields of Win32 structures */
static inline u64 rdp(Cpu *c, u64 a) { return c->mode32 ? rd32(c, a) : rd64(c, a); }
#define PSZ (c->mode32 ? 4 : 8)
void wr8(Cpu *c, u64 a, u8 v); void wr16(Cpu *c, u64 a, u16 v); void wr32(Cpu *c, u64 a, u32 v); void wr64(Cpu *c, u64 a, u64 v);
static inline void wrp(Cpu *c, u64 a, u64 v) { if (c->mode32) wr32(c, a, (u32)v); else wr64(c, a, v); }
const char *rd_str(Cpu *c, u64 a, size_t max);     /* NUL-terminated guest string → host pointer (NULL if unmapped) */

/* ---------- Win32 shims ---------- */
typedef void (*ShimFn)(Cpu *c);
void call_shim(Cpu *c, ShimFn fn, u64 a0, u64 a1, u64 a2, u64 a3);   /* call a shim from another shim with explicit arguments (register ABI / 32-bit override) */
typedef struct { const char *dll; const char *name; ShimFn fn; } ShimDef;

#define SHIM_SLOT_SIZE 16
#define MAX_SHIMS 2048
typedef struct { const char *dll; const char *name; ShimFn fn; int calls; u16 clean32; const char *sig32; } ShimSlot;   /* clean32: stack bytes removed by the callee in 32-bit mode (stdcall); sig32: "ret:args" for shims needing argument marshaling (win32_x86.inc) */

/* guest layout */
#define G_SHIM_BASE   0x0000000000200000ull    /* import trampolines, TEB/PEB, strings  (2 MB) */
#define G_SHIM_SIZE   0x0000000000200000ull
#define G_STACK_BASE  0x0000000010000000ull    /* 8 MB stack */
#define G_STACK_SIZE  0x0000000000800000ull
#define G_HEAP_BASE   0x0000000020000000ull    /* guest heap; VirtualAlloc/HeapAlloc carve from here */
#define G_FB_BASE     0x0000000070000000ull    /* DIB sections (framebuffers) */


typedef struct {
  u64 hwnd; int w, h; char title[128];
  u64 wndproc; u32 style; int shown; int created; u64 brush;
} GuestWindow;

typedef struct {
  u64 handle; int kind;               /* 1 DC, 2 BITMAP, 3 BRUSH, 4 PEN, 5 FONT, 6 RGN */
  /* DC */ u64 bitmap; u32 text_color, bk_color; int bk_mode; u64 font, brush, pen;
  int wnd, ox, oy, vw, vh;            /* window DC: window index+1 (-1 = whole screen), framebuffer origin and visible size */
  int lox, loy;                       /* DC: logical → device shift (viewport org − window org) */
  int clip_on, cl, ct, cr, cb;        /* DC: clip rectangle in device coordinates (IntersectClipRect / BeginPaint) */
  int save_depth;                     /* DC: SaveDC nesting */
  /* BITMAP */ int bw, bh; u64 bits; /* guest address of BGRA pixels */
  int dib_bpp, dib_stride, dib_section; u64 dib_native; u32 *dib_pal; int dib_ncol; u8 *dib_native_prev; u32 *dib_shadow_prev; /* non-32-bpp DIB section: guest-visible native bits + 32-bpp shadow at `bits`, synced in dib_sync() */
  /* BRUSH/PEN */ u32 color; int pen_width;
  /* FONT */ int font_h; int font_bold; int font_em;   /* font_h = cell (line) height; font_em = em height the character width is derived from (0 = same as font_h) */
  /* RGN */ int rx, ry, rw, rh; int npts; s32 pts[64];
} GdiObj;

#define MAX_GDI 2048
#define MAX_MSGS 256
typedef struct { u64 hwnd; u32 msg; u64 wparam, lparam; u32 time; } GuestMsg;

/* ---- per-process file-descriptor / environment state (vfs.c) ---- */
#define FD_MAX 512
enum { FD_FILE = 1, FD_CONSOLE, FD_PIPE, FD_MAP, FD_PROC, FD_NULL, FD_KOBJ, FD_SOCK };   /* FD_SOCK: winsock socket (win32_net.inc table) */   /* FD_KOBJ: event / mutex / semaphore / thread (kobj table) */
typedef struct { int kind; int node; u64 pos; int append, eof, err, unget, alias, obj, wend; int noinherit; } FdEnt;   /* noinherit: not copied into children */
typedef struct { int used; char dir[512]; char pat[128]; int cursor; int dots; } VFind;
typedef struct { u64 addr, size; int fd; } MapView;
typedef struct {
  FdEnt fd[FD_MAX]; int redir[3]; char cwd[512]; VFind find[32];
  char *env[512]; int nenv, env_dirty; u64 strtok_next;
  MapView views[32]; int nviews;
} ProcFs;
extern ProcFs *vfs_P;
void vfs_use(ProcFs *p); ProcFs *vfs_root_fs(void); void vfs_init_proc(ProcFs *p, const ProcFs *parent); void vfs_release_proc(ProcFs *p);
int  vfs_kind(int fd); int vfs_fd_obj(int fd);
int  vfs_pipe(int *rfd, int *wfd); int vfs_pipe_avail(int fd); int vfs_pipe_id(int fd); int vfs_pipe_would_block(int pi, int for_write);
int  vfs_map_create(int file_fd, u64 size); u8 *vfs_map_data(int fd, u64 *size); int vfs_map_write_back(int fd, const u8 *src, u64 n);
int  vfs_proc_handle(int pid); int vfs_fd_pid(int fd);
/* ---- kernel objects (vfs.c): global, refcounted, referenced from fd tables as FD_KOBJ so inheritance/dup/close are uniform ---- */
enum { KO_EVENT = 1, KO_MUTEX, KO_SEM, KO_THREAD, KO_TIMER, KO_IOCP };
#define KOBJ_MAX 512
typedef struct {
  int used, kind, refs; char name[64];
  int manual, signaled;                 /* event */
  int count, max;                       /* semaphore */
  int owner_pid, owner_tid, recursion, abandoned;   /* mutex */
  int pid, tid, state; u32 exit_code;   /* thread: state 1 running, 2 exited */
  double due_ms; int period_ms;         /* waitable timer: due_ms < 0 = not set */
} KObj;
int   kobj_new(int kind, const char *name, int *existed);   /* returns index or -1; named objects are shared */
KObj *kobj_get(int idx);
void  kobj_ref(int idx, int delta);
int   vfs_kobj_handle(int idx);        /* new fd referencing the object (+1 ref) */
int   vfs_sock_handle(int idx);        /* new fd of kind FD_SOCK referencing socket table entry idx */
int   w32_net_pending(void);           /* win32_net.inc: parked socket operations (scheduler must keep polling) */
void  w32_sock_ref(int idx, int delta); /* win32_net.inc: refcount of a socket table entry (closed at 0) */
int   vfs_fd_kobj(int fd);             /* idx or -1 */
/* ---- process table (api.c) ---- */
enum { BLOCK_NONE = 0, BLOCK_STDIN, BLOCK_PROC, BLOCK_PIPE_R, BLOCK_PIPE_W, BLOCK_SLEEP, BLOCK_OBJ, BLOCK_CS, BLOCK_SRW, BLOCK_CV, BLOCK_ADDR, BLOCK_SELECT };
int  proc_main_thread_kobj(int pid);
Win32 *proc_w32_at(int i);                          /* iterate live processes (NULL when i is past the table) */
int  proc_spawn(Win32 *parent, const char *exe_path, const char *cmdline, const int std_fd[3]);   /* returns pid or -errno */
extern const char *const *g_spawn_env; extern int g_spawn_nenv; extern const char *g_spawn_cwd;   /* CreateProcess lpCurrentDirectory (0 = inherit) */   /* CreateProcess lpEnvironment: when set, the child gets exactly these "NAME=value" strings instead of inheriting */
void w32_env_replace(const char *const *kv, int n);
int  proc_state(int pid, int *exit_code);           /* 0 unknown, 1 running, 2 exited */
void proc_handle_ref(int pid, int delta);
ProcFs *proc_fs(int pid);
int  proc_read_mem(int pid, u64 addr, u8 *buf, u64 n);           /* ReadProcessMemory */
u64  proc_image_base(int pid, const char **name);
double proc_start_ms(int pid);
void proc_kill(int pid, int code);
int  proc_enum(int idx, int *pid, int *ppid, const char **name);   /* 1 if idx valid */

/* ---- per-thread Win32 state: lives in Win32 as the *current* thread's copy and is swapped on thread switch ---- */
#define THREAD_MAX 64
#define THREAD_CTX_FIELDS \
  u64 last_error; \
  struct { int kind; u64 ret_addr, rsp, data[16]; int idx; u32 clean32; u32 seq; u8 chain; } cont[64]; int ncont; u32 cont_seq;   /* seq: guest-entry generation when pushed; chain: result goes to the continuation below (pushed by the same shim step) */   /* continuation stack for guest callbacks (clean32: 32-bit stdcall bytes popped when the shim finally returns) */ \
  int sleeping; double sleep_until_ms; \
  int block; int block_pids[8]; int nblock_pids; int block_pipe; \
  u64 block_addr, block_val; int block_flag;                                       /* CS / SRW / CV / WaitOnAddress waits */ \
  int wobj_kind[64], wobj_idx[64]; int nwobj, wait_all, wait_granted;              /* BLOCK_OBJ: 1 = process pid, 2 = kobj index; wait_granted: index+1 handed over by SetEvent */ \
  int wait_active; u64 wait_rip; double wait_deadline; int wake_snap;              /* timed wait in progress (deadline < 0: infinite) */ \
  int spawn_wait_pid, spawn_wait_mode;                                             /* _spawn*(P_WAIT/P_OVERLAY) in progress */ \
  int pw_active, pw_fd; u64 pw_rip, pw_total, pw_done;                             /* partially completed pipe write, resumed on shim retry */ \
  struct { u64 fn, a0, a1, a2; } apc[8]; int napc;                                  /* user APCs queued by ReadFileEx/WriteFileEx/QueueUserAPC, delivered by alertable SleepEx */
typedef struct { THREAD_CTX_FIELDS } ThreadCtx;
#define MAX_TLS_IMAGES 8
typedef struct {
  int used, state, tid, suspend, kobj;   /* state 1 running, 2 exited */
  Cpu *cpu; u64 teb, stack, stack_size, tls_blk[MAX_TLS_IMAGES], tls_arr, start, param; u32 exit_code;
  ThreadCtx ctx;                         /* saved while not current */
} Thread;

#define MAX_MODULES 256
typedef struct { char name[64]; char dir[300]; u64 base, size, entry; u32 exp_rva, exp_size, pdata_rva, pdata_size; int attached; } GuestModule;   /* dir: where it was found (LoadLibrary searches the directories of loaded modules, like LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) */   /* a DLL loaded from the virtual disk */
struct Win32 {
  Cpu *cpu;                              /* the CURRENT thread's cpu */
  Mem *mem;
  ShimSlot slots[MAX_SHIMS]; int nslots;
  u64 image_base; u64 image_size; u64 entry; int subsystem;   /* 2 GUI, 3 console */
  char dll_dirs[8][260]; int ndll_dirs;   /* AddDllDirectory / SetDllDirectory: extra DLL search directories (delvewheel, Qt, game launchers) */
  char exe_name[260]; char exe_path[600]; char cmdline[1024];   /* exe_path: full guest path of the image (GetModuleFileName(NULL), PEB ImagePathName) */
  u64 teb, peb, cmdline_a, cmdline_w, env_w, env_a;           /* teb: main thread's TEB */
  int mode32; u64 dll_next32;                                 /* PE32 process: every thread's Cpu runs in 32-bit mode, DLLs are mapped below 2 GB */
  union { ThreadCtx tctx; struct { THREAD_CTX_FIELDS }; };    /* current thread's state (see Thread.ctx) */
  Thread th[THREAD_MAX]; int nth, cur_th; Cpu tcpu[THREAD_MAX]; int next_tid;
  struct { u64 tmpl, tmpl_size, blk_size; } tls[MAX_TLS_IMAGES]; int ntls;   /* static TLS templates (exe + DLLs), index = slot in the TEB+0x58 array */
  GuestModule mods[MAX_MODULES]; int nmods; u64 dll_next; u32 exe_exp_rva, exe_exp_size;   /* export directory of the main image (Bun exports napi_* for .node addons) */
  u32 exe_pdata_rva, exe_pdata_size;                          /* .pdata of the exe (RUNTIME_FUNCTION table) for SEH / C++ unwinding */     /* private DLLs (LoadLibrary / static imports) */
  int wake_gen;                                               /* bumped by WakeByAddress / InitOnceComplete */
  /* heap */
#define HEAP_BINS 128
  u64 heap_cur, heap_end; u64 bins[HEAP_BINS];              /* free-list heads per size class (guest addresses) */
  u64 fb_cur;                           /* bump pointer for DIB sections */
  u32 tls_slots[64];
  u64 tls_callbacks[16]; int n_tls_cb;
  int yield_frames; int dirty;
  /* window + GDI */
  GuestWindow win; GdiObj gdi[MAX_GDI]; int ngdi; u64 next_handle;
  GuestMsg msgs[MAX_MSGS]; int msg_head, msg_tail;
  u32 *screen; int screen_w, screen_h;  /* host-side composed window contents (BGRA) */
  int frames_presented; int blits_since_yield;
  int key_state[256]; int key_prev[256];
  double start_ms; u64 fake_time_ms;
  int unknown_import_calls;
  char report[4096]; int report_len;     /* capability report text */
  /* console */
  char con[65536]; int con_len;
  u64 at_exit[64]; int n_at_exit;
  int exited;
  u64 loc_c, loc_wc, sys_nerr, sys_errlist;
  int api_trace;
  u64 veh[16]; int nveh; u64 uef; char hw_msg[160]; u32 hw_code; u64 cxx32_obj, cxx32_ti;   /* x86: the exception being handled (for `throw;`) */   /* vectored exception handlers, top-level filter, text of the last hardware fault */
  int wndclass_registered; u64 wndclass_proc; char wndclass_name[64];
  u64 ubuf;                              /* small scratch buffer in guest memory */
  u64 rt_scratch;                        /* win32_rt.inc: zeroed block for the real msvcp140's locale-name / processing-throw pointers */
  int quit_posted; int shortcuts; double msgwait_dl; u64 msgwait_rip;   /* MsgWaitForMultipleObjects timeout across retries */   /* .lnk files written through IShellLink (reported after install) */
  /* x87 / misc */
  u32 rand_state;
  /* process */
  ProcFs *fs; int pid, ppid; u64 str_cur;
};

void   w32_init(Win32 *w, Cpu *c, Mem *m);
void   w32_set_current(Win32 *w);
u64    w32_shim_slot(Win32 *w, const char *dll, const char *name);   /* returns guest address of the import trampoline */
int    w32_dispatch(Cpu *c, u64 addr);                              /* called by cpu when RIP enters the shim region */
u64    w32_heap_alloc(Win32 *w, u64 size, int zero);
int    w32_heap_free(Win32 *w, u64 addr);
int    gdi_wic_cocreate(Cpu *c, u32 clsid_d1, u64 riid, u64 ppv);   /* win32_wic.inc: IWICImagingFactory; returns 1 when handled (RET set) */
u64    w32_heap_size(Win32 *w, u64 addr);
u64    w32_guest_strdup(Win32 *w, const char *s);
void   w32_report(Win32 *w, const char *fmt, ...);
int    w32_wait_poll(Cpu *c, u32 n, u64 arr, int all, int *which);   /* 1 = one of n handles (array arr) is signaled (consumed), 0 = none, -1 = bad handle */
void   w32_io_poll(void);                                          /* finish parked overlapped pipe I/O (IOCP); scheduler calls it before every pick */
void   w32_io_drop(Win32 *w);                                      /* process exit: forget its IOCP associations and parked operations */
void   w32_tick(Win32 *w);                                         /* called by host once per frame: input + timers */
void   w32_key(Win32 *w, int vk, int down);
void   w32_mouse(Win32 *w, int x, int y, int event);                 /* event: 0 move, 1 ldown, 2 lup, 3 rdown, 4 rup */
void   w32_wheel(Win32 *w, int delta);
u64    w32_cur_slot(void);
void   call_shim_tail(Cpu *c, ShimFn fn, u64 a0, u64 a1, u64 a2, u64 a3);   /* like call_shim but leaves registers as the callee set them (callee may start a guest call) */
void   gdi_set_msgbox_auto(int on);
u32    put_w8(Cpu *c, u64 dst, const char *s, u32 cap);        /* UTF-8 → UTF-16 guest string (cap in u16 units, 0 = unlimited); returns chars written */
u32    utf8_u16len(const char *s);
int    vfs_glob_match(const char *pat, const char *s);   /* case-insensitive * ? wildcard match */
u64    rsrc_lookup(Cpu *c, u64 hmod, u64 name_a, u64 type_a, int wide, u16 lang);   /* IMAGE_RESOURCE_DATA_ENTRY guest address or 0 */
void   shell_mkdirs(const char *path);
u64    pidl_make(Win32 *w, const char *path);
const char *pidl_path(Cpu *c, u64 pidl);
void   w32_start(Win32 *w, const char *cmdline);
void   w32_cont_resume(Cpu *c);
u64    w32_arg(Cpu *c, int n);
double w32_sleep_remaining_ms(Win32 *w);
/* threads (win32.c): the scheduler in api.c picks (process, thread) pairs */
void   w32_thread_sync(Win32 *w);                 /* save the live ctx into th[cur_th] */
int    w32_thread_pick(Win32 *w);                 /* switch to the next runnable thread after cur_th; 1 if one was found */
void   w32_threads_summary(Win32 *w, int *stdin_wait, int *sleeping, double *best_ms);   /* why nothing runs */
void   w32_proc_teardown(Win32 *w);
void   user_proc_exit(Win32 *w);
void   user_dump_windows(void);                  /* debug: list all windows on fd 3 */                 /* USER: destroy the windows and classes of an exited process */               /* process exit: release thread objects */
int    w32_thread_count(Win32 *w);
int    w32_thread_alive(Win32 *w);               /* current thread still runnable (not exited / suspended)? */

/* ---------- loader ---------- */
int  pe_load(Win32 *w, const u8 *bytes, size_t len, const char *name);
int  pe_load_dll(Win32 *w, const u8 *bytes, size_t len, const char *name);   /* → module index or -1 */
void pe_tls_thread_init(Win32 *w, int th);
int  pe_is_system_dll(const char *dll);
int  pe_managed_kind(const u8 *bytes, size_t len);   /* 0 native, 1 managed assembly, 2 .NET apphost, 3 mixed-mode */
int  pe_dll_native_ok(const char *dll);
int  pe_dll_native_only(const char *dll);   /* system-named DLL the emulator has NO shims for: it must be a real file (DirectX pack → C:\\Windows\\System32), otherwise LoadLibrary fails honestly */   /* system DLL that may be replaced by a real file on the virtual disk (msvcp140, msvcr120, d3dcompiler …) */
/* static inspection (no execution): fills a JSON-ish report of arch/subsystem/imports and supported ratio */
int  pe_inspect(const u8 *bytes, size_t len, char *out, size_t out_sz);

/* tiny libc replacements (we are freestanding in wasm) */
size_t e_strlen(const char *s); int e_strcmp(const char *a, const char *b); int e_stricmp(const char *a, const char *b);
int    e_strnicmp(const char *a, const char *b, size_t n);
int e_strncmp(const char *a, const char *b, size_t n);
void *e_memcpy(void *d, const void *s, size_t n); void *e_memset(void *d, int c, size_t n); int e_memcmp(const void *a, const void *b, size_t n);
void *e_memmove(void *d, const void *s, size_t n);
char *e_strcpy(char *d, const char *s); char *e_strchr(const char *s, int c);
int e_snprintf(char *buf, size_t n, const char *fmt, ...);
int e_vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);
double e_strtod(const char *s, char **end); long long e_strtoll(const char *s, char **end, int base);
double e_sqrt(double x); double e_floor(double x); double e_ceil(double x); double e_fabs(double x); double e_fmod(double a, double b);
void e_log(const char *fmt, ...);                      /* emulator diagnostics → host fd 3 */


/* ---- vfs.c: in-memory file system + stdin queue ---- */
typedef struct { const char *path; const u8 *data; u64 size; int is_dir; int written; } VNodeInfo;
const char *vfs_cwd(void);
void vfs_normalize(const char *in, char *out, size_t cap);
int  vfs_find_node(const char *path);
int  vfs_add(const char *path, const u8 *data, u64 n);
int  vfs_add_ref(const char *path, u8 *data, u64 n);
int  vfs_mkdir(const char *path);
int  vfs_remove(const char *path);
int  vfs_rename(const char *from, const char *to);
int  vfs_chdir(const char *path);
int  vfs_stat(const char *path, u64 *size);
const VNodeInfo *vfs_node_info(int i, VNodeInfo *out);
const u8 *vfs_node_data(int i);
int vfs_delete_on_close(int fd, int on);
int vfs_dir_nonempty(const char *path);               /* file bytes (reloads a node evicted to a write-back mount and pins it: images must stay resident) */
int  vfs_node_count(void);
int  vfs_open(const char *path, int access, int disp, int append);
int  vfs_close(int fd);
int  vfs_is_null_dev(const char *path);
int  vfs_place_low(int fd);
int  vfs_set_inherit(int fd, int inherit);   /* returns previous flag or -1 */
int  vfs_is_open(int fd);
int  vfs_fd_node(int fd);
s64  vfs_read(int fd, u8 *buf, u64 n);
s64  vfs_write(int fd, const u8 *buf, u64 n);
s64  vfs_seek(int fd, s64 off, int whence);
s64  vfs_tell(int fd);
s64  vfs_size(int fd);
int  vfs_eof(int fd);
void vfs_clearerr(int fd);
int  vfs_ungetc(int fd, int ch);
int  vfs_truncate(int fd);
int  vfs_fd_isdir(int fd);
const char *vfs_fd_path(int fd);
int  vfs_dup(int fd);
int  vfs_dup2(int src, int dst);
int  vfs_resolve(int fd);
void vfs_close_all(void);
int  vfs_find_first(const char *pattern);
int  vfs_find_next(int h, char *name, size_t cap, u64 *size, int *is_dir);
void vfs_find_close(int h);
void vfs_stdin_push(const u8 *d, u64 n);
void vfs_stdin_eof(void);
int  vfs_stdin_waiting(void);
s64  vfs_stdin_read(u8 *buf, u64 n);
int  vfs_stdin_available(void);
void vfs_stdin_unread(const u8 *p, int n);

#endif
