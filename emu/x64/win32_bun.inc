/* win32_bun.inc — what Bun-built executables (opencode.exe) need beyond node's surface: the native-API file calls Bun uses instead of
 * CreateFileW (NtCreateFile / NtOpenFile / NtQueryAttributesFile / NtCreateNamedPipeFile with OBJECT_ATTRIBUTES + NT paths), ntdll
 * aliases (RtlWaitOnAddress, RtlExitUserProcess, RtlFillMemory) and a handful of kernel32 stubs (fibers, jobs, pseudo consoles, resources).
 * Included from win32.c after win32_nt.inc. */

/* OBJECT_ATTRIBUTES { ULONG Length @0; HANDLE RootDirectory @8; PUNICODE_STRING ObjectName @16; ULONG Attributes @24; ... }
 * UNICODE_STRING { USHORT Length @0 (bytes); USHORT MaximumLength @2; PWSTR Buffer @8 }
 * NT paths: "\??\C:\x", "\??\UNC\srv\share", "\Device\NamedPipe\name", "\??\pipe\name"; relative names resolve against RootDirectory. */
static void s_GetModuleFileNameW(Cpu *c);
static int nt_object_path(Cpu *c, u64 oa, char *out, size_t cap) {
  if (!oa) return 0; u64 root = rd64(c, oa + 8), us = rd64(c, oa + 16); char name[1024]; name[0] = 0;
  if (us) { u32 len = rd16(c, us) / 2; u64 buf = rd64(c, us + 8); if (buf && len) { size_t o = 0; for (u32 k = 0; k < len && o + 4 < sizeof name; k++) { u32 ch = rd16(c, buf + k * 2);
        if (ch < 0x80) name[o++] = (char)ch; else if (ch < 0x800) { name[o++] = (char)(0xC0 | (ch >> 6)); name[o++] = (char)(0x80 | (ch & 63)); } else { name[o++] = (char)(0xE0 | (ch >> 12)); name[o++] = (char)(0x80 | ((ch >> 6) & 63)); name[o++] = (char)(0x80 | (ch & 63)); } } name[o] = 0; } }
  const char *n = name;
  if (!e_strnicmp_(n, "\\??\\UNC\\", 8)) { e_snprintf(out, cap, "\\\\%s", n + 8); return 1; }
  if (!e_strnicmp_(n, "\\??\\pipe\\", 9)) { e_snprintf(out, cap, "\\\\.\\pipe\\%s", n + 9); return 1; }
  if (!e_strnicmp_(n, "\\Device\\NamedPipe\\", 18)) { e_snprintf(out, cap, "\\\\.\\pipe\\%s", n + 18); return 1; }
  if (!e_strnicmp_(n, "\\??\\", 4)) n += 4; else if (!e_strnicmp_(n, "\\DosDevices\\", 12)) n += 12;
  if (!e_strnicmp_(n, "\\\\?\\", 4)) n += 4;
  if (root && root != ~0ull && !((n[0] && n[1] == ':') || n[0] == '\\')) { int fd = handle_fd(root); const char *base = fd > 0 ? vfs_fd_path(fd) : 0; if (!base) return 0;   /* Bun's deleteTree passes RootDirectory together with an absolute name: the name wins */
    if (!*n) e_snprintf(out, cap, "%s", base); else { size_t bl = e_strlen(base); e_snprintf(out, cap, "%s%s%s", base, bl && (base[bl - 1] == '\\' || base[bl - 1] == '/') ? "" : "\\", n); } return 1; }
  if (!e_strnicmp_(n, "\\Device\\Null", 12) || !e_strnicmp_(n, "\\??\\NUL", 7)) { e_snprintf(out, cap, "NUL"); return 1; }
  if (!*n) return 0;
  e_snprintf(out, cap, "%s", n); return 1;
}
/* NtCreateFile(&h, access, oa, iosb, alloc, attrs, share, disposition, options, ea, ealen) */
static void s_NtCreateFile(Cpu *c) {
  u64 hp = ARG(0); u32 access = (u32)ARG(1); u64 oa = ARG(2), iosb = ARG(3); u32 disp = (u32)ARG(7), opts = (u32)ARG(8);
  char path[1024]; if (!nt_object_path(c, oa, path, sizeof path)) { nt_iosb(c, iosb, 0xC0000033u, 0); RET(0xC0000033u); return; }   /* OBJECT_NAME_INVALID */
  /* named pipes: client side of a CreateNamedPipe server, or the server end created through NtCreateNamedPipeFile */
  if (npipe_prefix(path)) { for (int k = 0; k < NPIPE_MAX; k++) if (g_npipes[k].used && npipe_same(g_npipes[k].name, path)) { int fd = g_npipes[k].fd; g_npipes[k].used = 0; vfs_set_inherit(fd, 0); wr64(c, hp, H_OF(fd)); nt_iosb(c, iosb, 0, 1); RET(0); return; }
    nt_iosb(c, iosb, 0xC0000034u, 0); RET(0xC0000034u); return; }
  if (!e_stricmp(path, "CONOUT$")) { wr64(c, hp, 0x11); nt_iosb(c, iosb, 0, 1); RET(0); return; } if (!e_stricmp(path, "CONIN$")) { wr64(c, hp, 0x10); nt_iosb(c, iosb, 0, 1); RET(0); return; }
  int wdisp = disp == 0 ? 2 : disp == 1 ? 3 : disp == 2 ? 1 : disp == 3 ? 4 : disp == 4 ? 5 : 2;   /* SUPERSEDE OPEN CREATE OPEN_IF OVERWRITE OVERWRITE_IF */
  u64 size = 0; int st = vfs_stat(path, &size);   /* 0 missing, 1 file, 2 dir, 3 null device */
  if ((opts & 1) && disp != 1 && st == 0) { if (!vfs_mkdir(path)) { nt_iosb(c, iosb, 0xC000003Au, 0); RET(0xC000003Au); return; } st = 2; }   /* FILE_DIRECTORY_FILE + create */
  if ((opts & 1) && st == 1) { nt_iosb(c, iosb, 0xC0000103u, 0); RET(0xC0000103u); return; }     /* NOT_A_DIRECTORY */
  if ((opts & 0x40) && st == 2) { nt_iosb(c, iosb, 0xC00000BAu, 0); RET(0xC00000BAu); return; }  /* FILE_IS_A_DIRECTORY */
  if (st == 2 && disp == 2) { nt_iosb(c, iosb, 0xC0000035u, 0); RET(0xC0000035u); return; }      /* OBJECT_NAME_COLLISION */
  int acc = ((access & 0x80000001u) ? 1 : 0) | ((access & 0x40000006u) ? 2 : 0); if (!acc || st == 2) acc = st == 2 ? 1 : 1;
  if (st == 2) wdisp = 3;
  int fd = vfs_open(path, acc, wdisp, (access & 4) && !(access & 0x40000002u));
  if (fd < 0) { u32 ns = fd == -2 ? 0xC0000034u : fd == -17 ? 0xC0000035u : fd == -21 ? 0xC00000BAu : fd == -24 ? 0xC000009Au : 0xC0000022u; e_log("[x64] NtCreateFile(%s) access=0x%x disp=%u opts=0x%x -> 0x%x (st=%d)\n", path, access, disp, opts, ns, st); nt_iosb(c, iosb, ns, 0); RET(ns); return; }
  u64 info = st == 0 ? 2 : (disp == 4 || disp == 5) ? 3 : disp == 0 ? 0 : 1;   /* FILE_CREATED / OVERWRITTEN / SUPERSEDED / OPENED */
  wr64(c, hp, H_FILE_BASE + (u64)fd); nt_iosb(c, iosb, 0, info); RET(0);
}
/* NtOpenFile(&h, access, oa, iosb, share, options) = NtCreateFile with FILE_OPEN */
static void s_NtOpenFile(Cpu *c) { u64 a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), opts = ARG(5);
  u64 *sp = (u64 *)mem_ptr(c->mem, c->r[RSP], 0x60, 1); if (!sp) { RET(NT_INVALID_PARAMETER); return; }
  sp[4] = a0; sp[5] = a1; sp[6] = a2; sp[7] = a3; sp[8] = 0; sp[9] = 0; sp[10] = 0; sp[11] = 1; sp[12] = opts; sp[13] = 0; sp[14] = 0;   /* rebuild the 11-argument frame (ARG(n>=4) reads [rsp+8+8n]) */
  call_shim(c, s_NtCreateFile, a0, a1, a2, a3); }
/* NtQueryAttributesFile(oa, FILE_BASIC_INFORMATION*) */
static void s_NtQueryAttributesFile(Cpu *c) { char path[1024]; if (!nt_object_path(c, ARG(0), path, sizeof path)) { RET(0xC0000033u); return; }
  u64 size = 0; int st = vfs_stat(path, &size); if (!st) { RET(0xC0000034u); return; }
  u64 a = ARG(1), ft = vfs_filetime(); wr64(c, a, ft); wr64(c, a + 8, ft); wr64(c, a + 16, ft); wr64(c, a + 24, ft); wr32(c, a + 32, st == 2 ? 0x10 : 0x80); wr32(c, a + 36, 0); RET(0); }
/* NtQueryFullAttributesFile(oa, FILE_NETWORK_OPEN_INFORMATION* 56 B: 4 times, AllocationSize @32, EndOfFile @40, Attributes @48) */
static void s_NtQueryFullAttributesFile(Cpu *c) { char path[1024]; if (!nt_object_path(c, ARG(0), path, sizeof path)) { RET(0xC0000033u); return; }
  u64 size = 0; int st = vfs_stat(path, &size); if (!st) { RET(0xC0000034u); return; }
  u64 a = ARG(1), ft = vfs_filetime(); for (int k = 0; k < 4; k++) wr64(c, a + 8 * k, ft); wr64(c, a + 32, (size + 4095) & ~4095ull); wr64(c, a + 40, size); wr32(c, a + 48, st == 2 ? 0x10 : 0x80); wr32(c, a + 52, 0); RET(0); }
/* NtCreateNamedPipeFile(&h, access, oa, iosb, share, disposition, options, type, readmode, completionmode, maxinst, inquota, outquota, timeout):
 * server end; the client opens the same name through NtCreateFile/CreateFileW. Direction from the access mask (GENERIC_READ/WRITE, FILE_READ/WRITE_DATA). */
static void s_NtCreateNamedPipeFile(Cpu *c) { u64 hp = ARG(0); u32 access = (u32)ARG(1); u64 iosb = ARG(3);
  char path[1024]; if (!nt_object_path(c, ARG(2), path, sizeof path) || !npipe_prefix(path)) { nt_iosb(c, iosb, 0xC0000033u, 0); RET(0xC0000033u); return; }
  int r, wfd; if (vfs_pipe(&r, &wfd)) { nt_iosb(c, iosb, 0xC000009Au, 0); RET(0xC000009Au); return; }
  int server_writes = (access & 0x40000002u) && !(access & 0x80000001u);
  int ours = server_writes ? wfd : r, theirs = ours == r ? wfd : r; vfs_set_inherit(ours, 0); vfs_set_inherit(theirs, 0);
  int k; for (k = 0; k < NPIPE_MAX && g_npipes[k].used; k++) { } if (k == NPIPE_MAX) { vfs_close(r); vfs_close(wfd); nt_iosb(c, iosb, 0xC000009Au, 0); RET(0xC000009Au); return; }
  g_npipes[k].used = 1; g_npipes[k].fd = theirs; e_snprintf(g_npipes[k].name, sizeof g_npipes[k].name, "%s", path);
  wr64(c, hp, H_OF(ours)); nt_iosb(c, iosb, 0, 2); RET(0); }
static void s_RtlExitUserProcess(Cpu *c) { do_exit(c, (int)ARG(0)); }
static void s_RtlFillMemory(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(0), ARG(1), 1); if (p) e_memset(p, (int)(ARG(2) & 0xff), (size_t)ARG(1)); RET(0); }
/* RtlWaitOnAddress(addr, cmp, size, timeout*) — like WaitOnAddress but the timeout is a LARGE_INTEGER* (negative = relative 100 ns) and the result is NTSTATUS */
static void s_RtlWaitOnAddress(Cpu *c) { int retry = W->wait_active && W->wait_rip == c->rip;
  u32 ms = 0xffffffffu;
  if (c->mode32 || !retry) { u64 tp = ARG(3); if (tp) { s64 t = (s64)rd64(c, tp); ms = t >= 0 ? 0 : (u32)((-t) / 10000); } }   /* x64 retry: R9 was already replaced by the ms — ARG(3) is no longer a pointer */
  if (c->mode32) call_shim(c, s_WaitOnAddress, ARG(0), ARG(1), ARG(2), ms);
  else { if (!retry) c->r[R9] = ms; s_WaitOnAddress(c); }   /* re-executed after a park: R9 already holds the ms */
   if (W->block != BLOCK_NONE) return; c->r[RAX] = c->r[RAX] ? 0 : 0x102; }   /* 0x102 = STATUS_TIMEOUT */
/* Zig's std.os.windows (OpenTUI's native DLL) calls the Nt* memory functions directly */
static void s_VirtualAlloc(Cpu *c); static void s_VirtualFree(Cpu *c); static void s_VirtualQuery(Cpu *c);
/* NtAllocateVirtualMemory(process, PVOID *base, zerobits, SIZE_T *size, type, protect) */
static void s_NtAllocateVirtualMemory(Cpu *c) { u64 bp = ARG(1), sp = ARG(3); u32 type = (u32)ARG(4), prot = (u32)ARG(5);
  u64 base = bp ? rd64(c, bp) : 0, size = sp ? rd64(c, sp) : 0; if (!size) { RET(NT_INVALID_PARAMETER); return; }
  call_shim(c, s_VirtualAlloc, base, size, type, prot); u64 r = c->r[RAX];
  if (!r) { RET(W->last_error == 487 ? 0xC0000018u : 0xC0000017u); return; }   /* CONFLICTING_ADDRESSES / NO_MEMORY */
  if (bp) wr64(c, bp, r); if (sp) wr64(c, sp, (size + 4095) & ~4095ull); RET(0); }
/* NtFreeVirtualMemory(process, PVOID *base, SIZE_T *size, type) */
static void s_NtFreeVirtualMemory(Cpu *c) { u64 bp = ARG(1), sp = ARG(2); u32 type = (u32)ARG(3); u64 base = bp ? rd64(c, bp) : 0, size = sp ? rd64(c, sp) : 0;
  call_shim(c, s_VirtualFree, base, type == 0x8000 ? 0 : size, type, 0); RET(c->r[RAX] ? 0 : 0xC000009Au); }
static void s_NtProtectVirtualMemory(Cpu *c) { if (ARG(4)) wr32(c, ARG(4), 0x40); RET(0); }
/* NtQueryVirtualMemory(process, addr, class, buf, len, *retlen): MemoryBasicInformation only */
static void s_NtQueryVirtualMemory(Cpu *c) { if ((u32)ARG(2) != 0) { RET(NT_INVALID_INFO_CLS); return; } u64 addr = ARG(1), buf = ARG(3), rl = ARG(5);
  call_shim(c, s_VirtualQuery, addr, buf, 48, 0); if (rl) wr64(c, rl, 48); RET(c->r[RAX] ? 0 : 0xC000000Du); }
/* RtlGetFullPathName_U(name, cap_bytes, buf, *filepart) → bytes written (without NUL); GetFullPathNameW's full_path() counts characters */
static void s_RtlGetFullPathName_U(Cpu *c) { char out[1024]; vfs_normalize(name_w(c, ARG(0)), out, sizeof out); size_t l = e_strlen(out); u32 cap = (u32)ARG(1) / 2;
  if (cap <= l) { RET((l + 1) * 2); return; } put_w(c, ARG(2), out, 0); if (ARG(3)) { size_t b = l; while (b > 0 && out[b - 1] != '\\') b--; wr64(c, ARG(3), ARG(2) + b * 2); } RET(l * 2); }
static void s_RtlEqualUnicodeString(Cpu *c) { u64 a = ARG(0), b = ARG(1); int ci = (int)(ARG(2) & 0xff); u32 la = rd16(c, a) / 2, lb = rd16(c, b) / 2; if (la != lb) { RET(0); return; }
  u64 pa = rd64(c, a + 8), pb = rd64(c, b + 8); for (u32 i = 0; i < la; i++) { u32 x = rd16(c, pa + 2 * i), y = rd16(c, pb + 2 * i); if (ci) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; } if (x != y) { RET(0); return; } } RET(1); }
static void s_RtlUpcaseUnicodeChar(Cpu *c) { u32 x = (u32)ARG(0) & 0xffff; RET(x >= 'a' && x <= 'z' ? x - 32 : x); }
static void s_RtlGetSystemTimePrecise(Cpu *c) { RET(now_100ns()); }
static void s_NtLockFile(Cpu *c) { nt_iosb(c, ARG(4), 0, 0); RET(0); }
static void s_NtUnlockFile(Cpu *c) { nt_iosb(c, ARG(1), 0, 0); RET(0); }
static void s_Module32First(Cpu *c) { W->last_error = 18; RET(0); }
static void s_GetNamedPipeInfo(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); if (ARG(2)) wr32(c, ARG(2), 65536); if (ARG(3)) wr32(c, ARG(3), 65536); if (ARG(4)) wr32(c, ARG(4), 1); RET(1); }
/* wcsrtombs(dst, const wchar_t **src, n, mbstate*) → bytes written; UTF-8 */
static void s_wcsrtombs(Cpu *c) { u64 d = ARG(0), sp = ARG(1), n = ARG(2); u64 s = rd64(c, sp); u64 o = 0;
  for (;;) { u32 ch = rd16(c, s); u8 b[4]; int k = ch < 0x80 ? 1 : ch < 0x800 ? 2 : 3; if (k == 1) b[0] = (u8)ch; else if (k == 2) { b[0] = (u8)(0xC0 | (ch >> 6)); b[1] = (u8)(0x80 | (ch & 63)); } else { b[0] = (u8)(0xE0 | (ch >> 12)); b[1] = (u8)(0x80 | ((ch >> 6) & 63)); b[2] = (u8)(0x80 | (ch & 63)); }
    if (d && o + k > n) { wr64(c, sp, s); break; } if (d) for (int j = 0; j < k; j++) wr8(c, d + o + j, b[j]); if (!ch) { if (d) wr64(c, sp, 0); break; } o += k; s += 2; }
  RET(o); }
/* node.exe static imports that are only reached on error/crypto-store paths: honest failure stubs instead of "unimplemented API" faults */
static void s_GetUserProfileDirectoryW(Cpu *c) { static const char p[] = "C:\\Users\\user"; u64 lp = ARG(2); u32 cap = lp ? rd32(c, lp) : 0; if (cap < 14) { if (lp) wr32(c, lp, 14); W->last_error = 122; RET(0); return; } put_w(c, ARG(1), p, 0); if (lp) wr32(c, lp, 14); RET(1); }
static void s_CoTaskMemFree(Cpu *c) { RET(0); }
static void s_GetBestRoute2(Cpu *c) { RET(1168); }   /* ERROR_NOT_FOUND */
static void s_SymInitialize(Cpu *c) { W->last_error = 87; RET(0); }
static void s_ImageNtHeader(Cpu *c) { u64 base = ARG(0); if (!base || rd16(c, base) != 0x5A4D) { RET(0); return; } u32 e = rd32(c, base + 0x3c); if (e > 0x1000 || rd32(c, base + e) != 0x4550) { RET(0); return; } RET(base + e); }
static void s_ImageRvaToVa(Cpu *c) { u64 nt = ARG(0), base = ARG(1), rva = ARG(2); if (!nt) { RET(0); return; } u16 nsec = rd16(c, nt + 6), opt = rd16(c, nt + 20); u64 sec = nt + 24 + opt; for (int k = 0; k < nsec; k++, sec += 40) { u32 va = rd32(c, sec + 12), vs = rd32(c, sec + 8), raw = rd32(c, sec + 20); if (rva >= va && rva < va + vs) { RET(base + raw + (rva - va)); return; } } RET(0); }
static void s_IsThreadAFiber(Cpu *c) { RET(0); }
static void s_GetHostNameW(Cpu *c) { static const char h[] = "nebula"; if ((u32)ARG(1) > 6) { for (int i = 0; i < 7; i++) wr16(c, ARG(0) + 2 * i, (u8)h[i]); RET(0); return; } W->last_error = 10014; RET(-1); }
static void s_GlobalSize(Cpu *c) { RET(w32_heap_size(W, ARG(0))); }
static void s_IsProcessInJob(Cpu *c) { if (ARG(2)) wr32(c, ARG(2), 0); RET(1); }
static void s_QueryDosDeviceW(Cpu *c) { W->last_error = 2; RET(0); }
static void s_K32GetModuleFileNameExW(Cpu *c) { call_shim(c, s_GetModuleFileNameW, ARG(1), ARG(2), ARG(3), 0); }
static void s_CreateDirectoryExW(Cpu *c) { call_shim(c, s_CreateDirectoryW, ARG(1), ARG(2), 0, 0); }
static void s_CreatePseudoConsole(Cpu *c) { RET(0x80004001u); }   /* E_NOTIMPL: Bun falls back to pipes */
static void s_ResizePseudoConsole(Cpu *c) { RET(0x80004001u); }
static void s_ClosePseudoConsole(Cpu *c) { RET(0); }
static void s_CreateMemoryResourceNotification(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, 0, 0, &ex, &k); RET(h); }   /* never signalled */
/* GetTimeZoneInformationForYear(year, dynamic*, TIME_ZONE_INFORMATION* 172 B): UTC */
static void s_GetTimeZoneInformationForYear(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(2), 172, 1); if (p) { e_memset(p, 0, 172); static const char n[] = "UTC"; for (int i = 0; i < 4; i++) { p[4 + 2 * i] = (u8)n[i]; p[88 + 2 * i] = (u8)n[i]; } } RET(1); }

#define BUN_SHIMS \
  S(GetUserProfileDirectoryW), S(CoTaskMemFree), S(GetBestRoute2), S(SymInitialize), S(ImageNtHeader), S(ImageRvaToVa), SA("SymCleanup", ret0), SA("SymFromAddr", ret0), SA("SymFunctionTableAccess64", ret0), SA("SymGetLineFromAddr64", ret0), \
  SA("SymGetModuleBase64", ret0), SA("SymGetOptions", ret0), SA("SymSetOptions", ret0), SA("SymGetSearchPathW", ret0), SA("SymSetSearchPathW", ret0), SA("StackWalk64", ret0), SA("MiniDumpWriteDump", ret0), SA("UnDecorateSymbolName", ret0), \
  SA("CertOpenStore", ret0), SA("CryptQueryObject", ret0), SA("CryptMsgGetParam", ret0), SA("CryptMsgClose", ret1), SA("CertGetNameStringW", ret0), SA("CertGetCertificateChain", ret0), SA("CertFreeCertificateChain", ret1), SA("CertVerifyCertificateChainPolicy", ret0), SA("CryptDecodeObject", ret0), SA("CertOpenSystemStoreW", ret0), SA("CertCloseStore", ret1), SA("CertEnumCertificatesInStore", ret0), SA("CertFindCertificateInStore", ret0), SA("CertDuplicateCertificateContext", ret0), SA("CertFreeCertificateContext", ret1), \
  SA("CertGetCertificateContextProperty", ret0), SA("CertGetEnhancedKeyUsage", ret0), SA("CryptCreateHash", ret0), SA("CryptDecrypt", ret0), SA("CryptDestroyHash", ret1), SA("CryptDestroyKey", ret1), SA("CryptEnumProvidersW", ret0), \
  SA("CryptExportKey", ret0), SA("CryptGetProvParam", ret0), SA("CryptGetUserKey", ret0), SA("CryptSetHashParam", ret0), SA("CryptSignHashW", ret0), \
  S(RtlGetFullPathName_U), S(RtlEqualUnicodeString), S(RtlUpcaseUnicodeChar), S(RtlGetSystemTimePrecise), S(NtLockFile), S(NtUnlockFile), S(Module32First), SA("Module32Next", Module32First), SA("Module32FirstW", Module32First), SA("Module32NextW", Module32First), S(GetNamedPipeInfo), S(wcsrtombs), \
  SA("NtCreateSection", NtQueryObject), SA("NtMapViewOfSection", NtQueryObject), SA("NtUnmapViewOfSection", NtQueryObject), \
  S(NtAllocateVirtualMemory), S(NtFreeVirtualMemory), S(NtProtectVirtualMemory), S(NtQueryVirtualMemory), \
  S(NtCreateFile), S(NtOpenFile), S(NtQueryAttributesFile), S(NtQueryFullAttributesFile), S(NtCreateNamedPipeFile), S(RtlExitUserProcess), S(RtlFillMemory), S(RtlWaitOnAddress), \
  SA("RtlWakeAddressAll", WakeByAddressAll), SA("RtlWakeAddressSingle", WakeByAddressSingle), S(IsThreadAFiber), S(GetHostNameW), SA("RtlDllShutdownInProgress", ret0), S(GlobalSize), S(IsProcessInJob), S(QueryDosDeviceW), S(K32GetModuleFileNameExW), \
  S(CreateDirectoryExW), S(CreatePseudoConsole), S(ResizePseudoConsole), S(ClosePseudoConsole), S(CreateMemoryResourceNotification), S(GetTimeZoneInformationForYear), \
  SA("CreateJobObjectA", CreateJobObjectW), SA("AttachConsole", ret0), SA("BeginUpdateResourceW", ret0), SA("EndUpdateResourceW", ret0), SA("UpdateResourceW", ret0), \
  SA("EnumResourceLanguagesW", ret0),
