/* win32_nt.inc — the ntdll surface that libuv (node.exe) and other MSVC programs reach through GetProcAddress:
 * RtlGetVersion, NtQueryInformationFile / NtSetInformationFile / NtQueryVolumeInformationFile (file stat on handles),
 * NtQueryDirectoryFile (directory enumeration), NtQuerySystemInformation, NtQueryInformationProcess, NtDeviceIoControlFile.
 * Included from win32.c after win32_fs.inc (uses handle_fd, vfs_*, IO_STATUS_BLOCK = { Status @0 (u32, 8-byte slot), Information @8 }). */

#define NT_SUCCESS_          0x00000000u
#define NT_BUFFER_OVERFLOW   0x80000005u
#define NT_NO_MORE_FILES     0x80000006u
#define NT_NOT_IMPLEMENTED   0xC0000002u
#define NT_INVALID_INFO_CLS  0xC0000003u
#define NT_INVALID_HANDLE    0xC0000008u
#define NT_INVALID_PARAMETER 0xC000000Du
#define NT_NOT_SUPPORTED     0xC00000BBu
#define NT_PIPE_BROKEN       0xC000014Bu
#define NT_BUFFER_TOO_SMALL  0xC0000023u

static int e_strnicmp_(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { int x = (u8)a[i], y = (u8)b[i]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return x - y; if (!x) return 0; } return 0; }
static void nt_iosb(Cpu *c, u64 iosb, u32 status, u64 info) { if (iosb) { wr32(c, iosb, status); wr32(c, iosb + 4, 0); wr64(c, iosb + 8, info); } }

/* RtlGetVersion(RTL_OSVERSIONINFOW*): dwOSVersionInfoSize @0, Major @4, Minor @8, Build @12, PlatformId @16, szCSDVersion @20 (128 WCHAR) */
static void s_RtlGetVersion(Cpu *c) { u64 a = ARG(0); if (!a) { RET(NT_INVALID_PARAMETER); return; }
  wr32(c, a + 4, 10); wr32(c, a + 8, 0); wr32(c, a + 12, 19045); wr32(c, a + 16, 2); wr16(c, a + 20, 0); RET(0); }

/* fills FILE_BASIC_INFORMATION (40 B) at a */
static void nt_basic_info(Cpu *c, u64 a, int fd) { u64 ft = vfs_filetime(); int dir = vfs_fd_isdir(fd); wr64(c, a, ft); wr64(c, a + 8, ft); wr64(c, a + 16, ft); wr64(c, a + 24, ft); wr32(c, a + 32, dir ? 0x10 : 0x80); wr32(c, a + 36, 0); }
/* FILE_STANDARD_INFORMATION (24 B) */
static void nt_standard_info(Cpu *c, u64 a, int fd) { s64 sz = vfs_kind(fd) == FD_PIPE ? 0 : vfs_size(fd); int dir = vfs_fd_isdir(fd); wr64(c, a, (u64)((sz + 4095) & ~4095ll)); wr64(c, a + 8, (u64)sz); wr32(c, a + 16, 1); wr8(c, a + 20, 0); wr8(c, a + 21, (u8)dir); wr16(c, a + 22, 0); }
static u64 nt_file_id(int fd) { int n = vfs_fd_node(fd); return n >= 0 ? 0x1000000ull + (u64)n : 0x2000000ull + (u64)fd; }

/* NtQueryInformationFile(h, iosb, buf, len, class) */
static void s_NtQueryInformationFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); u64 iosb = ARG(1), buf = ARG(2); u32 len = (u32)ARG(3), cls = (u32)ARG(4);
  if (fd == 0 && ARG(0) != 0x10) { RET(NT_INVALID_HANDLE); return; }
  if (fd == -1) fd = 0;   /* stdin */
  int kind = vfs_kind(fd); u32 st = 0; u64 info = 0;
  switch (cls) {
  case 4:  /* FileBasicInformation */ if (len < 40) { st = NT_BUFFER_TOO_SMALL; break; } nt_basic_info(c, buf, fd); info = 40; break;
  case 5:  /* FileStandardInformation */ if (len < 24) { st = NT_BUFFER_TOO_SMALL; break; } nt_standard_info(c, buf, fd); info = 24; break;
  case 6:  /* FileInternalInformation */ if (len < 8) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, nt_file_id(fd)); info = 8; break;
  case 7:  /* FileEaInformation */ if (len < 4) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, 0); info = 4; break;
  case 8:  /* FileAccessInformation */ if (len < 4) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, 0x12019f); info = 4; break;
  case 9:  /* FileNameInformation: Length @0, Name @4 */ {
    const char *p = kind == FD_FILE ? vfs_fd_path(fd) : "\\Device\\NamedPipe\\nebula"; char tmp[600]; if (p && p[1] == ':') { e_snprintf(tmp, sizeof tmp, "%s", p + 2); p = tmp; } if (!p) p = "\\";
    u32 bl = (u32)e_strlen(p) * 2; if (len < 4) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, bl); u32 room = len - 4, wrote = 0;
    for (u32 i = 0; p[i] && wrote + 2 <= room; i++, wrote += 2) wr16(c, buf + 4 + wrote, (u8)p[i]); if (wrote < bl) st = NT_BUFFER_OVERFLOW; info = 4 + wrote; break; }
  case 14: /* FilePositionInformation */ if (len < 8) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, kind == FD_FILE ? (u64)vfs_tell(fd) : 0); info = 8; break;
  case 16: /* FileModeInformation */ if (len < 4) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */); info = 4; break;
  case 17: /* FileAlignmentInformation */ if (len < 4) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, 0); info = 4; break;
  case 18: /* FileAllInformation */ {
    /* layout (natural alignment, sizeof == 104 with a 1-WCHAR name): Basic@0 Standard@40 Internal@64 Ea@72 Access@76 Position@80 Mode@88 Alignment@92 NameLength@96 Name@100 */
    if (len < 100) { st = NT_BUFFER_TOO_SMALL; break; }
    nt_basic_info(c, buf, fd); nt_standard_info(c, buf + 40, fd); wr64(c, buf + 64, nt_file_id(fd)); wr32(c, buf + 72, 0); wr32(c, buf + 76, 0x12019f);
    wr64(c, buf + 80, kind == FD_FILE ? (u64)vfs_tell(fd) : 0); wr32(c, buf + 88, 0x20); wr32(c, buf + 92, 0);
    const char *p = kind == FD_FILE ? vfs_fd_path(fd) : "\\nebula"; char tmp[600]; if (p && p[1] == ':') { e_snprintf(tmp, sizeof tmp, "%s", p + 2); p = tmp; } if (!p) p = "\\";
    u32 bl = (u32)e_strlen(p) * 2; wr32(c, buf + 96, bl); u32 room = len > 100 ? len - 100 : 0, wrote = 0;
    for (u32 i = 0; p[i] && wrote + 2 <= room; i++, wrote += 2) wr16(c, buf + 100 + wrote, (u8)p[i]); if (wrote < bl) st = NT_BUFFER_OVERFLOW; info = 100 + wrote; break; }
  case 24: /* FilePipeLocalInformation: NamedPipeType, Configuration, MaximumInstances, CurrentInstances, InboundQuota, ReadDataAvailable, OutboundQuota, WriteQuotaAvailable, NamedPipeState, NamedPipeEnd (10 u32) */
    if (len < 40) { st = NT_BUFFER_TOO_SMALL; break; }
    wr32(c, buf, 0); wr32(c, buf + 4, 0); wr32(c, buf + 8, 1); wr32(c, buf + 12, 1); wr32(c, buf + 16, 65536); wr32(c, buf + 20, kind == FD_PIPE ? (u32)vfs_pipe_avail(fd) : 0); wr32(c, buf + 24, 65536); wr32(c, buf + 28, 65536); wr32(c, buf + 32, 3); wr32(c, buf + 36, 0); info = 40; break;
  case 34: /* FileAttributeTagInformation */ if (len < 8) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, vfs_fd_isdir(fd) ? 0x10 : 0x80); wr32(c, buf + 4, 0); info = 8; break;
  case 59: /* FileIdInformation: VolumeSerialNumber u64, FileId 16 B */ if (len < 24) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 0x4e45); wr64(c, buf + 8, nt_file_id(fd)); wr64(c, buf + 16, 0); info = 24; break;
  case 68: /* FileStatInformation: FileId 0, Creation 8, LastAccess 16, LastWrite 24, Change 32, AllocationSize 40, EndOfFile 48, FileAttributes 56, ReparseTag 60, NumberOfLinks 64, EffectiveAccess 68 */ {
    if (len < 72) { st = NT_BUFFER_TOO_SMALL; break; } u64 ft = vfs_filetime(); s64 sz = kind == FD_PIPE ? 0 : vfs_size(fd);
    wr64(c, buf, nt_file_id(fd)); wr64(c, buf + 8, ft); wr64(c, buf + 16, ft); wr64(c, buf + 24, ft); wr64(c, buf + 32, ft); wr64(c, buf + 40, (u64)((sz + 4095) & ~4095ll)); wr64(c, buf + 48, (u64)sz);
    wr32(c, buf + 56, vfs_fd_isdir(fd) ? 0x10 : 0x80); wr32(c, buf + 60, 0); wr32(c, buf + 64, 1); wr32(c, buf + 68, 0x12019f); info = 72; break; }
  default: st = NT_INVALID_INFO_CLS; break;
  }
  nt_iosb(c, iosb, st, info); RET(st); }

/* NtSetInformationFile(h, iosb, buf, len, class): FilePositionInformation 14, FileEndOfFileInformation 20, FileDispositionInformation 13, FileBasicInformation 4 (times: accepted) */
static void s_NtSetInformationFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); u64 iosb = ARG(1), buf = ARG(2); u32 cls = (u32)ARG(4); if (fd < 3 && ARG(0) < 0x10) { RET(NT_INVALID_HANDLE); return; }
  u32 st = 0;
  switch (cls) {
  case 4: case 16: case 24: break;
  case 13: /* FileDispositionInformation: DeleteFile u8 */ if (rd8(c, buf) && vfs_dir_nonempty(vfs_fd_path(fd))) { st = 0xC0000101u; break; } vfs_delete_on_close(fd, rd8(c, buf) != 0); break;
  case 64: /* FileDispositionInformationEx: Flags u32 (1 delete, 2 POSIX semantics) */ { u32 fl = rd32(c, buf); if ((fl & 1) && vfs_dir_nonempty(vfs_fd_path(fd))) { st = 0xC0000101u; break; } vfs_delete_on_close(fd, (fl & 1) != 0); if ((fl & 3) == 3 && vfs_fd_path(fd)) vfs_remove(vfs_fd_path(fd)); } break;
  case 14: vfs_seek(fd, (s64)rd64(c, buf), 0); break;
  case 20: /* FileEndOfFileInformation */ { s64 end = (s64)rd64(c, buf); s64 cur = vfs_tell(fd); vfs_seek(fd, end, 0); vfs_truncate(fd); vfs_seek(fd, cur < end ? cur : end, 0); break; }
  case 19: /* FileAllocationInformation */ break;
  case 10: case 65: { /* FileRenameInformation(Ex): Flags/ReplaceIfExists @0, RootDirectory @8, FileNameLength @16, FileName @20 (NT path or relative to RootDirectory) */
    const char *from = vfs_fd_path(fd); if (!from) { st = NT_INVALID_HANDLE; break; }
    u64 root = rd64(c, buf + 8); u32 nlen = rd32(c, buf + 16) / 2; char name[1024]; size_t o = 0;
    for (u32 k = 0; k < nlen && o + 4 < sizeof name; k++) { u32 ch = rd16(c, buf + 20 + 2 * k); if (ch < 0x80) name[o++] = (char)ch; else if (ch < 0x800) { name[o++] = (char)(0xC0 | (ch >> 6)); name[o++] = (char)(0x80 | (ch & 63)); } else { name[o++] = (char)(0xE0 | (ch >> 12)); name[o++] = (char)(0x80 | ((ch >> 6) & 63)); name[o++] = (char)(0x80 | (ch & 63)); } } name[o] = 0;
    char to[1100]; const char *n = name; if (!e_strnicmp_(n, "\\??\\", 4)) n += 4; if (!e_strnicmp_(n, "\\\\?\\", 4)) n += 4;
    if (root) { int rfd = handle_fd(root); const char *base = rfd > 0 ? vfs_fd_path(rfd) : 0; if (!base) { st = NT_INVALID_HANDLE; break; } e_snprintf(to, sizeof to, "%s\\%s", base, n); } else e_snprintf(to, sizeof to, "%s", n);
    int replace = cls == 10 ? rd8(c, buf) : (rd32(c, buf) & 1); u64 sz; int ex = vfs_stat(to, &sz);
    if (ex && !replace) { st = 0xC0000035u; break; } if (ex) vfs_remove(to);
    if (!vfs_rename(from, to)) st = 0xC0000034u; break; }
  default: st = NT_INVALID_INFO_CLS; break;
  }
  nt_iosb(c, iosb, st, 0); RET(st); }

/* NtQueryVolumeInformationFile(h, iosb, buf, len, class): FileFsVolumeInformation 1, FileFsSizeInformation 3, FileFsDeviceInformation 4, FileFsAttributeInformation 5, FileFsFullSizeInformation 7 */
static void s_NtQueryVolumeInformationFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); u64 iosb = ARG(1), buf = ARG(2); u32 len = (u32)ARG(3), cls = (u32)ARG(4); if (fd == 0 && ARG(0) != 0x10) { RET(NT_INVALID_HANDLE); return; }
  int kind = fd > 0 ? vfs_kind(fd) : FD_FILE; u32 st = 0; u64 info = 0;
  switch (cls) {
  case 1: { if (len < 18) { st = NT_BUFFER_TOO_SMALL; break; } static const char lbl[] = "NEBULA"; u32 bl = (u32)(sizeof lbl - 1) * 2;
    wr64(c, buf, vfs_filetime()); wr32(c, buf + 8, 0x4e45); wr32(c, buf + 12, bl); wr8(c, buf + 16, 1); wr8(c, buf + 17, 0); u32 room = len - 18, wrote = 0;
    for (u32 i = 0; lbl[i] && wrote + 2 <= room; i++, wrote += 2) wr16(c, buf + 18 + wrote, (u8)lbl[i]); if (wrote < bl) st = NT_BUFFER_OVERFLOW; info = 18 + wrote; break; }
  case 3: /* TotalAllocationUnits, AvailableAllocationUnits (u64), SectorsPerAllocationUnit, BytesPerSector (u32) */ if (len < 24) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 1u << 20); wr64(c, buf + 8, 1u << 19); wr32(c, buf + 16, 8); wr32(c, buf + 20, 512); info = 24; break;
  case 4: /* DeviceType, Characteristics */ if (len < 8) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, kind == FD_PIPE ? 0x11 /* FILE_DEVICE_NAMED_PIPE */ : (kind == FD_FILE ? 7 /* FILE_DEVICE_DISK */ : 0x50 /* FILE_DEVICE_CONSOLE */)); wr32(c, buf + 4, 0); info = 8; break;
  case 5: { /* FileSystemAttributes, MaximumComponentNameLength, FileSystemNameLength, FileSystemName */ if (len < 12) { st = NT_BUFFER_TOO_SMALL; break; } static const char fsn[] = "NTFS"; u32 bl = 8;
    wr32(c, buf, 0x00c706ff); wr32(c, buf + 4, 255); wr32(c, buf + 8, bl); u32 room = len - 12, wrote = 0; for (u32 i = 0; fsn[i] && wrote + 2 <= room; i++, wrote += 2) wr16(c, buf + 12 + wrote, (u8)fsn[i]); if (wrote < bl) st = NT_BUFFER_OVERFLOW; info = 12 + wrote; break; }
  case 7: if (len < 32) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 1u << 20); wr64(c, buf + 8, 1u << 19); wr64(c, buf + 16, 1u << 19); wr32(c, buf + 24, 8); wr32(c, buf + 28, 512); info = 32; break;
  default: st = NT_INVALID_INFO_CLS; break;
  }
  nt_iosb(c, iosb, st, info); RET(st); }

/* NtQueryDirectoryFile(h, event, apc, apcctx, iosb, buf, len, class, single, mask, restart) — per-handle enumeration state */
#define NT_DIRSCAN_MAX 32
static struct { int used, fd, fh, dots; } g_dirscan[NT_DIRSCAN_MAX];
static int nt_dirscan_open(int fd, int restart) {
  int k; for (k = 0; k < NT_DIRSCAN_MAX; k++) if (g_dirscan[k].used && g_dirscan[k].fd == fd) break;
  if (k < NT_DIRSCAN_MAX && !restart) return k;
  if (k < NT_DIRSCAN_MAX) { vfs_find_close(g_dirscan[k].fh); g_dirscan[k].used = 0; }
  for (k = 0; k < NT_DIRSCAN_MAX && g_dirscan[k].used; k++); if (k == NT_DIRSCAN_MAX) return -1;
  const char *p = vfs_fd_path(fd); if (!p) return -1; char pat[600]; e_snprintf(pat, sizeof pat, "%s%s*", p, p[e_strlen(p) - 1] == '\\' ? "" : "\\");
  int fh = vfs_find_first(pat); if (fh < 0) return -1; g_dirscan[k].used = 1; g_dirscan[k].fd = fd; g_dirscan[k].fh = fh; g_dirscan[k].dots = 0; return k; }
static void nt_dirscan_drop(int fd) { for (int k = 0; k < NT_DIRSCAN_MAX; k++) if (g_dirscan[k].used && g_dirscan[k].fd == fd) { vfs_find_close(g_dirscan[k].fh); g_dirscan[k].used = 0; } }
static void s_NtQueryDirectoryFile(Cpu *c) {
  int fd = handle_fd(ARG(0)); u64 iosb = ARG(4), buf = ARG(5); u32 len = (u32)ARG(6), cls = (u32)ARG(7); int single = (int)(ARG(8) & 0xff); int restart = (int)(ARG(10) & 0xff);
  if (fd < 3 || !vfs_fd_isdir(fd)) { nt_iosb(c, iosb, NT_INVALID_PARAMETER, 0); RET(NT_INVALID_PARAMETER); return; }
  /* FileDirectoryInformation 1 (name @64), FileFullDirectoryInformation 2 (EaSize @64, name @68), FileBothDirectoryInformation 3 (EaSize @64, ShortNameLength @68, ShortName @70, name @94), FileNamesInformation 12 (name @12), FileIdBothDirectoryInformation 37 (name @104, FileId @96), FileIdFullDirectoryInformation 38 (FileId @72, name @80) */
  u32 name_off; switch (cls) { case 1: name_off = 64; break; case 2: name_off = 68; break; case 3: name_off = 94; break; case 12: name_off = 12; break; case 37: name_off = 104; break; case 38: name_off = 80; break; default: nt_iosb(c, iosb, NT_INVALID_INFO_CLS, 0); RET(NT_INVALID_INFO_CLS); return; }
  int k = nt_dirscan_open(fd, restart); if (k < 0) { nt_iosb(c, iosb, NT_NO_MORE_FILES, 0); RET(NT_NO_MORE_FILES); return; }
  u64 pos = buf, last = 0; u32 count = 0; u64 ft = vfs_filetime();
  for (;;) {
    char name[300]; u64 size = 0; int is_dir = 0;
    if (g_dirscan[k].dots < 2) { e_snprintf(name, sizeof name, g_dirscan[k].dots == 0 ? "." : ".."); is_dir = 1; }
    else if (!vfs_find_next(g_dirscan[k].fh, name, sizeof name, &size, &is_dir)) break;
    if (!e_strcmp(name, ".") || !e_strcmp(name, "..")) { if (g_dirscan[k].dots >= 2) continue; }
    u32 nl = (u32)e_strlen(name) * 2, need = (name_off + nl + 7) & ~7u;
    if (pos + need > buf + len) { if (count == 0) { nt_iosb(c, iosb, NT_BUFFER_OVERFLOW, 0); RET(NT_BUFFER_OVERFLOW); return; } break; }   /* entry stays for the next call */
    if (g_dirscan[k].dots < 2) g_dirscan[k].dots++;
    u8 *p = mem_ptr(c->mem, pos, need, 1); if (!p) break; e_memset(p, 0, need);
    if (cls != 12) { wr64(c, pos + 8, ft); wr64(c, pos + 16, ft); wr64(c, pos + 24, ft); wr64(c, pos + 32, ft); wr64(c, pos + 40, size); wr64(c, pos + 48, (size + 4095) & ~4095ull); wr32(c, pos + 56, is_dir ? 0x10 : 0x80); wr32(c, pos + 60, nl); }
    else wr32(c, pos + 8, nl);
    if (cls == 37) wr64(c, pos + 96, 0x3000000ull + count); if (cls == 38) wr64(c, pos + 72, 0x3000000ull + count);
    for (u32 i = 0; i < nl / 2; i++) wr16(c, pos + name_off + i * 2, (u8)name[i]);
    if (last) wr32(c, last, (u32)(pos - last)); last = pos; pos += need; count++;
    if (single) break;
  }
  if (count == 0) { nt_dirscan_drop(fd); nt_iosb(c, iosb, NT_NO_MORE_FILES, 0); RET(NT_NO_MORE_FILES); return; }
  nt_iosb(c, iosb, 0, pos - buf); RET(0); }

/* NtQuerySystemInformation(class, buf, len, retlen): SystemBasicInformation 0, SystemProcessorPerformanceInformation 8, SystemTimeOfDayInformation 3 */
static void s_NtQuerySystemInformation(Cpu *c) {
  u32 cls = (u32)ARG(0); u64 buf = ARG(1); u32 len = (u32)ARG(2); u64 rl = ARG(3); u32 st = 0; u32 need = 0;
  switch (cls) {
  case 0: need = 64; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } e_memset(mem_ptr(c->mem, buf, 64, 1), 0, 64); wr32(c, buf + 8, 4096); wr32(c, buf + 12, 0x10000); wr32(c, buf + 16, 1u << 20); wr64(c, buf + 24, 0x10000); wr64(c, buf + 32, 0x7ffffffeffff); wr64(c, buf + 40, 1); wr8(c, buf + 56, 1); break;
  case 3: need = 48; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, vfs_filetime() - (u64)host_now_ms() * 10000ull); wr64(c, buf + 8, vfs_filetime()); wr64(c, buf + 16, 0); wr32(c, buf + 24, 0); wr32(c, buf + 28, 0); wr64(c, buf + 32, 0); wr64(c, buf + 40, 0); break;
  case 8: /* one processor: IdleTime, KernelTime, UserTime, DpcTime, InterruptTime (u64), InterruptCount u32 = 48 B */ need = 48; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } { u64 up = (u64)host_now_ms() * 10000ull; wr64(c, buf, up / 2); wr64(c, buf + 8, up); wr64(c, buf + 16, up / 2); wr64(c, buf + 24, 0); wr64(c, buf + 32, 0); wr32(c, buf + 40, 0); wr32(c, buf + 44, 0); } break;
  default: st = NT_NOT_IMPLEMENTED; break;
  }
  if (rl) wr32(c, rl, need); RET(st); }

/* NtQueryInformationProcess(h, class, buf, len, retlen): ProcessBasicInformation 0 (48 B), ProcessImageFileName 27, ProcessWow64Information 26 */
static void s_NtQueryInformationProcess(Cpu *c) {
  u32 cls = (u32)ARG(1); u64 buf = ARG(2); u32 len = (u32)ARG(3); u64 rl = ARG(4); u32 st = 0, need = 0;
  switch (cls) {
  case 0: if (c->mode32) { need = 24; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } wr32(c, buf, 0x103); wr32(c, buf + 4, (u32)W->peb); wr32(c, buf + 8, 1); wr32(c, buf + 12, 8); wr32(c, buf + 16, (u32)W->pid); wr32(c, buf + 20, (u32)W->ppid); break; }
    need = 48; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 0x103); wr64(c, buf + 8, W->peb); wr64(c, buf + 16, 1); wr64(c, buf + 24, 8); wr64(c, buf + 32, (u64)W->pid); wr64(c, buf + 40, (u64)W->ppid); break;
  case 26: need = 8; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 0); break;
  case 7: /* ProcessDebugPort */ need = 8; if (len < need) { st = NT_BUFFER_TOO_SMALL; break; } wr64(c, buf, 0); break;
  default: st = NT_NOT_IMPLEMENTED; break;
  }
  if (rl) wr32(c, rl, need); RET(st); }

/* NtDeviceIoControlFile(h, event, apc, ctx, iosb, code, in, inlen, out, outlen): no AFD/pipe FSCTLs yet */
static void s_NtDeviceIoControlFile(Cpu *c) { nt_iosb(c, ARG(4), NT_NOT_SUPPORTED, 0); RET(NT_NOT_SUPPORTED); }
static void s_NtClose(Cpu *c) { call_shim(c, s_CloseHandle, ARG(0), 0, 0, 0); RET(0); }
static void s_NtQueryObject(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0); RET(NT_NOT_IMPLEMENTED); }
static void s_NtSetInformationThread(Cpu *c) { RET(0); }
static void s_NtQueryInformationThread(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0); RET(NT_NOT_IMPLEMENTED); }
static void s_RtlGetNtVersionNumbers(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 10); if (ARG(1)) wr32(c, ARG(1), 0); if (ARG(2)) wr32(c, ARG(2), 19045 | 0xF0000000u); }

#define NT_SHIMS \
  S(RtlGetVersion), S(RtlGetNtVersionNumbers), S(NtQueryInformationFile), S(NtSetInformationFile), S(NtQueryVolumeInformationFile), S(NtQueryDirectoryFile), \
  S(NtQuerySystemInformation), S(NtQueryInformationProcess), S(NtDeviceIoControlFile), S(NtClose), S(NtQueryObject), S(NtSetInformationThread), S(NtQueryInformationThread),
