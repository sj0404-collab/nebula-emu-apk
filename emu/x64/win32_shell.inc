/* Resources (RT_DIALOG / RT_STRING / … from the image's .rsrc), the registry (an in-memory hive that persists to the
 * virtual disk), shell folders (SHGetFolderPath & co.) and the lstr* helpers — what installers and ordinary desktop
 * programs call around the dialog manager (user.inc). Shared by x64 and x86 processes: all pointer-sized reads/writes go
 * through rdp/wrp and PSZ_OF, so the same code serves PE32+ and PE32 images. */

/* ---------- UTF-8 ↔ UTF-16 helpers ---------- */
/* write a UTF-8 host string as UTF-16 into guest memory; cap = capacity in u16 (0 = unlimited); returns chars written without the NUL */
u32 put_w8(Cpu *c, u64 dst, const char *s, u32 cap) {
  u32 o = 0; const u8 *p = (const u8 *)s;
  while (*p) { u32 cp = *p, extra = 0; if (cp >= 0xF0) { cp &= 7; extra = 3; } else if (cp >= 0xE0) { cp &= 15; extra = 2; } else if (cp >= 0xC0) { cp &= 31; extra = 1; } p++;
    for (u32 k = 0; k < extra && (*p & 0xC0) == 0x80; k++) cp = (cp << 6) | (*p++ & 0x3f);
    if (cp >= 0x10000) { if (cap && o + 2 >= cap) break; cp -= 0x10000; wr16(c, dst + o * 2, (u16)(0xD800 | (cp >> 10))); wr16(c, dst + o * 2 + 2, (u16)(0xDC00 | (cp & 0x3ff))); o += 2; }
    else { if (cap && o + 1 >= cap) break; wr16(c, dst + o * 2, (u16)cp); o++; } }
  if (!cap || o < cap) wr16(c, dst + o * 2, 0); return o;
}
u32 utf8_u16len(const char *s) { u32 n = 0; for (const u8 *p = (const u8 *)s; *p; p++) if ((*p & 0xC0) != 0x80) n += (*p >= 0xF0) ? 2 : 1; return n; }

/* ---------- lstr* ---------- */
static void s_lstrcpyW(Cpu *c) { u64 d = ARG(0), s = ARG(1); for (u64 i = 0;; i++) { u16 ch = rd16(c, s + i * 2); wr16(c, d + i * 2, ch); if (!ch || c->status) break; } RET(d); }
static void s_lstrcatW(Cpu *c) { u64 d = ARG(0), s = ARG(1); u64 l = 0; while (rd16(c, d + l * 2)) l++; for (u64 i = 0;; i++) { u16 ch = rd16(c, s + i * 2); wr16(c, d + (l + i) * 2, ch); if (!ch || c->status) break; } RET(d); }
static void s_lstrcatA(Cpu *c) { u64 d = ARG(0), s = ARG(1); u64 l = 0; while (rd8(c, d + l)) l++; for (u64 i = 0;; i++) { u8 ch = rd8(c, s + i); wr8(c, d + l + i, ch); if (!ch || c->status) break; } RET(d); }
static void s_lstrcpynW(Cpu *c) { u64 d = ARG(0), s = ARG(1); int n = (int)ARG(2); if (n <= 0) { RET(d); return; } int i = 0; for (; i < n - 1; i++) { u16 ch = rd16(c, s + i * 2); if (!ch) break; wr16(c, d + i * 2, ch); } wr16(c, d + i * 2, 0); RET(d); }
static void s_lstrcpynA(Cpu *c) { u64 d = ARG(0), s = ARG(1); int n = (int)ARG(2); if (n <= 0) { RET(d); return; } int i = 0; for (; i < n - 1; i++) { u8 ch = rd8(c, s + i); if (!ch) break; wr8(c, d + i, ch); } wr8(c, d + i, 0); RET(d); }
static void s_lstrcmpiW(Cpu *c) { char a[2048], b[2048]; wstr_to_utf8(c, ARG(0), a, sizeof a, -1); wstr_to_utf8(c, ARG(1), b, sizeof b, -1); int r = e_stricmp(a, b); RET((u64)(s64)(r < 0 ? -1 : r > 0 ? 1 : 0)); }

/* ---------- resources ---------- */
/* resource directory of the image mapped at `base` (0 = main exe): rva/size from the PE header in guest memory */
static int rsrc_dir(Cpu *c, u64 base, u64 *dir, u32 *size) {
  if (!base) base = W->image_base; u32 e_lfanew = rd32(c, base + 0x3c); if (e_lfanew > 0x1000 || rd32(c, base + e_lfanew) != 0x4550) return 0;
  u64 opt = base + e_lfanew + 24; u16 magic = rd16(c, opt); u64 dd = opt + (magic == 0x20b ? 112 : 96) + 2 * 8;   /* IMAGE_DIRECTORY_ENTRY_RESOURCE */
  u32 rva = rd32(c, dd), sz = rd32(c, dd + 4); if (!rva || !sz) return 0; *dir = base + rva; *size = sz; return 1;
}
/* find an entry (id, or name string in UTF-8 when id == 0) in a resource directory level; returns the entry's OffsetToData field */
static u32 rsrc_find(Cpu *c, u64 root, u64 dir, u32 id, const char *name) {
  u16 nn = rd16(c, dir + 12), ni = rd16(c, dir + 14); u64 e = dir + 16;
  if (name) { for (u32 i = 0; i < nn; i++, e += 8) { u32 no = rd32(c, e) & 0x7fffffff; u16 l = rd16(c, root + no); char buf[256]; int p = 0; for (u32 k = 0; k < l && p < 255; k++) { u16 ch = rd16(c, root + no + 2 + k * 2); buf[p++] = ch < 128 ? (char)ch : '?'; } buf[p] = 0; if (!e_stricmp(buf, name)) return rd32(c, e + 4); } return 0; }
  e += (u64)nn * 8; for (u32 i = 0; i < ni; i++, e += 8) if (rd32(c, e) == id) return rd32(c, e + 4);
  return 0;
}
/* first entry of a level regardless of its id (language level, or "any" lookups) */
static u32 rsrc_first(Cpu *c, u64 dir) { u16 nn = rd16(c, dir + 12), ni = rd16(c, dir + 14); if (!nn && !ni) return 0; return rd32(c, dir + 16 + 4); }
/* the resource type/name argument: integer (MAKEINTRESOURCE) or string ("#123" counts as an integer) */
static void rsrc_key(Cpu *c, u64 a, int wide, u32 *id, char *name, size_t cap) {
  *id = 0; name[0] = 0; if (a < 0x10000) { *id = (u32)a; return; }
  if (wide) wstr_to_utf8(c, a, name, cap, -1); else { const char *s = rd_str(c, a, cap - 1); e_snprintf(name, cap, "%s", s ? s : ""); }
  if (name[0] == '#') { u32 v = 0; for (const char *p = name + 1; *p >= '0' && *p <= '9'; p++) v = v * 10 + (u32)(*p - '0'); *id = v; name[0] = 0; }
}
/* FindResource(hModule, name, type) → address of the IMAGE_RESOURCE_DATA_ENTRY (the HRSRC Windows hands out is a pointer to it as well) */
u64 rsrc_lookup(Cpu *c, u64 hmod, u64 name_a, u64 type_a, int wide, u16 lang) {
  u64 root; u32 size; if (!rsrc_dir(c, hmod, &root, &size)) return 0;
  u32 tid, nid; char tname[128], nname[128]; rsrc_key(c, type_a, wide, &tid, tname, sizeof tname); rsrc_key(c, name_a, wide, &nid, nname, sizeof nname);
  u32 t = rsrc_find(c, root, root, tid, tname[0] ? tname : 0); if (!(t & 0x80000000u)) return 0;
  u32 n = rsrc_find(c, root, root + (t & 0x7fffffff), nid, nname[0] ? nname : 0); if (!(n & 0x80000000u)) return 0;
  u64 ldir = root + (n & 0x7fffffff); u32 l = lang ? rsrc_find(c, root, ldir, lang, 0) : 0; if (!l) l = rsrc_first(c, ldir); if (!l || (l & 0x80000000u)) return 0;
  return root + l;
}
static void find_resource(Cpu *c, u64 hmod, u64 name, u64 type, int wide, u16 lang) { u64 e = rsrc_lookup(c, hmod, name, type, wide, lang); if (!e) { W->last_error = 1814; RET(0); return; } RET(e); }   /* ERROR_RESOURCE_NAME_NOT_FOUND */
static void s_FindResourceW(Cpu *c) { find_resource(c, ARG(0), ARG(1), ARG(2), 1, 0); }
static void s_FindResourceA(Cpu *c) { find_resource(c, ARG(0), ARG(1), ARG(2), 0, 0); }
static void s_FindResourceExW(Cpu *c) { find_resource(c, ARG(0), ARG(2), ARG(1), 1, (u16)ARG(3)); }
static void s_FindResourceExA(Cpu *c) { find_resource(c, ARG(0), ARG(2), ARG(1), 0, (u16)ARG(3)); }
/* the data entry holds an RVA relative to the image the resource came from: recover the base from the directory address */
static u64 rsrc_base_of(Cpu *c, u64 entry) { if (entry >= W->image_base && entry < W->image_base + W->image_size) return W->image_base; for (int i = 0; i < W->nmods; i++) if (entry >= W->mods[i].base && entry < W->mods[i].base + W->mods[i].size) return W->mods[i].base; return 0; }
static void s_LoadResource(Cpu *c) { u64 e = ARG(1); if (!e) { RET(0); return; } u64 base = ARG(0) ? ARG(0) : rsrc_base_of(c, e); if (!base) base = W->image_base; RET(base + rd32(c, e)); }
static void s_LockResource(Cpu *c) { RET(ARG(0)); }
static void s_SizeofResource(Cpu *c) { u64 e = ARG(1); RET(e ? rd32(c, e + 4) : 0); }
static void s_FreeResource(Cpu *c) { RET(0); }
/* LoadString: RT_STRING blocks of 16 length-prefixed UTF-16 strings; block = id / 16 + 1 */
static int load_string_u16(Cpu *c, u64 hmod, u32 id, u64 *str, u32 *len) {
  u64 e = rsrc_lookup(c, hmod, (id >> 4) + 1, 6, 1, 0); if (!e) return 0; u64 base = hmod ? hmod : W->image_base; u64 p = base + rd32(c, e);
  for (u32 i = 0; i < 16; i++) { u16 l = rd16(c, p); p += 2; if (i == (id & 15)) { *str = p; *len = l; return 1; } p += (u64)l * 2; }
  return 0;
}
static void s_LoadStringW(Cpu *c) { u64 str; u32 len; u64 buf = ARG(2); int cap = (int)ARG(3);
  if (!load_string_u16(c, ARG(0), (u32)ARG(1), &str, &len)) { if (cap > 0 && buf) wr16(c, buf, 0); RET(0); return; }
  if (cap == 0) { wrp(c, buf, str); RET(len); return; }   /* cchBufferMax 0: pointer to the read-only string */
  u32 n = len < (u32)(cap - 1) ? len : (u32)(cap - 1); for (u32 i = 0; i < n; i++) wr16(c, buf + i * 2, rd16(c, str + i * 2)); wr16(c, buf + n * 2, 0); RET(n); }
static void s_LoadStringA(Cpu *c) { u64 str; u32 len; u64 buf = ARG(2); int cap = (int)ARG(3);
  if (!load_string_u16(c, ARG(0), (u32)ARG(1), &str, &len) || cap <= 0) { if (cap > 0 && buf) wr8(c, buf, 0); RET(0); return; }
  char tmp[2048]; wstr_to_utf8(c, str, tmp, sizeof tmp, (int)len); size_t l = e_strlen(tmp); if (l > (size_t)cap - 1) l = cap - 1; u8 *p = mem_ptr(c->mem, buf, l + 1, 1); if (p) { e_memcpy(p, tmp, l); p[l] = 0; } RET(l); }
/* EnumResourceNames(hmod, type, callback(hmod, type, name, param), param) — one guest call per name, through the continuation stack */
#define CONT_ENUMRES 330
static void enum_res_step(Cpu *c) { Win32 *w = W; int ci = w->ncont - 1; u64 *d = w->cont[ci].data; u64 root = d[0], dir = d[1]; u32 i = (u32)d[2], nn = rd16(c, dir + 12), ni = rd16(c, dir + 14);
  if (i >= (u32)nn + ni) { w32_cont_finish(c, 1); return; }
  u64 e = dir + 16 + (u64)i * 8; u32 nm = rd32(c, e); d[2] = i + 1; u64 name;
  if (nm & 0x80000000u) { u32 no = nm & 0x7fffffff; u16 l = rd16(c, root + no); name = w32_heap_alloc(w, (u64)l * 2 + 2, 1); for (u32 k = 0; k < l; k++) wr16(c, name + k * 2, rd16(c, root + no + 2 + k * 2)); } else name = nm & 0xffff;
  w32_guest_call(c, d[3], d[4], d[5], name, d[6]); }
static void s_EnumResourceNamesW(Cpu *c) { u64 root; u32 size; u64 hmod = ARG(0) ? ARG(0) : W->image_base; if (!rsrc_dir(c, hmod, &root, &size)) { W->last_error = 1812; RET(0); return; }
  u32 tid; char tname[128]; rsrc_key(c, ARG(1), 1, &tid, tname, sizeof tname); u32 t = rsrc_find(c, root, root, tid, tname[0] ? tname : 0); if (!(t & 0x80000000u)) { W->last_error = 1813; RET(0); return; }
  Win32 *w = W; w32_push_cont(c, CONT_ENUMRES, 0); u64 *d = w->cont[w->ncont - 1].data; d[0] = root; d[1] = root + (t & 0x7fffffff); d[2] = 0; d[3] = ARG(2); d[4] = hmod; d[5] = ARG(1); d[6] = ARG(3); enum_res_step(c); }
static int shell_cont_resume(Cpu *c, int kind, u64 rax) { if (kind != CONT_ENUMRES) return 0; if (!(u32)rax) { w32_cont_finish(c, 1); return 1; } enum_res_step(c); return 1; }

/* ---------- registry ---------- */
/* keys are full paths ("HKEY_LOCAL_MACHINE\SOFTWARE\7-Zip"), values live in a flat table; both persist as a text hive at
   C:\Windows\nebula.reg on the virtual disk so installs survive across runs where the disk does. */
#define REG_MAX_KEYS 512
#define REG_MAX_VALS 2048
#define REG_HANDLE_TAG 0x52450000u
typedef struct { int used; char path[300]; } RegKey;
typedef struct { int used; int key; char name[128]; u32 type; u32 len; u8 *data; } RegVal;
static RegKey g_rkeys[REG_MAX_KEYS]; static RegVal g_rvals[REG_MAX_VALS]; static int g_reg_init, g_reg_dirty;
typedef struct { int used; int key; } RegHandle;
static RegHandle g_rh[256];
static const char *reg_root_name(u64 h) { switch ((u32)h) { case 0x80000000u: return "HKEY_CLASSES_ROOT"; case 0x80000001u: return "HKEY_CURRENT_USER"; case 0x80000002u: return "HKEY_LOCAL_MACHINE"; case 0x80000003u: return "HKEY_USERS"; case 0x80000005u: return "HKEY_CURRENT_CONFIG"; case 0x80000004u: return "HKEY_PERFORMANCE_DATA"; default: return 0; } }
static int reg_key_find(const char *path) { for (int i = 0; i < REG_MAX_KEYS; i++) if (g_rkeys[i].used && !e_stricmp(g_rkeys[i].path, path)) return i; return -1; }
static int reg_key_create(const char *path) { int k = reg_key_find(path); if (k >= 0) return k;
  /* parents first */ char parent[300]; e_snprintf(parent, sizeof parent, "%s", path); size_t l = e_strlen(parent); while (l > 0 && parent[l - 1] != '\\') l--; if (l > 1) { parent[l - 1] = 0; if (reg_key_find(parent) < 0) reg_key_create(parent); }
  for (int i = 0; i < REG_MAX_KEYS; i++) if (!g_rkeys[i].used) { g_rkeys[i].used = 1; e_snprintf(g_rkeys[i].path, sizeof g_rkeys[i].path, "%s", path); g_reg_dirty = 1; return i; } return -1; }
static int reg_val_find(int key, const char *name) { for (int i = 0; i < REG_MAX_VALS; i++) if (g_rvals[i].used && g_rvals[i].key == key && !e_stricmp(g_rvals[i].name, name)) return i; return -1; }
static void reg_val_set(int key, const char *name, u32 type, const u8 *data, u32 len) { int v = reg_val_find(key, name);
  if (v < 0) { for (int i = 0; i < REG_MAX_VALS; i++) if (!g_rvals[i].used) { v = i; break; } if (v < 0) return; g_rvals[v].used = 1; g_rvals[v].key = key; e_snprintf(g_rvals[v].name, sizeof g_rvals[v].name, "%s", name); g_rvals[v].data = 0; }
  if (g_rvals[v].data) emu_free(g_rvals[v].data); g_rvals[v].data = emu_alloc(len ? len : 1); if (len) e_memcpy(g_rvals[v].data, data, len); g_rvals[v].len = len; g_rvals[v].type = type; g_reg_dirty = 1; }
static void reg_seed_sz(const char *key, const char *name, const char *val) { int k = reg_key_create(key); if (k < 0) return; if (reg_val_find(k, name) >= 0) return; u16 tmp[512]; u32 n = 0; for (const u8 *p = (const u8 *)val; *p && n < 510; p++) tmp[n++] = *p; tmp[n++] = 0; reg_val_set(k, name, 1, (const u8 *)tmp, n * 2); }
static void reg_seed_dw(const char *key, const char *name, u32 v) { int k = reg_key_create(key); if (k < 0) return; if (reg_val_find(k, name) >= 0) return; reg_val_set(k, name, 4, (const u8 *)&v, 4); }
static const char *REG_HIVE_PATH = "C:\\Windows\\nebula.reg";
static void reg_save(void) {
  if (!g_reg_dirty) return; g_reg_dirty = 0; size_t cap = 1 << 16; char *buf = emu_alloc(cap); size_t n = 0;
  #define RPUT(...) do { int k_ = e_snprintf(buf + n, cap - n, __VA_ARGS__); if (k_ > 0) { n += (size_t)k_; if (n + 4096 > cap) { char *nb = emu_alloc(cap * 2); e_memcpy(nb, buf, n); emu_free(buf); buf = nb; cap *= 2; } } } while (0)
  RPUT("Nebula Registry Hive 1\n");
  for (int i = 0; i < REG_MAX_KEYS; i++) if (g_rkeys[i].used) { RPUT("[%s]\n", g_rkeys[i].path);
    for (int v = 0; v < REG_MAX_VALS; v++) if (g_rvals[v].used && g_rvals[v].key == i) { RPUT("\"%s\"=%u:", g_rvals[v].name, g_rvals[v].type); for (u32 b = 0; b < g_rvals[v].len; b++) RPUT("%02x", g_rvals[v].data[b]); RPUT("\n"); } }
  #undef RPUT
  int fd = vfs_open(REG_HIVE_PATH, 2, 2, 0); if (fd >= 0) { vfs_write(fd, (const u8 *)buf, n); vfs_close(fd); } emu_free(buf);
}
static void reg_load(void) {
  int node = vfs_find_node(REG_HIVE_PATH); if (node < 0) return; VNodeInfo vi; if (!vfs_node_info(node, &vi) || vi.is_dir) return; const u8 *d = vfs_node_data(node); if (!d) return; u64 n = vi.size; u64 p = 0; int key = -1;
  while (p < n) { u64 e = p; while (e < n && d[e] != '\n') e++; u64 len = e - p; const char *line = (const char *)d + p; p = e + 1;
    if (len > 2 && line[0] == '[') { char path[300]; u64 l = len - 2 < 299 ? len - 2 : 299; e_memcpy(path, line + 1, (size_t)l); path[l] = 0; key = reg_key_create(path); }
    else if (len > 3 && line[0] == '"' && key >= 0) { char name[128]; u64 q = 1; u64 i = 0; while (q < len && line[q] != '"' && i < 127) name[i++] = line[q++]; name[i] = 0; q += 2; u32 type = 0; while (q < len && line[q] >= '0' && line[q] <= '9') type = type * 10 + (u32)(line[q++] - '0'); q++;
      u32 bl = (u32)((len - q) / 2); u8 *data = emu_alloc(bl ? bl : 1); for (u32 b = 0; b < bl; b++) { u32 v = 0; for (int h = 0; h < 2; h++) { char ch = line[q + b * 2 + h]; v = v * 16 + (u32)(ch >= 'a' ? ch - 'a' + 10 : ch >= 'A' ? ch - 'A' + 10 : ch - '0'); } data[b] = (u8)v; }
      reg_val_set(key, name, type, data, bl); emu_free(data); } }
  g_reg_dirty = 0;
}
static void reg_init(void) {
  if (g_reg_init) return; g_reg_init = 1; reg_load();
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", "Windows 10 Pro"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentVersion", "6.3");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", "19045"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber", "19045");
  reg_seed_dw("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMajorVersionNumber", 10); reg_seed_dw("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMinorVersionNumber", 0);
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "SystemRoot", "C:\\Windows"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "RegisteredOwner", "user");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", "ProgramFilesDir", "C:\\Program Files"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", "CommonFilesDir", "C:\\Program Files\\Common Files");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", "ProgramFilesDir (x86)", "C:\\Program Files (x86)"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", "ProgramW6432Dir", "C:\\Program Files");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "Path", "C:\\Windows\\System32;C:\\Windows"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "TEMP", "C:\\tmp");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "ACP", "1252"); reg_seed_sz("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "OEMCP", "437");
  reg_seed_sz("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName", "ComputerName", "NEBULA");
  reg_key_create("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"); reg_key_create("HKEY_CURRENT_USER\\Environment"); reg_key_create("HKEY_CLASSES_ROOT"); reg_key_create("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
  reg_seed_sz("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", "Desktop", "C:\\Users\\user\\Desktop"); reg_seed_sz("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", "AppData", "C:\\Users\\user\\AppData\\Roaming");
  reg_seed_sz("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", "Personal", "C:\\Users\\user\\Documents"); reg_seed_sz("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", "Local AppData", "C:\\Users\\user\\AppData\\Local");
  g_reg_dirty = 0;
}
/* HKEY → key index (-1 = invalid) */
static int reg_key_of(u64 h) { reg_init(); const char *root = reg_root_name(h); if (root) return reg_key_create(root); if ((u32)(h >> 16) != (REG_HANDLE_TAG >> 16)) return -1; u32 i = (u32)h & 0xffff; if (i >= 256 || !g_rh[i].used) return -1; return g_rh[i].key; }
static u64 reg_handle_new(int key) { for (u32 i = 1; i < 256; i++) if (!g_rh[i].used) { g_rh[i].used = 1; g_rh[i].key = key; return REG_HANDLE_TAG | i; } return 0; }
/* the subkey argument (A or W), joined to the parent path; WOW6432Node is folded away (one view of the registry) */
static void reg_subpath(Cpu *c, int parent, u64 sub, int wide, char *out, size_t cap) {
  char s[300]; s[0] = 0; if (sub) { if (wide) wstr_to_utf8(c, sub, s, sizeof s, -1); else { const char *p = rd_str(c, sub, 299); e_snprintf(s, sizeof s, "%s", p ? p : ""); } }
  size_t l = e_strlen(s); while (l && s[l - 1] == '\\') s[--l] = 0; const char *q = s; while (*q == '\\') q++;
  if (*q) e_snprintf(out, cap, "%s\\%s", g_rkeys[parent].path, q); else e_snprintf(out, cap, "%s", g_rkeys[parent].path);
  /* "SOFTWARE\WOW6432Node\X" → "SOFTWARE\X" */
  char *w6 = 0; for (char *p = out; *p; p++) if ((p[0] == 'W' || p[0] == 'w') && !e_strnicmp(p, "WOW6432Node\\", 12)) { w6 = p; break; }
  if (w6) e_memmove(w6, w6 + 12, e_strlen(w6 + 12) + 1); else { size_t ol = e_strlen(out); if (ol >= 11 && !e_stricmp(out + ol - 11, "WOW6432Node")) out[ol - 12 < ol ? ol - 12 : 0] = 0; }
}
static void reg_open(Cpu *c, u64 hkey, u64 sub, int wide, u64 out, int create, u64 disp_out) {
  int parent = reg_key_of(hkey); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, sub, wide, path, sizeof path);
  int k = reg_key_find(path); int created = 0; if (k < 0) { if (!create) { if (out) wrp(c, out, 0); RET(2); return; } k = reg_key_create(path); created = 1; if (k < 0) { RET(1450); return; } }
  if (disp_out) wr32(c, disp_out, created ? 1 : 2); if (out) wrp(c, out, reg_handle_new(k)); RET(0);
}
static void s_RegOpenKeyExW(Cpu *c) { reg_open(c, ARG(0), ARG(1), 1, ARG(4), 0, 0); }
static void s_RegOpenKeyExA(Cpu *c) { reg_open(c, ARG(0), ARG(1), 0, ARG(4), 0, 0); }
static void s_RegOpenKeyW(Cpu *c) { reg_open(c, ARG(0), ARG(1), 1, ARG(2), 0, 0); }
static void s_RegOpenKeyA(Cpu *c) { reg_open(c, ARG(0), ARG(1), 0, ARG(2), 0, 0); }
static void s_RegCreateKeyExW(Cpu *c) { reg_open(c, ARG(0), ARG(1), 1, ARG(7), 1, ARG(8)); reg_save(); }
static void s_RegCreateKeyExA(Cpu *c) { reg_open(c, ARG(0), ARG(1), 0, ARG(7), 1, ARG(8)); reg_save(); }
static void s_RegCreateKeyW(Cpu *c) { reg_open(c, ARG(0), ARG(1), 1, ARG(2), 1, 0); reg_save(); }
static void s_RegCreateKeyA(Cpu *c) { reg_open(c, ARG(0), ARG(1), 0, ARG(2), 1, 0); reg_save(); }
static void s_RegCloseKey(Cpu *c) { u64 h = ARG(0); if ((u32)(h >> 16) == (REG_HANDLE_TAG >> 16)) { u32 i = (u32)h & 0xffff; if (i < 256) g_rh[i].used = 0; } reg_save(); RET(0); }
static void s_RegFlushKey(Cpu *c) { reg_save(); RET(0); }
static void reg_value_name(Cpu *c, u64 a, int wide, char *out, size_t cap) { out[0] = 0; if (!a) return; if (wide) wstr_to_utf8(c, a, out, cap, -1); else { const char *p = rd_str(c, a, cap - 1); e_snprintf(out, cap, "%s", p ? p : ""); } }
static void reg_set_value(Cpu *c, u64 hkey, u64 name_a, int wide, u32 type, u64 data, u32 len) {
  int k = reg_key_of(hkey); if (k < 0) { RET(6); return; } char name[128]; reg_value_name(c, name_a, wide, name, sizeof name);
  u8 tmp[8192]; const u8 *src = 0; u32 n = len;
  if (!wide && (type == 1 || type == 2 || type == 7)) { /* ANSI string → UTF-16 storage */ char s[4096]; u32 m = len < 4095 ? len : 4095; const u8 *p = mem_ptr(c->mem, data, m, 0); if (!p) { RET(87); return; } e_memcpy(s, p, m); s[m] = 0; n = 0; for (u32 i = 0; i < m && n + 2 < sizeof tmp; i++) { tmp[n++] = s[i]; tmp[n++] = 0; } if (type == 7 || (n && (tmp[n - 2] | tmp[n - 1]))) { tmp[n++] = 0; tmp[n++] = 0; } src = tmp; }
  else { if (n > sizeof tmp) n = sizeof tmp; const u8 *p = mem_ptr(c->mem, data, n, 0); if (n && !p) { RET(87); return; } if (n) e_memcpy(tmp, p, n); src = tmp; }
  reg_val_set(k, name, type, src, n); reg_save(); RET(0);
}
static void s_RegSetValueExW(Cpu *c) { reg_set_value(c, ARG(0), ARG(1), 1, (u32)ARG(3), ARG(4), (u32)ARG(5)); }
static void s_RegSetValueExA(Cpu *c) { reg_set_value(c, ARG(0), ARG(1), 0, (u32)ARG(3), ARG(4), (u32)ARG(5)); }
static void s_RegSetValueW(Cpu *c) { /* (hkey, subkey, REG_SZ, data, cb): default value of the subkey */ int parent = reg_key_of(ARG(0)); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, ARG(1), 1, path, sizeof path); int k = reg_key_create(path); u16 tmp[2048]; u32 n = 0; for (;; n++) { u16 ch = rd16(c, ARG(3) + n * 2); if (n >= 2047) break; tmp[n] = ch; if (!ch) { n++; break; } } reg_val_set(k, "", 1, (const u8 *)tmp, n * 2); reg_save(); RET(0); }
static void s_RegSetValueA(Cpu *c) { int parent = reg_key_of(ARG(0)); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, ARG(1), 0, path, sizeof path); int k = reg_key_create(path); const char *s = rd_str(c, ARG(3), 2047); u16 tmp[2048]; u32 n = 0; if (s) for (; s[n] && n < 2047; n++) tmp[n] = (u8)s[n]; tmp[n++] = 0; reg_val_set(k, "", 1, (const u8 *)tmp, n * 2); reg_save(); RET(0); }
/* RegQueryValueEx(hkey, name, reserved, *type, data, *len) with the classic size protocol (len out = needed; ERROR_MORE_DATA when too small) */
static void reg_query_value(Cpu *c, u64 hkey, u64 name_a, int wide, u64 type_out, u64 data, u64 len_p) {
  int k = reg_key_of(hkey); if (k < 0) { RET(6); return; } char name[128]; reg_value_name(c, name_a, wide, name, sizeof name); int v = reg_val_find(k, name); if (v < 0) { RET(2); return; }
  RegVal *rv = &g_rvals[v]; u8 conv[8192]; const u8 *src = rv->data; u32 n = rv->len;
  if (!wide && (rv->type == 1 || rv->type == 2 || rv->type == 7)) { n = 0; for (u32 i = 0; i + 1 < rv->len && n < sizeof conv; i += 2) { u16 ch = (u16)(rv->data[i] | rv->data[i + 1] << 8); conv[n++] = ch < 256 ? (u8)ch : '?'; } src = conv; }
  if (type_out) wr32(c, type_out, rv->type);
  if (!data) { if (len_p) wr32(c, len_p, n); RET(0); return; }
  u32 cap = len_p ? rd32(c, len_p) : 0; if (len_p) wr32(c, len_p, n); if (cap < n) { RET(234); return; }
  u8 *p = mem_ptr(c->mem, data, n, 1); if (n && !p) { RET(87); return; } if (n) e_memcpy(p, src, n); RET(0);
}
static void s_RegQueryValueExW(Cpu *c) { reg_query_value(c, ARG(0), ARG(1), 1, ARG(3), ARG(4), ARG(5)); }
static void s_RegQueryValueExA(Cpu *c) { reg_query_value(c, ARG(0), ARG(1), 0, ARG(3), ARG(4), ARG(5)); }
static void s_RegQueryValueW(Cpu *c) { /* (hkey, subkey, data, *len): default value */ int parent = reg_key_of(ARG(0)); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, ARG(1), 1, path, sizeof path); int k = reg_key_find(path); if (k < 0) { RET(2); return; } int v = reg_val_find(k, ""); u32 n = v >= 0 ? g_rvals[v].len : 2; u32 cap = ARG(3) ? rd32(c, ARG(3)) : 0; if (ARG(3)) wr32(c, ARG(3), n); if (!ARG(2)) { RET(0); return; } if (cap < n) { RET(234); return; } u8 *p = mem_ptr(c->mem, ARG(2), n, 1); if (p) { if (v >= 0) e_memcpy(p, g_rvals[v].data, n); else { p[0] = p[1] = 0; } } RET(0); }
/* RegGetValue(hkey, subkey, value, flags, *type, data, *len): RRF_RT_* filtering ignored, REG_EXPAND_SZ expanded unless RRF_NOEXPAND */
static void s_RegGetValueW(Cpu *c) { int parent = reg_key_of(ARG(0)); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, ARG(1), 1, path, sizeof path); int k = reg_key_find(path); if (k < 0) { RET(2); return; }
  char name[128]; reg_value_name(c, ARG(2), 1, name, sizeof name); int v = reg_val_find(k, name); if (v < 0) { RET(2); return; } RegVal *rv = &g_rvals[v];
  if (ARG(4)) wr32(c, ARG(4), rv->type); u32 n = rv->len; if (!ARG(5)) { if (ARG(6)) wr32(c, ARG(6), n); RET(0); return; } u32 cap = ARG(6) ? rd32(c, ARG(6)) : 0; if (ARG(6)) wr32(c, ARG(6), n); if (cap < n) { RET(234); return; } u8 *p = mem_ptr(c->mem, ARG(5), n, 1); if (p && n) e_memcpy(p, rv->data, n); RET(0); }
static void s_RegGetValueA(Cpu *c) { int parent = reg_key_of(ARG(0)); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, ARG(1), 0, path, sizeof path); int k = reg_key_find(path); if (k < 0) { RET(2); return; } u64 tmp = w32_heap_alloc(W, 8, 1); wrp(c, tmp, reg_handle_new(k)); u64 h = rdp(c, tmp); w32_heap_free(W, tmp); reg_query_value(c, h, ARG(2), 0, ARG(4), ARG(5), ARG(6)); call_shim(c, s_RegCloseKey, h, 0, 0, 0); }
static void reg_delete_key(Cpu *c, u64 hkey, u64 sub, int wide) { int parent = reg_key_of(hkey); if (parent < 0) { RET(6); return; } char path[300]; reg_subpath(c, parent, sub, wide, path, sizeof path); int k = reg_key_find(path); if (k < 0) { RET(2); return; }
  size_t pl = e_strlen(path); for (int i = 0; i < REG_MAX_KEYS; i++) if (g_rkeys[i].used && (i == k || (!e_strnicmp(g_rkeys[i].path, path, pl) && g_rkeys[i].path[pl] == '\\'))) { g_rkeys[i].used = 0; for (int v = 0; v < REG_MAX_VALS; v++) if (g_rvals[v].used && g_rvals[v].key == i) { g_rvals[v].used = 0; if (g_rvals[v].data) { emu_free(g_rvals[v].data); g_rvals[v].data = 0; } } }
  g_reg_dirty = 1; reg_save(); RET(0); }
static void s_RegDeleteKeyW(Cpu *c) { reg_delete_key(c, ARG(0), ARG(1), 1); }
static void s_RegDeleteKeyA(Cpu *c) { reg_delete_key(c, ARG(0), ARG(1), 0); }
static void s_RegDeleteKeyExW(Cpu *c) { reg_delete_key(c, ARG(0), ARG(1), 1); }
static void s_RegDeleteKeyExA(Cpu *c) { reg_delete_key(c, ARG(0), ARG(1), 0); }
static void s_RegDeleteTreeW(Cpu *c) { if (!ARG(1)) { int k = reg_key_of(ARG(0)); if (k < 0) { RET(6); return; } for (int v = 0; v < REG_MAX_VALS; v++) if (g_rvals[v].used && g_rvals[v].key == k) g_rvals[v].used = 0; size_t pl = e_strlen(g_rkeys[k].path); for (int i = 0; i < REG_MAX_KEYS; i++) if (g_rkeys[i].used && i != k && !e_strnicmp(g_rkeys[i].path, g_rkeys[k].path, pl) && g_rkeys[i].path[pl] == '\\') g_rkeys[i].used = 0; g_reg_dirty = 1; reg_save(); RET(0); return; } reg_delete_key(c, ARG(0), ARG(1), 1); }
static void reg_delete_value(Cpu *c, u64 hkey, u64 name_a, int wide) { int k = reg_key_of(hkey); if (k < 0) { RET(6); return; } char name[128]; reg_value_name(c, name_a, wide, name, sizeof name); int v = reg_val_find(k, name); if (v < 0) { RET(2); return; } g_rvals[v].used = 0; g_reg_dirty = 1; reg_save(); RET(0); }
static void s_RegDeleteValueW(Cpu *c) { reg_delete_value(c, ARG(0), ARG(1), 1); }
static void s_RegDeleteValueA(Cpu *c) { reg_delete_value(c, ARG(0), ARG(1), 0); }
/* enumeration: subkeys are keys whose path is parent + "\" + one component */
static int reg_nth_subkey(int k, u32 n, char *name, size_t cap) { size_t pl = e_strlen(g_rkeys[k].path); u32 seen = 0; for (int i = 0; i < REG_MAX_KEYS; i++) if (g_rkeys[i].used && i != k && !e_strnicmp(g_rkeys[i].path, g_rkeys[k].path, pl) && g_rkeys[i].path[pl] == '\\') { const char *rest = g_rkeys[i].path + pl + 1; int has_sep = 0; for (const char *p = rest; *p; p++) if (*p == '\\') { has_sep = 1; break; } if (has_sep) continue; if (seen++ == n) { e_snprintf(name, cap, "%s", rest); return 1; } } return 0; }
static int reg_nth_value(int k, u32 n) { u32 seen = 0; for (int v = 0; v < REG_MAX_VALS; v++) if (g_rvals[v].used && g_rvals[v].key == k) { if (seen++ == n) return v; } return -1; }
static void reg_enum_key(Cpu *c, int wide) { int k = reg_key_of(ARG(0)); if (k < 0) { RET(6); return; } char name[300]; if (!reg_nth_subkey(k, (u32)ARG(1), name, sizeof name)) { RET(259); return; }
  u64 buf = ARG(2), lenp = ARG(3); u32 cap = lenp ? rd32(c, lenp) : 0; u32 need = wide ? utf8_u16len(name) : (u32)e_strlen(name); if (cap <= need) { if (lenp) wr32(c, lenp, need + 1); RET(234); return; }
  if (wide) put_w8(c, buf, name, cap); else put_a(c, buf, name, cap); if (lenp) wr32(c, lenp, need); if (ARG(5)) wr32(c, ARG(5), 0); RET(0); }
static void s_RegEnumKeyExW(Cpu *c) { reg_enum_key(c, 1); }
static void s_RegEnumKeyExA(Cpu *c) { reg_enum_key(c, 0); }
static void s_RegEnumKeyW(Cpu *c) { int k = reg_key_of(ARG(0)); if (k < 0) { RET(6); return; } char name[300]; if (!reg_nth_subkey(k, (u32)ARG(1), name, sizeof name)) { RET(259); return; } put_w8(c, ARG(2), name, (u32)ARG(3)); RET(0); }
static void reg_enum_value(Cpu *c, int wide) { int k = reg_key_of(ARG(0)); if (k < 0) { RET(6); return; } int v = reg_nth_value(k, (u32)ARG(1)); if (v < 0) { RET(259); return; } RegVal *rv = &g_rvals[v];
  u64 nbuf = ARG(2), nlenp = ARG(3); u32 ncap = nlenp ? rd32(c, nlenp) : 0; u32 need = wide ? utf8_u16len(rv->name) : (u32)e_strlen(rv->name); if (ncap <= need) { if (nlenp) wr32(c, nlenp, need + 1); RET(234); return; }
  if (wide) put_w8(c, nbuf, rv->name, ncap); else put_a(c, nbuf, rv->name, ncap); if (nlenp) wr32(c, nlenp, need);
  if (ARG(5)) wr32(c, ARG(5), rv->type); u64 data = ARG(6), lenp = ARG(7); if (lenp) { u32 cap = rd32(c, lenp); wr32(c, lenp, rv->len); if (data) { if (cap < rv->len) { RET(234); return; } u8 *p = mem_ptr(c->mem, data, rv->len, 1); if (p && rv->len) e_memcpy(p, rv->data, rv->len); } } RET(0); }
static void s_RegEnumValueW(Cpu *c) { reg_enum_value(c, 1); }
static void s_RegEnumValueA(Cpu *c) { reg_enum_value(c, 0); }
/* RegQueryInfoKey(hkey, class, *classlen, reserved, *subkeys, *maxsubkeylen, *maxclasslen, *values, *maxvaluenamelen, *maxvaluelen, *security, *lastwrite) */
static void s_RegQueryInfoKeyW(Cpu *c) { int k = reg_key_of(ARG(0)); if (k < 0) { RET(6); return; } u32 nsub = 0, maxsub = 0, nval = 0, maxname = 0, maxlen = 0; char name[300]; while (reg_nth_subkey(k, nsub, name, sizeof name)) { u32 l = utf8_u16len(name); if (l > maxsub) maxsub = l; nsub++; }
  for (int v = 0; v < REG_MAX_VALS; v++) if (g_rvals[v].used && g_rvals[v].key == k) { nval++; u32 l = utf8_u16len(g_rvals[v].name); if (l > maxname) maxname = l; if (g_rvals[v].len > maxlen) maxlen = g_rvals[v].len; }
  if (ARG(2)) wr32(c, ARG(2), 0); if (ARG(4)) wr32(c, ARG(4), nsub); if (ARG(5)) wr32(c, ARG(5), maxsub); if (ARG(6)) wr32(c, ARG(6), 0); if (ARG(7)) wr32(c, ARG(7), nval); if (ARG(8)) wr32(c, ARG(8), maxname); if (ARG(9)) wr32(c, ARG(9), maxlen); if (ARG(10)) wr32(c, ARG(10), 0); if (ARG(11)) { wr32(c, ARG(11), 0); wr32(c, ARG(11) + 4, 0); } RET(0); }
static void s_RegQueryInfoKeyA(Cpu *c) { s_RegQueryInfoKeyW(c); }
static void s_RegNotifyChangeKeyValue(Cpu *c) { RET(0); }
static void s_RegOverridePredefKey(Cpu *c) { RET(0); }
static void s_RegLoadMUIStringW(Cpu *c) { RET(2); }
static void s_RegConnectRegistryW(Cpu *c) { if (ARG(2)) wrp(c, ARG(2), ARG(1)); RET(0); }

/* ---------- shell folders ---------- */
static const char *csidl_path(u32 csidl) {
  switch (csidl & 0xff) {
    case 0x00: return "C:\\Users\\user\\Desktop"; case 0x02: return "C:\\Users\\user\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs"; case 0x05: return "C:\\Users\\user\\Documents";
    case 0x06: return "C:\\Users\\user\\Favorites"; case 0x07: return "C:\\Users\\user\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"; case 0x08: return "C:\\Users\\user\\Recent";
    case 0x09: return "C:\\Users\\user\\SendTo"; case 0x0b: return "C:\\Users\\user\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"; case 0x0d: return "C:\\Users\\user\\Music"; case 0x0e: return "C:\\Users\\user\\Videos";
    case 0x10: return "C:\\Users\\user\\Desktop"; case 0x13: return "C:\\Users\\user\\NetHood"; case 0x14: return "C:\\Windows\\Fonts"; case 0x15: return "C:\\Users\\user\\Templates";
    case 0x16: return "C:\\ProgramData\\Microsoft\\Windows\\Start Menu"; case 0x17: return "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"; case 0x18: return "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    case 0x19: return "C:\\Users\\Public\\Desktop"; case 0x1a: return "C:\\Users\\user\\AppData\\Roaming"; case 0x1b: return "C:\\Users\\user\\PrintHood"; case 0x1c: return "C:\\Users\\user\\AppData\\Local";
    case 0x1f: return "C:\\Users\\user\\Favorites"; case 0x20: return "C:\\Users\\user\\AppData\\Local\\Microsoft\\Windows\\INetCache"; case 0x21: return "C:\\Users\\user\\AppData\\Local\\Microsoft\\Windows\\INetCookies"; case 0x22: return "C:\\Users\\user\\AppData\\Local\\Microsoft\\Windows\\History";
    case 0x23: return "C:\\ProgramData"; case 0x24: return "C:\\Windows"; case 0x25: return "C:\\Windows\\System32"; case 0x26: return "C:\\Program Files"; case 0x27: return "C:\\Users\\user\\Pictures"; case 0x28: return "C:\\Users\\user";
    case 0x29: return "C:\\Windows\\System32"; case 0x2a: return "C:\\Program Files (x86)"; case 0x2b: return "C:\\Program Files\\Common Files"; case 0x2c: return "C:\\Program Files (x86)\\Common Files"; case 0x2d: return "C:\\ProgramData\\Microsoft\\Windows\\Templates";
    case 0x2e: return "C:\\Users\\Public\\Documents"; case 0x2f: return "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools"; case 0x30: return "C:\\Users\\user\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools";
    case 0x35: return "C:\\Users\\Public\\Music"; case 0x36: return "C:\\Users\\Public\\Pictures"; case 0x37: return "C:\\Users\\Public\\Videos"; case 0x38: return "C:\\Windows\\Resources"; case 0x3b: return "C:\\Users\\user\\AppData\\Local\\Microsoft\\Windows\\Burn\\Burn";
    default: return 0;
  }
}
void shell_mkdirs(const char *path) { char tmp[520]; e_snprintf(tmp, sizeof tmp, "%s", path); for (size_t i = 3; tmp[i]; i++) if (tmp[i] == '\\') { tmp[i] = 0; vfs_mkdir(tmp); tmp[i] = '\\'; } vfs_mkdir(tmp); }
/* SHGetFolderPath(hwnd, csidl, token, flags, out[MAX_PATH]) */
static void sh_get_folder(Cpu *c, u32 csidl, u64 out, int wide) { const char *p = csidl_path(csidl); if (!p) { RET(0x80070057u); return; } if (csidl & 0x8000) shell_mkdirs(p); if (out) { if (wide) put_w8(c, out, p, 260); else put_a(c, out, p, 260); } RET(0); }
static void s_SHGetFolderPathW(Cpu *c) { sh_get_folder(c, (u32)ARG(1), ARG(4), 1); }
static void s_SHGetFolderPathA(Cpu *c) { sh_get_folder(c, (u32)ARG(1), ARG(4), 0); }
static void s_SHGetFolderPathAndSubDirW(Cpu *c) { const char *p = csidl_path((u32)ARG(1)); if (!p) { RET(0x80070057u); return; } char full[520]; char sub[260]; sub[0] = 0; if (ARG(4)) wstr_to_utf8(c, ARG(4), sub, sizeof sub, -1); if (sub[0]) e_snprintf(full, sizeof full, "%s\\%s", p, sub); else e_snprintf(full, sizeof full, "%s", p); shell_mkdirs(full); put_w8(c, ARG(5), full, 260); RET(0); }
static void s_SHGetSpecialFolderPathW(Cpu *c) { const char *p = csidl_path((u32)ARG(2)); if (!p) { RET(0); return; } if (ARG(3)) shell_mkdirs(p); put_w8(c, ARG(1), p, 260); RET(1); }
static void s_SHGetSpecialFolderPathA(Cpu *c) { const char *p = csidl_path((u32)ARG(2)); if (!p) { RET(0); return; } if (ARG(3)) shell_mkdirs(p); put_a(c, ARG(1), p, 260); RET(1); }
/* KNOWNFOLDERID → CSIDL (the GUIDs' first 32 bits are distinct for everything we support) */
static int kf_csidl(u32 d1) { switch (d1) {
  case 0xB4BFCC3Au: return 0x00; case 0xA77F5D77u: return 0x02; case 0xFDD39AD0u: return 0x05; case 0x1777F761u: return 0x06; case 0xB97D20BBu: return 0x07; case 0xAE50C081u: return 0x08; case 0x8983036Cu: return 0x09; case 0x625B53C3u: return 0x0b;
  case 0x4BD8D571u: return 0x0d; case 0x18989B1Du: return 0x0e; case 0xFD228CB7u: return 0x14; case 0xA63293E8u: return 0x15; case 0xA4115719u: return 0x16; case 0x0139D44Eu: return 0x17; case 0x82A5EA35u: return 0x18; case 0xC4AA340Du: return 0x19;
  case 0x3EB685DBu: return 0x1a; case 0xF1B32785u: return 0x1c; case 0x352481E8u: return 0x20; case 0x2B0F765Du: return 0x21; case 0xD9DC8A3Bu: return 0x22; case 0x62AB5D82u: return 0x23; case 0xF38BF404u: return 0x24; case 0x1AC14E77u: return 0x25;
  case 0x905E63B6u: return 0x26; case 0x6D809377u: return 0x26; case 0x33E28130u: return 0x27; case 0x5E6C858Fu: return 0x28; case 0xD65231B0u: return 0x29; case 0x7C5A40EFu: return 0x2a; case 0xF7F1ED05u: return 0x2b; case 0xDE974D24u: return 0x2c;
  case 0xB94237E7u: return 0x2d; case 0xED4824AFu: return 0x2e; case 0xD0384E7Du: return 0x2f; case 0x724EF170u: return 0x30; case 0x3214FAB5u: return 0x35; case 0xB6EBFB86u: return 0x36; case 0x2400183Au: return 0x37; case 0x8AD10C31u: return 0x38;
  case 0x5CD7AEE2u: return 0x05; /* UserProgramFiles → mapped below */ case 0xBCBD3057u: return 0x05; /* UserProgramFilesCommon */ case 0x374DE290u: return 0x05; /* Downloads → Documents\..: map to a real folder below */ case 0xA520A1A4u: return 0x1c; /* LocalAppDataLow */ case 0xDFDF76A2u: return 0x19; /* Public */ case 0x0AC0837Cu: return 0x28; /* Computer → profile (never a path) */
  default: return -1; } }
static void s_SHGetKnownFolderPath(Cpu *c) { u32 d1 = rd32(c, ARG(0)); int csidl = kf_csidl(d1); if (ARG(3)) wrp(c, ARG(3), 0); if (csidl < 0) { RET(0x80070002u); return; }
  const char *p = d1 == 0x374DE290u ? "C:\\Users\\user\\Downloads" : d1 == 0x5CD7AEE2u ? "C:\\Users\\user\\AppData\\Local\\Programs" : d1 == 0xBCBD3057u ? "C:\\Users\\user\\AppData\\Local\\Programs\\Common" : csidl_path((u32)csidl); if (!p) { RET(0x80070002u); return; } if ((u32)ARG(1) & 0x8000) shell_mkdirs(p);
  u64 mem = w32_heap_alloc(W, (e_strlen(p) + 1) * 2, 1); put_w8(c, mem, p, 0); if (ARG(3)) wrp(c, ARG(3), mem); RET(0); }
/* PIDLs: an opaque heap block = { u16 cb; char path[]; u16 0 } — enough for SHGetPathFromIDList / ILFree / CoTaskMemFree round trips */
u64 pidl_make(Win32 *w, const char *path) { size_t l = e_strlen(path); u64 p = w32_heap_alloc(w, l + 6, 1); Cpu *c = w->cpu; wr16(c, p, (u16)(l + 3)); u8 *d = mem_ptr(w->mem, p + 2, l + 1, 1); if (d) { e_memcpy(d, path, l + 1); } wr16(c, p + 2 + l + 1 + 1, 0); return p; }
const char *pidl_path(Cpu *c, u64 pidl) { if (!pidl) return ""; const char *s = rd_str(c, pidl + 2, 1000); return s ? s : ""; }
static void s_SHGetPathFromIDListW(Cpu *c) { const char *p = pidl_path(c, ARG(0)); if (!p[0]) { RET(0); return; } put_w8(c, ARG(1), p, 260); RET(1); }
static void s_SHGetPathFromIDListA(Cpu *c) { const char *p = pidl_path(c, ARG(0)); if (!p[0]) { RET(0); return; } put_a(c, ARG(1), p, 260); RET(1); }
static void s_SHGetPathFromIDListEx(Cpu *c) { const char *p = pidl_path(c, ARG(0)); if (!p[0]) { RET(0); return; } put_w8(c, ARG(1), p, (u32)ARG(2)); RET(1); }
static void s_SHGetSpecialFolderLocation(Cpu *c) { const char *p = csidl_path((u32)ARG(1)); if (!p) { RET(0x80070057u); return; } wrp(c, ARG(2), pidl_make(W, p)); RET(0); }
static void s_SHGetFolderLocation(Cpu *c) { const char *p = csidl_path((u32)ARG(1)); if (!p) { RET(0x80070057u); return; } wrp(c, ARG(4), pidl_make(W, p)); RET(0); }
static void s_SHParseDisplayName(Cpu *c) { char path[520]; wstr_to_utf8(c, ARG(0), path, sizeof path, -1); wrp(c, ARG(2), pidl_make(W, path)); if (ARG(4)) wr32(c, ARG(4), 0); RET(0); }
static void s_ILCreateFromPathW(Cpu *c) { char path[520]; wstr_to_utf8(c, ARG(0), path, sizeof path, -1); RET(pidl_make(W, path)); }
static void s_ILFree(Cpu *c) { if (ARG(0)) w32_heap_free(W, ARG(0)); }
static void s_ILClone(Cpu *c) { RET(pidl_make(W, pidl_path(c, ARG(0)))); }
static void s_ILIsEqual(Cpu *c) { RET(!e_stricmp(pidl_path(c, ARG(0)), pidl_path(c, ARG(1)))); }
/* IMalloc over the process heap: a real COM vtable whose slots are named shims (32-bit callers get stdcall cleanup from sig32_tab) */
static void s_IMalloc_QueryInterface(Cpu *c) { wrp(c, ARG(2), ARG(0)); RET(0); }
static void s_IMalloc_AddRef(Cpu *c) { RET(2); }
static void s_IMalloc_Release(Cpu *c) { RET(1); }
static void s_IMalloc_Alloc(Cpu *c) { RET(w32_heap_alloc(W, ARG(1) ? ARG(1) : 1, 0)); }
static void s_IMalloc_Realloc(Cpu *c) { RET(heap_realloc(W, ARG(1), ARG(2) ? ARG(2) : 1, 0)); }
static void s_IMalloc_Free(Cpu *c) { if (ARG(1)) w32_heap_free(W, ARG(1)); RET(0); }
static void s_IMalloc_GetSize(Cpu *c) { RET(w32_heap_size(W, ARG(1))); }
static void s_IMalloc_DidAlloc(Cpu *c) { RET(w32_heap_size(W, ARG(1)) ? 1 : 0); }
static void s_IMalloc_HeapMinimize(Cpu *c) { (void)c; }
static u64 shell_imalloc(Cpu *c) { Win32 *w = W; static u64 obj; static Win32 *owner; if (obj && owner == w) return obj; u64 P = PSZ_OF(c);
  static const char *names[] = { "IMalloc::QueryInterface", "IMalloc::AddRef", "IMalloc::Release", "IMalloc::Alloc", "IMalloc::Realloc", "IMalloc::Free", "IMalloc::GetSize", "IMalloc::DidAlloc", "IMalloc::HeapMinimize" };
  u64 vt = w32_heap_alloc(w, 9 * P, 1); for (int i = 0; i < 9; i++) wrp(c, vt + (u64)i * P, w32_shim_slot(w, "shell32", names[i])); obj = w32_heap_alloc(w, P * 2, 1); wrp(c, obj, vt); owner = w; return obj; }
static void s_SHGetMalloc(Cpu *c) { wrp(c, ARG(0), shell_imalloc(c)); RET(0); }
static void s_CoGetMalloc(Cpu *c) { wrp(c, ARG(1), shell_imalloc(c)); RET(0); }
static void s_SHCreateDirectoryExW(Cpu *c) { char path[520]; wstr_to_utf8(c, ARG(1), path, sizeof path, -1); u64 sz; if (vfs_stat(path, &sz)) { RET(183); return; } shell_mkdirs(path); RET(vfs_stat(path, &sz) ? 0 : 3); }
static void s_SHCreateDirectoryExA(Cpu *c) { const char *s = rd_str(c, ARG(1), 519); char path[520]; e_snprintf(path, sizeof path, "%s", s ? s : ""); u64 sz; if (vfs_stat(path, &sz)) { RET(183); return; } shell_mkdirs(path); RET(vfs_stat(path, &sz) ? 0 : 3); }
static void s_SHChangeNotify(Cpu *c) { (void)c; }
static void s_SHAddToRecentDocs(Cpu *c) { (void)c; }
static void s_IsUserAnAdmin(Cpu *c) { RET(1); }
static void s_SHGetFileInfoW(Cpu *c) { /* (path, attrs, SHFILEINFO*, cb, flags): type name + display name only */ u64 fi = ARG(2); u32 flags = (u32)ARG(4); if (!fi) { RET(0); return; } char path[520]; if ((flags & 0x10) == 0 && ARG(0) >= 0x10000) wstr_to_utf8(c, ARG(0), path, sizeof path, -1); else path[0] = 0;
  u64 attr_off = c->mode32 ? 8 : 12, disp_off = c->mode32 ? 12 : 16, type_off = disp_off + 520; wrp(c, fi, 0); wr32(c, fi + (c->mode32 ? 4 : 8), 0); int is_dir = 0; u32 attrs = 0x80; int n = vfs_find_node(path); if (n >= 0) { VNodeInfo vi; if (vfs_node_info(n, &vi)) { is_dir = vi.is_dir; attrs = is_dir ? 0x10 : 0x20; } }
  wr32(c, fi + attr_off, attrs); const char *bn = path; for (const char *p = path; *p; p++) if (*p == '\\') bn = p + 1; put_w8(c, fi + disp_off, bn, 260); put_w8(c, fi + type_off, is_dir ? "File folder" : "File", 80); RET(1); }
static void s_SHGetStockIconInfo(Cpu *c) { RET(0x80004001u); }
static void s_Shell_NotifyIconW(Cpu *c) { RET(1); }
static void s_Shell_NotifyIconA(Cpu *c) { RET(1); }
static void s_SHAppBarMessage(Cpu *c) { RET(0); }
static void s_SHEmptyRecycleBinW(Cpu *c) { RET(0); }
static void s_ExtractIconW(Cpu *c) { RET(0); }
static void s_ExtractIconExW(Cpu *c) { if (ARG(2)) wrp(c, ARG(2), 0); if (ARG(3)) wrp(c, ARG(3), 0); RET(0); }
static void s_DragAcceptFiles(Cpu *c) { (void)c; }
static void s_DragQueryFileW(Cpu *c) { RET(0); }
static void s_DragFinish(Cpu *c) { (void)c; }
/* SHFileOperation: FO_DELETE (3) of one path or a double-NUL list; FO_COPY/FO_MOVE of single files */
static void s_SHFileOperationW(Cpu *c) { u64 op = ARG(0); u32 func = rd32(c, op + PSZ_OF(c)); u64 from = rdp(c, op + PSZ_OF(c) * 2), to = rdp(c, op + PSZ_OF(c) * 3); int rc = 0;
  for (u64 p = from; p && rd16(c, p);) { char path[520]; int n = wstr_to_utf8(c, p, path, sizeof path, -1); (void)n; u64 l = 0; while (rd16(c, p + l * 2)) l++; p += (l + 1) * 2;
    if (func == 3) { /* delete, recursively */ for (int guard = 0; guard < 10000; guard++) { int h = vfs_find_first(path); char sub[520]; u64 sz; int is_dir; int found = 0; char pat[530]; e_snprintf(pat, sizeof pat, "%s\\*", path); vfs_find_close(h); h = vfs_find_first(pat);
        while (h >= 0 && vfs_find_next(h, sub, sizeof sub, &sz, &is_dir)) { if (!e_strcmp(sub, ".") || !e_strcmp(sub, "..")) continue; char full[1100]; e_snprintf(full, sizeof full, "%s\\%s", path, sub); if (is_dir) { /* recurse by renaming the loop target */ vfs_remove(full); } else vfs_remove(full); found = 1; } if (h >= 0) vfs_find_close(h); if (!found) break; }
      if (!vfs_remove(path)) rc = 2; }
    else if (func == 2 || func == 1) { char dst[520]; wstr_to_utf8(c, to, dst, sizeof dst, -1); if (func == 1) { if (!vfs_rename(path, dst)) rc = 2; } else { int n2 = vfs_find_node(path); if (n2 < 0) { rc = 2; continue; } VNodeInfo vi; vfs_node_info(n2, &vi); const u8 *d = vfs_node_data(n2); if (!vfs_add(dst, d, vi.size)) rc = 2; } }
    else rc = 0x78; }
  if (ARG(0)) wr32(c, op + PSZ_OF(c) * 4 + 4, 0); RET(rc); }
/* ShellExecute: "open" an .exe → CreateProcess; anything else (documents, URLs, folders) is reported honestly */
static void s_CreateProcessW(Cpu *c);
static void shell_execute(Cpu *c, u64 verb_a, u64 file_a, u64 params_a, u64 dir_a, int wide, u64 *hproc_out) {
  char verb[64], file[520], params[1024]; verb[0] = file[0] = params[0] = 0;
  if (wide) { if (verb_a) wstr_to_utf8(c, verb_a, verb, sizeof verb, -1); if (file_a) wstr_to_utf8(c, file_a, file, sizeof file, -1); if (params_a) wstr_to_utf8(c, params_a, params, sizeof params, -1); }
  else { const char *s; if (verb_a && (s = rd_str(c, verb_a, 63))) e_snprintf(verb, sizeof verb, "%s", s); if (file_a && (s = rd_str(c, file_a, 519))) e_snprintf(file, sizeof file, "%s", s); if (params_a && (s = rd_str(c, params_a, 1023))) e_snprintf(params, sizeof params, "%s", s); }
  (void)dir_a; size_t fl = e_strlen(file); int is_exe = fl > 4 && (!e_stricmp(file + fl - 4, ".exe") || !e_stricmp(file + fl - 4, ".bat") || !e_stricmp(file + fl - 4, ".cmd"));
  if (hproc_out) *hproc_out = 0;
  if (!is_exe && fl > 0) { /* any PE image counts as a program regardless of its extension (Inno Setup re-launches its own "setup.tmp") */ int fd = vfs_open(file, 1, 3, 0); if (fd >= 0) { u8 mz[2] = { 0, 0 }; if (vfs_read(fd, mz, 2) == 2 && mz[0] == 'M' && mz[1] == 'Z') is_exe = 1; vfs_close(fd); } }
  if (is_exe && (!verb[0] || !e_stricmp(verb, "open") || !e_stricmp(verb, "runas"))) {
    char cmd[1600]; e_snprintf(cmd, sizeof cmd, "\"%s\" %s", file, params); u64 cl = w32_heap_alloc(W, (e_strlen(cmd) + 1) * 2, 1); put_w8(c, cl, cmd, 0);
    u64 si = w32_heap_alloc(W, 0x80, 1), pi = w32_heap_alloc(W, 0x20, 1); wr32(c, si, c->mode32 ? 68 : 104);
    u64 sp0 = c->r[RSP]; if (c->mode32) { /* 10 stack args */ for (int i = 9; i >= 4; i--) { u64 v = i == 4 ? 0 : i == 5 ? 0 : i == 6 ? 0 : i == 7 ? 0 : i == 8 ? si : pi; cpu_push(c, v); } cpu_push(c, 0); cpu_push(c, 0); cpu_push(c, cl); cpu_push(c, 0); cpu_push(c, 0); }
    else { c->r[RSP] -= 0x60; wr64(c, c->r[RSP] + 0x28, 0); wr64(c, c->r[RSP] + 0x30, 0); wr64(c, c->r[RSP] + 0x38, 0); wr64(c, c->r[RSP] + 0x40, 0); wr64(c, c->r[RSP] + 0x48, si); wr64(c, c->r[RSP] + 0x50, pi); c->r[RCX] = 0; c->r[RDX] = cl; c->r[R8] = 0; c->r[R9] = 0; }
    s_CreateProcessW(c); u64 ok = c->r[RAX]; c->r[RSP] = sp0; if (hproc_out) *hproc_out = ok ? rdp(c, pi) : 0; w32_heap_free(W, cl);
    if (ok) { w32_report(W, "ShellExecute: started %s", file); RET(42); } else { W->last_error = 2; RET(2); } return; }
  w32_report(W, "ShellExecute(%s, %s): no shell associations here — only .exe files can be started", verb[0] ? verb : "open", file); e_log("[shell] ShellExecute(%s, %s) not available\n", verb[0] ? verb : "open", file); W->last_error = 1155; RET(31);   /* SE_ERR_NOASSOC */
}
static void s_ShellExecuteW(Cpu *c) { shell_execute(c, ARG(1), ARG(2), ARG(3), ARG(4), 1, 0); }
static void s_ShellExecuteA(Cpu *c) { shell_execute(c, ARG(1), ARG(2), ARG(3), ARG(4), 0, 0); }
/* SHELLEXECUTEINFO: cbSize@0 fMask@4 hwnd@8 verb@P file@P+P params parent dir show@… hInstApp … hProcess (last) */
static void s_ShellExecuteExW(Cpu *c) { u64 e = ARG(0); u64 P = PSZ_OF(c); u64 verb = rdp(c, e + 8 + P), file = rdp(c, e + 8 + 2 * P), params = rdp(c, e + 8 + 3 * P), dir = rdp(c, e + 8 + 4 * P); u64 hp; shell_execute(c, verb, file, params, dir, 1, &hp);
  u64 inst_off = 8 + 5 * P + 4; if (c->mode32) inst_off = 8 + 5 * 4 + 4; u64 r = c->r[RAX]; wrp(c, e + inst_off, r); u32 mask = rd32(c, e + 4); if (mask & 0x40) { u64 hproc_off = c->mode32 ? 60 : 104; wrp(c, e + hproc_off, hp); } RET(r > 32); }
static void s_ShellExecuteExA(Cpu *c) { u64 e = ARG(0); u64 P = PSZ_OF(c); u64 verb = rdp(c, e + 8 + P), file = rdp(c, e + 8 + 2 * P), params = rdp(c, e + 8 + 3 * P), dir = rdp(c, e + 8 + 4 * P); u64 hp; shell_execute(c, verb, file, params, dir, 0, &hp); u64 r = c->r[RAX]; wrp(c, e + (c->mode32 ? 32 : 52), r); if (rd32(c, e + 4) & 0x40) wrp(c, e + (c->mode32 ? 60 : 104), hp); RET(r > 32); }
static void s_FindExecutableW(Cpu *c) { u64 sz; char f[520]; wstr_to_utf8(c, ARG(0), f, sizeof f, -1); if (!vfs_stat(f, &sz)) { RET(2); return; } put_w8(c, ARG(2), f, 260); RET(33); }
static void s_ExitWindowsEx(Cpu *c) { w32_report(W, "ExitWindowsEx(0x%x): a reboot/logoff was requested — not possible inside the emulator, the program continues", (u32)ARG(0)); W->last_error = 5; RET(0); }
static void s_InitiateSystemShutdownExW(Cpu *c) { W->last_error = 5; RET(0); }
static void s_LockWorkStation(Cpu *c) { RET(0); }
static void s_GetSystemWow64DirectoryW(Cpu *c) { W->last_error = 120; RET(0); }   /* ERROR_CALL_NOT_IMPLEMENTED: 32-bit Windows has no Wow64 directory */
static void s_GetSystemWow64DirectoryA(Cpu *c) { W->last_error = 120; RET(0); }
static void s_Wow64DisableWow64FsRedirection(Cpu *c) { if (ARG(0)) wrp(c, ARG(0), 0); RET(1); }
static void s_Wow64RevertWow64FsRedirection(Cpu *c) { RET(1); }
static void s_Wow64EnableWow64FsRedirection(Cpu *c) { RET(1); }
/* StrFormatByteSizeW / PathAppend & co. come from shlwapi; most are already in win32_compat.inc */

/* ---- IShellLinkW/A + IPersistFile (CLSID_ShellLink {00021401-…}): installers create Start-menu / desktop shortcuts through COM.
   Object layout (guest heap): [IShellLink vtable][IPersistFile vtable][IPropertyStore vtable][refs u32][wide u32][path 520][args 520][workdir 520][desc 520][icon 520][iconidx u32][showcmd u32][hotkey u32]
   Save() writes a real MS-SHLLINK file (header + unicode string data) so the .lnk is also readable by Windows. */
enum { SL_PATH = 0, SL_ARGS = 1, SL_WD = 2, SL_DESC = 3, SL_ICON = 4 };
static u64 sl_hdr(Cpu *c) { return 3 * PSZ_OF(c); }   /* IShellLink, IPersistFile, IPropertyStore vtables */
static u64 sl_str(Cpu *c, u64 obj, int which) { return obj + sl_hdr(c) + 8 + (u64)which * 520; }
static u64 sl_int(Cpu *c, u64 obj, int which) { return obj + sl_hdr(c) + 8 + 5 * 520 + (u64)which * 4; }
static int sl_wide(Cpu *c, u64 obj) { return rd32(c, obj + sl_hdr(c) + 4) != 0; }
static void sl_get_str(Cpu *c, u64 obj, int which, u64 dst, u32 cch) {   /* (obj, which, buf, cch) → string in the object's encoding */
  if (!dst || !cch) return; char s[520]; const char *p = rd_str(c, sl_str(c, obj, which), 519); e_snprintf(s, sizeof s, "%s", p ? p : "");
  if (sl_wide(c, obj)) put_w8(c, dst, s, cch); else put_a(c, dst, s, cch); }
static void sl_set_str(Cpu *c, u64 obj, int which, u64 src) { char s[520] = { 0 }; if (src) { if (sl_wide(c, obj)) wstr_to_utf8(c, src, s, sizeof s, -1); else { const char *p = rd_str(c, src, 519); e_snprintf(s, sizeof s, "%s", p ? p : ""); } }
  u64 d = sl_str(c, obj, which); for (u32 i = 0; i < 520; i++) wr8(c, d + i, (u8)s[i]); }
static u64 sl_vt(Cpu *c, const char *iface, const char **names, int n) { u64 P = PSZ_OF(c), vt = w32_heap_alloc(W, (u64)n * P, 1); char full[96];
  for (int i = 0; i < n; i++) { e_snprintf(full, sizeof full, "%s::%s", iface, names[i]); wrp(c, vt + (u64)i * P, w32_shim_slot(W, "shell32", full)); } return vt; }
static u64 sl_create(Cpu *c, int wide) { u64 P = PSZ_OF(c), obj = w32_heap_alloc(W, 3 * P + 8 + 5 * 520 + 12, 1); if (!obj) return 0;
  static const char *ps_names[] = { "QueryInterface", "AddRef", "Release", "GetCount", "GetAt", "GetValue", "SetValue", "Commit" };
  static const char *sl_names[] = { "QueryInterface", "AddRef", "Release", "GetPath", "GetIDList", "SetIDList", "GetDescription", "SetDescription", "GetWorkingDirectory", "SetWorkingDirectory", "GetArguments", "SetArguments", "GetHotkey", "SetHotkey", "GetShowCmd", "SetShowCmd", "GetIconLocation", "SetIconLocation", "SetRelativePath", "Resolve", "SetPath" };
  static const char *pf_names[] = { "QueryInterface", "AddRef", "Release", "GetClassID", "IsDirty", "Load", "Save", "SaveCompleted", "GetCurFile" };
  wrp(c, obj, sl_vt(c, "IShellLink", sl_names, 21)); wrp(c, obj + P, sl_vt(c, "IPersistFile", pf_names, 9)); wrp(c, obj + 2 * P, sl_vt(c, "IShellLinkProps", ps_names, 8)); wr32(c, obj + 3 * P, 1); wr32(c, obj + 3 * P + 4, (u32)wide); wr32(c, sl_int(c, obj, 1), 1); return obj; }
/* IIDs (Data1): IUnknown 0, IShellLinkA 000214EE, IShellLinkW 000214F9, IPersist 0000010C, IPersistFile 0000010B */
static int sl_qi(Cpu *c, u64 obj, u64 riid, u64 ppv) { u32 d1 = rd32(c, riid); u64 P = PSZ_OF(c); if (!ppv) return 0;
  if (d1 == 0 || d1 == 0x000214F9u || d1 == 0x000214EEu) { if (d1) wr32(c, obj + 3 * P + 4, d1 == 0x000214F9u); wrp(c, ppv, obj); }
  else if (d1 == 0x0000010Bu || d1 == 0x0000010Cu) wrp(c, ppv, obj + P); else if (d1 == 0x886D8EEBu) wrp(c, ppv, obj + 2 * P); /* IPropertyStore: AppUserModelID & co are accepted and dropped */ else { wrp(c, ppv, 0); return 0; }
  wr32(c, obj + 3 * P, rd32(c, obj + 3 * P) + 1); return 1; }
static u64 sl_this(Cpu *c, int pf) { return ARG(0) - (u64)pf * PSZ_OF(c); }
static void s_IShellLink_QueryInterface(Cpu *c) { RET(sl_qi(c, sl_this(c, 0), ARG(1), ARG(2)) ? 0 : E_NOINTERFACE_); }
static void s_IPersistFile_QueryInterface(Cpu *c) { RET(sl_qi(c, sl_this(c, 1), ARG(1), ARG(2)) ? 0 : E_NOINTERFACE_); }
static void sl_addref(Cpu *c, u64 obj) { u64 a = obj + 3 * PSZ_OF(c); wr32(c, a, rd32(c, a) + 1); RET(rd32(c, a)); }
static void sl_release(Cpu *c, u64 obj) { u64 a = obj + 3 * PSZ_OF(c); u32 n = rd32(c, a); if (n) n--; wr32(c, a, n); if (!n) w32_heap_free(W, obj); RET(n); }
static void s_IShellLink_AddRef(Cpu *c) { sl_addref(c, sl_this(c, 0)); }
static void s_IPersistFile_AddRef(Cpu *c) { sl_addref(c, sl_this(c, 1)); }
static void s_IPropertyStore_QueryInterface(Cpu *c) { RET(sl_qi(c, sl_this(c, 2), ARG(1), ARG(2)) ? 0 : E_NOINTERFACE_); }
static void s_IPropertyStore_AddRef(Cpu *c) { sl_addref(c, sl_this(c, 2)); }
static void s_IPropertyStore_Release(Cpu *c) { sl_release(c, sl_this(c, 2)); }
static void s_IPropertyStore_GetCount(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(0); }
static void s_IPropertyStore_GetAt(Cpu *c) { RET(0x80070057u); }
static void s_IPropertyStore_GetValue(Cpu *c) { if (ARG(2)) { u8 *p = mem_ptr(c->mem, ARG(2), 16, 1); if (p) e_memset(p, 0, 16); } RET(0); }   /* VT_EMPTY */
static void s_IPropertyStore_SetValue(Cpu *c) { RET(0); }
static void s_IPropertyStore_Commit(Cpu *c) { RET(0); }
static void s_IShellLink_Release(Cpu *c) { sl_release(c, sl_this(c, 0)); }
static void s_IPersistFile_Release(Cpu *c) { sl_release(c, sl_this(c, 1)); }
static void s_IShellLink_GetPath(Cpu *c) { sl_get_str(c, ARG(0), SL_PATH, ARG(1), (u32)ARG(2)); if (ARG(3)) { for (int i = 0; i < 36; i++) wr8(c, ARG(3) + i, 0); } RET(0); }   /* (buf, cch, WIN32_FIND_DATA*, flags) */
static void s_IShellLink_GetIDList(Cpu *c) { if (!ARG(1)) { RET(E_INVALIDARG_); return; } const char *p = rd_str(c, sl_str(c, ARG(0), SL_PATH), 519); u64 pidl = pidl_make(W, p ? p : ""); wrp(c, ARG(1), pidl); RET(pidl ? 0 : 1); }
static void s_IShellLink_SetIDList(Cpu *c) { if (ARG(1)) { char path[520]; e_snprintf(path, sizeof path, "%s", pidl_path(c, ARG(1))); u64 d = sl_str(c, ARG(0), SL_PATH); for (u32 i = 0; i < 520; i++) wr8(c, d + i, (u8)path[i]); } RET(0); }
static void s_IShellLink_GetDescription(Cpu *c) { sl_get_str(c, ARG(0), SL_DESC, ARG(1), (u32)ARG(2)); RET(0); }
static void s_IShellLink_SetDescription(Cpu *c) { sl_set_str(c, ARG(0), SL_DESC, ARG(1)); RET(0); }
static void s_IShellLink_GetWorkingDirectory(Cpu *c) { sl_get_str(c, ARG(0), SL_WD, ARG(1), (u32)ARG(2)); RET(0); }
static void s_IShellLink_SetWorkingDirectory(Cpu *c) { sl_set_str(c, ARG(0), SL_WD, ARG(1)); RET(0); }
static void s_IShellLink_GetArguments(Cpu *c) { sl_get_str(c, ARG(0), SL_ARGS, ARG(1), (u32)ARG(2)); RET(0); }
static void s_IShellLink_SetArguments(Cpu *c) { sl_set_str(c, ARG(0), SL_ARGS, ARG(1)); RET(0); }
static void s_IShellLink_GetHotkey(Cpu *c) { if (ARG(1)) wr16(c, ARG(1), (u16)rd32(c, sl_int(c, ARG(0), 2))); RET(0); }
static void s_IShellLink_SetHotkey(Cpu *c) { wr32(c, sl_int(c, ARG(0), 2), (u32)ARG(1) & 0xffff); RET(0); }
static void s_IShellLink_GetShowCmd(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), rd32(c, sl_int(c, ARG(0), 1))); RET(0); }
static void s_IShellLink_SetShowCmd(Cpu *c) { wr32(c, sl_int(c, ARG(0), 1), (u32)ARG(1)); RET(0); }
static void s_IShellLink_GetIconLocation(Cpu *c) { sl_get_str(c, ARG(0), SL_ICON, ARG(1), (u32)ARG(2)); if (ARG(3)) wr32(c, ARG(3), rd32(c, sl_int(c, ARG(0), 0))); RET(0); }
static void s_IShellLink_SetIconLocation(Cpu *c) { sl_set_str(c, ARG(0), SL_ICON, ARG(1)); wr32(c, sl_int(c, ARG(0), 0), (u32)ARG(2)); RET(0); }
static void s_IShellLink_SetRelativePath(Cpu *c) { RET(0); }
static void s_IShellLink_Resolve(Cpu *c) { RET(0); }
static void s_IShellLink_SetPath(Cpu *c) { sl_set_str(c, ARG(0), SL_PATH, ARG(1)); RET(0); }
static void s_IPersistFile_GetClassID(Cpu *c) { static const u8 clsid[16] = { 0x01,0x14,0x02,0x00,0,0,0,0,0xC0,0,0,0,0,0,0,0x46 }; if (ARG(1)) for (int i = 0; i < 16; i++) wr8(c, ARG(1) + i, clsid[i]); RET(0); }
static void s_IPersistFile_IsDirty(Cpu *c) { RET(1); }
static void s_IPersistFile_GetCurFile(Cpu *c) { RET(E_NOINTERFACE_); }
static void s_IPersistFile_SaveCompleted(Cpu *c) { RET(0); }
static u8 *sl_put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); return p + 2; }
static u8 *sl_put_u32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); return p + 4; }
static u8 *sl_put_wstr(u8 *p, const char *s) {   /* StringData: u16 count + UTF-16 chars (no terminator) */
  u16 tmp[520]; u32 n = 0; const u8 *u = (const u8 *)s; while (*u && n < 519) { u32 cp = *u++; if (cp >= 0xF0 && u[0] && u[1] && u[2]) { cp = ((cp & 7) << 18) | ((u[0] & 0x3f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f); u += 3; cp -= 0x10000; tmp[n++] = (u16)(0xD800 + (cp >> 10)); cp = 0xDC00 + (cp & 0x3ff); } else if (cp >= 0xE0 && u[0] && u[1]) { cp = ((cp & 0xf) << 12) | ((u[0] & 0x3f) << 6) | (u[1] & 0x3f); u += 2; } else if (cp >= 0xC0 && u[0]) { cp = ((cp & 0x1f) << 6) | (u[0] & 0x3f); u++; } tmp[n++] = (u16)cp; }
  p = sl_put_u16(p, n); for (u32 i = 0; i < n; i++) p = sl_put_u16(p, tmp[i]); return p; }
static void s_IPersistFile_Save(Cpu *c) {   /* (this, pszFileName (wide), fRemember) */
  u64 obj = sl_this(c, 1); char file[520]; if (!ARG(1)) { RET(E_INVALIDARG_); return; } wstr_to_utf8(c, ARG(1), file, sizeof file, -1);
  char s[5][520]; for (int i = 0; i < 5; i++) { const char *p = rd_str(c, sl_str(c, obj, i), 519); e_snprintf(s[i], 520, "%s", p ? p : ""); }
  static u8 buf[76 + 5 * 1042 + 64]; u8 *p = buf; u32 flags = 0x80 /* IsUnicode */; if (s[SL_DESC][0]) flags |= 4; if (s[SL_PATH][0]) flags |= 8 /* HasRelativePath (we store the full target here) */; if (s[SL_WD][0]) flags |= 0x10; if (s[SL_ARGS][0]) flags |= 0x20; if (s[SL_ICON][0]) flags |= 0x40;
  p = sl_put_u32(p, 0x4C); static const u8 clsid[16] = { 0x01,0x14,0x02,0x00,0,0,0,0,0xC0,0,0,0,0,0,0,0x46 }; for (int i = 0; i < 16; i++) *p++ = clsid[i];
  p = sl_put_u32(p, flags); p = sl_put_u32(p, 0x20 /* FILE_ATTRIBUTE_ARCHIVE */); for (int i = 0; i < 24; i++) *p++ = 0; /* 3 FILETIMEs */ p = sl_put_u32(p, 0); /* FileSize */ p = sl_put_u32(p, rd32(c, sl_int(c, obj, 0))); p = sl_put_u32(p, rd32(c, sl_int(c, obj, 1))); p = sl_put_u16(p, (u16)rd32(c, sl_int(c, obj, 2))); p = sl_put_u16(p, 0); p = sl_put_u32(p, 0); p = sl_put_u32(p, 0);
  if (flags & 4) p = sl_put_wstr(p, s[SL_DESC]); if (flags & 8) p = sl_put_wstr(p, s[SL_PATH]); if (flags & 0x10) p = sl_put_wstr(p, s[SL_WD]); if (flags & 0x20) p = sl_put_wstr(p, s[SL_ARGS]); if (flags & 0x40) p = sl_put_wstr(p, s[SL_ICON]);
  p = sl_put_u32(p, 0);   /* ExtraData terminal block */
  char dir[520]; e_snprintf(dir, sizeof dir, "%s", file); for (int i = (int)e_strlen(dir) - 1; i > 0; i--) if (dir[i] == '\\') { dir[i] = 0; break; } shell_mkdirs(dir);
  int fd = vfs_open(file, 2, 2, 0); if (fd < 0) { w32_report(W, "IPersistFile::Save(%s): cannot create", file); RET(0x80070003u); return; }
  vfs_write(fd, buf, (u64)(p - buf)); vfs_close(fd); W->shortcuts++; RET(0); }
static void s_IPersistFile_Load(Cpu *c) {   /* parse back what Save() wrote (StringData only) */
  u64 obj = sl_this(c, 1); char file[520]; if (!ARG(1)) { RET(E_INVALIDARG_); return; } wstr_to_utf8(c, ARG(1), file, sizeof file, -1);
  int fd = vfs_open(file, 1, 3, 0); if (fd < 0) { RET(0x80070002u); return; } static u8 buf[76 + 5 * 1042 + 64]; s64 n = vfs_read(fd, buf, sizeof buf); vfs_close(fd); if (n < 76 || buf[0] != 0x4C) { RET(0x80004005u); return; }
  u32 flags = (u32)buf[20] | ((u32)buf[21] << 8) | ((u32)buf[22] << 16) | ((u32)buf[23] << 24); u32 off = 76; wr32(c, sl_int(c, obj, 0), (u32)buf[56] | ((u32)buf[57] << 8)); wr32(c, sl_int(c, obj, 1), (u32)buf[60] | ((u32)buf[61] << 8)); wr32(c, sl_int(c, obj, 2), (u32)buf[64] | ((u32)buf[65] << 8));
  if (flags & 1) { if (off + 2 > (u32)n) { RET(0); return; } off += 2 + ((u32)buf[off] | ((u32)buf[off + 1] << 8)); }
  if (flags & 2) { if (off + 4 > (u32)n) { RET(0); return; } off += (u32)buf[off] | ((u32)buf[off + 1] << 8) | ((u32)buf[off + 2] << 16) | ((u32)buf[off + 3] << 24); }
  static const int order[5] = { SL_DESC, SL_PATH, SL_WD, SL_ARGS, SL_ICON }; static const u32 bits[5] = { 4, 8, 0x10, 0x20, 0x40 };
  for (int k = 0; k < 5; k++) { if (!(flags & bits[k])) continue; if (off + 2 > (u32)n) break; u32 cnt = (u32)buf[off] | ((u32)buf[off + 1] << 8); off += 2; char s[520]; u32 sl = 0;
    for (u32 i = 0; i < cnt && off + 2 <= (u32)n; i++, off += (flags & 0x80) ? 2 : 1) { u32 ch = (flags & 0x80) ? ((u32)buf[off] | ((u32)buf[off + 1] << 8)) : buf[off]; if (ch < 0x80) { if (sl < 518) s[sl++] = (char)ch; } else if (ch < 0x800) { if (sl < 517) { s[sl++] = (char)(0xC0 | (ch >> 6)); s[sl++] = (char)(0x80 | (ch & 0x3f)); } } else if (sl < 516) { s[sl++] = (char)(0xE0 | (ch >> 12)); s[sl++] = (char)(0x80 | ((ch >> 6) & 0x3f)); s[sl++] = (char)(0x80 | (ch & 0x3f)); } }
    s[sl] = 0; u64 d = sl_str(c, obj, order[k]); for (u32 i = 0; i <= sl && i < 520; i++) wr8(c, d + i, (u8)s[i]); }
  RET(0); }
/* CoCreateInstance hook: returns 1 when the class is served here */
static int shell_cocreate(Cpu *c, u32 clsid_d1, u64 riid, u64 ppv) {
  if (clsid_d1 != 0x00021401u) return 0; u32 i1 = rd32(c, riid); int wide = i1 != 0x000214EEu; u64 obj = sl_create(c, wide); if (!obj) { RET(E_OUTOFMEMORY_); return 1; }
  if (i1 == 0x0000010Bu || i1 == 0x0000010Cu) wrp(c, ppv, obj + PSZ_OF(c)); else wrp(c, ppv, obj); RET(0); return 1; }
#define SHELLLINK_SHIMS \
  SA("IShellLink::QueryInterface", IShellLink_QueryInterface), SA("IShellLink::AddRef", IShellLink_AddRef), SA("IShellLink::Release", IShellLink_Release), SA("IShellLink::GetPath", IShellLink_GetPath), SA("IShellLink::GetIDList", IShellLink_GetIDList), SA("IShellLink::SetIDList", IShellLink_SetIDList), \
  SA("IShellLink::GetDescription", IShellLink_GetDescription), SA("IShellLink::SetDescription", IShellLink_SetDescription), SA("IShellLink::GetWorkingDirectory", IShellLink_GetWorkingDirectory), SA("IShellLink::SetWorkingDirectory", IShellLink_SetWorkingDirectory), SA("IShellLink::GetArguments", IShellLink_GetArguments), SA("IShellLink::SetArguments", IShellLink_SetArguments), \
  SA("IShellLink::GetHotkey", IShellLink_GetHotkey), SA("IShellLink::SetHotkey", IShellLink_SetHotkey), SA("IShellLink::GetShowCmd", IShellLink_GetShowCmd), SA("IShellLink::SetShowCmd", IShellLink_SetShowCmd), SA("IShellLink::GetIconLocation", IShellLink_GetIconLocation), SA("IShellLink::SetIconLocation", IShellLink_SetIconLocation), \
  SA("IShellLink::SetRelativePath", IShellLink_SetRelativePath), SA("IShellLink::Resolve", IShellLink_Resolve), SA("IShellLink::SetPath", IShellLink_SetPath), \
  SA("IShellLinkProps::QueryInterface", IPropertyStore_QueryInterface), SA("IShellLinkProps::AddRef", IPropertyStore_AddRef), SA("IShellLinkProps::Release", IPropertyStore_Release), SA("IShellLinkProps::GetCount", IPropertyStore_GetCount), SA("IShellLinkProps::GetAt", IPropertyStore_GetAt), SA("IShellLinkProps::GetValue", IPropertyStore_GetValue), SA("IShellLinkProps::SetValue", IPropertyStore_SetValue), SA("IShellLinkProps::Commit", IPropertyStore_Commit), SA("IPersistFile::QueryInterface", IPersistFile_QueryInterface), SA("IPersistFile::AddRef", IPersistFile_AddRef), SA("IPersistFile::Release", IPersistFile_Release), SA("IPersistFile::GetClassID", IPersistFile_GetClassID), SA("IPersistFile::IsDirty", IPersistFile_IsDirty), SA("IPersistFile::Load", IPersistFile_Load), SA("IPersistFile::Save", IPersistFile_Save), SA("IPersistFile::SaveCompleted", IPersistFile_SaveCompleted), SA("IPersistFile::GetCurFile", IPersistFile_GetCurFile),

#define SHELL_SHIMS \
  SHELLLINK_SHIMS \
  S(lstrcpyW), S(lstrcatW), S(lstrcatA), S(lstrcpynW), S(lstrcpynA), S(lstrcmpiW), \
  S(FindResourceW), S(FindResourceA), S(FindResourceExW), S(FindResourceExA), S(LoadResource), S(LockResource), S(SizeofResource), S(FreeResource), S(LoadStringW), S(LoadStringA), S(EnumResourceNamesW), \
  S(RegOpenKeyExW), S(RegOpenKeyExA), S(RegOpenKeyW), S(RegOpenKeyA), S(RegCreateKeyExW), S(RegCreateKeyExA), S(RegCreateKeyW), S(RegCreateKeyA), S(RegCloseKey), S(RegFlushKey), S(RegSetValueExW), S(RegSetValueExA), S(RegSetValueW), S(RegSetValueA), \
  S(RegQueryValueExW), S(RegQueryValueExA), S(RegQueryValueW), S(RegGetValueW), S(RegGetValueA), S(RegDeleteKeyW), S(RegDeleteKeyA), S(RegDeleteKeyExW), S(RegDeleteKeyExA), S(RegDeleteTreeW), S(RegDeleteValueW), S(RegDeleteValueA), \
  S(RegEnumKeyExW), S(RegEnumKeyExA), S(RegEnumKeyW), S(RegEnumValueW), S(RegEnumValueA), S(RegQueryInfoKeyW), S(RegQueryInfoKeyA), S(RegNotifyChangeKeyValue), S(RegOverridePredefKey), S(RegLoadMUIStringW), S(RegConnectRegistryW), \
  S(SHGetFolderPathW), S(SHGetFolderPathA), S(SHGetFolderPathAndSubDirW), S(SHGetSpecialFolderPathW), S(SHGetSpecialFolderPathA), S(SHGetKnownFolderPath), S(SHGetPathFromIDListW), S(SHGetPathFromIDListA), S(SHGetPathFromIDListEx), \
  S(SHGetSpecialFolderLocation), S(SHGetFolderLocation), S(SHParseDisplayName), S(ILCreateFromPathW), S(ILFree), S(ILClone), S(ILIsEqual), S(SHGetMalloc), S(CoGetMalloc), SA("IMalloc::QueryInterface", IMalloc_QueryInterface), SA("IMalloc::AddRef", IMalloc_AddRef), SA("IMalloc::Release", IMalloc_Release), SA("IMalloc::Alloc", IMalloc_Alloc), SA("IMalloc::Realloc", IMalloc_Realloc), SA("IMalloc::Free", IMalloc_Free), SA("IMalloc::GetSize", IMalloc_GetSize), SA("IMalloc::DidAlloc", IMalloc_DidAlloc), SA("IMalloc::HeapMinimize", IMalloc_HeapMinimize), S(SHCreateDirectoryExW), S(SHCreateDirectoryExA), S(SHChangeNotify), S(SHAddToRecentDocs), S(IsUserAnAdmin), \
  S(SHGetFileInfoW), S(SHGetStockIconInfo), S(Shell_NotifyIconW), S(Shell_NotifyIconA), S(SHAppBarMessage), S(SHEmptyRecycleBinW), S(ExtractIconW), S(ExtractIconExW), S(DragAcceptFiles), S(DragQueryFileW), S(DragFinish), S(SHFileOperationW), \
  S(ShellExecuteW), S(ShellExecuteA), S(ShellExecuteExW), S(ShellExecuteExA), S(FindExecutableW), S(ExitWindowsEx), S(InitiateSystemShutdownExW), S(LockWorkStation), \
  S(GetSystemWow64DirectoryW), S(GetSystemWow64DirectoryA), S(Wow64DisableWow64FsRedirection), S(Wow64RevertWow64FsRedirection), S(Wow64EnableWow64FsRedirection),
