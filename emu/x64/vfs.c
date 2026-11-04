/* Nebula x64 — in-memory virtual file system + stdin queue.
 * Files live in host memory (emu_alloc, grown by doubling); paths are Windows-style, case-insensitive,
 * rooted at C:\ with the process cwd C:\nebula.  Also provides CRT-style fds (0,1,2 = console, 3.. = files). */
#include "emu.h"

#define VFS_NODES_INIT 2048        /* the node table and its hash index grow by doubling (npm trees have tens of thousands of files) */
#define VFS_MAX_OPEN  128
#define VFS_MAX_FIND  32
#define VFS_PATH      512

typedef struct { char path[VFS_PATH]; u8 *data; u64 size, cap; int is_dir; int written; int used; int owned, evicted, opens, pinned, del_pending, unlinked; } VNode;   /* del_pending: remove when the last handle closes; unlinked: name already gone from the index */   /* owned: data from emu_alloc (else a host mapping); evicted: bytes live on a write-back mount, reloaded on open; opens: FdEnts referencing the node */

static VNode *g_nodes; static int g_ncap;            /* dynamic node table */
static int *g_hash; static int g_hcap;                /* open-addressing index: 0 empty, -1 tombstone, else node index + 1 */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static u32 path_hash(const char *p) { u32 h = 2166136261u; for (; *p; p++) h = (h ^ (u32)lc((u8)*p)) * 16777619u; return h; }
static int path_eq(const char *a, const char *b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return lc(*a) == lc(*b); }
static void hash_insert(int i) { u32 m = (u32)g_hcap - 1, h = path_hash(g_nodes[i].path) & m; int first_tomb = -1;
  for (;;) { int v = g_hash[h]; if (v == 0) { g_hash[first_tomb >= 0 ? first_tomb : (int)h] = i + 1; return; } if (v == -1 && first_tomb < 0) first_tomb = (int)h; h = (h + 1) & m; } }
static void hash_remove(int i) { u32 m = (u32)g_hcap - 1, h = path_hash(g_nodes[i].path) & m; for (;;) { int v = g_hash[h]; if (v == 0) return; if (v == i + 1) { g_hash[h] = -1; return; } h = (h + 1) & m; } }
static int hash_lookup(const char *norm) { if (!g_hcap) return -1; u32 m = (u32)g_hcap - 1, h = path_hash(norm) & m; for (;;) { int v = g_hash[h]; if (v == 0) return -1; if (v > 0 && g_nodes[v - 1].used && path_eq(g_nodes[v - 1].path, norm)) return v - 1; h = (h + 1) & m; } }
static void nodes_grow(void) {
  int nc = g_ncap ? g_ncap * 2 : VFS_NODES_INIT; VNode *nn = emu_alloc(sizeof(VNode) * (size_t)nc); if (!nn) return;
  if (g_ncap) e_memcpy(nn, g_nodes, sizeof(VNode) * (size_t)g_ncap); g_nodes = nn; g_ncap = nc;
  g_hcap = nc * 4; g_hash = emu_alloc(sizeof(int) * (size_t)g_hcap); for (int i = 0; i < g_ncap; i++) if (g_nodes[i].used) hash_insert(i); }
static void node_free(int i) { if (i >= 0 && i < g_ncap && g_nodes[i].used) { if (!g_nodes[i].unlinked) hash_remove(i); g_nodes[i].used = 0; } }
/* per-process state (fd table, cwd, redirections, find handles, environment) lives in a ProcFs; the scheduler
 * points vfs_P at the running process.  Files, pipes and mappings are kernel objects shared by all processes. */
static ProcFs g_root_fs = { .cwd = "C:\\nebula", .redir = { -1, -1, -1 } };
ProcFs *vfs_P = &g_root_fs;
#define g_cwd (vfs_P->cwd)
#define g_redir (vfs_P->redir)
#define g_find (vfs_P->find)
void vfs_use(ProcFs *p) { vfs_P = p ? p : &g_root_fs; }
ProcFs *vfs_root_fs(void) { return &g_root_fs; }
/* stdin */
static u8 *g_in; static u64 g_in_len, g_in_cap, g_in_pos; static int g_in_eof, g_in_wait;

const char *vfs_cwd(void) { return g_cwd; }

/* normalise: slashes, \\?\ prefix, relative → cwd, . and .., trailing slash */
void vfs_normalize(const char *in, char *out, size_t cap) {
  char tmp[VFS_PATH * 2]; size_t n = 0;
  if (!e_strncmp(in, "\\\\?\\", 4)) in += 4;
  int abs_drive = ((in[0] >= 'a' && in[0] <= 'z') || (in[0] >= 'A' && in[0] <= 'Z')) && in[1] == ':';
  if (abs_drive) { tmp[n++] = (char)(in[0] >= 'a' ? in[0] - 32 : in[0]); tmp[n++] = ':'; in += 2; if (*in != '\\' && *in != '/') { tmp[n++] = '\\'; } }
  else if (in[0] == '\\' || in[0] == '/') { tmp[n++] = 'C'; tmp[n++] = ':'; }
  else { size_t l = e_strlen(g_cwd); e_memcpy(tmp, g_cwd, l); n = l; tmp[n++] = '\\'; }
  for (; *in && n + 1 < sizeof tmp; in++) tmp[n++] = (*in == '/') ? '\\' : *in;
  tmp[n] = 0;
  /* resolve components */
  char *comps[128]; int nc = 0; char buf[VFS_PATH * 2]; e_memcpy(buf, tmp, n + 1);
  char *p = buf + 2; /* after "C:" */
  while (*p) {
    while (*p == '\\') p++; if (!*p) break;
    char *s = p; while (*p && *p != '\\') p++; if (*p) *p++ = 0;
    if (!e_strcmp(s, ".")) continue;
    if (!e_strcmp(s, "..")) { if (nc) nc--; continue; }
    if (nc < 128) comps[nc++] = s;
  }
  size_t o = 0; out[o++] = buf[0]; out[o++] = ':';
  for (int i = 0; i < nc; i++) { size_t l = e_strlen(comps[i]); if (o + l + 2 >= cap) break; out[o++] = '\\'; e_memcpy(out + o, comps[i], l); o += l; }
  if (nc == 0 && o + 1 < cap) out[o++] = '\\';
  out[o] = 0;
}

int vfs_find_node(const char *path) {
  char n[VFS_PATH]; vfs_normalize(path, n, sizeof n);
  { int i = hash_lookup(n); if (i >= 0) return i; }
  if (!e_stricmp(n, "C:\\") || path_eq(n, g_cwd)) return -2;   /* implicit directories */
  return -1;
}
static int node_alloc(const char *norm) {
  static int next_free = 0; if (!g_ncap) nodes_grow();
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 0; k < g_ncap; k++) { int i = (next_free + k) % g_ncap; if (!g_nodes[i].used) { e_memset(&g_nodes[i], 0, sizeof g_nodes[i]); g_nodes[i].used = 1; e_snprintf(g_nodes[i].path, VFS_PATH, "%s", norm); hash_insert(i); next_free = i + 1; return i; } }
    int old = g_ncap; nodes_grow(); if (g_ncap == old) break; next_free = old; }
  return -1;
}
static void ensure_parents(const char *norm) {
  char d[VFS_PATH]; e_snprintf(d, sizeof d, "%s", norm);
  for (;;) {
    size_t l = e_strlen(d); while (l > 0 && d[l - 1] != '\\') l--; if (l <= 3) return; d[l - 1] = 0;
    if (vfs_find_node(d) >= -2 && vfs_find_node(d) != -1) return;
    int i = node_alloc(d); if (i < 0) return; g_nodes[i].is_dir = 1; host_fs_op(3, d, 0, 0, 0);
  }
}
static int grow(VNode *v, u64 need) {
  if (need <= v->cap) return 1;
  u64 nc = v->cap ? v->cap : 4096; while (nc < need) nc *= 2;
  u8 *nd = emu_alloc((size_t)nc); if (!nd) return 0;
  if (v->size) e_memcpy(nd, v->data, (size_t)v->size);
  if (v->data && v->owned) emu_free(v->data);
  v->data = nd; v->cap = nc; v->owned = 1; return 1;
}
/* write-back mounts: a node whose bytes were persisted by the host and that nobody has open drops its memory copy */
static void node_drop_data(VNode *v) { if (v->data) { if (v->owned) emu_free(v->data); else { u8 *d = v->data; u64 n = v->cap; host_fs_op(6, 0, 0, &d, &n); } } v->data = 0; v->cap = 0; v->owned = 0; }
static int node_load(int i) {
  VNode *v = &g_nodes[i]; if (!v->evicted) return 1;
  u8 *d = 0; u64 n = 0; if (!host_fs_op(2, v->path, 0, &d, &n)) { v->evicted = 0; v->size = 0; return 0; }   /* vanished on the host: behave like an empty file */
  v->data = d; v->cap = n; v->size = n; v->owned = 0; v->evicted = 0; return 1;
}
static void node_sync(int i) {
  VNode *v = &g_nodes[i]; if (!v->used || v->is_dir || v->evicted) return;
  if (v->written) { u8 *d = v->data; u64 n = v->size; if (!host_fs_op(1, v->path, 0, &d, &n)) return; v->written = 0; }
  else if (!host_fs_op(2, v->path, 0, 0, 0)) return;   /* only files that exist on a write-back mount may be evicted */
  if (v->opens <= 0 && !v->pinned) { node_drop_data(v); v->evicted = 1; }
}
void vfs_sync_all(void) { for (int i = 0; i < g_ncap; i++) if (g_nodes[i].used && g_nodes[i].written && !g_nodes[i].is_dir) { u8 *d = g_nodes[i].data; u64 n = g_nodes[i].size; if (host_fs_op(1, g_nodes[i].path, 0, &d, &n)) g_nodes[i].written = 0; } }
/* add/replace a file (host-provided bytes are copied) */
int vfs_add(const char *path, const u8 *data, u64 n) {
  char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm);
  int i = vfs_find_node(norm); if (i == -2) return -1;
  if (i < 0) { i = node_alloc(norm); if (i < 0) return -1; ensure_parents(norm); }
  VNode *v = &g_nodes[i]; v->is_dir = 0; v->size = 0; v->evicted = 0; if (!grow(v, n)) return -1; if (n) e_memcpy(v->data, data, (size_t)n); v->size = n; v->written = 0; return i;
}
/* add a file that REFERENCES host-owned bytes (no copy; the host keeps the buffer alive for the whole session).
   Used for the exe image and for --mount'ed / browser-provided files: a 200 MB Bun executable must not exist three times. */
int vfs_add_ref(const char *path, u8 *data, u64 n) {
  char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm);
  int i = vfs_find_node(norm); if (i == -2) return -1;
  if (i < 0) { i = node_alloc(norm); if (i < 0) return -1; ensure_parents(norm); }
  VNode *v = &g_nodes[i]; v->is_dir = 0; if (v->data && v->owned) emu_free(v->data); v->data = data; v->cap = n; v->size = n; v->written = 0; v->owned = 0; v->evicted = 0; return i;
}
int vfs_mkdir(const char *path) {
  char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm);
  { int f = vfs_find_node(norm); if (f >= 0 || (f == -2 && !e_stricmp(norm, "C:\\"))) return 0; }   /* the implicit cwd directory becomes a real node (it must survive a chdir away from it) */
  int i = node_alloc(norm); if (i < 0) return 0; g_nodes[i].is_dir = 1; ensure_parents(norm); host_fs_op(3, norm, 0, 0, 0); return 1;
}
int vfs_remove(const char *path) { int i = vfs_find_node(path); if (i < 0) return 0; if (g_nodes[i].is_dir) { for (int k = 0; k < g_ncap; k++) if (g_nodes[k].used && k != i && !e_strncmp(g_nodes[k].path, g_nodes[i].path, e_strlen(g_nodes[i].path)) && g_nodes[k].path[e_strlen(g_nodes[i].path)] == '\\') return 0; } { u64 isd = (u64)g_nodes[i].is_dir; host_fs_op(4, g_nodes[i].path, 0, 0, &isd); }
  if (g_nodes[i].opens > 0) { hash_remove(i); g_nodes[i].unlinked = 1; g_nodes[i].del_pending = 1; return 1; }   /* POSIX-style delete: the name goes now, the bytes when the last handle closes */
  if (!g_nodes[i].is_dir) node_drop_data(&g_nodes[i]); node_free(i); return 1; }

int vfs_rename(const char *from, const char *to) { int i = vfs_find_node(from); if (i < 0) return 0; char from_norm[VFS_PATH]; e_snprintf(from_norm, sizeof from_norm, "%s", g_nodes[i].path); char norm[VFS_PATH]; vfs_normalize(to, norm, sizeof norm); int j = vfs_find_node(norm); if (j == i) return 1; if (j >= 0) { if (g_nodes[j].is_dir != g_nodes[i].is_dir) return 0; if (!g_nodes[j].is_dir) node_drop_data(&g_nodes[j]); node_free(j); } hash_remove(i); e_snprintf(g_nodes[i].path, VFS_PATH, "%s", norm); hash_insert(i); ensure_parents(norm); host_fs_op(5, from_norm, norm, 0, 0);
  if (g_nodes[i].is_dir) {   /* move the subtree */ size_t fl = e_strlen(from_norm); for (int k = 0; k < g_ncap; k++) if (g_nodes[k].used && k != i && !e_strncmp(g_nodes[k].path, from_norm, fl) && g_nodes[k].path[fl] == '\\') { char np[VFS_PATH]; e_snprintf(np, sizeof np, "%s%s", norm, g_nodes[k].path + fl); hash_remove(k); e_snprintf(g_nodes[k].path, VFS_PATH, "%s", np); hash_insert(k); } }
  return 1; }
int vfs_chdir(const char *path) { char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm); int i = vfs_find_node(norm); if (i == -1 || (i >= 0 && !g_nodes[i].is_dir)) return 0; e_snprintf(g_cwd, sizeof g_cwd, "%s", norm); return 1; }
/* stat: returns 0 missing, 1 file, 2 dir */
int vfs_stat(const char *path, u64 *size) { if (vfs_is_null_dev(path)) { if (size) *size = 0; return 3; } int i = vfs_find_node(path); if (i == -1) return 0; if (i == -2) { if (size) *size = 0; return 2; } if (size) *size = g_nodes[i].size; return g_nodes[i].is_dir ? 2 : 1; }
const VNodeInfo *vfs_node_info(int i, VNodeInfo *out) { if (i < 0 || i >= g_ncap || !g_nodes[i].used) return 0; out->path = g_nodes[i].path; out->data = g_nodes[i].data; out->size = g_nodes[i].size; out->is_dir = g_nodes[i].is_dir; out->written = g_nodes[i].written; return out; }
const u8 *vfs_node_data(int i) { if (i < 0 || i >= g_ncap || !g_nodes[i].used || g_nodes[i].is_dir) return 0; node_load(i); g_nodes[i].pinned = 1; return g_nodes[i].data; }
int vfs_node_count(void) { return g_ncap; }

/* ---------- kernel objects: pipes and anonymous mappings ---------- */
#define PIPE_MAX 32
#define PIPE_CAP 65536
typedef struct { int used; u8 *buf; u32 head, len; int readers, writers; } Pipe;
static Pipe g_pipes[PIPE_MAX];
#define MAP_MAX 32
typedef struct { int used; u8 *data; u64 size; int refs; int node; } Mapping;   /* node ≥ 0: backed by a VFS file */
static Mapping g_maps[MAP_MAX];

/* ---------- per-process fd table (fd ≥ 3) ---------- */
/* access: 1 read, 2 write; disp: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING; returns fd or -errno (-2 ENOENT, -17 EEXIST, -13 EACCES, -24 EMFILE, -21 EISDIR) */
static FdEnt *vf(int fd) { fd -= 3; if (fd < 0 || fd >= FD_MAX || !vfs_P->fd[fd].kind) return 0; return &vfs_P->fd[fd]; }
static int slot_alloc(void) { for (int k = 0; k < FD_MAX; k++) if (!vfs_P->fd[k].kind) return k; return -1; }
static void ent_addref(FdEnt *e) {
  if (e->kind == FD_FILE) { if (e->node >= 0) g_nodes[e->node].opens++; }
  else if (e->kind == FD_PIPE) { if (e->wend) g_pipes[e->obj].writers++; else g_pipes[e->obj].readers++; }
  else if (e->kind == FD_MAP) g_maps[e->obj].refs++;
  else if (e->kind == FD_PROC) proc_handle_ref(e->obj, 1);
  else if (e->kind == FD_KOBJ) kobj_ref(e->obj, 1);
  else if (e->kind == FD_SOCK) w32_sock_ref(e->obj, 1);
}
static void ent_release(FdEnt *e) {
  if (e->kind == FD_FILE) { if (e->node >= 0) { VNode *v = &g_nodes[e->node]; if (--v->opens <= 0) { v->opens = 0; if (v->del_pending) { if (!v->unlinked) { u64 isd = (u64)v->is_dir; host_fs_op(4, v->path, 0, 0, &isd); } if (!v->is_dir) node_drop_data(v); node_free(e->node); } else node_sync(e->node); } } }
  else if (e->kind == FD_MAP) { int nd = g_maps[e->obj].node; if (--g_maps[e->obj].refs <= 0) { g_maps[e->obj].used = 0; if (nd >= 0 && g_nodes[nd].used) node_sync(nd); } }
  else if (e->kind == FD_PIPE) { Pipe *p = &g_pipes[e->obj]; if (e->wend) p->writers--; else p->readers--; if (p->readers <= 0 && p->writers <= 0) p->used = 0; }
  else if (e->kind == FD_PROC) proc_handle_ref(e->obj, -1);
  else if (e->kind == FD_KOBJ) kobj_ref(e->obj, -1);
  else if (e->kind == FD_SOCK) w32_sock_ref(e->obj, -1);
  e->kind = 0;
}
static int ent_new(int kind) { int k = slot_alloc(); if (k < 0) return -1; FdEnt *e = &vfs_P->fd[k]; e_memset(e, 0, sizeof *e); e->kind = kind; e->node = -1; e->unget = -1; return k + 3; }
/* NUL / /dev/null / /dev/zero: a character device that swallows writes and reads EOF (zero for /dev/zero) */
int vfs_is_null_dev(const char *path) {
  if (!e_stricmp(path, "NUL") || !e_stricmp(path, "\\\\.\\NUL") || !e_stricmp(path, "/dev/null") || !e_stricmp(path, "\\dev\\null")) return 1;
  if (!e_stricmp(path, "/dev/zero") || !e_stricmp(path, "\\dev\\zero")) return 2;
  char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm);
  if (!e_stricmp(norm, "C:\\dev\\null") || !e_stricmp(norm, "C:\\nebula\\.nul") || !e_stricmp(norm, "C:\\NUL")) return 1;
  if (!e_stricmp(norm, "C:\\dev\\zero")) return 2;
  return 0;
}
int vfs_open(const char *path, int access, int disp, int append) {
  int nd = vfs_is_null_dev(path);
  if (nd) { int fd = ent_new(FD_NULL); if (fd < 0) return -24; vf(fd)->node = -1; vf(fd)->obj = nd; return fd; }
  char norm[VFS_PATH]; vfs_normalize(path, norm, sizeof norm);
  int i = vfs_find_node(norm);
  if (i == -2 || (i >= 0 && g_nodes[i].is_dir)) { if (access & 2) return -21; }
  if (i == -1) {
    if (disp == 3 || disp == 5) return -2;
    i = node_alloc(norm); if (i < 0) return -24; ensure_parents(norm); g_nodes[i].written = 1;
  } else if (i >= 0) {
    if (disp == 1) return -17;
    if (disp == 2 || disp == 5) { if (g_nodes[i].evicted) { node_drop_data(&g_nodes[i]); g_nodes[i].evicted = 0; } g_nodes[i].size = 0; g_nodes[i].written = 1; }
    else if (!g_nodes[i].is_dir) node_load(i);
  }
  int fd = ent_new(FD_FILE); if (fd < 0) return -24; FdEnt *e = vf(fd); e->node = i; e->append = append; if (i >= 0) g_nodes[i].opens++; return fd;
}
/* std slots: -1 console, -2 closed (POSIX close(0); open() must then return 0), >=3 an fd-table entry */
/* 1 = path is a directory with entries (deleting it must fail with ERROR_DIR_NOT_EMPTY, which is how std::filesystem::remove_all decides to recurse) */
int vfs_dir_nonempty(const char *path) { int i = vfs_find_node(path); if (i < 0 || !g_nodes[i].is_dir) return 0; size_t l = e_strlen(g_nodes[i].path); for (int k = 0; k < g_ncap; k++) if (g_nodes[k].used && k != i && !e_strncmp(g_nodes[k].path, g_nodes[i].path, l) && g_nodes[k].path[l] == '\\') return 1; return 0; }
/* FILE_FLAG_DELETE_ON_CLOSE / FileDispositionInfo on an open handle */
int vfs_delete_on_close(int fd, int on) { FdEnt *f = vf(fd); if (!f || f->kind != FD_FILE || f->node < 0) return 0; g_nodes[f->node].del_pending = on; return 1; }
int vfs_close(int fd) { if (fd >= 0 && fd < 3) { if (g_redir[fd] == -2) return 0; if (g_redir[fd] >= 3) vfs_close(g_redir[fd]); g_redir[fd] = -2; return 1; } FdEnt *f = vf(fd); if (!f) return 0; ent_release(f); return 1; }
/* CRT open/dup/pipe: a closed std slot is the lowest free descriptor */
int vfs_place_low(int fd) { if (fd < 3) return fd; for (int k = 0; k < 3; k++) if (g_redir[k] == -2) { g_redir[k] = fd; return k; } return fd; }
int vfs_set_inherit(int fd, int inherit) { if (fd >= 0 && fd < 3) { if (g_redir[fd] < 3) return 1; fd = g_redir[fd]; } FdEnt *f = vf(fd); if (!f) return -1; int prev = !f->noinherit; f->noinherit = !inherit; return prev; }
int vfs_is_open(int fd) { return vf(fd) != 0; }
int vfs_kind(int fd) { FdEnt *f = vf(fd); return f ? f->kind : 0; }
int vfs_fd_obj(int fd) { FdEnt *f = vf(fd); return f ? f->obj : -1; }
int vfs_fd_node(int fd) { FdEnt *f = vf(fd); return f ? f->node : -1; }
/* resolve any fd to: -1 console stdin, 1/2 console out, >=3 table entry, 0 invalid */
int vfs_resolve(int fd) {
  if (fd >= 0 && fd < 3) { if (g_redir[fd] == -1) return fd == 0 ? -1 : fd; if (g_redir[fd] == -2) return 0; fd = g_redir[fd]; }
  FdEnt *f = vf(fd); if (!f) return 0; if (f->kind == FD_CONSOLE) return f->alias; return fd;
}
int vfs_dup(int fd) {
  if (fd >= 0 && fd < 3) {
    if (g_redir[fd] == -1) { int d = ent_new(FD_CONSOLE); if (d < 0) return -1; vf(d)->alias = fd == 0 ? -1 : fd; return d; }
    if (g_redir[fd] == -2) return -1;
    fd = g_redir[fd];
  }
  FdEnt *f = vf(fd); if (!f) return -1; int k = slot_alloc(); if (k < 0) return -1; vfs_P->fd[k] = *f; vfs_P->fd[k].noinherit = 0; ent_addref(&vfs_P->fd[k]); return k + 3;
}
int vfs_dup2(int src, int dst) {
  if (src == dst) return dst;
  if (dst >= 0 && dst < 3) {
    int r = vfs_resolve(src); if (!r) return -1;
    if (g_redir[dst] >= 3) vfs_close(g_redir[dst]);
    if (r < 3) { g_redir[dst] = -1; return dst; }
    int d = vfs_dup(src); if (d < 0) return -1; g_redir[dst] = d; return dst;
  }
  if (dst < 3 || dst - 3 >= FD_MAX) return -1;
  int d = vfs_dup(src); if (d < 0) return -1;
  FdEnt *old = vf(dst); if (old) ent_release(old);
  vfs_P->fd[dst - 3] = vfs_P->fd[d - 3]; vfs_P->fd[d - 3].kind = 0; return dst;
}
/* pipes: returns 0 on success */
int vfs_pipe(int *rfd, int *wfd) {
  int pi = -1; for (int i = 0; i < PIPE_MAX; i++) if (!g_pipes[i].used) { pi = i; break; } if (pi < 0) return -1;
  Pipe *p = &g_pipes[pi]; if (!p->buf) p->buf = emu_alloc(PIPE_CAP); p->used = 1; p->head = p->len = 0; p->readers = p->writers = 0;
  int r = ent_new(FD_PIPE); if (r < 0) { p->used = 0; return -1; } vf(r)->obj = pi; vf(r)->wend = 0; p->readers = 1;
  int w = ent_new(FD_PIPE); if (w < 0) { ent_release(vf(r)); return -1; } vf(w)->obj = pi; vf(w)->wend = 1; p->writers = 1;
  *rfd = r; *wfd = w; return 0;
}
int vfs_pipe_avail(int fd) { FdEnt *f = vf(fd); if (!f || f->kind != FD_PIPE) return -1; return (int)g_pipes[f->obj].len; }
int vfs_pipe_id(int fd) { FdEnt *f = vf(fd); return f && f->kind == FD_PIPE ? f->obj : -1; }
/* does a blocked pipe read/write still have to wait? (used by the scheduler) */
int vfs_pipe_would_block(int pi, int for_write) { if (pi < 0 || pi >= PIPE_MAX || !g_pipes[pi].used) return 0; Pipe *p = &g_pipes[pi]; return for_write ? (p->len >= PIPE_CAP && p->readers > 0) : (p->len == 0 && p->writers > 0); }
static s64 pipe_read(Pipe *p, u8 *buf, u64 n) {
  if (p->len == 0) return p->writers > 0 ? -2 : 0;
  u64 o = 0; while (o < n && p->len) { buf[o++] = p->buf[p->head]; p->head = (p->head + 1) % PIPE_CAP; p->len--; }
  return (s64)o;
}
static s64 pipe_write(Pipe *p, const u8 *buf, u64 n) {
  if (p->readers <= 0) return -3;                       /* EPIPE */
  if (p->len >= PIPE_CAP) return -2;
  u64 o = 0; while (o < n && p->len < PIPE_CAP) { p->buf[(p->head + p->len) % PIPE_CAP] = buf[o++]; p->len++; }
  return (s64)o;
}
/* anonymous / file-backed mappings */
int vfs_map_create(int file_fd, u64 size) {
  int mi = -1; for (int i = 0; i < MAP_MAX; i++) if (!g_maps[i].used) { mi = i; break; } if (mi < 0) return -1;
  Mapping *m = &g_maps[mi]; m->used = 1; m->refs = 0; m->node = -1;
  if (file_fd >= 3) { FdEnt *f = vf(file_fd); if (!f || f->kind != FD_FILE || f->node < 0) { m->used = 0; return -1; } m->node = f->node; m->data = 0; m->size = size ? size : g_nodes[f->node].size; }
  else { if (!size) { m->used = 0; return -1; } m->data = emu_alloc((size_t)size); m->size = size; }
  int fd = ent_new(FD_MAP); if (fd < 0) { m->used = 0; return -1; } vf(fd)->obj = mi; m->refs = 1; return fd;
}
/* host pointer + size of a mapping's backing store (file-backed → the file bytes, grown to size) */
u8 *vfs_map_data(int fd, u64 *size) {
  FdEnt *f = vf(fd); if (!f || f->kind != FD_MAP) return 0; Mapping *m = &g_maps[f->obj];
  if (m->node >= 0) { VNode *v = &g_nodes[m->node]; node_load(m->node); if (!grow(v, m->size)) return 0; if (v->size < m->size) { e_memset(v->data + v->size, 0, (size_t)(m->size - v->size)); } *size = m->size; return v->data; }
  *size = m->size; return m->data;
}
int vfs_map_write_back(int fd, const u8 *src, u64 n) { u64 sz; u8 *d = vfs_map_data(fd, &sz); if (!d) return 0; if (n > sz) n = sz; if (!e_memcmp(d, src, (size_t)n)) return 1;   /* read-only views (and unchanged ones) must not dirty the node: a write-back would truncate the very host file the data is mapped from */
  e_memcpy(d, src, (size_t)n); FdEnt *f = vf(fd); if (f && g_maps[f->obj].node >= 0) { VNode *v = &g_nodes[g_maps[f->obj].node]; if (v->size < n) v->size = n; v->written = 1; } return 1; }
/* process handles */
int vfs_proc_handle(int pid) { int fd = ent_new(FD_PROC); if (fd < 0) return -1; vf(fd)->obj = pid; proc_handle_ref(pid, 1); return fd; }
int vfs_fd_pid(int fd) { FdEnt *f = vf(fd); return f && f->kind == FD_PROC ? f->obj : -1; }
/* kernel objects: events, mutexes, semaphores, threads */
static KObj g_kobjs[KOBJ_MAX];
int kobj_new(int kind, const char *name, int *existed) {
  if (existed) *existed = 0;
  if (name && name[0]) for (int i = 0; i < KOBJ_MAX; i++) if (g_kobjs[i].used && g_kobjs[i].kind == kind && !e_strcmp(g_kobjs[i].name, name)) { if (existed) *existed = 1; return i; }
  for (int i = 0; i < KOBJ_MAX; i++) if (!g_kobjs[i].used) { KObj *k = &g_kobjs[i]; e_memset(k, 0, sizeof *k); k->used = 1; k->kind = kind; if (name) e_snprintf(k->name, sizeof k->name, "%s", name); return i; }
  return -1;
}
KObj *kobj_get(int idx) { return idx >= 0 && idx < KOBJ_MAX && g_kobjs[idx].used ? &g_kobjs[idx] : 0; }
void kobj_ref(int idx, int delta) { KObj *k = kobj_get(idx); if (!k) return; k->refs += delta; if (k->refs <= 0) k->used = 0; }
int vfs_sock_handle(int idx) { int fd = ent_new(FD_SOCK); if (fd < 0) return -1; vf(fd)->obj = idx; w32_sock_ref(idx, 1); return fd; }
int vfs_kobj_handle(int idx) { if (!kobj_get(idx)) return -1; int fd = ent_new(FD_KOBJ); if (fd < 0) return -1; vf(fd)->obj = idx; kobj_ref(idx, 1); return fd; }
int vfs_fd_kobj(int fd) { FdEnt *f = vf(fd); return f && f->kind == FD_KOBJ ? f->obj : -1; }

s64 vfs_read(int fd, u8 *buf, u64 n) {
  FdEnt *f = vf(fd); if (!f) return -1;
  if (f->kind == FD_PIPE) return f->wend ? -1 : pipe_read(&g_pipes[f->obj], buf, n);
  if (f->kind == FD_NULL) { if (f->obj == 2) { e_memset(buf, 0, (size_t)n); return (s64)n; } f->eof = 1; return 0; }
  if (f->kind != FD_FILE) return -1;
  if (f->node < 0) return 0; VNode *v = &g_nodes[f->node]; if (v->is_dir) return -1;
  u64 o = 0;
  if (f->unget >= 0 && n) { buf[o++] = (u8)f->unget; f->unget = -1; }
  u64 avail = f->pos < v->size ? v->size - f->pos : 0; u64 c = n - o < avail ? n - o : avail;
  if (c) e_memcpy(buf + o, v->data + f->pos, (size_t)c); f->pos += c; o += c; if (o == 0 && n) f->eof = 1; return (s64)o;
}
const char *g_vfs_tap;
static const char *e_strstr_(const char *h, const char *nd) { size_t l = e_strlen(nd); for (; *h; h++) if (!e_strncmp(h, nd, l)) return h; return 0; }
s64 vfs_write(int fd, const u8 *buf, u64 n) {
  FdEnt *f = vf(fd); if (!f) return -1;
  if (f->kind == FD_PIPE) return f->wend ? pipe_write(&g_pipes[f->obj], buf, n) : -1;
  if (f->kind == FD_NULL) return (s64)n;
  if (f->kind != FD_FILE || f->node < 0) return -1; VNode *v = &g_nodes[f->node]; if (v->is_dir) return -1;
  if (f->append) f->pos = v->size;
  if (!grow(v, f->pos + n)) return -1;
  if (f->pos > v->size) e_memset(v->data + v->size, 0, (size_t)(f->pos - v->size));
  e_memcpy(v->data + f->pos, buf, (size_t)n); f->pos += n; if (f->pos > v->size) v->size = f->pos; v->written = 1;
  if (g_vfs_tap && e_strstr_(v->path, g_vfs_tap)) host_write(3, (const char *)buf, (size_t)n);   /* debugging aid: mirror writes to files matching X64_TAP to the emulator log */
  return (s64)n;
}
s64 vfs_seek(int fd, s64 off, int whence) {
  FdEnt *f = vf(fd); if (f && f->kind == FD_NULL) return 0; if (!f || f->kind != FD_FILE || f->node < 0) return -1; VNode *v = &g_nodes[f->node];
  s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : (s64)v->size; s64 np = base + off; if (np < 0) return -1; f->pos = (u64)np; f->eof = 0; f->unget = -1; return np;
}
s64 vfs_tell(int fd) { FdEnt *f = vf(fd); return f && f->kind == FD_NULL ? 0 : f && f->kind == FD_FILE ? (s64)f->pos : -1; }
s64 vfs_size(int fd) { FdEnt *f = vf(fd); if (!f) return -1; if (f->kind == FD_PIPE) return (s64)g_pipes[f->obj].len; if (f->kind != FD_FILE || f->node < 0) return -1; return (s64)g_nodes[f->node].size; }
int vfs_eof(int fd) { FdEnt *f = vf(fd); return f ? f->eof : 1; }
void vfs_clearerr(int fd) { FdEnt *f = vf(fd); if (f) { f->eof = 0; f->err = 0; } }
int vfs_ungetc(int fd, int ch) { FdEnt *f = vf(fd); if (!f) return -1; f->unget = ch & 0xff; f->eof = 0; return ch & 0xff; }
int vfs_truncate(int fd) { FdEnt *f = vf(fd); if (!f || f->kind != FD_FILE || f->node < 0) return 0; VNode *v = &g_nodes[f->node];
  if (f->pos > v->size) { if (!grow(v, f->pos)) return 0; e_memset(v->data + v->size, 0, (size_t)(f->pos - v->size)); }   /* extending: zero-fill (SetEndOfFile past EOF) */
  v->size = f->pos; v->written = 1; return 1; }
int vfs_fd_isdir(int fd) { FdEnt *f = vf(fd); if (!f || f->kind != FD_FILE) return 0; return f->node == -2 || (f->node >= 0 && g_nodes[f->node].is_dir); }
const char *vfs_fd_path(int fd) { FdEnt *f = vf(fd); if (!f || f->kind != FD_FILE || f->node < 0) return g_cwd; return g_nodes[f->node].path; }
/* new process: inherit the parent's table (all entries, same numbers — Windows semantics for inheritable handles) */
void vfs_init_proc(ProcFs *p, const ProcFs *parent) {
  e_memset(p, 0, sizeof *p); p->redir[0] = p->redir[1] = p->redir[2] = -1; e_snprintf(p->cwd, sizeof p->cwd, "%s", parent ? parent->cwd : "C:\\nebula");
  if (g_spawn_cwd && g_spawn_cwd[0]) { char norm[VFS_PATH]; const ProcFs *save = vfs_P; vfs_P = (ProcFs *)(parent ? parent : &g_root_fs); vfs_normalize(g_spawn_cwd, norm, sizeof norm); vfs_P = (ProcFs *)save; e_snprintf(p->cwd, sizeof p->cwd, "%s", norm); }   /* lpCurrentDirectory, relative to the parent's cwd */
  if (parent) {
    for (int k = 0; k < FD_MAX; k++) if (parent->fd[k].kind && !parent->fd[k].noinherit) { p->fd[k] = parent->fd[k]; ent_addref(&p->fd[k]); }
    for (int k = 0; k < 3; k++) { if (parent->redir[k] >= 3) { ProcFs *save = vfs_P; vfs_P = p; int d = vfs_dup(parent->redir[k]); vfs_P = save; p->redir[k] = d; } else p->redir[k] = parent->redir[k]; }
    p->nenv = parent->nenv; for (int i = 0; i < parent->nenv; i++) p->env[i] = parent->env[i]; p->env_dirty = 1;
  }
}
/* release every handle of a process (exit) */
void vfs_release_proc(ProcFs *p) { ProcFs *save = vfs_P; vfs_P = p; for (int k = 0; k < 3; k++) if (p->redir[k] >= 3) { vfs_close(p->redir[k]); p->redir[k] = -1; } for (int k = 0; k < FD_MAX; k++) if (p->fd[k].kind) ent_release(&p->fd[k]); for (int k = 0; k < VFS_MAX_FIND; k++) p->find[k].used = 0; vfs_P = save; }
void vfs_close_all(void) { vfs_release_proc(&g_root_fs); e_snprintf(g_root_fs.cwd, sizeof g_root_fs.cwd, "C:\\nebula"); g_root_fs.nenv = 0; g_root_fs.env_dirty = 1; for (int i = 0; i < PIPE_MAX; i++) g_pipes[i].used = 0; for (int i = 0; i < KOBJ_MAX; i++) g_kobjs[i].used = 0; for (int i = 0; i < MAP_MAX; i++) g_maps[i].used = 0; g_in_pos = 0; g_in_len = 0; g_in_eof = 0; g_in_wait = 0; vfs_P = &g_root_fs; }

/* ---------- directory enumeration (FindFirstFile semantics) ---------- */
static int glob_match(const char *pat, const char *s);
int vfs_glob_match(const char *pat, const char *s) { return glob_match(pat, s); }
static int glob_match(const char *pat, const char *s) {
  if (!*pat) return !*s;
  if (*pat == '*') { for (;; s++) { if (glob_match(pat + 1, s)) return 1; if (!*s) return 0; } }
  if (!*s) return 0;
  if (*pat == '?' || lc(*pat) == lc(*s)) return glob_match(pat + 1, s + 1);
  return 0;
}
/* Win32 rule: a trailing ".*" also matches names without any dot ("*.*" lists everything, "setup.*" matches "setup") */
static int win_match(const char *pat, const char *s) {
  if (glob_match(pat, s)) return 1;
  size_t l = e_strlen(pat); if (l >= 2 && pat[l - 1] == '*' && pat[l - 2] == '.' && !e_strchr(s, '.')) { char p2[VFS_PATH]; if (l - 2 < sizeof p2) { e_memcpy(p2, pat, l - 2); p2[l - 2] = 0; return glob_match(p2, s); } }
  return 0;
}
static const char *basename_of(const char *p) { const char *b = p; for (const char *q = p; *q; q++) if (*q == '\\') b = q + 1; return b; }
static int dir_of(const char *norm, char *dir, size_t cap) { const char *b = basename_of(norm); size_t l = (size_t)(b - norm); if (l > 0 && norm[l - 1] == '\\' && l > 3) l--; if (l >= cap) l = cap - 1; e_memcpy(dir, norm, l); dir[l] = 0; return 1; }
static int in_dir(const char *path, const char *dir) {   /* path's parent == dir ? */
  size_t dl = e_strlen(dir); if (e_strncmp(path, dir, 0) != 0) return 0;
  for (size_t i = 0; i < dl; i++) if (lc(path[i]) != lc(dir[i])) return 0;
  const char *rest = path + dl; if (dl == 3) rest = path + 3; else { if (*rest != '\\') return 0; rest++; }
  if (!*rest) return 0; for (const char *q = rest; *q; q++) if (*q == '\\') return 0; return 1;
}
/* returns find handle index ≥ 0, -1 none, fills first match via vfs_find_next */
int vfs_find_first(const char *pattern) {
  char norm[VFS_PATH]; vfs_normalize(pattern, norm, sizeof norm);
  int h = -1; for (int k = 0; k < VFS_MAX_FIND; k++) if (!g_find[k].used) { h = k; break; } if (h < 0) return -1;
  VFind *f = &g_find[h]; e_memset(f, 0, sizeof *f); f->used = 1;
  const char *pat = basename_of(norm); dir_of(norm, f->dir, sizeof f->dir);
  /* pattern without wildcard that names a directory → enumerate that single entry */
  e_snprintf(f->pat, sizeof f->pat, "%s", pat);
  if (!e_strchr(pat, '*') && !e_strchr(pat, '?')) { int i = vfs_find_node(norm); if (i == -1) { f->used = 0; return -1; } }
  else if (vfs_find_node(f->dir) == -1) { f->used = 0; return -1; }
  if (!e_strcmp(pat, "*") || !e_strcmp(pat, "*.*")) f->dots = 2;
  return h;
}
/* next entry: returns 1 and fills name/size/is_dir, or 0 when done */
int vfs_find_next(int h, char *name, size_t cap, u64 *size, int *is_dir) {
  if (h < 0 || h >= VFS_MAX_FIND || !g_find[h].used) return 0; VFind *f = &g_find[h];
  if (f->dots > 0) { e_snprintf(name, cap, f->dots == 2 ? "." : ".."); *size = 0; *is_dir = 1; f->dots--; return 1; }
  for (; f->cursor < g_ncap; f->cursor++) {
    VNode *v = &g_nodes[f->cursor]; if (!v->used || !in_dir(v->path, f->dir)) continue;
    const char *b = basename_of(v->path); if (!win_match(f->pat, b)) continue;
    e_snprintf(name, cap, "%s", b); *size = v->size; *is_dir = v->is_dir; f->cursor++; return 1;
  }
  return 0;
}
void vfs_find_close(int h) { if (h >= 0 && h < VFS_MAX_FIND) g_find[h].used = 0; }

/* ---------- stdin ---------- */
void vfs_stdin_push(const u8 *d, u64 n) {
  if (g_in_pos && g_in_pos == g_in_len) { g_in_pos = 0; g_in_len = 0; }
  if (g_in_len + n > g_in_cap) { u64 nc = g_in_cap ? g_in_cap : 4096; while (nc < g_in_len + n) nc *= 2; u8 *nb = emu_alloc((size_t)nc); if (g_in_len) e_memcpy(nb, g_in, (size_t)g_in_len); g_in = nb; g_in_cap = nc; }
  e_memcpy(g_in + g_in_len, d, (size_t)n); g_in_len += n; g_in_wait = 0;
}
void vfs_stdin_eof(void) { g_in_eof = 1; g_in_wait = 0; }
int  vfs_stdin_waiting(void) { return g_in_wait; }
/* read up to n bytes (stops after a newline so line-oriented programs behave); returns -2 when it would block */
s64 vfs_stdin_read(u8 *buf, u64 n) {
  if (g_in_pos >= g_in_len) { if (g_in_eof) return 0; g_in_wait = 1; return -2; }
  u64 o = 0; while (o < n && g_in_pos < g_in_len) { u8 ch = g_in[g_in_pos++]; buf[o++] = ch; if (ch == '\n') break; }
  return (s64)o;
}
int vfs_stdin_available(void) { return g_in_pos < g_in_len || g_in_eof; }
/* push bytes back in front of the unread input (they were just read, so the buffer has room before g_in_pos; otherwise grow) */
void vfs_stdin_unread(const u8 *p, int n) {
  if (n <= 0) return;
  if ((u64)n <= g_in_pos) { g_in_pos -= (u64)n; e_memcpy(g_in + g_in_pos, p, (size_t)n); return; }
  u64 rest = g_in_len - g_in_pos, need = rest + (u64)n;
  if (need > g_in_cap) { u64 nc = need + 4096; u8 *nb = emu_alloc((size_t)nc); if (!nb) return; e_memcpy(nb + n, g_in + g_in_pos, (size_t)rest); g_in = nb; g_in_cap = nc; }
  else e_memmove(g_in + n, g_in + g_in_pos, (size_t)rest);
  e_memcpy(g_in, p, (size_t)n); g_in_pos = 0; g_in_len = need;
}
