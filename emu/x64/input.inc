/* ---------- game input: Raw Input (user32) and DirectInput 8 (dinput8.dll) on top of the emulator's keyboard / mouse state ----------
   Raw Input: RegisterRawInputDevices records the interest per process; w32_key / w32_mouse then queue RAWINPUT records and post
   WM_INPUT to the target window (RIDEV_NOLEGACY suppresses the ordinary WM_KEY* and WM_MOUSE* messages).  DirectInput8: a COM
   IDirectInput8 whose CreateDevice hands out a system keyboard (256 DIK scan-code bytes) and a system mouse (relative axes,
   buttons, wheel, DIMOUSESTATE/DIMOUSESTATE2), immediate GetDeviceState and buffered GetDeviceData; no joysticks (the
   emulator reports "no game controller" here and through XInput). */

/* VK → DirectInput / scan-code-set-1 make code */
static u8 vk_to_dik(int vk) {
  static const u8 letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
  static const u8 digits[10] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };
  static const u8 numpad[10] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 };
  if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A']; if (vk >= '0' && vk <= '9') return digits[vk - '0']; if (vk >= 0x60 && vk <= 0x69) return numpad[vk - 0x60];
  if (vk >= 0x70 && vk <= 0x79) return (u8)(0x3B + vk - 0x70); if (vk == 0x7A) return 0x57; if (vk == 0x7B) return 0x58;
  switch (vk) { case 0x1B: return 0x01; case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C; case 0x10: case 0xA0: return 0x2A; case 0xA1: return 0x36; case 0x11: case 0xA2: return 0x1D; case 0xA3: return 0x9D;
    case 0x12: case 0xA4: return 0x38; case 0xA5: return 0xB8; case 0x20: return 0x39; case 0x14: return 0x3A; case 0x90: return 0x45; case 0x91: return 0x46; case 0x6A: return 0x37; case 0x6B: return 0x4E; case 0x6D: return 0x4A; case 0x6E: return 0x53; case 0x6F: return 0xB5;
    case 0x13: return 0xC5; case 0x24: return 0xC7; case 0x26: return 0xC8; case 0x21: return 0xC9; case 0x25: return 0xCB; case 0x27: return 0xCD; case 0x23: return 0xCF; case 0x28: return 0xD0; case 0x22: return 0xD1; case 0x2D: return 0xD2; case 0x2E: return 0xD3;
    case 0x5B: return 0xDB; case 0x5C: return 0xDC; case 0x5D: return 0xDD; case 0xBA: return 0x27; case 0xBB: return 0x0D; case 0xBC: return 0x33; case 0xBD: return 0x0C; case 0xBE: return 0x34; case 0xBF: return 0x35; case 0xC0: return 0x29; case 0xDB: return 0x1A; case 0xDC: return 0x2B; case 0xDD: return 0x1B; case 0xDE: return 0x28; default: return 0; } }


/* ===== Raw Input ===== */
typedef struct { Win32 *w; int mouse, kbd; u64 hwnd_mouse, hwnd_kbd; u32 fl_mouse, fl_kbd; } RawReg;
static RawReg g_ri;
typedef struct { u32 type, size; u64 hdev, wparam; u8 data[24]; } RawRec;
static RawRec g_rawq[128]; static u32 g_raw_seq;
#define RAW_HDEV_MOUSE 0x10001u
#define RAW_HDEV_KBD   0x10002u
static RawReg *ri_of(Win32 *w) { if (g_ri.w != w) { e_memset(&g_ri, 0, sizeof g_ri); g_ri.w = w; } return &g_ri; }
static u64 raw_push(RawRec *r) { u32 i = g_raw_seq++ & 127; g_rawq[i] = *r; return 0x52410000u | i | ((u64)(g_raw_seq & 0xffff) << 32); }
/* called from the host event hooks (no Cpu): returns 1 when the legacy message must be suppressed (RIDEV_NOLEGACY) */
static int raw_key(Win32 *w, int vk, int down) { RawReg *r = ri_of(w); if (!r->kbd) return 0; RawRec rr; e_memset(&rr, 0, sizeof rr); rr.type = 1; rr.hdev = RAW_HDEV_KBD; u16 mk = vk_to_dik(vk); int ext = mk >= 0x80; u16 *d = (u16 *)rr.data;
  d[0] = mk & 0x7f; d[1] = (u16)((down ? 0 : 1) | (ext ? 2 : 0)); d[2] = 0; d[3] = (u16)vk; *(u32 *)(rr.data + 8) = down ? 0x100 : 0x101; *(u32 *)(rr.data + 12) = 0;
  u64 h = raw_push(&rr); u64 t = r->hwnd_kbd ? r->hwnd_kbd : key_target(); post_msg_to(w, t, 0xFF, 0 /* RIM_INPUT */, h); return (r->fl_kbd & 0x30) != 0; }
static int raw_mouse(Win32 *w, int dx, int dy, int ev, int wheel) { RawReg *r = ri_of(w); if (!r->mouse) return 0; RawRec rr; e_memset(&rr, 0, sizeof rr); rr.type = 0; rr.hdev = RAW_HDEV_MOUSE; u16 bf = ev == 1 ? 1 : ev == 2 ? 2 : ev == 3 ? 4 : ev == 4 ? 8 : ev == 5 ? 0x10 : ev == 6 ? 0x20 : ev == 7 ? 0x400 : 0;
  *(u16 *)(rr.data + 0) = 0; /* MOUSE_MOVE_RELATIVE */ *(u16 *)(rr.data + 4) = bf; *(u16 *)(rr.data + 6) = (u16)(s16)wheel; *(u32 *)(rr.data + 8) = (u32)g_mouse_btn; *(s32 *)(rr.data + 12) = dx; *(s32 *)(rr.data + 16) = dy; *(u32 *)(rr.data + 20) = 0;
  u64 h = raw_push(&rr); u64 t = r->hwnd_mouse ? r->hwnd_mouse : (wnd_alive(g_focus) ? HWND_OF(g_focus) : (g_wnd[0].used ? HWND_OF(0) : 0)); if (!t) return 0; post_msg_to(w, t, 0xFF, 0, h); return (r->fl_mouse & 0x30) != 0; }
static void s_RegisterRawInputDevices(Cpu *c) { RawReg *r = ri_of(W); u64 p = ARG(0); u32 n = (u32)ARG(1), cb = (u32)ARG(2); u32 want = c->mode32 ? 12 : 16; if (!p || cb != want) { W->last_error = 87; RET(0); return; }
  for (u32 k = 0; k < n; k++, p += cb) { u16 page = rd16(c, p), usage = rd16(c, p + 2); u32 fl = rd32(c, p + 4); u64 hw = rdp(c, p + 8); if (page != 1) continue; int rem = (fl & 1) != 0;
    if (usage == 2) { r->mouse = !rem; r->hwnd_mouse = rem ? 0 : hw; r->fl_mouse = fl; } else if (usage == 6) { r->kbd = !rem; r->hwnd_kbd = rem ? 0 : hw; r->fl_kbd = fl; } }
  e_log("[input] RegisterRawInputDevices: mouse=%d kbd=%d (flags %x/%x)\n", r->mouse, r->kbd, r->fl_mouse, r->fl_kbd); RET(1); }
static void s_GetRegisteredRawInputDevices(Cpu *c) { RawReg *r = ri_of(W); u64 p = ARG(0), pn = ARG(1); u32 cb = c->mode32 ? 12 : 16; u32 n = (r->mouse ? 1 : 0) + (r->kbd ? 1 : 0); if (!pn) { RET((u64)(u32)-1); return; } if (!p) { wr32(c, pn, n); RET(0); return; } if (rd32(c, pn) < n) { wr32(c, pn, n); W->last_error = 122; RET((u64)(u32)-1); return; }
  u32 k = 0; if (r->mouse) { wr16(c, p + k * cb, 1); wr16(c, p + k * cb + 2, 2); wr32(c, p + k * cb + 4, r->fl_mouse); wrp(c, p + k * cb + 8, r->hwnd_mouse); k++; } if (r->kbd) { wr16(c, p + k * cb, 1); wr16(c, p + k * cb + 2, 6); wr32(c, p + k * cb + 4, r->fl_kbd); wrp(c, p + k * cb + 8, r->hwnd_kbd); k++; } RET(k); }
static void s_GetRawInputData(Cpu *c) { u64 h = ARG(0); u32 cmd = (u32)ARG(1); u64 pd = ARG(2), psz = ARG(3); if ((h & 0xffff0000u) != 0x52410000u || !psz) { RET((u64)(u32)-1); return; } RawRec *r = &g_rawq[h & 127]; u32 hs = c->mode32 ? 16 : 24; u32 total = hs + (r->type == 0 ? 24 : r->type == 1 ? 16 : 8); u32 need = cmd == 0x10000005 ? hs : total;
  if (!pd) { wr32(c, psz, need); RET(0); return; } if (rd32(c, psz) < need) { wr32(c, psz, need); W->last_error = 122; RET((u64)(u32)-1); return; }
  wr32(c, pd, r->type); wr32(c, pd + 4, total); wrp(c, pd + 8, r->hdev); wrp(c, pd + 8 + PSZ_OF(c), r->wparam); if (cmd != 0x10000005) { u8 *d = mem_ptr(c->mem, pd + hs, need - hs, 1); if (d) e_memcpy(d, r->data, need - hs); } RET(need); }
static void s_GetRawInputBuffer(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(0); }
static void s_DefRawInputProc(Cpu *c) { RET(0); }
static void s_GetRawInputDeviceList(Cpu *c) { u64 p = ARG(0), pn = ARG(1); u32 cb = (u32)ARG(2), P = (u32)PSZ_OF(c); if (!pn || cb != 2 * P) { W->last_error = 87; RET((u64)(u32)-1); return; } if (!p) { wr32(c, pn, 2); RET(0); return; } if (rd32(c, pn) < 2) { wr32(c, pn, 2); W->last_error = 122; RET((u64)(u32)-1); return; }
  wrp(c, p, RAW_HDEV_MOUSE); wr32(c, p + P, 0); wrp(c, p + cb, RAW_HDEV_KBD); wr32(c, p + cb + P, 1); RET(2); }
static void raw_device_info(Cpu *c, int wide) { u64 h = ARG(0); u32 cmd = (u32)ARG(1); u64 pd = ARG(2), psz = ARG(3); if (!psz || (h != RAW_HDEV_MOUSE && h != RAW_HDEV_KBD)) { W->last_error = 87; RET((u64)(u32)-1); return; } int mouse = h == RAW_HDEV_MOUSE;
  if (cmd == 0x20000007) { /* RIDI_DEVICENAME: count in characters */ const char *name = mouse ? "\\\\?\\HID#VID_1209&PID_0001&MI_00#Nebula#{378de44c-56ef-11d1-bc8c-00a0c91405dd}" : "\\\\?\\HID#VID_1209&PID_0002&MI_00#Nebula#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"; u32 n = (u32)e_strlen(name) + 1; if (!pd) { wr32(c, psz, n); RET(0); return; } if (rd32(c, psz) < n) { wr32(c, psz, n); W->last_error = 122; RET((u64)(u32)-1); return; } if (wide) put_w8(c, pd, name, 0); else { u8 *d = mem_ptr(c->mem, pd, n, 1); if (d) e_memcpy(d, name, n); } RET(n); return; }
  if (cmd == 0x2000000b) { /* RIDI_DEVICEINFO: RID_DEVICE_INFO (32 bytes) */ if (!pd) { wr32(c, psz, 32); RET(0); return; } if (rd32(c, psz) < 32) { wr32(c, psz, 32); W->last_error = 122; RET((u64)(u32)-1); return; } u8 *d = mem_ptr(c->mem, pd, 32, 1); if (d) e_memset(d, 0, 32); wr32(c, pd, 32); wr32(c, pd + 4, mouse ? 0 : 1);
    if (mouse) { wr32(c, pd + 8, 1); wr32(c, pd + 12, 3); wr32(c, pd + 16, 100); wr32(c, pd + 20, 0); } else { wr32(c, pd + 8, 4); wr32(c, pd + 12, 0); wr32(c, pd + 16, 1); wr32(c, pd + 20, 12); wr32(c, pd + 24, 3); wr32(c, pd + 28, 104); } RET(32); return; }
  if (cmd == 0x20000005) { wr32(c, psz, 0); RET(0); return; }   /* RIDI_PREPARSEDDATA: none */
  W->last_error = 87; RET((u64)(u32)-1); }
static void s_GetRawInputDeviceInfoW(Cpu *c) { raw_device_info(c, 1); }
static void s_GetRawInputDeviceInfoA(Cpu *c) { raw_device_info(c, 0); }

/* ===== DirectInput 8 ===== */
enum { DI_IFACE = 1, DI_DEV_KBD, DI_DEV_MOUSE };
typedef struct { int used, kind, refs, wide; Win32 *w; u64 addr; int acquired, exclusive, bufsize, axis_rel; u32 fmt_size; int dx, dy, dz; u32 seq; int qn; struct { u32 ofs, data, t; } q[64]; } DIObj;
#define DIOBJ_MAX 64
static DIObj g_diobj[DIOBJ_MAX];
static const char *const vt_IDirectInput8[] = { "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices", "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices" };
static const char *const vt_IDirectInputDevice8[] = { "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects", "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification", "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap", "SetActionMap", "GetImageInfo" };
static u64 g_divt[4]; static Win32 *g_divt_owner;
static u64 di_vtable(Cpu *c, int dev) { Win32 *w = W; if (g_divt_owner != w) { e_memset(g_divt, 0, sizeof g_divt); g_divt_owner = w; } if (g_divt[dev]) return g_divt[dev]; int n = dev ? 32 : 11; const char *const *m = dev ? vt_IDirectInputDevice8 : vt_IDirectInput8; const char *in = dev ? "IDirectInputDevice8" : "IDirectInput8"; u64 P = PSZ_OF(c); u64 vt = w32_heap_alloc(w, (u64)n * P, 1); if (!vt) return 0;
  for (int i = 0; i < n; i++) { char nm[96]; e_snprintf(nm, sizeof nm, "%s::%s", in, m[i]); wrp(c, vt + (u64)i * P, w32_shim_slot(w, "dinput8", nm)); } g_divt[dev] = vt; return vt; }
static DIObj *diobj_new(Cpu *c, int kind, int wide, u64 *addr_out) { int id; for (id = 1; id < DIOBJ_MAX && g_diobj[id].used; id++) { } if (id == DIOBJ_MAX) return 0; u64 vt = di_vtable(c, kind != DI_IFACE); u64 a = w32_heap_alloc(W, 32, 1); if (!vt || !a) return 0; wrp(c, a, vt); wr32(c, a + 8, (u32)id); wr32(c, a + 12, 1); DIObj *o = &g_diobj[id]; e_memset(o, 0, sizeof *o); o->used = 1; o->kind = kind; o->wide = wide; o->addr = a; o->w = W; o->refs = 1; o->axis_rel = 1; o->fmt_size = kind == DI_DEV_KBD ? 256 : 16; *addr_out = a; return o; }
static DIObj *diobj_of(Cpu *c, u64 self) { if (!self || !mem_ptr(c->mem, self, 16, 0)) return 0; u32 id = rd32(c, self + 8); if (id >= DIOBJ_MAX || !g_diobj[id].used || g_diobj[id].addr != self) return 0; return &g_diobj[id]; }
#define DIERR_INVALIDPARAM_ 0x80070057u
#define DIERR_NOTACQUIRED_ 0x8007000Cu
#define DIERR_NOTINITIALIZED_ 0x80070015u
#define DIERR_DEVICENOTREG_ 0x80040154u
#define DIERR_UNSUPPORTED_ 0x80004001u
#define DI_NOEFFECT_ 1u
static void di_queue(DIObj *o, u32 ofs, u32 data) { if (!o->bufsize) return; if (o->qn >= 64 || o->qn >= o->bufsize) { /* overflow: drop the oldest */ for (int k = 1; k < o->qn; k++) o->q[k - 1] = o->q[k]; o->qn--; } o->q[o->qn].ofs = ofs; o->q[o->qn].data = data; o->q[o->qn].t = (u32)host_now_ms(); o->qn++; }
/* host event hooks: keyboard (DIK offset, 0x80 pressed) and mouse (DIMOFS_X 0, _Y 4, _Z 8, buttons 12+) */
static void di_key(Win32 *w, int vk, int down) { u8 dik = vk_to_dik(vk); if (!dik) return; for (int i = 1; i < DIOBJ_MAX; i++) if (g_diobj[i].used && g_diobj[i].w == w && g_diobj[i].kind == DI_DEV_KBD && g_diobj[i].acquired) di_queue(&g_diobj[i], dik, down ? 0x80 : 0); }
static void di_mouse(Win32 *w, int dx, int dy, int ev, int wheel) { for (int i = 1; i < DIOBJ_MAX; i++) { DIObj *o = &g_diobj[i]; if (!o->used || o->w != w || o->kind != DI_DEV_MOUSE || !o->acquired) continue; o->dx += dx; o->dy += dy; o->dz += wheel;
    if (dx) di_queue(o, 0, (u32)dx); if (dy) di_queue(o, 4, (u32)dy); if (wheel) di_queue(o, 8, (u32)wheel); if (ev >= 1 && ev <= 6) di_queue(o, 12 + (ev - 1) / 2, (ev & 1) ? 0x80 : 0); } }
static int di_wide_iid(Cpu *c, u64 riid) { if (!riid) return 1; u32 d1 = rd32(c, riid); return (d1 & 1) != 0; /* IID_IDirectInput8W = ...8031, IID_IDirectInputDevice8W = 54D41081 */ }
static void s_DirectInput8Create(Cpu *c) { u64 ppv = ARG(3); if (!ppv) { RET(DIERR_INVALIDPARAM_); return; } u32 ver = (u32)ARG(1); if (ver < 0x0800) { wrp(c, ppv, 0); RET(0x8007047Eu /* DIERR_BETADIRECTINPUTVERSION */); return; } u64 a; DIObj *o = diobj_new(c, DI_IFACE, di_wide_iid(c, ARG(2)), &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wrp(c, ppv, a); e_log("[input] DirectInput8Create (system keyboard + mouse; no game controllers)\n"); RET(0); }
static void s_DirectInput8_QueryInterface(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 riid = ARG(1), ppv = ARG(2); if (!o || !ppv) { RET(E_INVALIDARG_); return; } u32 d1 = riid ? rd32(c, riid) : 0; int ok = d1 == 0 /* IUnknown */ || (o->kind == DI_IFACE && (d1 == 0xBF798030u || d1 == 0xBF798031u)) || (o->kind != DI_IFACE && (d1 == 0x54D41080u || d1 == 0x54D41081u)); if (!ok) { wrp(c, ppv, 0); RET(E_NOINTERFACE_); return; } o->refs++; wr32(c, o->addr + 12, (u32)o->refs); if (d1) o->wide = d1 & 1; wrp(c, ppv, o->addr); RET(0); }
static void s_DirectInput8_AddRef(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o) { RET(0); return; } o->refs++; RET((u64)o->refs); }
static void s_DirectInput8_Release(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o) { RET(0); return; } if (--o->refs <= 0) { e_memset(o, 0, sizeof *o); RET(0); return; } RET((u64)o->refs); }
static const u8 GUID_SysMouse_[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const u8 GUID_SysKeyboard_[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static void s_IDirectInput8_CreateDevice(Cpu *c) { DIObj *di = diobj_of(c, ARG(0)); u64 rguid = ARG(1), out = ARG(2); if (!di || !rguid || !out) { RET(DIERR_INVALIDPARAM_); return; } const u8 *g = mem_ptr(c->mem, rguid, 16, 0); if (!g) { RET(DIERR_INVALIDPARAM_); return; } int kind = !e_memcmp(g, GUID_SysKeyboard_, 16) ? DI_DEV_KBD : !e_memcmp(g, GUID_SysMouse_, 16) ? DI_DEV_MOUSE : 0; if (!kind) { wrp(c, out, 0); RET(DIERR_DEVICENOTREG_); return; }
  u64 a; DIObj *o = diobj_new(c, kind, di->wide, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wrp(c, out, a); e_log("[input] IDirectInput8::CreateDevice(%s)\n", kind == DI_DEV_KBD ? "SysKeyboard" : "SysMouse"); RET(0); }
/* DIDEVICEINSTANCE: dwSize guidInstance guidProduct dwDevType tszInstanceName[260] tszProductName[260] guidFFDriver wUsagePage wUsage */
static u32 di_fill_instance(Cpu *c, u64 p, u32 cb, int kind, int wide) { u32 cw = wide ? 2 : 1; u32 full = 4 + 16 + 16 + 4 + 260 * cw * 2 + 16 + 4; u32 dx3 = 4 + 16 + 16 + 4 + 260 * cw * 2; if (cb != full && cb != dx3) return 0; u8 *d = mem_ptr(c->mem, p, cb, 1); if (!d) return 0; e_memset(d, 0, cb); wr32(c, p, cb); const u8 *g = kind == DI_DEV_KBD ? GUID_SysKeyboard_ : GUID_SysMouse_; e_memcpy(d + 4, g, 16); e_memcpy(d + 20, g, 16); wr32(c, p + 36, kind == DI_DEV_KBD ? 0x413 : 0x212);
  const char *name = kind == DI_DEV_KBD ? "Keyboard" : "Mouse"; u64 n1 = p + 40, n2 = n1 + 260 * cw; if (wide) { put_w8(c, n1, name, 0); put_w8(c, n2, name, 0); } else { e_memcpy(d + 40, name, e_strlen(name) + 1); e_memcpy(d + 40 + 260, name, e_strlen(name) + 1); } if (cb == full) { wr16(c, p + cb - 4, 1); wr16(c, p + cb - 2, kind == DI_DEV_KBD ? 6 : 2); } return cb; }
/* EnumDevices(dwDevType, callback, pvRef, flags): the keyboard and the mouse, each through the callback; DIENUM_STOP (0) ends it */
static void dienum_step(Cpu *c) { Win32 *w = W; int ci = w->ncont - 1; int step = (int)w->cont[ci].data[1]; u64 cb = w->cont[ci].data[2], ref = w->cont[ci].data[3], buf = w->cont[ci].data[4]; int wide = (int)w->cont[ci].data[5]; u32 want = (u32)w->cont[ci].data[6];
  for (;;) { if (step >= 2) { w32_cont_finish(c, 0); return; } int kind = step == 0 ? DI_DEV_KBD : DI_DEV_MOUSE; step++; u32 cls = kind == DI_DEV_KBD ? 3 : 2, typ = kind == DI_DEV_KBD ? 0x13 : 0x12; if (want == 0 || want == cls || want == typ) { w->cont[ci].data[1] = (u64)step; di_fill_instance(c, buf, 4 + 16 + 16 + 4 + 260 * (wide ? 2 : 1) * 2 + 16 + 4, kind, wide); w32_guest_call(c, cb, buf, ref, 0, 0); return; } } }
static void dienum_resume(Cpu *c, u64 rax) { if (!(u32)rax) { w32_cont_finish(c, 0); return; } dienum_step(c); }
static void s_IDirectInput8_EnumDevices(Cpu *c) { DIObj *di = diobj_of(c, ARG(0)); u32 want = (u32)ARG(1); u64 cb = ARG(2), ref = ARG(3); if (!di || !cb) { RET(DIERR_INVALIDPARAM_); return; } if (want == 4 || want == 1 || (want >= 0x14 && want <= 0x1F)) { e_log("[input] IDirectInput8::EnumDevices(game controllers): none\n"); RET(0); return; }
  Win32 *w = W; u64 buf = w32_heap_alloc(w, 1200, 1); if (!buf) { RET(E_OUTOFMEMORY_); return; } w32_push_cont(c, CONT_DIENUM, 0); int ci = w->ncont - 1; w->cont[ci].data[1] = 0; w->cont[ci].data[2] = cb; w->cont[ci].data[3] = ref; w->cont[ci].data[4] = buf; w->cont[ci].data[5] = (u64)di->wide; w->cont[ci].data[6] = want; dienum_step(c); }
static void s_IDirectInput8_GetDeviceStatus(Cpu *c) { u64 g = ARG(1); const u8 *p = g ? mem_ptr(c->mem, g, 16, 0) : 0; RET(p && (!e_memcmp(p, GUID_SysKeyboard_, 16) || !e_memcmp(p, GUID_SysMouse_, 16)) ? 0 : DIERR_DEVICENOTREG_); }
static void s_IDirectInput8_RunControlPanel(Cpu *c) { RET(0); }
static void s_IDirectInput8_Initialize(Cpu *c) { RET(0); }
static void s_IDirectInput8_FindDevice(Cpu *c) { RET(DIERR_DEVICENOTREG_); }
static void s_IDirectInput8_EnumDevicesBySemantics(Cpu *c) { RET(0); }
static void s_IDirectInput8_ConfigureDevices(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
/* ---- device ---- */
static void s_IDirectInputDevice8_GetCapabilities(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 p = ARG(1); if (!o || !p) { RET(DIERR_INVALIDPARAM_); return; } u32 cb = rd32(c, p); if (cb != 44 && cb != 24) { RET(DIERR_INVALIDPARAM_); return; } u8 *d = mem_ptr(c->mem, p, cb, 1); if (d) e_memset(d, 0, cb); wr32(c, p, cb); wr32(c, p + 4, 1 | 4 /* ATTACHED | EMULATED */); wr32(c, p + 8, o->kind == DI_DEV_KBD ? 0x413 : 0x212); wr32(c, p + 12, o->kind == DI_DEV_KBD ? 0 : 3); wr32(c, p + 16, o->kind == DI_DEV_KBD ? 104 : 3); wr32(c, p + 20, 0); RET(0); }
static void s_IDirectInputDevice8_EnumObjects(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_GetProperty(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 prop = ARG(1), ph = ARG(2); if (!o || !ph) { RET(DIERR_INVALIDPARAM_); return; } u32 hs = rd32(c, ph + 4); if (hs != 16) { RET(DIERR_INVALIDPARAM_); return; }
  switch ((u32)prop) { case 1: wr32(c, ph + 16, (u32)o->bufsize); RET(0); return; case 2: wr32(c, ph + 16, o->axis_rel ? 0 : 1); RET(0); return; case 3: wr32(c, ph + 16, 10000); RET(0); return; case 4: wr32(c, ph + 16, 0); wr32(c, ph + 20, 0); RET(0); return; case 5: wr32(c, ph + 16, 0); RET(0); return; case 7: wr32(c, ph + 16, 0); RET(0); return; case 8: wr32(c, ph + 16, 0); RET(0); return; case 9: wr32(c, ph + 16, o->kind == DI_DEV_KBD ? 2 : 3); RET(0); return; case 10: wr32(c, ph + 16, 0); RET(0); return; case 24: wr32(c, ph + 16, 1); RET(0); return; default: RET(DIERR_UNSUPPORTED_); return; } }
static void s_IDirectInputDevice8_SetProperty(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 prop = ARG(1), ph = ARG(2); if (!o || !ph) { RET(DIERR_INVALIDPARAM_); return; } switch ((u32)prop) { case 1: o->bufsize = (int)rd32(c, ph + 16); if (o->bufsize > 64) o->bufsize = 64; RET(0); return; case 2: o->axis_rel = rd32(c, ph + 16) == 0; RET(0); return; default: RET(0); return; } }
static void s_IDirectInputDevice8_Acquire(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o || o->kind == DI_IFACE) { RET(DIERR_INVALIDPARAM_); return; } if (o->acquired) { RET(DI_NOEFFECT_); return; } o->acquired = 1; o->dx = o->dy = o->dz = 0; o->qn = 0; RET(0); }
static void s_IDirectInputDevice8_Unacquire(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o) { RET(DIERR_INVALIDPARAM_); return; } int was = o->acquired; o->acquired = 0; RET(was ? 0 : DI_NOEFFECT_); }
static void s_IDirectInputDevice8_GetDeviceState(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u32 cb = (u32)ARG(1); u64 p = ARG(2); if (!o || !p || o->kind == DI_IFACE) { RET(DIERR_INVALIDPARAM_); return; } if (!o->acquired) { RET(DIERR_NOTACQUIRED_); return; } u8 *d = mem_ptr(c->mem, p, cb, 1); if (!d) { RET(DIERR_INVALIDPARAM_); return; } e_memset(d, 0, cb);
  if (o->kind == DI_DEV_KBD) { if (cb < 256) { RET(DIERR_INVALIDPARAM_); return; } for (int vk = 1; vk < 256; vk++) if (W->key_state[vk]) { u8 k = vk_to_dik(vk); if (k) d[k] = 0x80; } RET(0); return; }
  if (cb < 16) { RET(DIERR_INVALIDPARAM_); return; } wr32(c, p, (u32)(o->axis_rel ? o->dx : g_mouse_x)); wr32(c, p + 4, (u32)(o->axis_rel ? o->dy : g_mouse_y)); wr32(c, p + 8, (u32)o->dz); d[12] = (g_mouse_btn & 1) ? 0x80 : 0; d[13] = (g_mouse_btn & 2) ? 0x80 : 0; d[14] = (g_mouse_btn & 4) ? 0x80 : 0; o->dx = o->dy = o->dz = 0; RET(0); }
static void s_IDirectInputDevice8_GetDeviceData(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u32 cb = (u32)ARG(1); u64 rg = ARG(2), pn = ARG(3); u32 fl = (u32)ARG(4); if (!o || !pn || o->kind == DI_IFACE) { RET(DIERR_INVALIDPARAM_); return; } if (!o->acquired) { RET(DIERR_NOTACQUIRED_); return; } if (!o->bufsize) { wr32(c, pn, 0); RET(0x80040207u /* DIERR_NOTBUFFERED */); return; } u32 want = rd32(c, pn), P = (u32)PSZ_OF(c); if (cb != 16 + P) { RET(DIERR_INVALIDPARAM_); return; }
  u32 n = (u32)o->qn; if (n > want && (rg || want != (u32)-1)) n = want; if (rg) for (u32 k = 0; k < n; k++) { u64 e = rg + (u64)k * cb; wr32(c, e, o->q[k].ofs); wr32(c, e + 4, o->q[k].data); wr32(c, e + 8, o->q[k].t); wr32(c, e + 12, ++o->seq); wrp(c, e + 16, 0); } if (!(fl & 1 /* DIGDD_PEEK */)) { for (u32 k = n; k < (u32)o->qn; k++) o->q[k - n] = o->q[k]; o->qn -= (int)n; } wr32(c, pn, n); RET(0); }
static void s_IDirectInputDevice8_SetDataFormat(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 df = ARG(1); if (!o || !df) { RET(DIERR_INVALIDPARAM_); return; } u32 ds = rd32(c, df + 12); if (ds) o->fmt_size = ds; RET(0); }
static void s_IDirectInputDevice8_SetEventNotification(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_SetCooperativeLevel(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o) { RET(DIERR_INVALIDPARAM_); return; } o->exclusive = (ARG(2) & 1) != 0; RET(0); }
static void s_IDirectInputDevice8_GetObjectInfo(Cpu *c) { RET(0x80070002u /* DIERR_OBJECTNOTFOUND */); }
static void s_IDirectInputDevice8_GetDeviceInfo(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); u64 p = ARG(1); if (!o || !p) { RET(DIERR_INVALIDPARAM_); return; } RET(di_fill_instance(c, p, rd32(c, p), o->kind, o->wide) ? 0 : DIERR_INVALIDPARAM_); }
static void s_IDirectInputDevice8_RunControlPanel(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_Initialize(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_CreateEffect(Cpu *c) { if (ARG(3)) wrp(c, ARG(3), 0); RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_EnumEffects(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_GetEffectInfo(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_GetForceFeedbackState(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_SendForceFeedbackCommand(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_EnumCreatedEffectObjects(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_Escape(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_Poll(Cpu *c) { DIObj *o = diobj_of(c, ARG(0)); if (!o) { RET(DIERR_INVALIDPARAM_); return; } RET(o->acquired ? DI_NOEFFECT_ : DIERR_NOTACQUIRED_); }
static void s_IDirectInputDevice8_SendDeviceData(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), 0); RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_EnumEffectsInFile(Cpu *c) { RET(0); }
static void s_IDirectInputDevice8_WriteEffectToFile(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_BuildActionMap(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_SetActionMap(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
static void s_IDirectInputDevice8_GetImageInfo(Cpu *c) { RET(DIERR_UNSUPPORTED_); }
/* a few keyboard helpers games use next to these */
static void s_GetKeyboardType(Cpu *c) { u32 t = (u32)ARG(0); RET(t == 0 ? 4 : t == 1 ? 0 : t == 2 ? 12 : 0); }
static void s_OemKeyScan(Cpu *c) { u32 ch = (u32)ARG(0) & 0xff; int vk = ch >= 'a' && ch <= 'z' ? ch - 32 : (int)ch; u8 sc = vk_to_dik(vk); RET(sc ? (u64)sc : 0xffffffffu); }
static void s_GetMouseMovePointsEx(Cpu *c) { RET((u64)(u32)-1); }
static void s_EnableMouseInPointer(Cpu *c) { RET(1); }

#define DI_M(m) SA("IDirectInput8::" #m, IDirectInput8_##m)
#define DD_M(m) SA("IDirectInputDevice8::" #m, IDirectInputDevice8_##m)
#define INPUT_SHIMS S(RegisterRawInputDevices), S(GetRegisteredRawInputDevices), S(GetRawInputData), S(GetRawInputBuffer), S(DefRawInputProc), S(GetRawInputDeviceList), S(GetRawInputDeviceInfoW), S(GetRawInputDeviceInfoA), \
  S(GetKeyboardType), S(OemKeyScan), S(GetMouseMovePointsEx), S(EnableMouseInPointer), S(DirectInput8Create), \
  SA("IDirectInput8::QueryInterface", DirectInput8_QueryInterface), SA("IDirectInput8::AddRef", DirectInput8_AddRef), SA("IDirectInput8::Release", DirectInput8_Release), DI_M(CreateDevice), DI_M(EnumDevices), DI_M(GetDeviceStatus), DI_M(RunControlPanel), DI_M(Initialize), DI_M(FindDevice), DI_M(EnumDevicesBySemantics), DI_M(ConfigureDevices), \
  SA("IDirectInputDevice8::QueryInterface", DirectInput8_QueryInterface), SA("IDirectInputDevice8::AddRef", DirectInput8_AddRef), SA("IDirectInputDevice8::Release", DirectInput8_Release), DD_M(GetCapabilities), DD_M(EnumObjects), DD_M(GetProperty), DD_M(SetProperty), DD_M(Acquire), DD_M(Unacquire), DD_M(GetDeviceState), DD_M(GetDeviceData), DD_M(SetDataFormat), DD_M(SetEventNotification), DD_M(SetCooperativeLevel), DD_M(GetObjectInfo), DD_M(GetDeviceInfo), DD_M(RunControlPanel), DD_M(Initialize), DD_M(CreateEffect), DD_M(EnumEffects), DD_M(GetEffectInfo), DD_M(GetForceFeedbackState), DD_M(SendForceFeedbackCommand), DD_M(EnumCreatedEffectObjects), DD_M(Escape), DD_M(Poll), DD_M(SendDeviceData), DD_M(EnumEffectsInFile), DD_M(WriteEffectToFile), DD_M(BuildActionMap), DD_M(SetActionMap), DD_M(GetImageInfo)
