/* win32_node.inc — shims node.exe / libuv / V8 import beyond the classic set (included from win32.c).
 * Everything here is deliberately minimal: UTC time zone, "en-US" locale, no job objects, no fibers,
 * RegisterWaitForSingleObject implemented with a helper thread (libuv watches child processes with it). */

/* ---- time zone: UTC ---- */
static void s_GetDynamicTimeZoneInformation(Cpu *c) {              /* DYNAMIC_TIME_ZONE_INFORMATION = 432 bytes */
  u8 *p = mem_ptr(c->mem, ARG(0), 432, 1); if (!p) { RET(0xffffffffu); return; } e_memset(p, 0, 432);
  static const char *nm = "Coordinated Universal Time"; for (int i = 0; nm[i]; i++) { p[4 + i * 2] = (u8)nm[i]; p[88 + i * 2] = (u8)nm[i]; }   /* StandardName @4, DaylightName @88 */
  static const char *key = "UTC"; for (int i = 0; key[i]; i++) p[172 + i * 2] = (u8)key[i];                                                /* TimeZoneKeyName @172 */
  p[428] = 1;                                                                                                                                 /* DynamicDaylightTimeDisabled */
  RET(0); }                                                                                                                                   /* TIME_ZONE_ID_UNKNOWN */

/* ---- locale / geo: en-US, no formatting services ---- */
static void s_GetUserGeoID(Cpu *c) { RET(244); }                   /* GEOID United States */
static void s_GetGeoInfoW(Cpu *c) { W->last_error = 87; RET(0); }
static void s_LCIDToLocaleName(Cpu *c) { const char *n = "en-US"; u64 buf = ARG(1); u32 cch = (u32)ARG(2); if (!buf || !cch) { RET(6); return; } if (cch < 6) { W->last_error = 122; RET(0); return; } for (int i = 0; i <= 5; i++) wr16(c, buf + i * 2, (u8)n[i]); RET(6); }
static void s_LocaleNameToLCID(Cpu *c) { RET(0x409); }
static void s_ResolveLocaleName(Cpu *c) { const char *n = "en-US"; u64 buf = ARG(1); u32 cch = (u32)ARG(2); if (!buf || !cch) { RET(6); return; } if (cch < 6) { W->last_error = 122; RET(0); return; } for (int i = 0; i <= 5; i++) wr16(c, buf + i * 2, (u8)n[i]); RET(6); }
static void s_GetNumberFormatEx(Cpu *c) { W->last_error = 1004; RET(0); }
static void s_GetCurrencyFormatEx(Cpu *c) { W->last_error = 1004; RET(0); }
static void s_GetTimeFormatEx(Cpu *c) { W->last_error = 1004; RET(0); }

/* ---- process / thread misc ---- */
static void s_GetCurrentProcessorNumber(Cpu *c) { RET(0); }
static void s_QueryThreadCycleTime(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), c->icount); RET(1); }
static void s_GetProcessIoCounters(Cpu *c) { u8 *p = mem_ptr(c->mem, ARG(1), 48, 1); if (p) e_memset(p, 0, 48); RET(p != 0); }
static void s_NeedCurrentDirectoryForExePathW(Cpu *c) { RET(1); }
static void s_CancelSynchronousIo(Cpu *c) { W->last_error = 1168; RET(0); }
static void s_K32EnumProcessModules(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), 8); if (ARG(1) && ARG(2) >= 8) wr64(c, ARG(1), W->image_base); RET(1); }
static void s_K32GetModuleBaseNameW(Cpu *c) { const char *n = W->exe_name; size_t l = e_strlen(n); u32 cch = (u32)ARG(3); if (!cch) { RET(0); return; } if (l >= cch) l = cch - 1; for (size_t i = 0; i < l; i++) wr16(c, ARG(2) + i * 2, (u8)n[i]); wr16(c, ARG(2) + l * 2, 0); RET(l); }

/* ---- job objects: accepted, no-ops (libuv puts children into a kill-on-close job) ---- */
static void s_CreateJobObjectW(Cpu *c) { KObj *k = 0; int ex = 0; u64 h = new_kobj_handle(c, KO_EVENT, 0, 0, &ex, &k); if (h && k) { k->manual = 1; k->signaled = 0; } RET(h); }
static void s_SetInformationJobObject(Cpu *c) { RET(1); }
static void s_AssignProcessToJobObject(Cpu *c) { RET(1); }

/* ---- fibers: not supported (V8 only uses them for JSPI stack switching) ---- */
static void s_ConvertThreadToFiberEx(Cpu *c) { W->last_error = 120; RET(0); }
static void s_ConvertFiberToThread(Cpu *c) { RET(0); }
static void s_CreateFiberEx(Cpu *c) { W->last_error = 120; RET(0); }
static void s_DeleteFiber(Cpu *c) { }
static void s_SwitchToFiber(Cpu *c) { }

/* ---- resources: node.exe only looks for a single-executable-application blob — none ---- */

/* ---- pipes / files ---- */
static void s_GetNamedPipeClientProcessId(Cpu *c) { wr32(c, ARG(1), (u32)W->pid); RET(1); }
static void s_GetNamedPipeServerProcessId(Cpu *c) { wr32(c, ARG(1), (u32)W->pid); RET(1); }
static void s_GetNamedPipeHandleStateA(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); if (ARG(2)) wr32(c, ARG(2), 1); if (ARG(3)) wr32(c, ARG(3), 0); if (ARG(4)) wr32(c, ARG(4), 0); RET(1); }
static void s_WaitNamedPipeW(Cpu *c) { RET(1); }
static void s_ReOpenFile(Cpu *c) { W->last_error = 50; RET(~0ull); }
static void s_ReadDirectoryChangesW(Cpu *c) { W->last_error = 50; RET(0); }
static void s_SetFileInformationByHandle(Cpu *c) {                  /* FileEndOfFileInfo (6): truncate at the given size; FileBasicInfo (0) / FileDispositionInfo (4) / FileIoPriorityHintInfo: accepted */
  int fd = handle_fd(ARG(0)); u32 cls = (u32)ARG(1);
  if (fd < 3) { W->last_error = 6; RET(0); return; }
  if (cls == 6) { s64 eof = (s64)rd64(c, ARG(2)); s64 cur = vfs_tell(fd); if (vfs_seek(fd, eof, 0) < 0 || !vfs_truncate(fd)) { W->last_error = 87; RET(0); return; } vfs_seek(fd, cur < eof ? cur : eof, 0); RET(1); return; }
  if (cls == 4 || cls == 21) { int del = cls == 4 ? rd8(c, ARG(2)) != 0 : (rd32(c, ARG(2)) & 1) != 0; if (del && vfs_dir_nonempty(vfs_fd_path(fd))) { W->last_error = 145; RET(0); return; } vfs_delete_on_close(fd, del); if (del && (cls == 21 && (rd32(c, ARG(2)) & 2))) vfs_remove(vfs_fd_path(fd)); RET(1); return; }   /* FileDispositionInfo(Ex): delete on close; FILE_DISPOSITION_POSIX_SEMANTICS unlinks the name at once (std::filesystem::remove_all) */
  if (cls == 3 || cls == 22) {                                     /* FileRenameInfo(Ex): { ReplaceIfExists/Flags @0, RootDirectory @8, FileNameLength @16, FileName @20 } */
    u32 len = rd32(c, ARG(2) + 16); char to[1024]; u32 n = len / 2; if (n >= sizeof to) n = sizeof to - 1; for (u32 i = 0; i < n; i++) to[i] = (char)rd16(c, ARG(2) + 20 + i * 2); to[n] = 0;
    const char *tp = to; if (tp[0] == '\\' && tp[1] == '?' && tp[2] == '?' && tp[3] == '\\') tp += 4;
    char from[1024]; e_snprintf(from, sizeof from, "%s", vfs_fd_path(fd)); RET(vfs_rename(from, tp)); return; }
  RET(1); }

/* ---- RegisterWaitForSingleObject: helper thread that waits on the handle, then tail-calls the callback(context, timed_out) ----
 * param block (guest heap): [0]=handle [8]=callback [16]=context [24]=timeout ms [32]=flags [40]=cancelled [44]=done */
static void s_nebula_regwait_thunk(Cpu *c) {
  u64 blk = ARG(0); u64 h = rd64(c, blk), cb = rd64(c, blk + 8), ctx = rd64(c, blk + 16); u32 ms = rd32(c, blk + 24);
  do_wait(c, 1, 0, h, 0, ms); if (c->status == RUN_YIELD) return;    /* parked: re-executed here after the wake-up */
  u32 r = (u32)c->r[RAX]; wr32(c, blk + 44, 1);
  if (rd32(c, blk + 40)) { RET(0); return; }                         /* UnregisterWait raced the signal: just finish the helper thread */
  guest_call(c, cb, ctx, r == 0x102 ? 1 : 0, 0, 0);                 /* callback(context, timed_out) returns into the continuation slot → thread exit */
}
static void s_RegisterWaitForSingleObject(Cpu *c) {
  u64 out = ARG(0), h = ARG(1), cb = ARG(2), ctx = ARG(3), ms = ARG(4), flags = ARG(5);
  int kind, idx; if (!wait_handle(c, h, &kind, &idx)) { W->last_error = 6; RET(0); return; }
  u64 blk = w32_heap_alloc(W, 48, 1); if (!blk) { W->last_error = 8; RET(0); return; }
  wr64(c, blk, h); wr64(c, blk + 8, cb); wr64(c, blk + 16, ctx); wr32(c, blk + 24, (u32)ms); wr32(c, blk + 32, (u32)flags);
  u64 thunk = w32_shim_slot(W, "dynamic", "nebula_regwait_thunk"); u32 tid = 0;
  u64 th = thread_create(c, thunk, blk, 0, 0, &tid); if (!th) { W->last_error = 8; RET(0); return; }
  { int fd = handle_fd(th); vfs_close(fd); }                          /* detached */
  wr64(c, out, blk); RET(1); }
static void s_UnregisterWaitEx(Cpu *c) { u64 blk = ARG(0); if (blk && mem_ptr(c->mem, blk, 48, 1)) { wr32(c, blk + 40, 1); if (rd32(c, blk + 44)) { RET(1); return; } } W->last_error = 997; RET(0); }   /* ERROR_IO_PENDING: callback may still run (libuv ignores it) */
static void s_UnregisterWait(Cpu *c) { s_UnregisterWaitEx(c); }

#define NODE_SHIMS \
  S(GetDynamicTimeZoneInformation), S(GetUserGeoID), S(GetGeoInfoW), S(LCIDToLocaleName), S(LocaleNameToLCID), S(ResolveLocaleName), \
  S(GetNumberFormatEx), S(GetCurrencyFormatEx), S(GetTimeFormatEx), S(GetCurrentProcessorNumber), S(QueryThreadCycleTime), S(GetProcessIoCounters), \
  S(NeedCurrentDirectoryForExePathW), S(CancelSynchronousIo), S(K32EnumProcessModules), S(K32GetModuleBaseNameW), \
  S(CreateJobObjectW), S(SetInformationJobObject), S(AssignProcessToJobObject), \
  S(ConvertThreadToFiberEx), S(ConvertFiberToThread), S(CreateFiberEx), S(DeleteFiber), S(SwitchToFiber), \
  \
  S(GetNamedPipeClientProcessId), S(GetNamedPipeServerProcessId), S(GetNamedPipeHandleStateA), S(WaitNamedPipeW), S(ReOpenFile), S(ReadDirectoryChangesW), S(SetFileInformationByHandle), \
  S(nebula_regwait_thunk), S(RegisterWaitForSingleObject), S(UnregisterWaitEx), S(UnregisterWait)
