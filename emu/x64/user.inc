/* user.inc — the window manager: a real window tree (top-level windows, owned popups, child controls), z-order, per-window
 * DCs clipped to the window, mouse hit-testing, keyboard focus, built-in control classes (Button, Static, Edit, ComboBox,
 * ListBox, progress), the dialog manager (DLGTEMPLATE/DLGTEMPLATEEX, CreateDialogParam, DialogBoxParam with a modal loop,
 * IsDialogMessage, EndDialog, MessageBox) and DrawText.  Included by gdi.c; shared by x64 and x86 processes.
 *
 * Model: every window has a guest wndproc address.  Built-in classes point at shim slots ("NebulaButtonProc" …), so
 * SendMessage/DispatchMessage/CallWindowProc are uniform (one guest_call per message) and subclassing works.  Painting
 * goes into one framebuffer (the "screen" = the main window's client area, enlarged when a popup needs more room); a
 * window DC is a view into that framebuffer at the window's client origin, clipped to its visible size. */

#define MAX_WND 192
#define HWND_FIRST 0x10042ull
#define HWND_OF(i) (HWND_FIRST + (u64)(i) * 16)
#define HWND_DESKTOP 0x10001ull
#define SCREEN_X0 64
#define SCREEN_Y0 64
#define CAPTION_H 24
#define MENU_H 20
#define SUPER_ADOPT(i, k) do { if (!g_wnd[i].builtin) { g_wnd[i].builtin = (k); g_wnd[i].superclassed = 1; } } while (0)
enum { BI_NONE = 0, BI_BUTTON, BI_STATIC, BI_EDIT, BI_COMBO, BI_LISTBOX, BI_SCROLLBAR, BI_DIALOG, BI_PROGRESS, BI_LINK, BI_COMBOLIST, BI_UNSUPPORTED, BI_MSGBOX, BI_MENU, BI_STATUS, BI_TOOLBAR, BI_TAB };
enum { CONT_WNDCREATE = 6 /* = CONT_CREATEWND in win32.c */, CONT_WNDDESTROY = 400, CONT_CTLPAINT = 401, CONT_DLGINIT = 402, CONT_MODAL = 403, CONT_DLGMSG = 404, CONT_SENDCMD = 405, CONT_DLGPROC = 406, CONT_ERASEBG = 407, CONT_SETPOS = 408, CONT_STREAMIN = 409, CONT_ILREAD = 410, CONT_WICREAD = 411, CONT_MENUOPEN = 412, CONT_MENUDRAW = 413, CONT_TABSEL = 414, CONT_SUBCLASS = 415, CONT_DIENUM = 416, CONT_RET_VAL_ = 5 };
#define MAX_ITEMS 512
typedef struct {
  int used, destroyed; int parent, owner; int builtin; int superclassed; /* app-registered class whose wndproc delegates to a built-in proc (VCL TNewButton …): behaves like the built-in for painting but all messages flow through the guest wndproc */ int pid;   /* owning process: messages from other processes are posted to its queue */ char clsname[40]; u64 wndproc; u64 cls_brush; u32 cls_style; int cls_extra;
  u32 style, exstyle; u64 id, inst;
  int x, y, w, h;            /* relative to the parent's client origin (top-level: framebuffer coordinates) */
  int ncx, ncy;              /* client offset inside the window (caption/border of secondary top-level windows) */
  char *text; int textcap; char tbuf[1024]; u32 bgcol; int bgcol_set; /* EM_SETBKGNDCOLOR */ char *stream; int stream_len, stream_cap; u64 stream_buf; u32 stream_flags; /* EM_STREAMIN in progress */ void *cc; /* common-control state (comctl.inc) */ u64 sub_proc[8], sub_id[8], sub_ref[8], sub_orig; int nsub, subcur; /* comctl32 SetWindowSubclass chain: subcur = index of the subclass currently running (nsub = none) */   /* window text: inline buffer, grown on the heap for long edit contents (licence viewers) */
  u64 userdata; u64 extra[16];
  int visible, enabled, invalid, erase;
  u64 font; u64 icon; u64 menu; u64 sysmenu; int bar_hot1;   /* menu bar: highlighted item + 1 */
  /* dialog */ int is_dlg; u64 dlgproc; int modal; int ended; u64 result; int def_id; u64 msgresult; int base_x, base_y;
  /* controls */ int checked, pushed, hot, focus_draw; int sel0, sel1, scroll_x, modified, limit; int nitems; char (*items)[128]; u64 *itemdata; int cursel, top_index, dropped, drop_wnd; int pmin, pmax, ppos, pstep; int zseq;
} Wnd;
static Wnd g_wnd[MAX_WND]; static int g_zseq;
static int g_focus = -1, g_capture = -1, g_active = -1;
static int g_mouse_x, g_mouse_y, g_mouse_btn;
static u64 g_slot_button, g_slot_static, g_slot_edit, g_slot_combo, g_slot_listbox, g_slot_dialog, g_slot_progress, g_slot_unsupported, g_slot_scrollbar, g_slot_msgbox, g_slot_combolist, g_slot_link, g_slot_menu, g_slot_status, g_slot_toolbar, g_slot_tab, g_slot_subclass;
static u64 g_dlg_font; static int g_msgbox_auto = 1;
/* registered window classes */
typedef struct { int used; int pid; char name[64]; u64 proc, brush, cursor, icon; u32 style; int extra; u64 inst; } WndClass;   /* pid: classes are per process (the procedure address only means something in its own address space) */
static WndClass g_cls[64];
static int g_timers_n; static struct { u64 id; int wnd; u32 ms; double next; u64 proc; } g_timers[32];
/* clipboard (text only, internal to the emulator) */
static char g_clip[8192]; static int g_clip_open;
static u32 g_sys_colors[31];
static void edge_rect(Surf *s, int l, int t, int r, int b, u32 tl, u32 br);

static int dlg_key_message(Cpu *c, int d, GuestMsg *m);
/* game input (input.inc) */ static int raw_key(Win32 *w, int vk, int down); static int raw_mouse(Win32 *w, int dx, int dy, int ev, int wheel); static void di_key(Win32 *w, int vk, int down); static void di_mouse(Win32 *w, int dx, int dy, int ev, int wheel); static void dienum_resume(Cpu *c, u64 rax);
/* common controls (comctl.inc) */ static void s_NebulaStatusProc(Cpu *c); static void s_NebulaToolbarProc(Cpu *c); static void s_NebulaTabProc(Cpu *c); static void tabsel_resume(Cpu *c, u64 rax);
/* menus (menu.inc) */ static void menu_bar_draw(Win32 *w, Surf *f, GdiObj *d, int i, int wx, int wy); static int menu_bar_hit(Win32 *w, int i, int fx, int fy); static int menu_top_wnd(void); static int menu_bar_press(Cpu *c, int i, u64 lp); static int menu_bar_mnemonic(Cpu *c, int i, u32 ch); static void menu_apply_bar(Win32 *w, int i); static int menu_of(u64 h); static void s_NebulaMenuProc(Cpu *c); static void menuopen_resume(Cpu *c, u64 rax); static void menudraw_resume(Cpu *c, u64 rax);
static void modal_reexec(Cpu *c);
static int wnd_of(u64 h) { if (h < HWND_FIRST || (h - HWND_FIRST) % 16) return -1; u64 i = (h - HWND_FIRST) / 16; if (i >= MAX_WND || !g_wnd[i].used) return -1; return (int)i; }
static int wnd_alive(int i) { return i >= 0 && i < MAX_WND && g_wnd[i].used && !g_wnd[i].destroyed; }
static int wnd_toplevel(int i) { return g_wnd[i].parent < 0; }
static int wnd_root(int i) { while (i >= 0 && g_wnd[i].parent >= 0) i = g_wnd[i].parent; return i; }
/* framebuffer (screen) coordinates of a window's client origin */
static void wnd_client_origin(int i, int *sx, int *sy) { int x = 0, y = 0; for (int k = i; k >= 0; k = g_wnd[k].parent) { x += g_wnd[k].x + g_wnd[k].ncx; y += g_wnd[k].y + g_wnd[k].ncy; } *sx = x; *sy = y; }
static void wnd_window_origin(int i, int *sx, int *sy) { int px = 0, py = 0; if (g_wnd[i].parent >= 0) wnd_client_origin(g_wnd[i].parent, &px, &py); *sx = px + g_wnd[i].x; *sy = py + g_wnd[i].y; }
static int wnd_client_w(int i) { return g_wnd[i].w - 2 * g_wnd[i].ncx; }
static int wnd_client_h(int i) { return g_wnd[i].h - g_wnd[i].ncy - g_wnd[i].ncx; }
static int wnd_visible_chain(int i) { for (int k = i; k >= 0; k = g_wnd[k].parent) if (!g_wnd[k].visible) return 0; return 1; }
static int wnd_enabled_chain(int i) { for (int k = i; k >= 0; k = g_wnd[k].parent) if (!g_wnd[k].enabled) return 0; return 1; }
static int wnd_is_child_of(int i, int anc) { for (int k = g_wnd[i].parent; k >= 0; k = g_wnd[k].parent) if (k == anc) return 1; return 0; }
static void wnd_invalidate(int i, int erase) { if (!wnd_alive(i)) return; g_wnd[i].invalid = 1; if (erase) g_wnd[i].erase = 1; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && g_wnd[k].parent == i) wnd_invalidate(k, erase); }
/* debug aid (X64_WNDDUMP): every live window with its geometry and paint state */
void user_dump_windows(void) { for (int k = 0; k < MAX_WND; k++) { Wnd *n = &g_wnd[k]; if (!n->used) continue; e_log("[wnd] #%d hwnd=%llx pid=%d \"%s\" \"%.40s\" parent=%d at %d,%d %dx%d nc=%d,%d style=%08x vis=%d inv=%d erase=%d destroyed=%d builtin=%d proc=%llx\n", k, (unsigned long long)HWND_OF(k), n->pid, n->clsname, n->text, n->parent, n->x, n->y, n->w, n->h, n->ncx, n->ncy, n->style, n->visible, n->invalid, n->erase, n->destroyed, n->builtin, (unsigned long long)n->wndproc); } }
/* a window painted underneath an open popup menu / dropped combo list: the popup repaints on top afterwards (window DCs are
   clipped to ancestors, not to overlapping top-level windows) */
static void popups_over(int i) { if (!wnd_alive(i)) return; int ax, ay; wnd_window_origin(i, &ax, &ay); int aw = g_wnd[i].w, ah = g_wnd[i].h; int r = wnd_root(i); int rz = r >= 0 ? g_wnd[r].zseq : 0;
  for (int k = 1; k < MAX_WND; k++) { Wnd *n = &g_wnd[k]; if (!n->used || n->destroyed || !n->visible || n->parent >= 0 || k == r || n->invalid) continue; int popup = n->builtin == BI_MENU || n->builtin == BI_COMBOLIST; if (!popup && n->zseq <= rz) continue;
    if (ax < n->x + n->w && ax + aw > n->x && ay < n->y + n->h && ay + ah > n->y) { if (popup) n->invalid = 1; else wnd_invalidate(k, 1); } } }
static void wnd_invalidate_all(void) { for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed) { g_wnd[k].invalid = 1; g_wnd[k].erase = 1; } }
/* the main window (index 0) is mirrored into w->win for the parts win32.c / d3d11 read */
static void sync_main(Win32 *w) { Wnd *m = &g_wnd[0]; w->win.created = m->used && !m->destroyed; w->win.hwnd = HWND_OF(0); w->win.w = wnd_client_w(0); w->win.h = wnd_client_h(0); w->win.style = (m->style & ~0x40000000u) | (m->invalid ? 0x40000000u : 0); w->win.wndproc = m->wndproc; w->win.shown = m->visible; w->win.brush = m->cls_brush; e_snprintf(w->win.title, sizeof w->win.title, "%s", m->text); }
/* a top-level window that is hidden, moved or destroyed leaves the desktop colour behind it (the other windows repaint themselves through wnd_invalidate_all) */
static void desktop_clear(Win32 *w, int i) { Wnd *n = &g_wnd[i]; if (!w->screen || n->parent >= 0) return; int x0 = n->x < 0 ? 0 : n->x, y0 = n->y < 0 ? 0 : n->y, x1 = n->x + n->w > w->screen_w ? w->screen_w : n->x + n->w, y1 = n->y + n->h > w->screen_h ? w->screen_h : n->y + n->h;
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) w->screen[(size_t)y * w->screen_w + x] = 0x3b3b3b; w->dirty = 1; }
static int any_window(void) { for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed) return 1; return 0; }
static void screen_ensure(Win32 *w, int cw, int ch) {
  if (cw < 16) cw = 16; if (ch < 16) ch = 16; if (cw > 4096) cw = 4096; if (ch > 4096) ch = 4096;
  if (w->screen && w->screen_w == cw && w->screen_h == ch) return;
  u32 *ns = emu_alloc((size_t)cw * ch * 4); for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) ns[(size_t)y * cw + x] = 0x3b3b3b;
  if (w->screen) { int mw = cw < w->screen_w ? cw : w->screen_w, mh = ch < w->screen_h ? ch : w->screen_h; for (int y = 0; y < mh; y++) e_memcpy(ns + (size_t)y * cw, w->screen + (size_t)y * w->screen_w, (size_t)mw * 4); emu_free(w->screen); }
  w->screen = ns; w->screen_w = cw; w->screen_h = ch; w->dirty = 1; wnd_invalidate_all();
}
/* UTF-16 guest string → UTF-8 host buffer (also accepts the ANSI flavour) */
static void text_in(Cpu *c, u64 a, int wide, char *out, size_t cap) { out[0] = 0; if (!a || a < 0x10000) return; if (wide) wstr_to_utf8(c, a, out, cap, -1); else { const char *s = rd_str(c, a, cap - 1); if (s) e_snprintf(out, cap, "%s", s); } }
static void wnd_text_reserve(Wnd *n, size_t need) { if (need + 1 <= (size_t)n->textcap) return; size_t cap = (size_t)n->textcap * 2; while (cap < need + 1) cap *= 2; char *nt = emu_alloc(cap); e_memcpy(nt, n->text, e_strlen(n->text) + 1); if (n->text != n->tbuf) emu_free(n->text); n->text = nt; n->textcap = (int)cap; }
static void wnd_set_text(Wnd *n, const char *s) { size_t l = e_strlen(s); wnd_text_reserve(n, l); e_memcpy(n->text, s, l + 1); }
/* window text straight from a guest string of any length (WM_SETTEXT on a multi-line edit may carry a whole licence) */
static void wnd_text_from_guest(Cpu *c, Wnd *n, u64 a, int wide) { if (!a || a < 0x10000) { n->text[0] = 0; return; } size_t chars = 0; if (wide) { while (chars < (1u << 20) && rd16(c, a + chars * 2)) chars++; } else { while (chars < (1u << 20) && rd8(c, a + chars)) chars++; }
  wnd_text_reserve(n, chars * 3); if (wide) wstr_to_utf8(c, a, n->text, (size_t)n->textcap, -1); else { u8 *p = mem_ptr(c->mem, a, chars + 1, 0); if (p) { e_memcpy(n->text, p, chars); n->text[chars] = 0; } else n->text[0] = 0; } }
static u32 text_out_cap(Cpu *c, u64 dst, const char *s, u32 cap, int wide) { if (!cap) return 0; if (wide) return put_w8(c, dst, s, cap); size_t l = e_strlen(s); if (l + 1 > cap) l = cap - 1; u8 *p = mem_ptr(c->mem, dst, l + 1, 1); if (p) { e_memcpy(p, s, l); p[l] = 0; } return (u32)l; }
static int font_dims(Win32 *w, u64 hfont, int *cw, int *fh) { GdiObj *f = gdi_get(w, hfont); int h = f && f->kind == K_FONT ? f->font_h : 16; if (h < 4) h = 4; *fh = h; int em = f && f->kind == K_FONT && f->font_em ? f->font_em : h; *cw = (em * 11 + 10) / 20; if (*cw < 3) *cw = 3; return f && f->kind == K_FONT ? f->font_bold : 0; }
static int utf8_prev(const char *s, int pos) { if (pos <= 0) return 0; pos--; while (pos > 0 && ((u8)s[pos] & 0xC0) == 0x80) pos--; return pos; }   /* byte offset of the previous code point */
static int utf8_nextb(const char *s, int pos, int len) { if (pos >= len) return len; pos++; while (pos < len && ((u8)s[pos] & 0xC0) == 0x80) pos++; return pos; }
static int utf8_off(const char *s, int cols) { int i = 0; while (cols > 0 && s[i]) { i = utf8_nextb(s, i, 1 << 30); cols--; } return i; }   /* byte offset of column `cols` */
static int text_width(Win32 *w, u64 hfont, const char *s, int n) { int cw, fh; font_dims(w, hfont, &cw, &fh); return cw * utf8_cols(s, n); }
static int utf8_len_chars(const char *s, int n) { int k = 0; for (int i = 0; i < n && s[i]; i++) if (((u8)s[i] & 0xC0) != 0x80) k++; return k; }

/* ---------- window DCs: a view into the framebuffer at the client origin, clipped to the visible part ---------- */
static void dc_bind_window(Win32 *w, GdiObj *dc, int i) {
  int ox, oy; wnd_client_origin(i, &ox, &oy); int cw = wnd_client_w(i), ch = wnd_client_h(i);
  /* clip to every ancestor's client area and to the screen */
  int x0 = ox, y0 = oy, x1 = ox + cw, y1 = oy + ch;
  for (int k = g_wnd[i].parent; k >= 0; k = g_wnd[k].parent) { int px, py; wnd_client_origin(k, &px, &py); int pw = wnd_client_w(k), ph = wnd_client_h(k); if (px > x0) x0 = px; if (py > y0) y0 = py; if (px + pw < x1) x1 = px + pw; if (py + ph < y1) y1 = py + ph; }
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w->screen_w) x1 = w->screen_w; if (y1 > w->screen_h) y1 = w->screen_h;
  dc->wnd = i + 1; dc->ox = ox < 0 ? 0 : ox; dc->oy = oy < 0 ? 0 : oy; dc->vw = x1 - dc->ox; dc->vh = y1 - dc->oy; if (dc->vw < 0) dc->vw = 0; if (dc->vh < 0) dc->vh = 0;
}
static u64 wnd_dc(Cpu *c, int i) { Win32 *w = W; u64 h = make_dc(w); GdiObj *dc = gdi_get(w, h); if (!dc) return 0; if (i >= 0) { dc_bind_window(w, dc, i); if (g_wnd[i].font) dc->font = g_wnd[i].font; } else { dc->wnd = -1; } return h; }
static void wnd_dc_free(Win32 *w, u64 h) { GdiObj *dc = gdi_get(w, h); if (dc && dc->kind == K_DC && h != window_dc) { dc->kind = 0; dcsave_drop(h); } }

/* ---------- messages ---------- */
static void post_msg_to(Win32 *w, u64 hwnd, u32 msg, u64 wp, u64 lp) {
  int next = (w->msg_tail + 1) % MAX_MSGS; if (next == w->msg_head) return;
  GuestMsg *m = &w->msgs[w->msg_tail]; m->hwnd = hwnd; m->msg = msg; m->wparam = wp; m->lparam = lp; m->time = (u32)(host_now_ms() - w->start_ms); w->msg_tail = next;
}
static void post_msg(Win32 *w, u32 msg, u64 wp, u64 lp) { post_msg_to(w, g_wnd[0].used ? HWND_OF(0) : 0, msg, wp, lp); }
void w32_post_msg(Win32 *w, u32 msg, u64 wp, u64 lp) { post_msg(w, msg, wp, lp); }
static u64 key_target(void) { int mt = menu_top_wnd(); if (mt >= 0) return HWND_OF(mt); if (wnd_alive(g_focus)) return HWND_OF(g_focus); if (wnd_alive(g_active)) return HWND_OF(g_active); return g_wnd[0].used ? HWND_OF(0) : 0; }
void w32_key(Win32 *w, int vk, int down) {
  if (vk < 0 || vk > 255) return; int prev = w->key_state[vk]; w->key_state[vk] = down;
  if (prev != down) di_key(w, vk, down); if (raw_key(w, vk, down)) return;   /* Raw Input registered with RIDEV_NOLEGACY: no WM_KEY* */
  u64 lp = 1 | ((u64)(vk & 0x7f) << 16) | (prev ? 1ull << 30 : 0) | (down ? 0 : (1ull << 31) | (1ull << 30));
  int sys = (vk == 0x12) || (w->key_state[0x12] && vk != 0x12 && vk != 0x11);
  post_msg_to(w, key_target(), down ? (sys ? 0x104 : 0x100) : (sys ? 0x105 : 0x101), (u64)vk, lp);
}
/* topmost visible window under a framebuffer point (children before parents, later siblings first) */
static int hit_in(int i, int fx, int fy) {
  if (!g_wnd[i].visible) return -1; int wx, wy; wnd_window_origin(i, &wx, &wy); if (fx < wx || fy < wy || fx >= wx + g_wnd[i].w || fy >= wy + g_wnd[i].h) return -1;
  int best = -1, bestz = -1; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent == i && g_wnd[k].zseq > bestz) { int r = hit_in(k, fx, fy); if (r >= 0) { best = r; bestz = g_wnd[k].zseq; } }
  return best >= 0 ? best : i;
}
static int hit_test(int fx, int fy) { int best = -1, bestz = -1; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent < 0 && g_wnd[k].zseq > bestz) { int r = hit_in(k, fx, fy); if (r >= 0) { best = r; bestz = g_wnd[k].zseq; } } return best; }
void w32_mouse(Win32 *w, int x, int y, int ev) {
  int dx = x - g_mouse_x, dy = y - g_mouse_y; g_mouse_x = x; g_mouse_y = y;
  if (ev == 1) g_mouse_btn |= 1; else if (ev == 2) g_mouse_btn &= ~1; else if (ev == 3) g_mouse_btn |= 2; else if (ev == 4) g_mouse_btn &= ~2; else if (ev == 5) g_mouse_btn |= 4; else if (ev == 6) g_mouse_btn &= ~4;
  di_mouse(w, dx, dy, ev == 7 ? 0 : ev, ev == 7 ? (x < 0 ? -120 : 120) : 0); if (raw_mouse(w, dx, dy, ev, ev == 7 ? (x < 0 ? -120 : 120) : 0)) return;   /* RIDEV_NOLEGACY */
  static const u32 msgs[] = { 0x200, 0x201, 0x202, 0x204, 0x205, 0x207, 0x208, 0x20A };
  int t = wnd_alive(g_capture) ? g_capture : hit_test(x, y); int mt = menu_top_wnd(); if (mt >= 0) t = mt;   /* an open menu takes all mouse input */ if (t < 0) t = g_wnd[0].used ? 0 : -1; if (t < 0) return;
  if (!wnd_enabled_chain(t)) { /* a disabled window (owner of a modal dialog): mouse input is dropped, like Windows */ if (ev != 0) return; }
  int cx, cy; wnd_client_origin(t, &cx, &cy); int lx = x - cx, ly = y - cy; u64 lp = (u64)(u16)lx | ((u64)(u16)ly << 16);
  if (ev == 1 && ly < 0 && mt < 0 && menu_bar_hit(w, t, x, y) != -1) { /* the menu bar: WM_NCLBUTTONDOWN(HTMENU) → DefWindowProc opens the popup */ post_msg_to(w, HWND_OF(t), 0xA1, 5, (u64)(u16)(x + SCREEN_X0) | ((u64)(u16)(y + SCREEN_Y0) << 16)); return; }
  if (ev == 7) { /* wheel: wParam high word = delta, lParam = screen coords */ post_msg_to(w, HWND_OF(t), 0x20A, ((u64)(u16)(x < 0 ? -120 : 120) << 16) | (u64)g_mouse_btn, (u64)(u16)(x + SCREEN_X0) | ((u64)(u16)(y + SCREEN_Y0) << 16)); return; }
  post_msg_to(w, HWND_OF(t), msgs[ev < 8 ? ev : 0], (u64)g_mouse_btn, lp);
}
void w32_wheel(Win32 *w, int delta) { di_mouse(w, 0, 0, 0, delta); if (raw_mouse(w, 0, 0, 7, delta)) return; int t = wnd_alive(g_focus) ? g_focus : hit_test(g_mouse_x, g_mouse_y); if (t < 0) return; post_msg_to(w, HWND_OF(t), 0x20A, ((u64)(u16)(s16)delta << 16) | (u64)g_mouse_btn, (u64)(u16)(g_mouse_x + SCREEN_X0) | ((u64)(u16)(g_mouse_y + SCREEN_Y0) << 16)); }
void w32_tick(Win32 *w) { (void)w; }
static void check_timers(Win32 *w) {
  double now = host_now_ms();
  for (int i = 0; i < g_timers_n; i++) if (now >= g_timers[i].next) { g_timers[i].next = now + g_timers[i].ms; int t = g_timers[i].wnd; u64 h = t >= 0 && wnd_alive(t) ? HWND_OF(t) : 0;
    /* one pending WM_TIMER per timer: do not flood the queue when the guest is slow */ int dup = 0; for (int q = w->msg_head; q != w->msg_tail; q = (q + 1) % MAX_MSGS) if (w->msgs[q].msg == 0x113 && w->msgs[q].hwnd == h && w->msgs[q].wparam == g_timers[i].id) { dup = 1; break; } if (!dup) post_msg_to(w, h, 0x113, g_timers[i].id, g_timers[i].proc); }
}
/* MSG: x64 hwnd@0 message@8 wParam@16 lParam@24 time@32 pt@36 (48 B); x86 hwnd@0 message@4 wParam@8 lParam@12 time@16 pt@20 (28 B) */
#define MSG_MSG(a) ((a) + (c->mode32 ? 4 : 8))
#define MSG_WP(a) ((a) + (c->mode32 ? 8 : 16))
#define MSG_LP(a) ((a) + (c->mode32 ? 12 : 24))
static void write_msg(Cpu *c, u64 a, GuestMsg *m) {
  if (c->mode32) { wr32(c, a, (u32)m->hwnd); wr32(c, a + 4, m->msg); wr32(c, a + 8, (u32)m->wparam); wr32(c, a + 12, (u32)m->lparam); wr32(c, a + 16, m->time); wr32(c, a + 20, (u32)(g_mouse_x + SCREEN_X0)); wr32(c, a + 24, (u32)(g_mouse_y + SCREEN_Y0)); return; }
  wr64(c, a, m->hwnd); wr32(c, a + 8, m->msg); wr32(c, a + 12, 0); wr64(c, a + 16, m->wparam); wr64(c, a + 24, m->lparam); wr32(c, a + 32, m->time); wr32(c, a + 36, (u32)(g_mouse_x + SCREEN_X0)); wr32(c, a + 40, (u32)(g_mouse_y + SCREEN_Y0)); wr32(c, a + 44, 0); }
/* paint order: top-level windows bottom-up, each followed by its children in z order; returns the first invalid visible window */
static int next_invalid_in(int i) { if (!g_wnd[i].visible) return -1; if (g_wnd[i].invalid) return i; int order[MAX_WND]; int n = 0; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent == i) order[n++] = k;
  for (int a = 1; a < n; a++) { int v = order[a], b = a - 1; while (b >= 0 && g_wnd[order[b]].zseq > g_wnd[v].zseq) { order[b + 1] = order[b]; b--; } order[b + 1] = v; }
  for (int a = 0; a < n; a++) { int r = next_invalid_in(order[a]); if (r >= 0) return r; } return -1; }
static int next_invalid(void) { int order[MAX_WND]; int n = 0; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent < 0) order[n++] = k;
  for (int a = 1; a < n; a++) { int v = order[a], b = a - 1; while (b >= 0 && g_wnd[order[b]].zseq > g_wnd[v].zseq) { order[b + 1] = order[b]; b--; } order[b + 1] = v; }
  for (int a = 0; a < n; a++) { int r = next_invalid_in(order[a]); if (r >= 0) return r; } return -1; }
static int next_message(Cpu *c, GuestMsg *out, int remove, u64 filter_hwnd) {
  Win32 *w = W; check_timers(w);
  if (w->msg_head != w->msg_tail) {
    if (filter_hwnd) { int fi = wnd_of(filter_hwnd); for (int q = w->msg_head; q != w->msg_tail; q = (q + 1) % MAX_MSGS) { int mi = wnd_of(w->msgs[q].hwnd); if (w->msgs[q].hwnd == filter_hwnd || (mi >= 0 && fi >= 0 && wnd_is_child_of(mi, fi))) { *out = w->msgs[q]; if (remove) { for (int r = q; r != w->msg_head; r = (r + MAX_MSGS - 1) % MAX_MSGS) w->msgs[r] = w->msgs[(r + MAX_MSGS - 1) % MAX_MSGS]; w->msg_head = (w->msg_head + 1) % MAX_MSGS; } return 1; } } }
    else { *out = w->msgs[w->msg_head]; if (remove) w->msg_head = (w->msg_head + 1) % MAX_MSGS; return 1; } }
  if (w->quit_posted && !filter_hwnd) { out->hwnd = 0; out->msg = 0x12; out->wparam = (u64)(u32)c->exit_code; out->lparam = 0; out->time = 0; if (remove) w->quit_posted = 0; return 1; }
  int inv = next_invalid(); if (inv >= 0 && (!filter_hwnd || HWND_OF(inv) == filter_hwnd || wnd_is_child_of(inv, wnd_of(filter_hwnd)))) { out->hwnd = HWND_OF(inv); out->msg = 0xF; out->wparam = out->lparam = 0; out->time = 0; return 1; }
  return 0;
}
static void s_PeekMessageA(Cpu *c) {
  Win32 *w = W; GuestMsg m; u32 flags = (u32)ARG(4);
  if (next_message(c, &m, flags & 1, ARG(1))) { write_msg(c, ARG(0), &m); RET(1); return; }
  if (w->blits_since_yield || w->yield_frames++ > 20000) { w->blits_since_yield = 0; w->yield_frames = 0; w32_retry_yield(c); return; }
  RET(0);
}
static void s_GetMessageA(Cpu *c) {
  GuestMsg m; if (next_message(c, &m, 1, ARG(1))) { write_msg(c, ARG(0), &m); RET(m.msg == 0x12 ? 0 : 1); return; }
  W->blits_since_yield = 0; w32_retry_yield(c);
}
/* US keyboard layout for WM_CHAR synthesis */
static u32 vk_to_char(Win32 *w, u64 vk) { int shift = w->key_state[0x10], ctrl = w->key_state[0x11]; u32 ch = 0;
  if (vk >= 'A' && vk <= 'Z') ch = ctrl ? (u32)vk - 'A' + 1 : shift ? (u32)vk : (u32)vk + 32;
  else if (vk >= '0' && vk <= '9') { static const char sh[] = ")!@#$%^&*("; ch = shift ? (u32)sh[vk - '0'] : (u32)vk; }
  else if (vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09) ch = (u32)vk;
  else if (vk >= 0x60 && vk <= 0x69) ch = '0' + (u32)vk - 0x60; else if (vk == 0x6A) ch = '*'; else if (vk == 0x6B) ch = '+'; else if (vk == 0x6D) ch = '-'; else if (vk == 0x6E) ch = '.'; else if (vk == 0x6F) ch = '/';
  else { static const struct { u8 vk; char n, s; } oem[] = { { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' }, { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' }, { 0xDE, '\'', '"' } };
    for (size_t i = 0; i < sizeof oem / sizeof oem[0]; i++) if (oem[i].vk == vk) { ch = (u32)(shift ? oem[i].s : oem[i].n); break; } }
  return ch; }
static void translate_msg(Cpu *c, u64 hwnd, u32 msg, u64 vk, u64 lp) { if (msg == 0x100 || msg == 0x104) { u32 ch = vk_to_char(W, vk); if (ch) post_msg_to(W, hwnd, msg == 0x104 ? 0x106 : 0x102, ch, lp); } }
static void s_TranslateMessage(Cpu *c) { u64 a = ARG(0); u32 msg = rd32(c, MSG_MSG(a)); translate_msg(c, rdp(c, a), msg, rdp(c, MSG_WP(a)), rdp(c, MSG_LP(a))); RET(msg == 0x100 || msg == 0x101 || msg == 0x104 || msg == 0x105); }
/* deliver a message to a window procedure (guest call; builtin classes are shim slots too) with a continuation that returns its result */
static void send_to(Cpu *c, int i, u32 msg, u64 wp, u64 lp, int cont_kind, u64 cont_val) { w32_push_cont(c, cont_kind, cont_val); w32_guest_call(c, g_wnd[i].wndproc, HWND_OF(i), msg, wp, lp); }
static void dispatch_to_wndproc(Cpu *c, u64 hwnd, u32 msg, u64 wp, u64 lp) {
  Win32 *w = W;
  if (msg == 0x113 && lp && lp > 0x10000) { w32_push_cont(c, CONT_RET_RAX, 0); w32_guest_call(c, lp, hwnd, msg, wp, (u64)(u32)(host_now_ms() - w->start_ms)); return; }
  int i = wnd_of(hwnd); if (i < 0 || g_wnd[i].destroyed || !g_wnd[i].wndproc) { RET(0); return; }
  if (g_wnd[i].pid != w->pid) { /* another process's window: its procedure lives in a different address space — queue the message there (SendMessage degrades to PostMessage; the reply is 1) */
    for (int k = 0; ; k++) { Win32 *o = proc_w32_at(k); if (!o) break; if (o == (Win32 *)1) continue; if (o->pid == g_wnd[i].pid) { post_msg_to(o, hwnd, msg, wp, lp); break; } } RET(1); return; }
  send_to(c, i, msg, wp, lp, CONT_RET_RAX, 0);
}
static void s_DispatchMessageA(Cpu *c) { u64 a = ARG(0); dispatch_to_wndproc(c, rdp(c, a), rd32(c, MSG_MSG(a)), rdp(c, MSG_WP(a)), rdp(c, MSG_LP(a))); }
static void s_SendMessageA(Cpu *c) { dispatch_to_wndproc(c, ARG(0), (u32)ARG(1), ARG(2), ARG(3)); }
static void s_SendMessageTimeoutW(Cpu *c) { if (ARG(6)) wrp(c, ARG(6), 0); dispatch_to_wndproc(c, ARG(0), (u32)ARG(1), ARG(2), ARG(3)); }
static void s_SendNotifyMessageW(Cpu *c) { dispatch_to_wndproc(c, ARG(0), (u32)ARG(1), ARG(2), ARG(3)); }
static void s_PostMessageA(Cpu *c) { u64 h = ARG(0); if (h == 0xffff || h == 0xffffffffull || h == ~0ull) { RET(1); return; } if (h && wnd_of(h) < 0) { W->last_error = 1400; RET(0); return; } post_msg_to(W, h, (u32)ARG(1), ARG(2), ARG(3)); RET(1); }
static void s_PostThreadMessageW(Cpu *c) { post_msg_to(W, 0, (u32)ARG(1), ARG(2), ARG(3)); RET(1); }
static void s_PostQuitMessage(Cpu *c) { W->quit_posted = 1; c->exit_code = (int)ARG(0); }
static void s_SetTimer(Cpu *c) { int wi = wnd_of(ARG(0)); u64 id = ARG(1) ? ARG(1) : 1; u32 ms = (u32)ARG(2); if (ms < 10) ms = 10; if (wi < 0 && !ARG(1)) id = 0x1000 + (u64)g_timers_n + 1;
  for (int i = 0; i < g_timers_n; i++) if (g_timers[i].id == id && g_timers[i].wnd == wi) { g_timers[i].ms = ms; g_timers[i].next = host_now_ms() + ms; g_timers[i].proc = ARG(3); RET(id); return; }
  if (g_timers_n < 32) { g_timers[g_timers_n].id = id; g_timers[g_timers_n].wnd = wi; g_timers[g_timers_n].ms = ms; g_timers[g_timers_n].next = host_now_ms() + ms; g_timers[g_timers_n].proc = ARG(3); g_timers_n++; } RET(id); }
static void s_KillTimer(Cpu *c) { int wi = wnd_of(ARG(0)); for (int i = 0; i < g_timers_n; i++) if (g_timers[i].id == ARG(1) && (g_timers[i].wnd == wi || wi < 0)) { g_timers[i] = g_timers[--g_timers_n]; RET(1); return; } RET(0); }
static void kill_timers_of(int wi) { for (int i = 0; i < g_timers_n;) { if (g_timers[i].wnd == wi) g_timers[i] = g_timers[--g_timers_n]; else i++; } }
/* MsgWaitForMultipleObjects(n, handles, all, ms, mask): handles are polled like WaitForMultipleObjects (a child process
   ending, an event, a thread), a pending message returns n, the timeout returns WAIT_TIMEOUT; otherwise the shim is retried */
static void msgwait_common(Cpu *c, u32 n, u64 arr, int all, u32 ms) { Win32 *w = W; GuestMsg m; int which = 0;
  int r = w32_wait_poll(c, n, arr, all, &which); if (r < 0) { w->last_error = 6; w->msgwait_rip = 0; RET(0xffffffffu); return; }
  if (r > 0) { w->msgwait_rip = 0; RET((u32)which); return; }
  if (next_message(c, &m, 0, 0) || w->quit_posted) { w->msgwait_rip = 0; RET(n); return; }
  if (ms != 0xffffffffu) { double now = host_now_ms(); if (w->msgwait_rip != c->rip) { w->msgwait_rip = c->rip; w->msgwait_dl = now + ms; } if (now >= w->msgwait_dl) { w->msgwait_rip = 0; RET(258); return; } }
  w->blits_since_yield = 0; w32_retry_yield(c); }
static void s_MsgWaitForMultipleObjects(Cpu *c) { msgwait_common(c, (u32)ARG(0), ARG(1), (u32)ARG(2) != 0, (u32)ARG(3)); }
static void s_MsgWaitForMultipleObjectsEx(Cpu *c) { msgwait_common(c, (u32)ARG(0), ARG(1), (ARG(4) & 1) != 0, (u32)ARG(3)); }
static void s_WaitMessage(Cpu *c) { msgwait_common(c, 0, 0, 0, 0xffffffffu); }   /* no arguments: wait for any message (was reading garbage as n/handles/timeout) */
static void s_GetQueueStatus(Cpu *c) { Win32 *w = W; RET(w->msg_head != w->msg_tail ? 0x00FF00FFull : 0); }
static void s_GetInputState(Cpu *c) { Win32 *w = W; RET(w->msg_head != w->msg_tail); }
static void s_InSendMessage(Cpu *c) { RET(0); }
static void s_ReplyMessage(Cpu *c) { RET(0); }
static void s_GetMessagePos(Cpu *c) { RET((u64)(u16)(g_mouse_x + SCREEN_X0) | ((u64)(u16)(g_mouse_y + SCREEN_Y0) << 16)); }
static void s_GetMessageTime(Cpu *c) { RET((u32)(host_now_ms() - W->start_ms)); }
static void s_GetMessageExtraInfo(Cpu *c) { RET(0); }

/* ---------- window classes ---------- */
static int cls_find(Win32 *w, const char *name) { for (int i = 0; i < 64; i++) if (g_cls[i].used && g_cls[i].pid == w->pid && !e_stricmp(g_cls[i].name, name)) return i; return -1; }
/* WNDCLASS(EX): x64 proc@8(16) extra@20(28) inst@24(32) icon@32(40) cursor@40(48) brush@48(56) menu@56(64) name@64(72); x86 proc@4(8) extra@12(16) inst@16(20) icon@20(24) cursor@24(28) brush@28(32) name@36(40) */
static void register_class(Cpu *c, u64 a, int ex, int wide) { Win32 *w = W; int m = c->mode32; u64 off = ex ? (m ? 4 : 0) : 0; /* WNDCLASSEX: x86 shifts every field by cbSize; x64 keeps the WNDCLASS offsets (style moves into the padding at +4) */ char name[64]; u64 np = rdp(c, a + off + (m ? 36 : 64));
  if (np < 0x10000) e_snprintf(name, sizeof name, "#%u", (u32)np); else text_in(c, np, wide, name, sizeof name);
  int i = cls_find(W, name); if (i >= 0 && g_cls[i].inst == rdp(c, a + off + (m ? 16 : 24))) { W->last_error = 1410; RET(0); return; }   /* ERROR_CLASS_ALREADY_EXISTS */
  if (i < 0) for (int k = 0; k < 64; k++) if (!g_cls[k].used) { i = k; break; } if (i < 0) { RET(0); return; }
  WndClass *k = &g_cls[i]; k->used = 1; k->pid = w->pid; e_snprintf(k->name, sizeof k->name, "%s", name); k->style = rd32(c, a + off + (ex && !m ? 4 : 0)); k->proc = rdp(c, a + off + (m ? 4 : 8)); k->extra = (int)rd32(c, a + off + (m ? 12 : 20)); k->inst = rdp(c, a + off + (m ? 16 : 24)); k->icon = rdp(c, a + off + (m ? 20 : 32)); k->cursor = rdp(c, a + off + (m ? 24 : 40)); k->brush = rdp(c, a + off + (m ? 28 : 48));
  w->wndclass_proc = k->proc; w->wndclass_registered = 1; e_snprintf(w->wndclass_name, sizeof w->wndclass_name, "%s", name); RET(0xC001 + i); }
static void s_RegisterClassA(Cpu *c) { register_class(c, ARG(0), 0, 0); }
static void s_RegisterClassExA(Cpu *c) { register_class(c, ARG(0), 1, 0); }
static void s_RegisterClassW(Cpu *c) { register_class(c, ARG(0), 0, 1); }
static void s_RegisterClassExW(Cpu *c) { register_class(c, ARG(0), 1, 1); }
static void s_UnregisterClassA(Cpu *c) { char name[64]; if (ARG(0) < 0x10000) { RET(1); return; } text_in(c, ARG(0), 0, name, sizeof name); int i = cls_find(W, name); if (i >= 0) g_cls[i].used = 0; RET(1); }
static void s_UnregisterClassW(Cpu *c) { char name[64]; if (ARG(0) < 0x10000) { RET(1); return; } text_in(c, ARG(0), 1, name, sizeof name); int i = cls_find(W, name); if (i >= 0) g_cls[i].used = 0; RET(1); }
static int builtin_of_class(const char *n) {
  if (!e_strnicmp(n, "RichEdit", 8) || !e_stricmp(n, "RICHEDIT50W")) return BI_EDIT;   /* rich edit = multi-line edit that understands RTF streams (licence viewers) */
  if (!e_stricmp(n, "Button")) return BI_BUTTON; if (!e_stricmp(n, "Static")) return BI_STATIC; if (!e_stricmp(n, "Edit")) return BI_EDIT; if (!e_stricmp(n, "ComboBox")) return BI_COMBO; if (!e_stricmp(n, "ListBox")) return BI_LISTBOX;
  if (!e_stricmp(n, "ScrollBar")) return BI_SCROLLBAR; if (!e_stricmp(n, "#32770")) return BI_DIALOG; if (!e_stricmp(n, "msctls_progress32")) return BI_PROGRESS; if (!e_stricmp(n, "SysLink")) return BI_LINK; if (!e_stricmp(n, "ComboLBox")) return BI_COMBOLIST; if (!e_strcmp(n, "#32768")) return BI_MENU;
  if (!e_stricmp(n, "msctls_statusbar32")) return BI_STATUS; if (!e_stricmp(n, "ToolbarWindow32")) return BI_TOOLBAR; if (!e_stricmp(n, "SysTabControl32")) return BI_TAB;
  if (!e_stricmp(n, "SysListView32") || !e_stricmp(n, "SysTreeView32") || !e_stricmp(n, "ReBarWindow32") || !e_stricmp(n, "tooltips_class32") || !e_stricmp(n, "msctls_trackbar32") || !e_stricmp(n, "msctls_updown32") || !e_stricmp(n, "SysHeader32") || !e_stricmp(n, "SysAnimate32") || !e_stricmp(n, "SysDateTimePick32") || !e_stricmp(n, "SysMonthCal32") || !e_stricmp(n, "ComboBoxEx32") || !e_stricmp(n, "SysIPAddress32") || !e_stricmp(n, "SysPager") || !e_stricmp(n, "MDIClient")) return BI_UNSUPPORTED;
  return BI_NONE; }
static u64 builtin_slot(int bi) { switch (bi) { case BI_BUTTON: return g_slot_button; case BI_STATIC: return g_slot_static; case BI_EDIT: return g_slot_edit; case BI_COMBO: return g_slot_combo; case BI_LISTBOX: return g_slot_listbox; case BI_MENU: return g_slot_menu; case BI_SCROLLBAR: return g_slot_scrollbar; case BI_DIALOG: return g_slot_dialog; case BI_PROGRESS: return g_slot_progress; case BI_STATUS: return g_slot_status; case BI_TOOLBAR: return g_slot_toolbar; case BI_TAB: return g_slot_tab; case BI_LINK: return g_slot_link; case BI_COMBOLIST: return g_slot_combolist; case BI_MSGBOX: return g_slot_dialog; default: return g_slot_unsupported; } }
static Win32 *g_slot_w;   /* slot addresses are per process (each process resolves shims in its own order) — recompute when another process asks */
static void builtin_init_slots(Win32 *w) { if (g_slot_button && g_slot_w == w) return; g_slot_w = w;
  g_slot_button = w32_shim_slot(w, "user32", "NebulaButtonProc"); g_slot_static = w32_shim_slot(w, "user32", "NebulaStaticProc"); g_slot_edit = w32_shim_slot(w, "user32", "NebulaEditProc"); g_slot_combo = w32_shim_slot(w, "user32", "NebulaComboProc");
  g_slot_listbox = w32_shim_slot(w, "user32", "NebulaListBoxProc"); g_slot_dialog = w32_shim_slot(w, "user32", "DefDlgProcW"); g_slot_progress = w32_shim_slot(w, "user32", "NebulaProgressProc"); g_slot_unsupported = w32_shim_slot(w, "user32", "NebulaUnsupportedProc"); g_slot_status = w32_shim_slot(w, "user32", "NebulaStatusProc"); g_slot_toolbar = w32_shim_slot(w, "user32", "NebulaToolbarProc"); g_slot_tab = w32_shim_slot(w, "user32", "NebulaTabProc"); g_slot_subclass = w32_shim_slot(w, "user32", "NebulaSubclassProc");
  g_slot_scrollbar = w32_shim_slot(w, "user32", "NebulaScrollBarProc"); g_slot_msgbox = w32_shim_slot(w, "user32", "NebulaMsgBoxProc"); g_slot_combolist = w32_shim_slot(w, "user32", "NebulaComboListProc"); g_slot_menu = w32_shim_slot(w, "user32", "NebulaMenuProc"); g_slot_link = w32_shim_slot(w, "user32", "NebulaLinkProc");
  { GdiObj *f = gdi_new(w, K_FONT); f->font_h = 13; f->font_em = 11; g_dlg_font = f->handle; } }
static void s_GetClassInfoExA(Cpu *c) { RET(0); }
static void s_GetClassInfoExW(Cpu *c) { char name[64]; if (ARG(1) < 0x10000) { RET(0); return; } text_in(c, ARG(1), 1, name, sizeof name); int i = cls_find(W, name); int bi = builtin_of_class(name); u64 a = ARG(2); int m = c->mode32; u64 off = m ? 4 : 0, so = m ? 4 : 4; /* WNDCLASSEX layout: see register_class */
  if (i < 0 && !bi) { W->last_error = 1411; RET(0); return; } u8 *p = mem_ptr(c->mem, a, m ? 48 : 80, 1); if (p) e_memset(p, 0, m ? 48 : 80); wr32(c, a, m ? 48 : 80);
  if (i >= 0) { wr32(c, a + so, g_cls[i].style); wrp(c, a + off + (m ? 4 : 8), g_cls[i].proc); wr32(c, a + off + (m ? 12 : 20), (u32)g_cls[i].extra); wrp(c, a + off + (m ? 16 : 24), g_cls[i].inst); wrp(c, a + off + (m ? 28 : 48), g_cls[i].brush); }
  else { builtin_init_slots(W); wrp(c, a + off + (m ? 4 : 8), builtin_slot(bi)); wr32(c, a + so, 0x0008 | 0x0003); }
  wrp(c, a + off + (m ? 36 : 64), ARG(1)); RET(1); }
static void s_GetClassInfoW(Cpu *c) { char name[64]; if (ARG(1) < 0x10000) { RET(0); return; } text_in(c, ARG(1), 1, name, sizeof name); int i = cls_find(W, name); int bi = builtin_of_class(name); u64 a = ARG(2); int m = c->mode32; if (i < 0 && !bi) { RET(0); return; } u8 *p = mem_ptr(c->mem, a, m ? 40 : 72, 1); if (p) e_memset(p, 0, m ? 40 : 72);
  builtin_init_slots(W); if (i >= 0) { wr32(c, a, g_cls[i].style); wrp(c, a + (m ? 4 : 8), g_cls[i].proc); wrp(c, a + (m ? 28 : 48), g_cls[i].brush); } else wrp(c, a + (m ? 4 : 8), builtin_slot(bi)); wrp(c, a + (m ? 36 : 64), ARG(1)); RET(1); }
static void s_GetClassInfoA(Cpu *c) { s_GetClassInfoW(c); }
static void s_GetClassNameA(Cpu *c) { int i = wnd_of(ARG(0)); const char *n = i >= 0 ? g_wnd[i].clsname : ""; RET(text_out_cap(c, ARG(1), n, (u32)ARG(2), 0)); }
static void s_GetClassNameW(Cpu *c) { int i = wnd_of(ARG(0)); const char *n = i >= 0 ? g_wnd[i].clsname : ""; RET(text_out_cap(c, ARG(1), n, (u32)ARG(2), 1)); }
static void s_RealGetWindowClassW(Cpu *c) { s_GetClassNameW(c); }
static void s_GetClassLongPtrW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } int idx = (int)(s32)ARG(1); int k = cls_find(W, g_wnd[i].clsname); switch (idx) { case -26: RET(g_wnd[i].cls_style); return; case -24: RET(g_wnd[i].wndproc); return; case -10: RET(g_wnd[i].cls_brush); return; case -14: RET(g_wnd[i].icon); return; case -12: RET(k >= 0 ? g_cls[k].cursor : 0x7A001); return; case -16: RET(g_wnd[i].inst); return; case -20: RET(g_wnd[i].cls_extra); return; case -18: RET(0); return; default: RET(0); return; } }
static void s_GetClassLongA(Cpu *c) { s_GetClassLongPtrW(c); }
static void s_SetClassLongPtrW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } int idx = (int)(s32)ARG(1); if (idx == -10) { u64 old = g_wnd[i].cls_brush; g_wnd[i].cls_brush = ARG(2); RET(old); return; } if (idx == -24) { int k = cls_find(W, g_wnd[i].clsname); u64 old = k >= 0 ? g_cls[k].proc : 0; if (k >= 0) g_cls[k].proc = ARG(2); RET(old); return; } RET(0); }

/* ---------- window creation / destruction ---------- */
static void s_AdjustWindowRectEx(Cpu *c) { u64 r = ARG(0); u32 style = (u32)ARG(1); if (style & 0x00C00000u) { wr32(c, r, rd32(c, r) - 8); wr32(c, r + 4, rd32(c, r + 4) - 31); wr32(c, r + 8, rd32(c, r + 8) + 8); wr32(c, r + 12, rd32(c, r + 12) + 8); } RET(1); }
static void s_AdjustWindowRect(Cpu *c) { s_AdjustWindowRectEx(c); }
static void s_AdjustWindowRectExForDpi(Cpu *c) { s_AdjustWindowRectEx(c); }
static int wnd_alloc(void) { for (int i = 1; i < MAX_WND; i++) if (!g_wnd[i].used) { e_memset(&g_wnd[i], 0, sizeof g_wnd[i]); g_wnd[i].text = g_wnd[i].tbuf; g_wnd[i].textcap = sizeof g_wnd[i].tbuf; g_wnd[i].used = 1; g_wnd[i].parent = g_wnd[i].owner = -1; g_wnd[i].enabled = 1; g_wnd[i].cursel = -1; g_wnd[i].drop_wnd = -1; g_wnd[i].zseq = ++g_zseq; return i; } return -1; }
static void items_free(Wnd *n) { if (n->items) { emu_free(n->items); n->items = 0; } if (n->itemdata) { emu_free(n->itemdata); n->itemdata = 0; } n->nitems = 0; }
/* create the window object; sends WM_CREATE to builtin classes directly and leaves guest classes to the CONT_WNDCREATE chain */
static int wnd_create_obj(Cpu *c, const char *cls, const char *title, u32 style, u32 exstyle, int x, int y, int cx, int cy, u64 parent_h, u64 id, u64 inst, u64 param, int wide, u64 title_a) {
  Win32 *w = W; builtin_init_slots(w); int parent = wnd_of(parent_h); int bi = BI_NONE; int ci = -1;
  if (cls[0] == '#' && cls[1] >= '0') { u32 ord = 0; for (const char *p = cls + 1; *p >= '0' && *p <= '9'; p++) ord = ord * 10 + (u32)(*p - '0'); if (ord == 32770) cls = "#32770"; else if (ord >= 0x80 && ord <= 0x85) { static const char *names[] = { "Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox" }; cls = names[ord - 0x80]; } }
  ci = cls_find(W, cls); if (ci < 0) bi = builtin_of_class(cls); else bi = BI_NONE;
  if (ci < 0 && !bi) { w32_report(w, "CreateWindow: class \"%s\" is not registered", cls); W->last_error = 1407; return -1; }
  int i; if (!g_wnd[0].used && !(style & 0x40000000u)) i = 0; else i = wnd_alloc(); if (i >= 0) g_wnd[i].pid = W->pid; if (i < 0) { W->last_error = 8; return -1; }
  if (i == 0) { e_memset(&g_wnd[0], 0, sizeof g_wnd[0]); g_wnd[0].text = g_wnd[0].tbuf; g_wnd[0].textcap = sizeof g_wnd[0].tbuf; g_wnd[0].used = 1; g_wnd[0].pid = W->pid; g_wnd[0].parent = g_wnd[0].owner = -1; g_wnd[0].enabled = 1; g_wnd[0].cursel = -1; g_wnd[0].drop_wnd = -1; g_wnd[0].zseq = ++g_zseq; }
  Wnd *n = &g_wnd[i]; n->builtin = bi; e_snprintf(n->clsname, sizeof n->clsname, "%s", cls); n->style = style; n->exstyle = exstyle; n->inst = inst; n->id = id; n->userdata = 0;
  if (ci >= 0) { n->wndproc = g_cls[ci].proc; n->cls_brush = g_cls[ci].brush; n->cls_style = g_cls[ci].style; n->cls_extra = g_cls[ci].extra; n->icon = g_cls[ci].icon; } else { n->wndproc = builtin_slot(bi); n->cls_brush = bi == BI_DIALOG ? 0 : 0; n->cls_style = 0x0B; }
  if (bi == BI_DIALOG) n->is_dlg = 1;
  if (style & 0x40000000u) { n->parent = parent >= 0 ? parent : 0; if (parent < 0 && !g_wnd[0].used) n->parent = -1; } else { n->parent = -1; n->owner = parent; }
  wnd_set_text(n, title); n->visible = 0; n->enabled = !(style & 0x08000000u); n->font = 0; n->limit = 30000;
  int cwdef = (u32)cx == 0x80000000u || cx < 0 ? 0 : cx, chdef = (u32)cy == 0x80000000u || cy < 0 ? 0 : cy;
  if (n->parent < 0) {   /* top-level */
    if (i == 0) {   /* the main window: its client area is the framebuffer */
      int cw = cwdef ? cwdef : 800, ch = chdef ? chdef : 600; if (style & 0x00C00000u) { cw -= 16; ch -= 39; } if (cw < 16) cw = 16; if (ch < 16) ch = 16; if (cw > 4096) cw = 4096; if (ch > 4096) ch = 4096;
      n->x = 0; n->y = 0; n->ncx = 0; n->ncy = 0; n->w = cw; n->h = ch; screen_ensure(w, cw, ch); n->style &= ~0x40000000u; }
    else {   /* secondary top-level (dialog, message box, popup): drawn inside the framebuffer with a caption bar */
      int cw = (u32)cx == 0x80000000u ? 300 : cwdef, ch = (u32)cy == 0x80000000u ? 200 : chdef; /* only CW_USEDEFAULT means "pick a size": a 0x0 popup (Delphi's TApplication window) stays 0x0 */ int caption = (style & 0x00C00000u) == 0x00C00000u || bi == BI_DIALOG || bi == BI_MSGBOX;
      if (style & 0x80000000u && !caption) { n->ncx = 0; n->ncy = 0; } else { n->ncx = 1; n->ncy = CAPTION_H; }
      if (bi == BI_COMBOLIST || bi == BI_MENU) { n->ncx = 0; n->ncy = 0; }
      n->w = cw + 2 * n->ncx; n->h = ch + n->ncy + n->ncx; if (n->w > 4096) n->w = 4096; if (n->h > 4096) n->h = 4096;
      if (!g_wnd[0].used) { /* no main window yet: this popup becomes the screen */ screen_ensure(w, n->w, n->h); }
      else if (bi != BI_COMBOLIST && bi != BI_MENU) screen_ensure(w, w->screen_w > n->w ? w->screen_w : n->w, w->screen_h > n->h ? w->screen_h : n->h);
      int px = (u32)x == 0x80000000u ? (w->screen_w - n->w) / 2 : x - SCREEN_X0, py = (u32)y == 0x80000000u ? (w->screen_h - n->h) / 2 : y - SCREEN_Y0;
      if (bi != BI_COMBOLIST && bi != BI_MENU) { if (px + n->w > w->screen_w) px = w->screen_w - n->w; if (py + n->h > w->screen_h) py = w->screen_h - n->h; } if (px < 0) px = 0; if (py < 0) py = 0; n->x = px; n->y = py; }
  } else { n->x = (u32)x == 0x80000000u ? 0 : x; n->y = (u32)y == 0x80000000u ? 0 : y; n->w = cwdef; n->h = chdef; n->ncx = n->ncy = 0; }
  if (i == 0) sync_main(w);
  if (n->parent < 0 && (i == 0 || n->ncy) && menu_of(id) >= 0) { n->menu = id; menu_apply_bar(w, i); }   /* CreateWindow hMenu of a framed top-level window */
  if (w->api_trace) { char msg[400]; e_snprintf(msg, sizeof msg, "[user] create %llx \"%s\" \"%.40s\" style=%08x at %d,%d %dx%d parent=%d\n", (unsigned long long)HWND_OF(i), cls, title, style, n->x, n->y, n->w, n->h, n->parent); host_write(3, msg, e_strlen(msg)); }
  (void)param; (void)wide; (void)title_a; return i;
}
/* CREATESTRUCT for WM_(NC)CREATE: x64 params@0 inst@8 menu@16 parent@24 cy@32 cx@36 y@40 x@44 style@48 name@56 class@64 exstyle@72; x86 params@0 inst@4 menu@8 parent@12 cy@16 cx@20 y@24 x@28 style@32 name@36 class@40 exstyle@44 */
static u64 make_createstruct(Cpu *c, int i, u64 param, u64 title_a, u64 cls_a) { Win32 *w = W; Wnd *n = &g_wnd[i]; u64 cs = w32_heap_alloc(w, 80, 1); u64 ph = n->parent >= 0 ? HWND_OF(n->parent) : n->owner >= 0 ? HWND_OF(n->owner) : 0;
  if (c->mode32) { wr32(c, cs, (u32)param); wr32(c, cs + 4, (u32)n->inst); wr32(c, cs + 8, (u32)n->id); wr32(c, cs + 12, (u32)ph); wr32(c, cs + 16, (u32)n->h); wr32(c, cs + 20, (u32)n->w); wr32(c, cs + 24, (u32)(n->y + (n->parent < 0 ? SCREEN_Y0 : 0))); wr32(c, cs + 28, (u32)(n->x + (n->parent < 0 ? SCREEN_X0 : 0))); wr32(c, cs + 32, n->style); wr32(c, cs + 36, (u32)title_a); wr32(c, cs + 40, (u32)cls_a); wr32(c, cs + 44, n->exstyle); }
  else { wr64(c, cs, param); wr64(c, cs + 8, n->inst); wr64(c, cs + 16, n->id); wr64(c, cs + 24, ph); wr32(c, cs + 32, (u32)n->h); wr32(c, cs + 36, (u32)n->w); wr32(c, cs + 40, (u32)(n->y + (n->parent < 0 ? SCREEN_Y0 : 0))); wr32(c, cs + 44, (u32)(n->x + (n->parent < 0 ? SCREEN_X0 : 0))); wr32(c, cs + 48, n->style); wr64(c, cs + 56, title_a); wr64(c, cs + 64, cls_a); wr32(c, cs + 72, n->exstyle); }
  return cs; }
static void builtin_proc_direct(Cpu *c, int i, u32 msg, u64 wp, u64 lp);
/* finish creation after WM_CREATE: WM_SIZE / WM_MOVE / show — guest classes get them through the CONT_WNDCREATE steps */
static void wnd_show_now(Cpu *c, int i) { Win32 *w = W; Wnd *n = &g_wnd[i]; if (n->visible) return; n->visible = 1; n->invalid = 1; n->erase = 1; wnd_invalidate(i, 1); if (n->parent < 0) { g_active = i; if (n->is_dlg || n->builtin == BI_MSGBOX) { /* focus goes to the dialog's first control if nothing has it */ } } if (i == 0) sync_main(w); w->dirty = 1; (void)c; }
static void create_window(Cpu *c, u64 cls_a, u64 title_a, int wide, u32 style, u32 exstyle, int x, int y, int cx, int cy, u64 parent_h, u64 id, u64 inst, u64 param) {
  Win32 *w = W; char cls[64], title[1024]; if (cls_a < 0x10000) e_snprintf(cls, sizeof cls, "#%u", (u32)cls_a); else text_in(c, cls_a, wide, cls, sizeof cls); text_in(c, title_a, wide, title, sizeof title);
  if (!e_stricmp(cls, "#32770") || !e_stricmp(cls, "#32768")) { /* dialog class (also used for message boxes) */ }
  int i = wnd_create_obj(c, cls, title, style, exstyle, x, y, cx, cy, parent_h, id, inst, param, wide, title_a); if (i < 0) { RET(0); return; }
  Wnd *n = &g_wnd[i];
  if (n->builtin) { builtin_proc_direct(c, i, 0x0001, 0, 0); if (c->status) return; if (n->parent >= 0 && g_wnd[n->parent].font && !n->font) n->font = g_wnd[n->parent].font; if (style & 0x10000000u) wnd_show_now(c, i); if (n->parent >= 0) { /* WM_PARENTNOTIFY is skipped */ } RET(HWND_OF(i)); return; }
  u64 cs = make_createstruct(c, i, param, title_a, cls_a);
  Win32 *ww = W; w32_push_cont(c, CONT_WNDCREATE, (u64)i); ww->cont[ww->ncont - 1].data[1] = cs;
  w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0081 /* WM_NCCREATE */, 0, cs); (void)w;
}
static void s_CreateWindowExA(Cpu *c) { create_window(c, ARG(1), ARG(2), 0, (u32)ARG(3), (u32)ARG(0), (int)(s32)ARG(4), (int)(s32)ARG(5), (int)(s32)ARG(6), (int)(s32)ARG(7), ARG(8), ARG(9), ARG(10), ARG(11)); }
static void s_CreateWindowExW(Cpu *c) { create_window(c, ARG(1), ARG(2), 1, (u32)ARG(3), (u32)ARG(0), (int)(s32)ARG(4), (int)(s32)ARG(5), (int)(s32)ARG(6), (int)(s32)ARG(7), ARG(8), ARG(9), ARG(10), ARG(11)); }
/* steps of the creation chain for guest classes: 0 after WM_NCCREATE, 1 after WM_CREATE, 2 after WM_SIZE, 3 after WM_MOVE, 4 after WM_SHOWWINDOW */
static void wndcreate_resume(Cpu *c, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; int step = w->cont[ci].idx++; Wnd *n = &g_wnd[i]; u64 cs = w->cont[ci].data[1];
  if (step == 0) { if (!(u32)rax) { n->used = 0; if (i == 0) sync_main(w); w32_cont_finish(c, 0); return; } w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0001, 0, cs); return; }
  if (step == 1) { if ((int)(s32)rax == -1) { n->used = 0; if (i == 0) sync_main(w); w32_cont_finish(c, 0); return; } w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0005, 0, ((u64)(wnd_client_h(i) & 0xffff) << 16) | (wnd_client_w(i) & 0xffff)); return; }
  if (step == 2) { int sx, sy; wnd_client_origin(i, &sx, &sy); w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0003, 0, ((u64)(u16)(sy + (n->parent < 0 ? SCREEN_Y0 : 0)) << 16) | (u16)(sx + (n->parent < 0 ? SCREEN_X0 : 0))); return; }
  if (step == 3 && (n->style & 0x10000000u)) { wnd_show_now(c, i); w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0018, 1, 0); return; }
  w32_cont_finish(c, HWND_OF(i));
}
/* DestroyWindow: WM_DESTROY to the window, then to its descendants, then WM_NCDESTROY (guest classes only), then free */
static struct { int list[64]; int n, pos, phase; int root; } g_destroy[6]; static int g_ndestroy;
static void destroy_collect(int i, int *list, int *n) { if (*n >= 64) return; list[(*n)++] = i; for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent == i) destroy_collect(k, list, n); }
static void wnd_free(int i) { Wnd *n = &g_wnd[i]; items_free(n); if (n->text && n->text != n->tbuf) emu_free(n->text); if (n->stream) { emu_free(n->stream); n->stream = 0; } if (n->cc) { emu_free(n->cc); n->cc = 0; } n->nsub = 0; n->subcur = 0; n->sub_orig = 0; n->text = n->tbuf; n->textcap = sizeof n->tbuf; n->tbuf[0] = 0; kill_timers_of(i); if (g_focus == i) g_focus = -1; if (g_capture == i) g_capture = -1; if (g_active == i) g_active = -1; n->used = 0; n->destroyed = 1; }
/* process exit: its windows vanish without WM_DESTROY (no code left to run them) and its window classes are released */
void user_proc_exit(Win32 *w) { for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].pid == w->pid && w->pid != 0) { g_wnd[k].destroyed = 1; g_wnd[k].visible = 0; wnd_free(k); if (k == 0) sync_main(w); }
  for (int i = 0; i < 64; i++) if (g_cls[i].used && g_cls[i].pid == w->pid && w->pid != 0) g_cls[i].used = 0; wnd_invalidate_all(); }
static void destroy_step(Cpu *c) {
  Win32 *w = W; int d = g_ndestroy - 1; for (;;) {
    if (g_destroy[d].pos >= g_destroy[d].n) { if (g_destroy[d].phase == 0) { g_destroy[d].phase = 1; g_destroy[d].pos = 0; continue; }
      for (int k = 0; k < g_destroy[d].n; k++) wnd_free(g_destroy[d].list[k]); g_ndestroy--; sync_main(w); wnd_invalidate_all(); w->dirty = 1;
      /* owned popups of a destroyed top-level go with it */ w32_cont_finish(c, 1); return; }
    int i = g_destroy[d].list[g_destroy[d].pos++]; Wnd *n = &g_wnd[i]; if (!n->used) continue;
    int direct = n->builtin && !n->superclassed && !(n->is_dlg && n->dlgproc);   /* dialogs with a dialog procedure see WM_DESTROY / WM_NCDESTROY through DefDlgProc */
    if (g_destroy[d].phase == 0) { if (n->visible) { n->visible = 0; desktop_clear(w, i); } if (direct) { builtin_proc_direct(c, i, 0x0002, 0, 0); continue; } w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0002, 0, 0); return; }
    else { n->destroyed = 1; if (direct) continue; w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0082, 0, 0); return; } }
}
static void destroy_window(Cpu *c, int i) { Win32 *w = W; if (g_ndestroy >= 6) { cpu_fault(c, "DestroyWindow nesting too deep"); return; }
  int d = g_ndestroy++; g_destroy[d].n = 0; g_destroy[d].pos = 0; g_destroy[d].phase = 0; g_destroy[d].root = i; destroy_collect(i, g_destroy[d].list, &g_destroy[d].n);
  for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && g_wnd[k].parent < 0 && g_wnd[k].owner == i && g_destroy[d].n < 64) destroy_collect(k, g_destroy[d].list, &g_destroy[d].n);
  if (g_wnd[i].parent < 0) { int ow = g_wnd[i].owner; if (ow >= 0 && wnd_alive(ow)) { g_active = ow; if (!wnd_alive(g_focus) || wnd_root(g_focus) == i) g_focus = ow; } }
  w32_push_cont(c, CONT_WNDDESTROY, (u64)i); (void)w; destroy_step(c); }
static void s_DestroyWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0 || g_wnd[i].destroyed) { W->last_error = 1400; RET(0); return; } destroy_window(c, i); }

/* ---------- painting: BeginPaint/GetDC hand out window DCs; a pending erase fills the client with the class brush ---------- */
static void wnd_frame(Cpu *c, int i);
static int looks_like_rtf(const char *t); static char *rtf_to_text(const char *r, size_t n);
static void ilr_resume(Cpu *c, u64 rax); static void wicread_resume(Cpu *c, u64 rax); static int dib_decode(Cpu *c, u64 bmi, u64 bits, u32 *dst, int dw, int dh, int flip_only_rows);
static void wnd_erase(Cpu *c, int i) { Win32 *w = W; Wnd *n = &g_wnd[i]; u64 hb = n->cls_brush; u32 px = 0xf0f0f0; int skip = 0;
  if (n->is_dlg || n->builtin == BI_MSGBOX) px = 0xf0f0f0; else { GdiObj *b = gdi_get(w, hb); if (b) { if (b->kind == K_BRUSH && b->pen_width == -1) skip = 1; px = b->color; } else if (hb >= 1 && hb <= 31) px = g_sys_colors[hb - 1]; else if (!hb) skip = 1; }
  if (!skip) { u64 hdc = wnd_dc(c, i); Surf s; if (hdc && dc_surf(c, hdc, &s)) { fill_rect_px(&s, 0, 0, s.w, s.h, px); mark_screen(w, hdc); } if (hdc) wnd_dc_free(w, hdc); }
  wnd_frame(c, i); }
static void wnd_frame(Cpu *c, int i) { Win32 *w = W; Wnd *n = &g_wnd[i];
  if (i == 0 && n->ncy) { /* the main window: menu bar at the top of the framebuffer */ u64 fdc = make_dc(w); GdiObj *d = gdi_get(w, fdc); if (d) { d->wnd = -1; Surf f; if (dc_surf(c, fdc, &f)) { GdiObj *fo = gdi_get(w, g_dlg_font); d->font = fo ? g_dlg_font : d->font; menu_bar_draw(w, &f, d, 0, 0, 0); w->dirty = 1; } d->kind = 0; } return; }
  if (n->parent < 0 && i != 0 && n->ncy && n->w > 2 * n->ncx && n->h > n->ncy + n->ncx) { /* caption bar + border of a secondary top-level window */ u64 fdc = make_dc(w); GdiObj *d = gdi_get(w, fdc); if (d) { d->wnd = -1; Surf f; if (dc_surf(c, fdc, &f)) { int wx, wy; wnd_window_origin(i, &wx, &wy); int active = g_active == i; fill_rect_px(&f, wx, wy, wx + n->w, wy + n->ncy, active ? 0xffffff : 0xf0f0f0); edge_rect(&f, wx, wy, wx + n->w, wy + n->h, active ? 0x3c7fb1 : 0xa0a0a0, active ? 0x3c7fb1 : 0xa0a0a0);
      d->text_color = px2cr(active ? 0x000000 : 0x6d6d6d); d->bk_mode = 1; GdiObj *fo = gdi_get(w, g_dlg_font); d->font = fo ? g_dlg_font : d->font; draw_text(&f, d, w, wx + 8, wy + (CAPTION_H - 13) / 2, n->text, (int)e_strlen(n->text)); draw_text(&f, d, w, wx + n->w - 20, wy + (CAPTION_H - 13) / 2, "x", 1); menu_bar_draw(w, &f, d, i, wx, wy); w->dirty = 1; } d->kind = 0; } } }
static void beginpaint_fill(Cpu *c, int i, u64 ps, u64 hdc, int erased) { u64 r = ps + (c->mode32 ? 8 : 12); wrp(c, ps, hdc); wr32(c, ps + (c->mode32 ? 4 : 8), erased ? 0 : 1); wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, (u32)wnd_client_w(i)); wr32(c, r + 12, (u32)wnd_client_h(i)); }
static void erasebg_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; u64 hdc = w->cont[ci].data[1], ps = w->cont[ci].data[2];
  if (!wnd_alive(i)) { wnd_dc_free(w, hdc); w32_cont_finish(c, 0); return; }
  if (!(u32)rax) { /* the window procedure declined: erase with the class brush */ wnd_erase(c, i); } else { mark_screen(w, hdc); wnd_frame(c, i); }
  beginpaint_fill(c, i, ps, hdc, 1); w32_cont_finish(c, hdc); }
static void s_BeginPaint(Cpu *c) { Win32 *w = W; int i = wnd_of(ARG(0)); u64 ps = ARG(1); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; int erase = n->erase; n->invalid = 0; n->erase = 0; if (i == 0) sync_main(w);
  u64 hdc = wnd_dc(c, i);
  if (erase && hdc && (!n->builtin || n->superclassed) && !(n->is_dlg && n->dlgproc) && n->wndproc) {   /* WM_ERASEBKGND goes through the guest window procedure (VCL / custom frameworks paint their own background) */
    w32_push_cont(c, CONT_ERASEBG, (u64)i); w->cont[w->ncont - 1].data[1] = hdc; w->cont[w->ncont - 1].data[2] = ps; w32_guest_call(c, n->wndproc, HWND_OF(i), 0x14, hdc, 0); return; }
  if (erase) wnd_erase(c, i);
  beginpaint_fill(c, i, ps, hdc, 1); RET(hdc); }
static void s_EndPaint(Cpu *c) { Win32 *w = W; u64 hdc = rdp(c, ARG(1)); { int i = wnd_of(ARG(0)); if (i >= 0) popups_over(i); } wnd_dc_free(w, hdc); w->dirty = 1; w->blits_since_yield++; RET(1); }
static void s_GetDC(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(ARG(0) == 0 || ARG(0) == HWND_DESKTOP ? wnd_dc(c, -1) : 0); return; } RET(wnd_dc(c, i)); }
static void s_GetWindowDC(Cpu *c) { Win32 *w = W; int i = wnd_of(ARG(0)); if (i < 0) { RET(wnd_dc(c, -1)); return; } u64 h = wnd_dc(c, i); GdiObj *dc = gdi_get(w, h); if (dc) { int wx, wy; wnd_window_origin(i, &wx, &wy); dc->ox = wx < 0 ? 0 : wx; dc->oy = wy < 0 ? 0 : wy; dc->vw = g_wnd[i].w; dc->vh = g_wnd[i].h; } RET(h); }
static void s_GetDCEx(Cpu *c) { s_GetDC(c); }
static void s_ReleaseDC(Cpu *c) { Win32 *w = W; { int i = wnd_of(ARG(0)); if (i >= 0) popups_over(i); } wnd_dc_free(w, ARG(1)); w->dirty = 1; RET(1); }
static void s_WindowFromDC(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); RET(dc && dc->wnd > 0 ? HWND_OF(dc->wnd - 1) : 0); }
static void s_GetUpdateRect(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } if (ARG(1)) { wr32(c, ARG(1), 0); wr32(c, ARG(1) + 4, 0); wr32(c, ARG(1) + 8, g_wnd[i].invalid ? (u32)wnd_client_w(i) : 0); wr32(c, ARG(1) + 12, g_wnd[i].invalid ? (u32)wnd_client_h(i) : 0); } if (ARG(2) && g_wnd[i].erase) { wnd_erase(c, i); g_wnd[i].erase = 0; } RET(g_wnd[i].invalid != 0); }
static void s_GetUpdateRgn(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 && g_wnd[i].invalid ? 2 : 1); }
static void s_ExcludeUpdateRgn(Cpu *c) { RET(2); }
static void s_PaintDesktop(Cpu *c) { RET(1); }

/* ---------- DefWindowProc ---------- */
static void def_window_proc(Cpu *c, int i, u32 msg, u64 wp, u64 lp) {
  Win32 *w = W; Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x81: RET(1); return;                                                                 /* WM_NCCREATE */
    case 0x0F: { if (n->erase && n->visible) wnd_erase(c, i); n->invalid = 0; n->erase = 0; if (i == 0) sync_main(w); RET(0); return; }      /* WM_PAINT: erase if still pending, validate */
    case 0x14: { u64 hb = n->cls_brush; u32 px = 0; GdiObj *b = gdi_get(w, hb); if (b) { if (b->kind == K_BRUSH && b->pen_width == -1) { RET(0); return; } px = b->color; } else if (hb >= 1 && hb <= 31) { extern u32 sys_color_px(u32); px = sys_color_px((u32)hb - 1); } else if (!hb) { RET(0); return; }
      Surf s; GdiObj *dc = gdi_get(w, wp); if (dc && dc_surf(c, wp, &s)) { fill_rect_px(&s, 0, 0, s.w, s.h, px); mark_screen(w, wp); } RET(1); return; }   /* WM_ERASEBKGND */
    case 0x10: { destroy_window(c, i); return; }                                                /* WM_CLOSE → DestroyWindow */
    case 0x0C: { char t[1024]; text_in(c, lp, 1, t, sizeof t); wnd_set_text(n, t); if (i == 0) sync_main(w); if (n->parent < 0 && i != 0) wnd_invalidate(i, 1); RET(1); return; }   /* WM_SETTEXT (W) */
    case 0x0D: { RET(text_out_cap(c, lp, n->text, (u32)wp, 1)); return; }                       /* WM_GETTEXT (W) */
    case 0x0E: RET(utf8_u16len(n->text)); return;                                                /* WM_GETTEXTLENGTH */
    case 0x30: n->font = wp; if (lp & 1) wnd_invalidate(i, 1); RET(0); return;                  /* WM_SETFONT */
    case 0x31: RET(n->font); return;                                                              /* WM_GETFONT */
    case 0x07: n->focus_draw = 1; RET(0); return; case 0x08: n->focus_draw = 0; RET(0); return;
    case 0x21: RET(1); return;                                                                    /* WM_MOUSEACTIVATE → MA_ACTIVATE */
    case 0x84: { RET(1); return; }                                                                /* WM_NCHITTEST → HTCLIENT */
    case 0x20: RET(1); return;                                                                    /* WM_SETCURSOR */
    case 0x201: { if (n->parent < 0 && i != 0) { g_active = i; } RET(0); return; }
    case 0x87: RET(0); return;                                                                    /* WM_GETDLGCODE */
    case 0x7F: RET(n->icon); return; case 0x80: { u64 old = n->icon; n->icon = lp; RET(old); return; }   /* WM_GETICON / WM_SETICON */
    case 0x112: { if ((wp & 0xFFF0) == 0xF060) { destroy_window(c, i); return; } RET(0); return; }   /* WM_SYSCOMMAND SC_CLOSE */
    case 0x18: RET(0); return; case 0x05: RET(0); return; case 0x03: RET(0); return; case 0x46: RET(0); return; case 0x47: RET(0); return; case 0x24: RET(0); return; case 0x83: RET(0); return; case 0x85: RET(0); return; case 0x86: RET(1); return;
    case 0x100: case 0x101: case 0x102: { /* keys that reach DefWindowProc: nothing */ RET(0); return; }
    case 0x104: case 0x105: case 0x106: { if (msg == 0x104 && wp == 0x73 /* Alt+F4 */) { destroy_window(c, i); return; } if (msg == 0x106 && menu_bar_mnemonic(c, i, (u32)wp)) return; RET(0); return; }
    case 0xA1: { if (wp == 5 && menu_bar_press(c, i, lp)) return; RET(0); return; }
    case 0x205: { /* WM_RBUTTONUP → WM_CONTEXTMENU (screen point) */ int cx, cy; wnd_client_origin(i, &cx, &cy); post_msg_to(w, HWND_OF(i), 0x7B, HWND_OF(i), (u64)(u16)((s16)(u16)lp + cx + SCREEN_X0) | ((u64)(u16)((s16)(u16)(lp >> 16) + cy + SCREEN_Y0) << 16)); RET(0); return; }
    case 0x7B: { if (n->parent >= 0 && wnd_alive(n->parent)) post_msg_to(w, HWND_OF(n->parent), 0x7B, wp, lp); RET(0); return; }   /* WM_CONTEXTMENU climbs to the parent */                 /* WM_NCLBUTTONDOWN on the menu bar */
    default: RET(0); return;
  }
}
static void s_DefWindowProcA(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1);
  if (msg == 0x0C) { char t[1024]; text_in(c, ARG(3), 0, t, sizeof t); wnd_set_text(&g_wnd[i], t); if (i == 0) sync_main(W); RET(1); return; }
  if (msg == 0x0D) { RET(text_out_cap(c, ARG(3), g_wnd[i].text, (u32)ARG(2), 0)); return; }
  def_window_proc(c, i, msg, ARG(2), ARG(3)); }
static void s_DefWindowProcW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } def_window_proc(c, i, (u32)ARG(1), ARG(2), ARG(3)); }
static void s_CallWindowProcW(Cpu *c) { u64 proc = ARG(0); if (!proc) { RET(0); return; } w32_push_cont(c, CONT_RET_RAX, 0); w32_guest_call(c, proc, ARG(1), ARG(2), ARG(3), ARG(4)); }
static void s_CallWindowProcA(Cpu *c) { s_CallWindowProcW(c); }

/* ---------- show / move / size / enable ---------- */
static void s_ShowWindow(Cpu *c) { Win32 *w = W; int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } int cmd = (int)ARG(1); int prev = g_wnd[i].visible;
  if (cmd == 0 /* SW_HIDE */) { g_wnd[i].style &= ~0x10000000u; if (prev) { g_wnd[i].visible = 0; desktop_clear(w, i); wnd_invalidate_all(); w->dirty = 1; if (g_focus == i || (g_focus >= 0 && wnd_is_child_of(g_focus, i))) g_focus = -1; } }
  else { g_wnd[i].style |= 0x10000000u; if (!prev) { wnd_show_now(c, i); if (!g_wnd[i].builtin || g_wnd[i].superclassed) post_msg_to(w, HWND_OF(i), 0x18, 1, 0); } if (g_wnd[i].parent < 0 && cmd != 4 && cmd != 7 && cmd != 8) g_active = i; }
  if (i == 0) sync_main(w); RET(prev); }
static void s_ShowWindowAsync(Cpu *c) { s_ShowWindow(c); }
static void s_UpdateWindow(Cpu *c) { Win32 *w = W; int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } if (g_wnd[i].visible && g_wnd[i].invalid) { w32_push_cont(c, CONT_RET_VAL_, 1); w32_guest_call(c, g_wnd[i].wndproc, HWND_OF(i), 0x0F, 0, 0); (void)w; return; } RET(1); }
static void s_InvalidateRect(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { if (!ARG(0)) wnd_invalidate_all(); RET(1); return; } g_wnd[i].invalid = 1; if (ARG(2)) g_wnd[i].erase = 1; if (i == 0) sync_main(W); RET(1); }
static void s_InvalidateRgn(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) { g_wnd[i].invalid = 1; if (ARG(2)) g_wnd[i].erase = 1; if (i == 0) sync_main(W); } RET(1); }
static void s_ValidateRect(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) { g_wnd[i].invalid = 0; g_wnd[i].erase = 0; if (i == 0) sync_main(W); } RET(1); }
static void s_ValidateRgn(Cpu *c) { s_ValidateRect(c); }
static void s_RedrawWindow(Cpu *c) { int i = wnd_of(ARG(0)); u32 fl = (u32)ARG(3); if (i < 0) { wnd_invalidate_all(); RET(1); return; } if (fl & 0x0001 /* RDW_INVALIDATE */) { if (fl & 0x80) wnd_invalidate(i, (fl & 4) != 0); else { g_wnd[i].invalid = 1; if (fl & 4) g_wnd[i].erase = 1; } } if (fl & 0x0008) { g_wnd[i].invalid = 0; } if (i == 0) sync_main(W); RET(1); }
static void s_GetClientRect(Cpu *c) { int i = wnd_of(ARG(0)); u64 r = ARG(1); int cw = i >= 0 ? wnd_client_w(i) : W->screen_w, ch = i >= 0 ? wnd_client_h(i) : W->screen_h; wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, (u32)cw); wr32(c, r + 12, (u32)ch); RET(i >= 0 || ARG(0) == HWND_DESKTOP); }
static void s_GetWindowRect(Cpu *c) { int i = wnd_of(ARG(0)); u64 r = ARG(1); if (i < 0) { wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, 1280); wr32(c, r + 12, 720); RET(ARG(0) == HWND_DESKTOP); return; } int sx, sy; wnd_window_origin(i, &sx, &sy); wr32(c, r, (u32)(sx + SCREEN_X0)); wr32(c, r + 4, (u32)(sy + SCREEN_Y0)); wr32(c, r + 8, (u32)(sx + SCREEN_X0 + g_wnd[i].w)); wr32(c, r + 12, (u32)(sy + SCREEN_Y0 + g_wnd[i].h)); RET(1); }
static void s_GetWindowInfo(Cpu *c) { int i = wnd_of(ARG(0)); u64 p = ARG(1); if (i < 0) { RET(0); return; } int sx, sy; wnd_window_origin(i, &sx, &sy); int cx, cy; wnd_client_origin(i, &cx, &cy); wr32(c, p, 60); wr32(c, p + 4, (u32)(sx + SCREEN_X0)); wr32(c, p + 8, (u32)(sy + SCREEN_Y0)); wr32(c, p + 12, (u32)(sx + SCREEN_X0 + g_wnd[i].w)); wr32(c, p + 16, (u32)(sy + SCREEN_Y0 + g_wnd[i].h)); wr32(c, p + 20, (u32)(cx + SCREEN_X0)); wr32(c, p + 24, (u32)(cy + SCREEN_Y0)); wr32(c, p + 28, (u32)(cx + SCREEN_X0 + wnd_client_w(i))); wr32(c, p + 32, (u32)(cy + SCREEN_Y0 + wnd_client_h(i))); wr32(c, p + 36, g_wnd[i].style); wr32(c, p + 40, g_wnd[i].exstyle); wr32(c, p + 44, g_active == i); wr32(c, p + 48, (u32)g_wnd[i].ncx); wr32(c, p + 52, (u32)g_wnd[i].ncx); wr16(c, p + 56, 0); wr16(c, p + 58, 0); RET(1); }
/* SetWindowPos family: WM_SIZE / WM_MOVE are delivered synchronously (VCL's UpdateBounds relies on it); returns 1 when a guest call was started — the caller must then return without RET, the API result is delivered by setpos_resume */
static int wnd_set_pos(Cpu *c, int i, int x, int y, int cx, int cy, u32 flags, u64 api_ret) {
  Win32 *w = W; Wnd *n = &g_wnd[i]; int moved = 0, sized = 0; int ox = n->x, oy = n->y, ow = n->w, oh = n->h;
  if (!(flags & 2 /* SWP_NOMOVE */)) { int nx = x, ny = y; if (n->parent < 0) { nx -= SCREEN_X0; ny -= SCREEN_Y0; } if (i == 0) { nx = 0; ny = 0; } else if (n->parent < 0) { /* the framebuffer is the whole visible screen: a top-level window that centres itself on the 1280x720 "desktop" (PuTTY, many setup wizards) is kept inside the presented area */ int sw = w->screen_w, sh = w->screen_h; if (nx + n->w > sw) nx = sw - n->w; if (ny + n->h > sh) ny = sh - n->h; if (nx < 0) nx = 0; if (ny < 0) ny = 0; } if (nx != n->x || ny != n->y) { n->x = nx; n->y = ny; moved = 1; } }
  if (!(flags & 1 /* SWP_NOSIZE */)) { int nw = cx, nh = cy; if (i == 0) { int cw = nw, ch = nh; if (n->style & 0x00C00000u) { cw -= 16; ch -= 39; } if (cw < 16) cw = 16; if (ch < 16) ch = 16; if (cw != n->w || ch != n->h - n->ncy) { n->w = cw; n->h = ch + n->ncy; screen_ensure(w, cw, n->h); sized = 1; } }
    else if (nw != n->w || nh != n->h) { n->w = nw < 0 ? 0 : nw; n->h = nh < 0 ? 0 : nh; sized = 1; if (n->parent < 0) screen_ensure(w, w->screen_w > n->x + n->w ? w->screen_w : n->x + n->w, w->screen_h > n->y + n->h ? w->screen_h : n->y + n->h); } }
  if (flags & 0x40 /* SWP_SHOWWINDOW */) { n->style |= 0x10000000u; wnd_show_now(c, i); } if (flags & 0x80 /* SWP_HIDEWINDOW */) { n->style &= ~0x10000000u; if (n->visible) desktop_clear(w, i); n->visible = 0; wnd_invalidate_all(); }
  if ((moved || sized) && n->visible && n->parent < 0 && i != 0) { int nx = n->x, ny = n->y, nw = n->w, nh = n->h; n->x = ox; n->y = oy; n->w = ow; n->h = oh; desktop_clear(w, i); n->x = nx; n->y = ny; n->w = nw; n->h = nh; }
  if (i == 0) sync_main(w);
  if (moved || sized) { wnd_invalidate_all(); w->dirty = 1; if (!n->builtin || n->superclassed) {
      u64 size_lp = ((u64)(wnd_client_h(i) & 0xffff) << 16) | (wnd_client_w(i) & 0xffff); int sx, sy; wnd_client_origin(i, &sx, &sy); u64 move_lp = ((u64)(u16)sy << 16) | (u16)sx;
      if (n->pid != w->pid || !n->wndproc) { if (sized) post_msg_to(w, HWND_OF(i), 0x05, 0, size_lp); if (moved) post_msg_to(w, HWND_OF(i), 0x03, 0, move_lp); return 0; }
      w32_push_cont(c, CONT_SETPOS, (u64)i); w->cont[w->ncont - 1].data[1] = api_ret; w->cont[w->ncont - 1].data[2] = sized && moved ? 1 : 0; w->cont[w->ncont - 1].data[3] = move_lp;
      w32_guest_call(c, n->wndproc, HWND_OF(i), sized ? 0x05 : 0x03, 0, sized ? size_lp : move_lp); return 1; } }
  return 0;
}
static void setpos_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; (void)rax; if (w->cont[ci].data[2] && wnd_alive(i)) { w->cont[ci].data[2] = 0; w32_guest_call(c, g_wnd[i].wndproc, HWND_OF(i), 0x03, 0, w->cont[ci].data[3]); return; } w32_cont_finish(c, w->cont[ci].data[1]); }
static void s_SetWindowPos(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u64 after = ARG(1); if (!(ARG(6) & 4) && (after == 0 || after == (u64)-1 || after == 0xffffffffull)) g_wnd[i].zseq = ++g_zseq; if (wnd_set_pos(c, i, (int)(s32)ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), (int)(s32)ARG(5), (u32)ARG(6), 1)) return; RET(1); }
static void s_MoveWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } if (ARG(5)) wnd_invalidate(i, 1); if (wnd_set_pos(c, i, (int)(s32)ARG(1), (int)(s32)ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), 0, 1)) return; RET(1); }
static void s_BeginDeferWindowPos(Cpu *c) { RET(0x5D001); }
static void s_DeferWindowPos(Cpu *c) { int i = wnd_of(ARG(1)); if (i >= 0 && wnd_set_pos(c, i, (int)(s32)ARG(3), (int)(s32)ARG(4), (int)(s32)ARG(5), (int)(s32)ARG(6), (u32)ARG(7), ARG(0))) return; RET(ARG(0)); }
static void s_EndDeferWindowPos(Cpu *c) { RET(1); }
static void s_GetWindowPlacement(Cpu *c) { int i = wnd_of(ARG(0)); u64 p = ARG(1); if (i < 0) { RET(0); return; } int sx, sy; wnd_window_origin(i, &sx, &sy); wr32(c, p, 44); wr32(c, p + 4, 0); wr32(c, p + 8, g_wnd[i].visible ? 1 : 0); wr32(c, p + 28, (u32)(sx + SCREEN_X0)); wr32(c, p + 32, (u32)(sy + SCREEN_Y0)); wr32(c, p + 36, (u32)(sx + SCREEN_X0 + g_wnd[i].w)); wr32(c, p + 40, (u32)(sy + SCREEN_Y0 + g_wnd[i].h)); RET(1); }
static void s_SetWindowPlacement(Cpu *c) { int i = wnd_of(ARG(0)); u64 p = ARG(1); if (i < 0) { RET(0); return; } u32 show = rd32(c, p + 8); int l = (s32)rd32(c, p + 28), t = (s32)rd32(c, p + 32), r = (s32)rd32(c, p + 36), b = (s32)rd32(c, p + 40); if (show && show != 0) wnd_show_now(c, i); if (wnd_set_pos(c, i, l, t, r - l, b - t, 0, 1)) return; RET(1); }
static void s_EnableWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } int prev = !g_wnd[i].enabled; int en = ARG(1) != 0; if (g_wnd[i].enabled != en) { g_wnd[i].enabled = en; wnd_invalidate(i, 1); if (!en && (g_focus == i || (g_focus >= 0 && wnd_is_child_of(g_focus, i)))) g_focus = -1; if (!g_wnd[i].builtin) { Wnd *n = &g_wnd[i]; if (n->pid == W->pid && n->wndproc) { /* WM_ENABLE is sent synchronously, like Windows: VCL's TApplication reacts to it inside DisableTaskWindows */ w32_push_cont(c, CONT_RET_VAL_, (u64)prev); w32_guest_call(c, n->wndproc, HWND_OF(i), 0x0A, (u64)en, 0); return; } post_msg_to(W, HWND_OF(i), 0x0A, en, 0); } } RET(prev); }
static void s_IsWindowEnabled(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 && wnd_enabled_chain(i)); }
static void s_IsWindow(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 && !g_wnd[i].destroyed); }
static void s_IsWindowVisible(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 && wnd_visible_chain(i)); }
static void s_IsIconic(Cpu *c) { RET(0); }
static void s_IsZoomed(Cpu *c) { RET(0); }
static void s_IsChild(Cpu *c) { int p = wnd_of(ARG(0)), i = wnd_of(ARG(1)); RET(p >= 0 && i >= 0 && wnd_is_child_of(i, p)); }
static void s_GetDesktopWindow(Cpu *c) { RET(HWND_DESKTOP); }
static void s_GetShellWindow(Cpu *c) { RET(HWND_DESKTOP); }
static void s_GetParent(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } if (g_wnd[i].parent >= 0) RET(HWND_OF(g_wnd[i].parent)); else if (g_wnd[i].owner >= 0) RET(HWND_OF(g_wnd[i].owner)); else RET(0); }
static void s_GetAncestor(Cpu *c) { int i = wnd_of(ARG(0)); u32 fl = (u32)ARG(1); if (i < 0) { RET(0); return; } if (fl == 1) RET(g_wnd[i].parent >= 0 ? HWND_OF(g_wnd[i].parent) : HWND_DESKTOP); else { int r = wnd_root(i); if (fl == 3) while (g_wnd[r].owner >= 0 && wnd_alive(g_wnd[r].owner)) r = g_wnd[r].owner; RET(HWND_OF(r)); } }
static void s_SetParent(Cpu *c) { int i = wnd_of(ARG(0)), p = wnd_of(ARG(1)); if (i < 0) { RET(0); return; } int old = g_wnd[i].parent; g_wnd[i].parent = p; if (p >= 0) g_wnd[i].style |= 0x40000000u; wnd_invalidate_all(); RET(old >= 0 ? HWND_OF(old) : HWND_DESKTOP); }
/* GetWindow(hwnd, cmd): 0 first sibling 1 last 2 next 3 prev 4 owner 5 child (first) 6 enabled popup */
static void s_GetWindow(Cpu *c) { int i = wnd_of(ARG(0)); u32 cmd = (u32)ARG(1); if (i < 0) { RET(0); return; } int parent = g_wnd[i].parent;
  if (cmd == 4) { RET(g_wnd[i].owner >= 0 && wnd_alive(g_wnd[i].owner) ? HWND_OF(g_wnd[i].owner) : 0); return; }
  if (cmd == 5) { int best = -1; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == i && (best < 0 || g_wnd[k].zseq < g_wnd[best].zseq)) best = k; RET(best >= 0 ? HWND_OF(best) : 0); return; }
  if (cmd == 6) { RET(ARG(0)); return; }
  int best = -1; for (int k = 0; k < MAX_WND; k++) { if (!wnd_alive(k) || g_wnd[k].parent != parent) continue;
    if (cmd == 0) { if (best < 0 || g_wnd[k].zseq < g_wnd[best].zseq) best = k; } else if (cmd == 1) { if (best < 0 || g_wnd[k].zseq > g_wnd[best].zseq) best = k; }
    else if (cmd == 2) { if (g_wnd[k].zseq > g_wnd[i].zseq && (best < 0 || g_wnd[k].zseq < g_wnd[best].zseq)) best = k; } else if (cmd == 3) { if (g_wnd[k].zseq < g_wnd[i].zseq && (best < 0 || g_wnd[k].zseq > g_wnd[best].zseq)) best = k; } }
  RET(best >= 0 ? HWND_OF(best) : 0); }
static void s_GetTopWindow(Cpu *c) { int i = wnd_of(ARG(0)); int best = -1; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == i && (best < 0 || g_wnd[k].zseq < g_wnd[best].zseq)) best = k; RET(best >= 0 ? HWND_OF(best) : 0); }
static void s_GetNextWindow(Cpu *c) { s_GetWindow(c); }
static void s_GetLastActivePopup(Cpu *c) { RET(ARG(0)); }
/* EnumChildWindows / EnumWindows / EnumThreadWindows: callback(hwnd, lparam) per window via continuations */
#define CONT_ENUMWND 107
static void enum_wnd_step(Cpu *c) { Win32 *w = W; int ci = w->ncont - 1; u64 *d = w->cont[ci].data; int parent = (int)(s64)d[0]; int from = (int)d[1]; int mode = (int)d[3];
  for (int k = from; k < MAX_WND; k++) { if (!wnd_alive(k)) continue; int ok = mode == 0 ? (g_wnd[k].parent == parent || (parent >= 0 && wnd_is_child_of(k, parent))) : g_wnd[k].parent < 0; if (!ok) continue; d[1] = (u64)(k + 1); w32_guest_call(c, d[2], HWND_OF(k), d[4], 0, 0); return; }
  w32_cont_finish(c, 1); }
static void s_EnumChildWindows(Cpu *c) { int p = wnd_of(ARG(0)); Win32 *w = W; w32_push_cont(c, CONT_ENUMWND, 0); u64 *d = w->cont[w->ncont - 1].data; d[0] = (u64)(s64)p; d[1] = 0; d[2] = ARG(1); d[3] = 0; d[4] = ARG(2); enum_wnd_step(c); }
static void s_EnumWindows(Cpu *c) { Win32 *w = W; w32_push_cont(c, CONT_ENUMWND, 0); u64 *d = w->cont[w->ncont - 1].data; d[0] = (u64)-1; d[1] = 0; d[2] = ARG(0); d[3] = 1; d[4] = ARG(1); enum_wnd_step(c); }
static void s_EnumThreadWindows(Cpu *c) { Win32 *w = W; w32_push_cont(c, CONT_ENUMWND, 0); u64 *d = w->cont[w->ncont - 1].data; d[0] = (u64)-1; d[1] = 0; d[2] = ARG(1); d[3] = 1; d[4] = ARG(2); enum_wnd_step(c); }
static void s_FindWindowA(Cpu *c) { char cls[64], title[256]; text_in(c, ARG(0), 0, cls, sizeof cls); text_in(c, ARG(1), 0, title, sizeof title); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent < 0 && (!cls[0] || !e_stricmp(g_wnd[k].clsname, cls)) && (!title[0] || !e_strcmp(g_wnd[k].text, title))) { RET(HWND_OF(k)); return; } RET(0); }
static void s_FindWindowW(Cpu *c) { char cls[64], title[256]; text_in(c, ARG(0), 1, cls, sizeof cls); text_in(c, ARG(1), 1, title, sizeof title); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent < 0 && (!cls[0] || !e_stricmp(g_wnd[k].clsname, cls)) && (!title[0] || !e_strcmp(g_wnd[k].text, title))) { RET(HWND_OF(k)); return; } RET(0); }
static void s_FindWindowExW(Cpu *c) { int p = wnd_of(ARG(0)); char cls[64], title[256]; text_in(c, ARG(2), 1, cls, sizeof cls); text_in(c, ARG(3), 1, title, sizeof title); int after = wnd_of(ARG(1)); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == p && (after < 0 || g_wnd[k].zseq > g_wnd[after].zseq) && (!cls[0] || !e_stricmp(g_wnd[k].clsname, cls)) && (!title[0] || !e_strcmp(g_wnd[k].text, title))) { RET(HWND_OF(k)); return; } RET(0); }
static void s_FindWindowExA(Cpu *c) { int p = wnd_of(ARG(0)); char cls[64], title[256]; text_in(c, ARG(2), 0, cls, sizeof cls); text_in(c, ARG(3), 0, title, sizeof title); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == p && (!cls[0] || !e_stricmp(g_wnd[k].clsname, cls)) && (!title[0] || !e_strcmp(g_wnd[k].text, title))) { RET(HWND_OF(k)); return; } RET(0); }
static void s_WindowFromPoint(Cpu *c) { s32 x = (s32)(u32)ARG(0), y = (s32)(u32)(ARG(0) >> 32); if (c->mode32) { x = (s32)(u32)ARG(0); y = (s32)(u32)ARG(1); } int t = hit_test(x - SCREEN_X0, y - SCREEN_Y0); RET(t >= 0 ? HWND_OF(t) : 0); }
static void s_ChildWindowFromPoint(Cpu *c) { int p = wnd_of(ARG(0)); if (p < 0) { RET(0); return; } s32 x, y; if (c->mode32) { x = (s32)(u32)ARG(1); y = (s32)(u32)ARG(2); } else { x = (s32)(u32)ARG(1); y = (s32)(u32)(ARG(1) >> 32); } int cx, cy; wnd_client_origin(p, &cx, &cy); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == p && x >= g_wnd[k].x && y >= g_wnd[k].y && x < g_wnd[k].x + g_wnd[k].w && y < g_wnd[k].y + g_wnd[k].h) { RET(HWND_OF(k)); return; } RET(ARG(0)); }
static void s_ChildWindowFromPointEx(Cpu *c) { s_ChildWindowFromPoint(c); }
static void s_GetWindowThreadProcessId(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), (u32)W->pid); RET((u32)W->th[0].tid); }
static void s_IsHungAppWindow(Cpu *c) { RET(0); }
static void s_IsWindowUnicode(Cpu *c) { RET(1); }
static void s_IsGUIThread(Cpu *c) { RET(1); }
static void s_FlashWindowEx(Cpu *c) { RET(0); }
static void s_FlashWindow(Cpu *c) { RET(0); }
static void s_SetLayeredWindowAttributes(Cpu *c) { RET(1); }
static void s_UpdateLayeredWindow(Cpu *c) { RET(1); }
static void s_AnimateWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) { if (ARG(2) & 0x10000) { g_wnd[i].visible = 0; wnd_invalidate_all(); } else wnd_show_now(c, i); } RET(1); }
static void s_SetWindowRgn(Cpu *c) { RET(1); }
static void s_GetWindowRgn(Cpu *c) { RET(0); }
static void s_LockWindowUpdate(Cpu *c) { RET(1); }
static void s_SetWindowDisplayAffinity(Cpu *c) { RET(1); }
static void s_GetWindowDisplayAffinity(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(1); }

/* ---------- text / long ptr / focus ---------- */
static void s_SetWindowTextA(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } char t[1024]; text_in(c, ARG(1), 0, t, sizeof t); if (g_wnd[i].builtin) { wnd_set_text(&g_wnd[i], t); if (g_wnd[i].builtin == BI_EDIT) { g_wnd[i].sel0 = g_wnd[i].sel1 = (int)e_strlen(g_wnd[i].text); } wnd_invalidate(i, 1); if (i == 0) sync_main(W); RET(1); return; }
  u64 tmp = w32_heap_alloc(W, (e_strlen(t) + 1) * 2, 1); put_w8(c, tmp, t, 0); send_to(c, i, 0x0C, 0, tmp, CONT_RET_RAX, 0); }
static void s_SetWindowTextW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } char t[1024]; text_in(c, ARG(1), 1, t, sizeof t); if (g_wnd[i].builtin) { if (g_wnd[i].builtin == BI_EDIT) { wnd_text_from_guest(c, &g_wnd[i], ARG(1), 1); if (looks_like_rtf(g_wnd[i].text)) { char *pl = rtf_to_text(g_wnd[i].text, e_strlen(g_wnd[i].text)); wnd_set_text(&g_wnd[i], pl); emu_free(pl); } g_wnd[i].top_index = 0; } else wnd_set_text(&g_wnd[i], t); if (g_wnd[i].builtin == BI_EDIT) { g_wnd[i].sel0 = g_wnd[i].sel1 = (int)e_strlen(g_wnd[i].text); } wnd_invalidate(i, 1); if (i == 0) sync_main(W); RET(1); return; }
  send_to(c, i, 0x0C, 0, ARG(1), CONT_RET_RAX, 0); }
static void s_GetWindowTextA(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { if ((u32)ARG(2)) wr8(c, ARG(1), 0); RET(0); return; } RET(text_out_cap(c, ARG(1), g_wnd[i].text, (u32)ARG(2), 0)); }
static void s_GetWindowTextW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { if ((u32)ARG(2)) wr16(c, ARG(1), 0); RET(0); return; } RET(text_out_cap(c, ARG(1), g_wnd[i].text, (u32)ARG(2), 1)); }
static void s_GetWindowTextLengthW(Cpu *c) { int i = wnd_of(ARG(0)); RET(i < 0 ? 0 : utf8_u16len(g_wnd[i].text)); }
static void s_GetWindowTextLengthA(Cpu *c) { int i = wnd_of(ARG(0)); RET(i < 0 ? 0 : e_strlen(g_wnd[i].text)); }
static void s_InternalGetWindowText(Cpu *c) { s_GetWindowTextW(c); }
static void s_SetWindowLongPtrA(Cpu *c) { int i = wnd_of(ARG(0)); int idx = (int)(s32)ARG(1); if (i < 0) { W->last_error = 1400; RET(0); return; } Wnd *n = &g_wnd[i]; u64 old;
  if (idx == -21) { old = n->userdata; n->userdata = ARG(2); } else if (idx == -4) { old = n->wndproc; n->wndproc = ARG(2); } else if (idx == -16) { old = n->style; n->style = (u32)ARG(2) | (n->style & 0x40000000u); } else if (idx == -20) { old = n->exstyle; n->exstyle = (u32)ARG(2); } else if (idx == -12) { old = n->id; n->id = ARG(2); } else if (idx == -8) { old = n->parent >= 0 ? HWND_OF(n->parent) : 0; } else if (idx == -6) { old = n->inst; n->inst = ARG(2); }
  else if (idx >= 0 && idx / 4 < 16) { int k = idx / (c->mode32 ? 4 : 8); if (c->mode32) { old = n->extra[idx / 4]; n->extra[idx / 4] = (u32)ARG(2); } else { if (k > 15) k = 15; old = n->extra[k]; n->extra[k] = ARG(2); } if (n->is_dlg && idx == (c->mode32 ? 4 : 8)) n->dlgproc = ARG(2); if (n->is_dlg && idx == 0) n->msgresult = ARG(2); } else old = 0;
  if (i == 0) sync_main(W); RET(old); }
static void s_GetWindowLongPtrA(Cpu *c) { int i = wnd_of(ARG(0)); int idx = (int)(s32)ARG(1); if (i < 0) { W->last_error = 1400; RET(0); return; } Wnd *n = &g_wnd[i];
  if (idx == -21) RET(n->userdata); else if (idx == -4) RET(n->wndproc); else if (idx == -16) RET(n->parent >= 0 ? (n->style | 0x40000000u) : (n->style & ~0x40000000u)); /* WS_CHILD must be reported for children: VCL's UpdateBounds converts GetWindowRect to parent coordinates only when it is set */ else if (idx == -20) RET(n->exstyle); else if (idx == -12) RET(n->id); else if (idx == -8) RET(n->parent >= 0 ? HWND_OF(n->parent) : 0); else if (idx == -6) RET(n->inst);
  else if (idx >= 0 && idx / 4 < 16) { if (c->mode32) RET(n->extra[idx / 4]); else { int k = idx / 8; if (k > 15) k = 15; RET(n->extra[k]); } } else RET(0); }
static void s_SetWindowLongA(Cpu *c) { s_SetWindowLongPtrA(c); }
static void s_GetWindowLongA(Cpu *c) { s_GetWindowLongPtrA(c); }
static void s_SetWindowLongPtrW(Cpu *c) { s_SetWindowLongPtrA(c); }
static void s_GetWindowLongPtrW(Cpu *c) { s_GetWindowLongPtrA(c); }
static void s_SetWindowLongW(Cpu *c) { s_SetWindowLongPtrA(c); }
static void s_GetWindowLongW(Cpu *c) { s_GetWindowLongPtrA(c); }
static void s_GetPropW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } char name[64]; if (ARG(1) < 0x10000) e_snprintf(name, sizeof name, "#%u", (u32)ARG(1)); else text_in(c, ARG(1), 1, name, sizeof name); /* props live in extra[8..15] as (hash, value) pairs */ u32 h = 0; for (const char *p = name; *p; p++) h = h * 31 + (u8)*p; for (int k = 8; k < 16; k += 2) if ((u32)g_wnd[i].extra[k] == h && g_wnd[i].extra[k]) { RET(g_wnd[i].extra[k + 1]); return; } RET(0); }
static void s_SetPropW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } char name[64]; if (ARG(1) < 0x10000) e_snprintf(name, sizeof name, "#%u", (u32)ARG(1)); else text_in(c, ARG(1), 1, name, sizeof name); u32 h = 0; for (const char *p = name; *p; p++) h = h * 31 + (u8)*p; if (!h) h = 1; for (int k = 8; k < 16; k += 2) if ((u32)g_wnd[i].extra[k] == h) { g_wnd[i].extra[k + 1] = ARG(2); RET(1); return; } for (int k = 8; k < 16; k += 2) if (!g_wnd[i].extra[k]) { g_wnd[i].extra[k] = h; g_wnd[i].extra[k + 1] = ARG(2); RET(1); return; } RET(0); }
static void s_RemovePropW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } char name[64]; if (ARG(1) < 0x10000) e_snprintf(name, sizeof name, "#%u", (u32)ARG(1)); else text_in(c, ARG(1), 1, name, sizeof name); u32 h = 0; for (const char *p = name; *p; p++) h = h * 31 + (u8)*p; if (!h) h = 1; for (int k = 8; k < 16; k += 2) if ((u32)g_wnd[i].extra[k] == h) { u64 v = g_wnd[i].extra[k + 1]; g_wnd[i].extra[k] = 0; g_wnd[i].extra[k + 1] = 0; RET(v); return; } RET(0); }
static void s_GetPropA(Cpu *c) { s_GetPropW(c); }
static void s_SetPropA(Cpu *c) { s_SetPropW(c); }
static void s_RemovePropA(Cpu *c) { s_RemovePropW(c); }
/* focus: WM_KILLFOCUS / WM_SETFOCUS are posted (not sent) so this never nests guest calls inside a shim that cannot continue */
static void set_focus(Cpu *c, int i) { Win32 *w = W; int old = g_focus; if (old == i) return; g_focus = i;
  if (wnd_alive(old)) { g_wnd[old].focus_draw = 0; wnd_invalidate(old, 1); if (g_wnd[old].builtin && !g_wnd[old].superclassed) builtin_proc_direct(c, old, 0x08, i >= 0 ? HWND_OF(i) : 0, 0); else post_msg_to(w, HWND_OF(old), 0x08, i >= 0 ? HWND_OF(i) : 0, 0); }
  if (wnd_alive(i)) { g_wnd[i].focus_draw = 1; wnd_invalidate(i, 1); if (g_wnd[i].builtin && !g_wnd[i].superclassed) builtin_proc_direct(c, i, 0x07, old >= 0 ? HWND_OF(old) : 0, 0); else post_msg_to(w, HWND_OF(i), 0x07, old >= 0 ? HWND_OF(old) : 0, 0); g_active = wnd_root(i); } }
static void s_SetFocus(Cpu *c) { int i = wnd_of(ARG(0)); int old = g_focus; if (ARG(0) && i < 0) { RET(0); return; } set_focus(c, i); RET(wnd_alive(old) ? HWND_OF(old) : 0); }
static void s_GetFocus(Cpu *c) { RET(wnd_alive(g_focus) ? HWND_OF(g_focus) : 0); }
static void s_SetForegroundWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) { g_active = wnd_root(i); g_wnd[g_active].zseq = ++g_zseq; wnd_invalidate_all(); } RET(1); }
static void s_GetForegroundWindow(Cpu *c) { RET(wnd_alive(g_active) ? HWND_OF(g_active) : g_wnd[0].used ? HWND_OF(0) : 0); }
static void s_GetActiveWindow(Cpu *c) { RET(wnd_alive(g_active) ? HWND_OF(g_active) : g_wnd[0].used ? HWND_OF(0) : 0); }
static void s_SetActiveWindow(Cpu *c) { int i = wnd_of(ARG(0)); int old = g_active; if (i >= 0) g_active = wnd_root(i); RET(wnd_alive(old) ? HWND_OF(old) : 0); }
static void s_BringWindowToTop(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) { g_wnd[i].zseq = ++g_zseq; wnd_invalidate_all(); } RET(1); }
static void s_SetCapture(Cpu *c) { int i = wnd_of(ARG(0)); int old = g_capture; g_capture = i; RET(wnd_alive(old) ? HWND_OF(old) : 0); }
static void s_ReleaseCapture(Cpu *c) { g_capture = -1; RET(1); }
static void s_GetCapture(Cpu *c) { RET(wnd_alive(g_capture) ? HWND_OF(g_capture) : 0); }
static void s_ClientToScreen(Cpu *c) { int i = wnd_of(ARG(0)); u64 p = ARG(1); int ox = 0, oy = 0; if (i >= 0) wnd_client_origin(i, &ox, &oy); wr32(c, p, rd32(c, p) + (u32)(ox + SCREEN_X0)); wr32(c, p + 4, rd32(c, p + 4) + (u32)(oy + SCREEN_Y0)); RET(1); }
static void s_ScreenToClient(Cpu *c) { int i = wnd_of(ARG(0)); u64 p = ARG(1); int ox = 0, oy = 0; if (i >= 0) wnd_client_origin(i, &ox, &oy); wr32(c, p, rd32(c, p) - (u32)(ox + SCREEN_X0)); wr32(c, p + 4, rd32(c, p + 4) - (u32)(oy + SCREEN_Y0)); RET(1); }
static void s_MapWindowPoints(Cpu *c) { int a = wnd_of(ARG(0)), b = wnd_of(ARG(1)); int ax = 0, ay = 0, bx = 0, by = 0; if (a >= 0) wnd_client_origin(a, &ax, &ay); else { ax = -SCREEN_X0; ay = -SCREEN_Y0; } if (b >= 0) wnd_client_origin(b, &bx, &by); else { bx = -SCREEN_X0; by = -SCREEN_Y0; } int dx = ax - bx, dy = ay - by; u32 n = (u32)ARG(3); for (u32 k = 0; k < n; k++) { u64 p = ARG(2) + k * 8; wr32(c, p, rd32(c, p) + (u32)dx); wr32(c, p + 4, rd32(c, p + 4) + (u32)dy); } RET((u64)(u16)dx | ((u64)(u16)dy << 16)); }
static void s_GetCursorPos(Cpu *c) { wr32(c, ARG(0), (u32)(g_mouse_x + SCREEN_X0)); wr32(c, ARG(0) + 4, (u32)(g_mouse_y + SCREEN_Y0)); RET(1); }
static void s_SetCursorPos(Cpu *c) { RET(1); }
static void s_ShowCursor(Cpu *c) { RET(ARG(0) ? 1 : (u64)-1); }
static void s_SetCursor(Cpu *c) { RET(0x7A001); }
static void s_GetCursor(Cpu *c) { RET(0x7A001); }
static void s_ClipCursor(Cpu *c) { RET(1); }
static void s_GetClipCursor(Cpu *c) { u64 r = ARG(0); wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, 1280); wr32(c, r + 12, 720); RET(1); }
static void s_LoadCursorA(Cpu *c) { RET(0x7A001); }
static void s_LoadCursorFromFileW(Cpu *c) { RET(0x7A001); }
static void s_LoadIconA(Cpu *c) { RET(0x7B001 + (ARG(1) & 0xfff) * 0x10); }
static void s_LoadIconWithScaleDown(Cpu *c) { if (!ARG(4)) { RET(0x80070057); return; } wrp(c, ARG(4), 0x7B001 + (ARG(1) & 0xfff) * 0x10); RET(0); }
static void s_LoadIconMetric(Cpu *c) { if (!ARG(3)) { RET(0x80070057); return; } wrp(c, ARG(3), 0x7B001 + (ARG(1) & 0xfff) * 0x10); RET(0); }
static void s_LoadImageA(Cpu *c) { RET(0x7C001 + (ARG(1) & 0xfff) * 0x10); }
static void s_LoadImageW(Cpu *c) { u32 type = (u32)ARG(2); if (type == 0 /* IMAGE_BITMAP */ && ARG(1) < 0x10000) { /* RT_BITMAP → a DIB at any depth becomes a real 32-bpp surface */ u64 e = rsrc_lookup(c, ARG(0), ARG(1), 2, 1, 0); if (e) { u64 base = ARG(0) ? ARG(0) : W->image_base; u64 bmi = base + rd32(c, e); u32 hsz = rd32(c, bmi); s32 bw = (s32)rd32(c, bmi + 4), bh = (s32)rd32(c, bmi + 8); u16 bpp = rd16(c, bmi + 14); u32 comp = rd32(c, bmi + 16), clr = rd32(c, bmi + 32); if (bh < 0) bh = -bh;
      if (bw > 0 && bh > 0 && bw < 4096 && bh < 4096 && (comp == 0 || comp == 3)) { u64 bits; u64 h = make_bitmap(c, bw, bh, 0, &bits); if (h) { u64 pal = bpp <= 8 ? (u64)(clr ? clr : 1u << bpp) * 4 : (comp == 3 ? 12 : 0); u32 *px = (u32 *)mem_ptr(c->mem, bits, (u64)bw * bh * 4, 1); if (px) dib_decode(c, bmi, bmi + hsz + pal, px, bw, bh, 0); RET(h); return; } } } }
  RET(0x7C001 + (ARG(1) & 0xfff) * 0x10); }
static void s_LoadBitmapW(Cpu *c) { call_shim_tail(c, s_LoadImageW, ARG(0), ARG(1), 0, 0); }
static void s_LoadBitmapA(Cpu *c) { call_shim_tail(c, s_LoadImageW, ARG(0), ARG(1), 0, 0); }
static void s_CopyImage(Cpu *c) { RET(ARG(0)); }
static void s_DestroyCursor(Cpu *c) { RET(1); }
static void s_DestroyIcon(Cpu *c) { RET(1); }
static void s_CreateIconIndirect(Cpu *c) { RET(0x7B801); }
static void s_CreateIconFromResourceEx(Cpu *c) { RET(0x7B901); }
static void s_LookupIconIdFromDirectoryEx(Cpu *c) { RET(1); }
static void s_DrawIconEx(Cpu *c) { RET(1); }
static void s_DrawIcon(Cpu *c) { RET(1); }
static void s_GetIconInfo(Cpu *c) { u64 p = ARG(1); wr32(c, p, 1); wr32(c, p + 4, 0); wr32(c, p + 8, 0); wrp(c, p + (c->mode32 ? 12 : 16), 0); wrp(c, p + (c->mode32 ? 16 : 24), 0); RET(1); }
static void s_GetAsyncKeyState(Cpu *c) { int vk = (int)ARG(0) & 0xff; RET(W->key_state[vk] ? 0x8000 : 0); }
static void s_GetKeyState(Cpu *c) { int vk = (int)ARG(0) & 0xff; RET(W->key_state[vk] ? 0x8000 : 0); }
static void s_GetKeyboardState(Cpu *c) { for (int i = 0; i < 256; i++) wr8(c, ARG(0) + i, W->key_state[i] ? 0x80 : 0); RET(1); }
static void s_MapVirtualKeyA(Cpu *c) { u32 t = (u32)ARG(1); u64 vk = ARG(0); if (t == 2) { int sh = W->key_state[0x10]; W->key_state[0x10] = 0; u32 ch = vk_to_char(W, vk); W->key_state[0x10] = sh; RET(ch); return; } RET(vk); }
static void s_MapVirtualKeyExW(Cpu *c) { s_MapVirtualKeyA(c); }
static void s_GetKeyboardLayout(Cpu *c) { RET(0x04090409); }
static void s_GetKeyboardLayoutList(Cpu *c) { if (ARG(1) && ARG(0)) wrp(c, ARG(1), 0x04090409); RET(1); }
static void s_ActivateKeyboardLayout(Cpu *c) { RET(0x04090409); }
static void s_GetKeyNameTextW(Cpu *c) { u32 vk = ((u32)ARG(0) >> 16) & 0xff; char n[16]; e_snprintf(n, sizeof n, "Key%u", vk); RET(text_out_cap(c, ARG(1), n, (u32)ARG(2), 1)); }
static void s_ToUnicode(Cpu *c) { u32 ch = vk_to_char(W, ARG(0)); if (!ch || (u32)ARG(4) == 0) { RET(0); return; } wr16(c, ARG(3), (u16)ch); if ((u32)ARG(4) > 1) wr16(c, ARG(3) + 2, 0); RET(1); }
static void s_ToUnicodeEx(Cpu *c) { s_ToUnicode(c); }
static void s_ToAscii(Cpu *c) { u32 ch = vk_to_char(W, ARG(0)); if (!ch) { RET(0); return; } wr16(c, ARG(3), (u16)ch); RET(1); }
static void s_VkKeyScanW(Cpu *c) { u32 ch = (u32)ARG(0); if (ch >= 'a' && ch <= 'z') RET(ch - 32); else if (ch >= 'A' && ch <= 'Z') RET(0x100 | ch); else if (ch >= '0' && ch <= '9') RET(ch); else RET(0xffff); }
static void s_VkKeyScanA(Cpu *c) { s_VkKeyScanW(c); }
static void s_keybd_event(Cpu *c) { w32_key(W, (int)(ARG(0) & 0xff) /* BYTE bVk: the upper register bits are garbage */, !(ARG(2) & 2)); }
static void s_mouse_event(Cpu *c) { RET(0); }
static void s_SendInput(Cpu *c) { Win32 *w = W; u32 n = (u32)ARG(0); u64 p = ARG(1); u32 cb = (u32)ARG(2); if (!p || cb < (u32)(c->mode32 ? 28 : 40)) { RET(0); return; } /* INPUT: type@0, union @8 (x64) / @4 (x86) */ u64 u = c->mode32 ? 4 : 8;
  for (u32 k = 0; k < n; k++, p += cb) { u32 type = rd32(c, p); if (type == 1) { u32 vk = rd16(c, p + u), fl = rd32(c, p + u + 4); if (fl & 4) { /* KEYEVENTF_UNICODE: a character */ u32 ch = rd16(c, p + u + 2); if (!(fl & 2)) post_msg_to(w, key_target(), 0x102, ch, 1); continue; } w32_key(w, (int)(vk & 0xff), !(fl & 2)); }
    else if (type == 0) { int dx = (int)(s32)rd32(c, p + u), dy = (int)(s32)rd32(c, p + u + 4); u32 fl = rd32(c, p + u + 12); int x = g_mouse_x, y = g_mouse_y; if (fl & 0x8000) { x = (int)((long long)dx * 1280 / 65536) - SCREEN_X0; y = (int)((long long)dy * 720 / 65536) - SCREEN_Y0; } else if (fl & 1) { x += dx; y += dy; }
      if (fl & 1) w32_mouse(w, x, y, 0); if (fl & 2) w32_mouse(w, x, y, 1); if (fl & 4) w32_mouse(w, x, y, 2); if (fl & 8) w32_mouse(w, x, y, 3); if (fl & 0x10) w32_mouse(w, x, y, 4); } }
  RET(n); }
static void s_BlockInput(Cpu *c) { RET(1); }
static void s_GetLastInputInfo(Cpu *c) { wr32(c, ARG(0) + 4, (u32)(host_now_ms() - W->start_ms)); RET(1); }
static void s_GetDoubleClickTime(Cpu *c) { RET(500); }
static void s_SetDoubleClickTime(Cpu *c) { RET(1); }
static void s_GetSystemMetrics(Cpu *c) { int i = (int)ARG(0); RET(i == 0 ? 1280 : i == 1 ? 720 : i == 2 ? 17 : i == 3 ? 17 : i == 4 ? 23 : i == 5 ? 1 : i == 6 ? 1 : i == 7 ? 1 : i == 8 ? 1 : i == 11 ? 32 : i == 12 ? 32 : i == 13 ? 32 : i == 14 ? 32 : i == 15 ? 20 : i == 16 ? 1280 : i == 17 ? 720 : i == 20 ? 17 : i == 21 ? 17 : i == 28 ? 16 : i == 29 ? 16 : i == 30 ? 16 : i == 31 ? 16 : i == 32 ? 4 : i == 33 ? 4 : i == 36 ? 4 : i == 37 ? 4 : i == 43 ? 2 : i == 45 ? 2 : i == 46 ? 2 : i == 47 ? 1280 : i == 48 ? 720 : i == 49 ? 2 : i == 50 ? 2 : i == 51 ? 18 : i == 52 ? 18 : i == 61 ? 1280 : i == 62 ? 720 : i == 76 ? 0 : i == 77 ? 0 : i == 78 ? 1280 : i == 79 ? 720 : i == 80 ? 1 : i == 92 ? 1 : 0); }
static void s_GetSystemMetricsForDpi(Cpu *c) { s_GetSystemMetrics(c); }
static void s_SystemParametersInfoA(Cpu *c) { u32 a = (u32)ARG(0); u64 p = ARG(2);
  if (a == 0x30 && p) { wr32(c, p, 0); wr32(c, p + 4, 0); wr32(c, p + 8, 1280); wr32(c, p + 12, 680); RET(1); return; }   /* SPI_GETWORKAREA */
  if (a == 0x29 && p) { /* SPI_GETNONCLIENTMETRICS: zero + font heights */ u32 sz = rd32(c, p); u8 *m = mem_ptr(c->mem, p, sz, 1); if (m) e_memset(m + 4, 0, sz - 4); for (u64 off = 24; off + 92 <= sz; off += 92 + 4) { wr32(c, p + off, (u32)-13); wr32(c, p + off + 16, 400); put_w8(c, p + off + 28, "MS Shell Dlg", 32); } RET(1); return; }
  if (a == 0x1F && p) { /* SPI_GETICONTITLELOGFONT */ u8 *m = mem_ptr(c->mem, p, 92, 1); if (m) e_memset(m, 0, 92); wr32(c, p, (u32)-13); wr32(c, p + 16, 400); put_w8(c, p + 28, "MS Shell Dlg", 32); RET(1); return; }
  if ((a == 0x1024 || a == 0x1026 || a == 0x1046 || a == 0x101E || a == 0x1034 || a == 0x1042 || a == 0x1048 || a == 0x1010) && p) { wr32(c, p, 0); RET(1); return; }
  if (a == 0x0F && p) { wr32(c, p, 0); RET(1); return; } if (a == 0x0D && p) { wr32(c, p, 0); RET(1); return; } if (a == 0x10 && p) { wr32(c, p, 0); RET(1); return; } if (a == 0x2002 && p) { wr32(c, p, 0); RET(1); return; } if (a == 0x0068 && p) { wr32(c, p, 3); RET(1); return; } if (a == 0x0062 && p) { wr32(c, p, 100); RET(1); return; } if (a == 0x0073 && p) { wr32(c, p, 1); RET(1); return; }
  RET(0); }
static void s_SystemParametersInfoW(Cpu *c) { s_SystemParametersInfoA(c); }
static void s_SystemParametersInfoForDpi(Cpu *c) { s_SystemParametersInfoA(c); }
static void s_GetMonitorInfoW(Cpu *c) { u64 p = ARG(1); u32 sz = rd32(c, p); wr32(c, p + 4, 0); wr32(c, p + 8, 0); wr32(c, p + 12, 1280); wr32(c, p + 16, 720); wr32(c, p + 20, 0); wr32(c, p + 24, 0); wr32(c, p + 28, 1280); wr32(c, p + 32, 680); wr32(c, p + 36, 1); if (sz >= 40 + 64) put_w8(c, p + 40, "\\\\.\\DISPLAY1", 32); RET(1); }
static void s_GetMonitorInfoA(Cpu *c) { s_GetMonitorInfoW(c); }
static void s_MonitorFromWindow(Cpu *c) { RET(0x6D001); }
static void s_MonitorFromPoint(Cpu *c) { RET(0x6D001); }
static void s_MonitorFromRect(Cpu *c) { RET(0x6D001); }
static void s_EnumDisplayMonitors(Cpu *c) { if (!ARG(2)) { RET(1); return; } u64 rc = w32_heap_alloc(W, 16, 1); wr32(c, rc + 8, 1280); wr32(c, rc + 12, 720); w32_push_cont(c, CONT_RET_VAL_, 1); w32_guest_call(c, ARG(2), 0x6D001, 0, rc, ARG(3)); }
static void s_GetDpiForMonitor(Cpu *c) { if (ARG(2)) wr32(c, ARG(2), 96); if (ARG(3)) wr32(c, ARG(3), 96); RET(0); }

/* ---------- DrawText ---------- */
/* DT_: TOP 0 LEFT 0 CENTER 1 RIGHT 2 VCENTER 4 BOTTOM 8 WORDBREAK 0x10 SINGLELINE 0x20 EXPANDTABS 0x40 NOCLIP 0x100 CALCRECT 0x400 NOPREFIX 0x800 END_ELLIPSIS 0x8000 */
static int draw_text_rect(Cpu *c, u64 hdc, const char *text, int len, int *l, int *t, int *r, int *b, u32 fmt) {
  Win32 *w = W; GdiObj *dc = gdi_get(w, hdc); if (!dc) return 0; int cw, fh; font_dims(w, dc->font, &cw, &fh); int rw = *r - *l; if (rw < cw) rw = cw;
  /* strip '&' prefixes unless DT_NOPREFIX; split into lines (\n, and word wrap when DT_WORDBREAK and not SINGLELINE) */
  char buf[2048]; int n = 0; int amp_pos = -1; for (int i = 0; i < len && n < 2040; i++) { char ch = text[i]; if (ch == '\r') continue; if (ch == '&' && !(fmt & 0x800)) { if (i + 1 < len && text[i + 1] == '&') { buf[n++] = '&'; i++; } else amp_pos = n; continue; } if (ch == '\t' && (fmt & 0x40)) { buf[n++] = ' '; continue; } buf[n++] = ch; } buf[n] = 0; (void)amp_pos;
  int starts[128], lens[128], nl = 0; int maxcols = rw / cw; if (maxcols < 1) maxcols = 1;
  int i = 0; while (i <= n && nl < 128) { int e = i; while (e < n && buf[e] != '\n') e++; if (fmt & 0x20) e = n;
    int seg = i; while (seg < e && nl < 128) { int cols = utf8_len_chars(buf + seg, e - seg); if (!(fmt & 0x10) || (fmt & 0x20) || cols <= maxcols) { starts[nl] = seg; lens[nl] = e - seg; nl++; seg = e; break; }
      int cut = seg, bytes = 0, chars = 0, last_space = -1; while (cut < e && chars < maxcols) { if (buf[cut] == ' ') last_space = cut; int cl = ((u8)buf[cut] >= 0xF0) ? 4 : ((u8)buf[cut] >= 0xE0) ? 3 : ((u8)buf[cut] >= 0xC0) ? 2 : 1; cut += cl; bytes += cl; chars++; }
      int endl = last_space > seg ? last_space : cut; starts[nl] = seg; lens[nl] = endl - seg; nl++; seg = endl; while (seg < e && buf[seg] == ' ') seg++; }
    if (seg == e && e == i) { starts[nl] = i; lens[nl] = 0; nl++; } i = e + 1; if (fmt & 0x20) break; }
  if (nl == 0) { starts[0] = 0; lens[0] = 0; nl = 1; }
  int maxw = 0; for (int k = 0; k < nl; k++) { int ww = utf8_len_chars(buf + starts[k], lens[k]) * cw; if (ww > maxw) maxw = ww; } int th = nl * fh;
  if (fmt & 0x400) { if (fmt & 0x20 || !(fmt & 0x10)) *r = *l + maxw; else if (maxw > rw) *r = *l + maxw; else *r = *l + rw; *b = *t + th; return th; }
  Surf s; if (!dc_surf(c, hdc, &s)) return th; int y0 = *t; int rh = *b - *t; if (fmt & 4) y0 = *t + (rh - th) / 2; else if (fmt & 8) y0 = *b - th;
  for (int k = 0; k < nl; k++) { int lw = utf8_len_chars(buf + starts[k], lens[k]) * cw; int x0 = *l; if (fmt & 1) x0 = *l + (rw - lw) / 2; else if (fmt & 2) x0 = *r - lw; int yy = y0 + k * fh; if (!(fmt & 0x100) && (yy + fh > *b + fh)) break;
    draw_text(&s, dc, w, x0, yy, buf + starts[k], lens[k]); }
  mark_screen(w, hdc); return th;
}
static void draw_text_api(Cpu *c, int wide, int ex) { u64 rp = ARG(3); u32 fmt = (u32)ARG(4); char t[2048]; int len = (int)(s32)ARG(2); if (len < 0) { text_in(c, ARG(1), wide, t, sizeof t); len = (int)e_strlen(t); } else { if (wide) { int n = len; if (n > 1000) n = 1000; wstr_to_utf8(c, ARG(1), t, sizeof t, n); } else { const u8 *p = mem_ptr(c->mem, ARG(1), (u64)len, 0); if (!p) { RET(0); return; } int n = len < 2047 ? len : 2047; e_memcpy(t, p, (size_t)n); t[n] = 0; } len = (int)e_strlen(t); }
  int l = (s32)rd32(c, rp), tp = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), b = (s32)rd32(c, rp + 12); (void)ex; int h = draw_text_rect(c, ARG(0), t, len, &l, &tp, &r, &b, fmt); if (fmt & 0x400) { wr32(c, rp + 8, (u32)r); wr32(c, rp + 12, (u32)b); } RET(h); }
static void s_DrawTextA(Cpu *c) { draw_text_api(c, 0, 0); }
static void s_DrawTextW(Cpu *c) { draw_text_api(c, 1, 0); }
static void s_DrawTextExW(Cpu *c) { draw_text_api(c, 1, 1); }
static void s_DrawTextExA(Cpu *c) { draw_text_api(c, 0, 1); }
static void s_TabbedTextOutW(Cpu *c) { call_shim_tail(c, s_TextOutW, ARG(0), ARG(1), ARG(2), ARG(3)); }
/* DrawEdge / DrawFrameControl / DrawFocusRect: the classic 3D looks */
static void edge_rect(Surf *s, int l, int t, int r, int b, u32 tl, u32 br) { hline(s, l, r - 1, t, tl); line_px(s, l, t, l, b - 1, tl); hline(s, l, r - 1, b - 1, br); line_px(s, r - 1, t, r - 1, b - 1, br); }
static void s_DrawEdge(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 rp = ARG(1); u32 edge = (u32)ARG(2), flags = (u32)ARG(3); int l = (s32)rd32(c, rp), t = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), b = (s32)rd32(c, rp + 12);
  u32 hi = 0xffffff, sh = 0xa0a0a0, dk = 0x696969, lt = 0xe3e3e3; int sunken = (edge & 0x0A) != 0;   /* BDR_SUNKENOUTER 2 / BDR_SUNKENINNER 8 */
  if (edge == 6 /* EDGE_ETCHED */) { edge_rect(&s, l, t, r, b, sh, hi); edge_rect(&s, l + 1, t + 1, r - 1, b - 1, hi, sh); } else { edge_rect(&s, l, t, r, b, sunken ? sh : hi, sunken ? hi : dk); if ((edge & 0x0C) || (edge & 0x03)) edge_rect(&s, l + 1, t + 1, r - 1, b - 1, sunken ? dk : lt, sunken ? lt : sh); }
  if (flags & 0x800 /* BF_MIDDLE */) fill_rect_px(&s, l + 2, t + 2, r - 2, b - 2, 0xf0f0f0); if (flags & 0x2000 /* BF_ADJUST */) { wr32(c, rp, (u32)(l + 2)); wr32(c, rp + 4, (u32)(t + 2)); wr32(c, rp + 8, (u32)(r - 2)); wr32(c, rp + 12, (u32)(b - 2)); } mark_screen(w, ARG(0)); RET(1); }
static void s_DrawFocusRect(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 rp = ARG(1); int l = (s32)rd32(c, rp), t = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), b = (s32)rd32(c, rp + 12); for (int x = l; x < r; x += 2) { put_px(&s, x, t, 0); put_px(&s, x, b - 1, 0); } for (int y = t; y < b; y += 2) { put_px(&s, l, y, 0); put_px(&s, r - 1, y, 0); } mark_screen(w, ARG(0)); RET(1); }
static void draw_check_mark(Surf *s, int x, int y, u32 col) { for (int k = 0; k < 3; k++) { put_px(s, x + k, y + 4 + k, col); put_px(s, x + k, y + 5 + k, col); } for (int k = 0; k < 5; k++) { put_px(s, x + 3 + k, y + 6 - k, col); put_px(s, x + 3 + k, y + 7 - k, col); } }
static void draw_radio(Surf *s, int l, int t, int on, int enabled) { int d = 13; fill_ellipse(s, l, t, l + d, t + d, 0xffffff, 1, enabled ? 0x333333 : 0xbcbcbc, 1); if (on) fill_ellipse(s, l + 4, t + 4, l + d - 4, t + d - 4, enabled ? 0x000000 : 0xbcbcbc, 1, 0, 0); }
static void s_DrawFrameControl(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 rp = ARG(1); u32 type = (u32)ARG(2), st = (u32)ARG(3); int l = (s32)rd32(c, rp), t = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), b = (s32)rd32(c, rp + 12);
  if (type == 4 /* DFC_BUTTON */) { u32 kind = st & 0x1F; int pushed = (st & 0x200) != 0, checked = (st & 0x400) != 0, dis = (st & 0x100) != 0;
    if (kind == 0x10 /* DFCS_BUTTONPUSH */) { fill_rect_px(&s, l, t, r, b, pushed ? 0xcce4f7 : 0xe1e1e1); edge_rect(&s, l, t, r, b, pushed ? 0x005499 : 0xadadad, pushed ? 0x005499 : 0xadadad); }
    else if (kind == 0 || kind == 1 || kind == 2 || kind == 3 /* check / 3-state */) { fill_rect_px(&s, l, t, l + 13, t + 13, dis ? 0xf0f0f0 : 0xffffff); edge_rect(&s, l, t, l + 13, t + 13, dis ? 0xbcbcbc : 0x333333, dis ? 0xbcbcbc : 0x333333); if (checked) draw_check_mark(&s, l + 2, t + 1, dis ? 0xa0a0a0 : 0); }
    else if (kind == 4 || kind == 5 || kind == 6 || kind == 7 /* radio */) draw_radio(&s, l, t, checked, !dis);
    mark_screen(w, ARG(0)); RET(1); return; }
  if (type == 3 /* DFC_SCROLL */) { fill_rect_px(&s, l, t, r, b, 0xf0f0f0); edge_rect(&s, l, t, r, b, 0xa0a0a0, 0xa0a0a0); int cx = (l + r) / 2, cy = (t + b) / 2; u32 k = st & 0xF; for (int i = 0; i < 4; i++) { if (k == 1) hline(&s, cx - i, cx + i, cy + i - 2, 0x404040); else if (k == 0) hline(&s, cx - i, cx + i, cy - i + 2, 0x404040); else if (k == 2) line_px(&s, cx + i - 2, cy - i, cx + i - 2, cy + i, 0x404040); else line_px(&s, cx - i + 2, cy - i, cx - i + 2, cy + i, 0x404040); } mark_screen(w, ARG(0)); RET(1); return; }
  fill_rect_px(&s, l, t, r, b, 0xf0f0f0); edge_rect(&s, l, t, r, b, 0xffffff, 0xa0a0a0); mark_screen(w, ARG(0)); RET(1); }
static void s_DrawStateW(Cpu *c) { RET(1); }
static void s_FrameRect(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 rp = ARG(1); GdiObj *b = gdi_get(w, ARG(2)); u32 px = b ? b->color : 0; int l = (s32)rd32(c, rp), t = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), bt = (s32)rd32(c, rp + 12); edge_rect(&s, l, t, r, bt, px, px); mark_screen(w, ARG(0)); RET(1); }
static void s_InvertRect(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 rp = ARG(1); int l = (s32)rd32(c, rp), t = (s32)rd32(c, rp + 4), r = (s32)rd32(c, rp + 8), b = (s32)rd32(c, rp + 12); if (l < 0) l = 0; if (t < 0) t = 0; if (r > s.w) r = s.w; if (b > s.h) b = s.h; for (int y = t; y < b; y++) { u32 *row = srow(&s, y); for (int x = l; x < r; x++) row[x] ^= 0xffffff; } mark_screen(w, ARG(0)); RET(1); }
static void s_DrawCaption(Cpu *c) { RET(1); }
static void s_GrayStringW(Cpu *c) { RET(1); }

/* ---------- clipboard (text, internal) ---------- */
static void s_OpenClipboard(Cpu *c) { g_clip_open = 1; RET(1); }
static void s_CloseClipboard(Cpu *c) { g_clip_open = 0; RET(1); }
static void s_EmptyClipboard(Cpu *c) { g_clip[0] = 0; RET(1); }
static void s_SetClipboardData(Cpu *c) { u32 fmt = (u32)ARG(0); u64 h = ARG(1); if (fmt == 13) wstr_to_utf8(c, h, g_clip, sizeof g_clip, -1); else if (fmt == 1 || fmt == 7) { const char *s = rd_str(c, h, sizeof g_clip - 1); e_snprintf(g_clip, sizeof g_clip, "%s", s ? s : ""); } RET(h); }
static void s_GetClipboardData(Cpu *c) { u32 fmt = (u32)ARG(0); if (!g_clip[0]) { RET(0); return; } if (fmt == 13) { u64 m = w32_heap_alloc(W, (e_strlen(g_clip) + 1) * 2, 1); put_w8(c, m, g_clip, 0); RET(m); return; } if (fmt == 1 || fmt == 7) { RET(w32_guest_strdup(W, g_clip)); return; } RET(0); }
static void s_IsClipboardFormatAvailable(Cpu *c) { u32 fmt = (u32)ARG(0); RET(g_clip[0] && (fmt == 13 || fmt == 1 || fmt == 7)); }
static void s_GetClipboardSequenceNumber(Cpu *c) { RET(1); }
static void s_CountClipboardFormats(Cpu *c) { RET(g_clip[0] ? 2 : 0); }
static void s_EnumClipboardFormats(Cpu *c) { u32 f = (u32)ARG(0); RET(!g_clip[0] ? 0 : f == 0 ? 13 : f == 13 ? 1 : 0); }
static void s_GetClipboardOwner(Cpu *c) { RET(0); }
static void s_SetClipboardViewer(Cpu *c) { RET(0); }
static void s_ChangeClipboardChain(Cpu *c) { RET(1); }
static void s_AddClipboardFormatListener(Cpu *c) { RET(1); }
static void s_RemoveClipboardFormatListener(Cpu *c) { RET(1); }
static void s_GetClipboardFormatNameW(Cpu *c) { RET(0); }
static void s_RegisterClipboardFormatA(Cpu *c) { RET(0xC100); }
static void s_RegisterClipboardFormatW(Cpu *c) { RET(0xC100); }
static void s_RegisterWindowMessageW(Cpu *c) { char n[128]; text_in(c, ARG(0), 1, n, sizeof n); u32 h = 0xC000; for (const char *p = n; *p; p++) h = (h * 31 + (u8)*p) & 0x3fff; RET(0xC000 + h); }
static void s_RegisterWindowMessageA(Cpu *c) { char n[128]; text_in(c, ARG(0), 0, n, sizeof n); u32 h = 0xC000; for (const char *p = n; *p; p++) h = (h * 31 + (u8)*p) & 0x3fff; RET(0xC000 + h); }

/* ---------- menus / scrollbars / accelerators: handles that behave, nothing drawn (honest limit) ---------- */
static void s_SetScrollInfo(Cpu *c) { RET(0); }
static void s_GetScrollInfo(Cpu *c) { u64 p = ARG(2); u32 sz = rd32(c, p); if (sz >= 28) { wr32(c, p + 8, 0); wr32(c, p + 12, 0); wr32(c, p + 16, 0); wr32(c, p + 20, 0); wr32(c, p + 24, 0); } RET(1); }
static void s_SetScrollPos(Cpu *c) { RET(0); }
static void s_GetScrollPos(Cpu *c) { RET(0); }
static void s_SetScrollRange(Cpu *c) { RET(1); }
static void s_GetScrollRange(Cpu *c) { if (ARG(2)) wr32(c, ARG(2), 0); if (ARG(3)) wr32(c, ARG(3), 0); RET(1); }
static void s_ShowScrollBar(Cpu *c) { RET(1); }
static void s_EnableScrollBar(Cpu *c) { RET(1); }
static void s_ScrollWindowEx(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) wnd_invalidate(i, 1); RET(0); }
static void s_ScrollWindow(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) wnd_invalidate(i, 1); RET(1); }
static void s_LoadAcceleratorsW(Cpu *c) { RET(0x6A001); }
static void s_LoadAcceleratorsA(Cpu *c) { RET(0x6A001); }
static void s_CreateAcceleratorTableW(Cpu *c) { RET(0x6A002); }
static void s_DestroyAcceleratorTable(Cpu *c) { RET(1); }
static void s_TranslateAcceleratorW(Cpu *c) { RET(0); }
static void s_TranslateAcceleratorA(Cpu *c) { RET(0); }
static void s_CreateCaret(Cpu *c) { RET(1); }
static void s_DestroyCaret(Cpu *c) { RET(1); }
static void s_ShowCaret(Cpu *c) { RET(1); }
static void s_HideCaret(Cpu *c) { RET(1); }
static void s_SetCaretPos(Cpu *c) { RET(1); }
static void s_GetCaretPos(Cpu *c) { wr32(c, ARG(0), 0); wr32(c, ARG(0) + 4, 0); RET(1); }
static void s_SetCaretBlinkTime(Cpu *c) { RET(1); }
static void s_GetCaretBlinkTime(Cpu *c) { RET(530); }
static void s_InitCommonControls(Cpu *c) { (void)c; }
static void s_InitCommonControlsEx(Cpu *c) { RET(1); }
static void s_CreateStatusWindowW(Cpu *c) { call_shim_tail(c, s_CreateWindowExW, 0, 0, 0, 0); RET(0); }
static void s_CreateToolbarEx(Cpu *c) { RET(0); }
/* comctl32 window subclassing: a chain of SUBCLASSPROCs (newest first) in front of the original window procedure; the window's
   wndproc becomes the NebulaSubclassProc slot, DefSubclassProc walks to the next older entry, then to the original procedure. */
static int sub_find(Wnd *n, u64 proc, u64 id) { for (int k = 0; k < n->nsub; k++) if (n->sub_proc[k] == proc && n->sub_id[k] == id) return k; return -1; }
static void sub_call(Cpu *c, int i, int k, u64 hwnd, u64 msg, u64 wp, u64 lp) { Wnd *n = &g_wnd[i]; if (k < 0 && !n->sub_orig) { def_window_proc(c, i, (u32)msg, wp, lp); return; } w32_push_cont(c, CONT_SUBCLASS, (u64)i); W->cont[W->ncont - 1].data[1] = (u64)n->subcur;
  if (k < 0) { n->subcur = -1; w32_guest_call(c, n->sub_orig, hwnd, msg, wp, lp); return; }
  n->subcur = k; w32_guest_call6(c, n->sub_proc[k], hwnd, msg, wp, lp, n->sub_id[k], n->sub_ref[k]); }
static void subclass_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; if (wnd_alive(i)) g_wnd[i].subcur = (int)(s64)w->cont[ci].data[1]; w32_cont_finish(c, rax); }
static void s_NebulaSubclassProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } sub_call(c, i, g_wnd[i].nsub - 1, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_SetWindowSubclass(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; int k = sub_find(n, ARG(1), ARG(2)); if (k >= 0) { n->sub_ref[k] = ARG(3); RET(1); return; } if (n->nsub >= 8) { RET(0); return; }
  builtin_init_slots(W); if (n->nsub == 0) { n->sub_orig = n->wndproc; n->wndproc = g_slot_subclass; } k = n->nsub++; n->sub_proc[k] = ARG(1); n->sub_id[k] = ARG(2); n->sub_ref[k] = ARG(3); n->subcur = n->nsub; RET(1); }
static void s_RemoveWindowSubclass(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; int k = sub_find(n, ARG(1), ARG(2)); if (k < 0) { RET(0); return; } for (int q = k; q + 1 < n->nsub; q++) { n->sub_proc[q] = n->sub_proc[q + 1]; n->sub_id[q] = n->sub_id[q + 1]; n->sub_ref[q] = n->sub_ref[q + 1]; } n->nsub--; if (n->subcur > k) n->subcur--;
  if (n->nsub == 0 && n->wndproc == g_slot_subclass) { n->wndproc = n->sub_orig; n->sub_orig = 0; } RET(1); }
static void s_DefSubclassProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; int k = n->subcur; if (k < 0) { if (n->sub_orig) { w32_push_cont(c, CONT_RET_RAX, 0); w32_guest_call(c, n->sub_orig, ARG(0), ARG(1), ARG(2), ARG(3)); } else def_window_proc(c, i, (u32)ARG(1), ARG(2), ARG(3)); return; } if (k > n->nsub) k = n->nsub; sub_call(c, i, k - 1, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_GetWindowSubclass(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } int k = sub_find(&g_wnd[i], ARG(1), ARG(2)); if (k >= 0 && ARG(3)) wrp(c, ARG(3), g_wnd[i].sub_ref[k]); RET(k >= 0); }
static void s_TaskDialogIndirect(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 1); if (ARG(2)) wr32(c, ARG(2), 0); if (ARG(3)) wr32(c, ARG(3), 0); RET(0); }
static void s_TaskDialog(Cpu *c) { if (ARG(6)) wr32(c, ARG(6), 1); RET(0); }

/* ---------- built-in controls ---------- */
static u32 g_sys_colors[31] = { 0xc0c0c0, 0x000000, 0x800000, 0x808080, 0xf0f0f0, 0xffffff, 0x646464, 0x000000, 0x000000, 0x000000, 0xb4b4b4, 0xf4f7fc, 0xababab, 0x0078d7, 0xffffff, 0xf0f0f0, 0xa0a0a0, 0x6d6d6d, 0x000000, 0x000000, 0xffffff, 0x696969, 0xe3e3e3, 0x000000, 0xffffe1, 0x000000, 0x3399ff, 0x99b4d1, 0xbfcddb, 0xf0f0f0, 0xffffff };
u32 sys_color_px(u32 i) { return i < 31 ? g_sys_colors[i] : 0; }
static void s_GetSysColor(Cpu *c) { u32 i = (u32)ARG(0); RET(i < 31 ? px2cr(g_sys_colors[i]) : 0); }
static u64 g_sys_brush[31];
static void s_GetSysColorBrush(Cpu *c) { u32 i = (u32)ARG(0); if (i >= 31) { RET(0); return; } if (!g_sys_brush[i]) { GdiObj *b = gdi_new(W, K_BRUSH); if (!b) { RET(0); return; } b->color = g_sys_colors[i]; g_sys_brush[i] = b->handle; } RET(g_sys_brush[i]); }
static void s_SetSysColors(Cpu *c) { RET(1); }
static int is_sys_brush(u64 h) { for (int i = 0; i < 31; i++) if (g_sys_brush[i] && g_sys_brush[i] == h) return 1; return 0; }
static u32 brush_px(Win32 *w, u64 hb, u32 dflt) { GdiObj *b = gdi_get(w, hb); if (b && b->kind == K_BRUSH && b->pen_width != -1) return b->color; if (hb >= 1 && hb <= 31) return g_sys_colors[hb - 1]; return dflt; }
/* notifications to the parent: WM_COMMAND(MAKELONG(id, code), hwnd) — a guest call that returns the given value afterwards */
static void notify_parent(Cpu *c, int i, u32 code, u64 ret) { int p = g_wnd[i].parent; if (p < 0 || !wnd_alive(p)) { RET(ret); return; } send_to(c, p, 0x111, ((u64)code << 16) | (g_wnd[i].id & 0xffff), HWND_OF(i), CONT_RET_VAL_, ret); }
static void notify_parent_wm(Cpu *c, int i, u32 msg, u64 wp, u64 lp, u64 ret) { int p = g_wnd[i].parent; if (p < 0 || !wnd_alive(p)) { RET(ret); return; } send_to(c, p, msg, wp, lp, CONT_RET_VAL_, ret); }
/* common control messages; returns 1 when handled */
static int ctl_common(Cpu *c, int i, u32 msg, u64 wp, u64 lp) { Win32 *w = W; Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x0C: { char t[1024]; text_in(c, lp, 1, t, sizeof t); wnd_set_text(n, t); n->sel0 = n->sel1 = (int)e_strlen(n->text); wnd_invalidate(i, 1); RET(1); return 1; }
    case 0x0D: RET(text_out_cap(c, lp, n->text, (u32)wp, 1)); return 1;
    case 0x0E: RET(utf8_u16len(n->text)); return 1;
    case 0x30: n->font = wp; wnd_invalidate(i, 1); RET(0); return 1;
    case 0x31: RET(n->font ? n->font : g_dlg_font); return 1;
    case 0x14: RET(1); return 1;   /* WM_ERASEBKGND: controls paint their whole area */
    case 0x0A: wnd_invalidate(i, 1); RET(0); return 1;
    case 0x07: n->focus_draw = 1; wnd_invalidate(i, 1); if (n->builtin == BI_EDIT) notify_parent(c, i, 0x100, 0); else RET(0); return 1;
    case 0x08: n->focus_draw = 0; wnd_invalidate(i, 1); if (n->builtin == BI_EDIT) notify_parent(c, i, 0x200, 0); else RET(0); return 1;
    case 0x01: case 0x02: case 0x82: RET(0); return 1;
    case 0x20: RET(1); return 1; case 0x84: RET(1); return 1; case 0x21: RET(1); return 1; case 0x05: case 0x03: case 0x18: case 0x46: case 0x47: RET(0); return 1;
    default: (void)w; return 0; } }
static u64 ctl_paint_begin(Cpu *c, int i) { u64 hdc = wnd_dc(c, i); return hdc; }
static void ctl_paint_end(Cpu *c, int i, u64 hdc) { Win32 *w = W; g_wnd[i].invalid = 0; g_wnd[i].erase = 0; popups_over(i); wnd_dc_free(w, hdc); w->dirty = 1; w->blits_since_yield++; (void)c; }
static void ctl_text(Cpu *c, u64 hdc, int i, int l, int t, int r, int b, u32 fmt, u32 color, const char *text) { Win32 *w = W; GdiObj *dc = gdi_get(w, hdc); if (!dc) return; dc->text_color = px2cr(color); dc->bk_mode = 1; int L = l, T = t, R = r, B = b; draw_text_rect(c, hdc, text, (int)e_strlen(text), &L, &T, &R, &B, fmt); (void)i; }
/* WM_CTLCOLOR*: ask the parent (guest) for a brush before painting; the continuation paints with the result */
static int ctl_color_query(Cpu *c, int i, u32 ctlmsg, u64 hdc) { int p = g_wnd[i].parent; if (p < 0 || !wnd_alive(p) || (g_wnd[p].builtin && !g_wnd[p].superclassed)) return 0; Win32 *w = W; w32_push_cont(c, CONT_CTLPAINT, (u64)i); w->cont[w->ncont - 1].data[1] = hdc; w->cont[w->ncont - 1].data[2] = ctlmsg; w32_guest_call(c, g_wnd[p].wndproc, HWND_OF(p), ctlmsg, hdc, HWND_OF(i)); return 1; }
static void button_paint(Cpu *c, int i, u64 hdc, u64 hbr);
static void static_paint(Cpu *c, int i, u64 hdc, u64 hbr);
static void edit_paint(Cpu *c, int i, u64 hdc, u64 hbr);
static void combo_paint(Cpu *c, int i, u64 hdc, u64 hbr);
static void listbox_paint(Cpu *c, int i, u64 hdc, u64 hbr);
static void progress_paint(Cpu *c, int i, u64 hdc);
static void ctl_paint_with(Cpu *c, int i, u64 hdc, u64 hbr) { switch (g_wnd[i].builtin) { case BI_BUTTON: button_paint(c, i, hdc, hbr); break; case BI_STATIC: case BI_LINK: static_paint(c, i, hdc, hbr); break; case BI_EDIT: edit_paint(c, i, hdc, hbr); break; case BI_COMBO: combo_paint(c, i, hdc, hbr); break; case BI_LISTBOX: case BI_COMBOLIST: listbox_paint(c, i, hdc, hbr); break; case BI_PROGRESS: progress_paint(c, i, hdc); break; default: break; } }
static void ctlpaint_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; u64 hdc = w->cont[ci].data[1]; if (wnd_alive(i)) ctl_paint_with(c, i, hdc, rax); if (wnd_alive(i)) ctl_paint_end(c, i, hdc); else wnd_dc_free(w, hdc); w32_cont_finish(c, 0); }
static void ctl_do_paint(Cpu *c, int i) { Wnd *n = &g_wnd[i]; u64 hdc = ctl_paint_begin(c, i); if (!hdc) { RET(0); return; }
  u32 q = 0; switch (n->builtin) { case BI_STATIC: case BI_LINK: q = 0x138; break; case BI_EDIT: q = (n->style & 0x800) || !n->enabled ? 0x138 : 0x133; break; case BI_BUTTON: { u32 bs = n->style & 0xF; if (bs >= 2 && bs <= 9 && bs != 8) q = 0x135; break; } case BI_LISTBOX: q = 0x134; break; default: break; }
  if (q && ctl_color_query(c, i, q, hdc)) return; ctl_paint_with(c, i, hdc, 0); ctl_paint_end(c, i, hdc); RET(0); }

/* Button: BS_PUSHBUTTON 0 DEFPUSHBUTTON 1 CHECKBOX 2 AUTOCHECKBOX 3 RADIOBUTTON 4 3STATE 5 AUTO3STATE 6 GROUPBOX 7 USERBUTTON 8 AUTORADIOBUTTON 9 OWNERDRAW 0xB; BS_PUSHLIKE 0x1000 */
static void button_paint(Cpu *c, int i, u64 hdc, u64 hbr) { Win32 *w = W; Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; u32 bs = n->style & 0xF; int cw = wnd_client_w(i), ch = wnd_client_h(i); int en = wnd_enabled_chain(i); int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh);
  u32 face = brush_px(w, hbr, 0xf0f0f0); int pushlike = (n->style & 0x1000) != 0;
  if (bs == 0 || bs == 1 || bs == 8 || bs == 0xB || pushlike) { int def = (bs == 1 || (n->parent >= 0 && g_wnd[n->parent].def_id == (int)n->id && n->parent >= 0)) && en; int pressed = n->pushed || (pushlike && n->checked);
    fill_rect_px(&s, 0, 0, cw, ch, pressed ? 0xcce4f7 : n->hot && en ? 0xe5f1fb : 0xe1e1e1); u32 bc = !en ? 0xcccccc : pressed ? 0x005499 : (def || n->focus_draw) ? 0x0078d7 : 0xadadad; edge_rect(&s, 0, 0, cw, ch, bc, bc); if (def && en && !pressed) edge_rect(&s, 1, 1, cw - 1, ch - 1, 0x0078d7, 0x0078d7);
    ctl_text(c, hdc, i, 0, 0, cw, ch, 1 | 4 | 0x20, en ? 0x000000 : 0x6d6d6d, n->text); if (n->focus_draw && en) { for (int x = 3; x < cw - 3; x += 2) { put_px(&s, x, 3, 0x202020); put_px(&s, x, ch - 4, 0x202020); } for (int y = 3; y < ch - 3; y += 2) { put_px(&s, 3, y, 0x202020); put_px(&s, cw - 4, y, 0x202020); } } return; }
  fill_rect_px(&s, 0, 0, cw, ch, face);
  if (bs == 7) { /* group box: etched frame with the caption on the top line */ int ty = fh / 2; edge_rect(&s, 0, ty, cw, ch, 0xdcdcdc, 0xdcdcdc); edge_rect(&s, 1, ty + 1, cw - 1, ch - 1, 0xffffff, 0xffffff); if (n->text[0]) { int tw = utf8_len_chars(n->text, (int)e_strlen(n->text)) * fcw; fill_rect_px(&s, 6, 0, 6 + tw + 4, fh, face); ctl_text(c, hdc, i, 8, 0, 8 + tw + 2, fh, 0x20, en ? 0 : 0x6d6d6d, n->text); } return; }
  int by = (ch - 13) / 2; if (by < 0) by = 0; int radio = bs == 4 || bs == 9; int right = (n->style & 0x20) != 0; int bx = right ? cw - 13 : 0;
  if (radio) draw_radio(&s, bx, by, n->checked != 0, en); else { fill_rect_px(&s, bx, by, bx + 13, by + 13, en ? 0xffffff : 0xf0f0f0); u32 bc = en ? (n->focus_draw ? 0x0078d7 : 0x333333) : 0xbcbcbc; edge_rect(&s, bx, by, bx + 13, by + 13, bc, bc); if (n->checked == 1) draw_check_mark(&s, bx + 2, by + 1, en ? 0 : 0xa0a0a0); else if (n->checked == 2) fill_rect_px(&s, bx + 3, by + 3, bx + 10, by + 10, 0x0078d7); }
  if (n->pushed) edge_rect(&s, bx, by, bx + 13, by + 13, 0x005499, 0x005499);
  ctl_text(c, hdc, i, right ? 0 : 17, 0, right ? cw - 17 : cw, ch, 4 | 0x20 | (n->style & 0x200 ? 1 : 0), en ? 0 : 0x6d6d6d, n->text); (void)hbr; }
static void button_click(Cpu *c, int i) { Wnd *n = &g_wnd[i]; u32 bs = n->style & 0xF;
  if (bs == 3 || bs == 6) { n->checked = bs == 6 ? (n->checked + 1) % 3 : !n->checked; wnd_invalidate(i, 1); }
  else if (bs == 9) { /* auto radio: uncheck the others in the group (siblings between WS_GROUP markers) */ int p = n->parent; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == p && g_wnd[k].builtin == BI_BUTTON && ((g_wnd[k].style & 0xF) == 9 || (g_wnd[k].style & 0xF) == 4) && k != i) { g_wnd[k].checked = 0; wnd_invalidate(k, 1); } n->checked = 1; wnd_invalidate(i, 1); }
  notify_parent(c, i, 0 /* BN_CLICKED */, 0); }
static void s_NebulaButtonProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_BUTTON); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x87: { u32 bs = n->style & 0xF; RET(bs == 1 ? 0x2001 : (bs == 0 || bs == 8) ? 0x2000 | 0x0020 : bs == 4 || bs == 9 ? 0x0040 : 0x0020); return; }   /* DLGC_DEFPUSHBUTTON / UNDEFPUSHBUTTON / RADIOBUTTON / BUTTON */
    case 0x201: { if (!wnd_enabled_chain(i)) { RET(0); return; } n->pushed = 1; g_capture = i; u32 bs = n->style & 0xF; if (bs != 7) set_focus(c, i); wnd_invalidate(i, 1); RET(0); return; }
    case 0x202: { if (!n->pushed) { RET(0); return; } n->pushed = 0; g_capture = -1; wnd_invalidate(i, 1); int lx = (s16)(u16)lp, ly = (s16)(u16)(lp >> 16); if (lx >= 0 && ly >= 0 && lx < wnd_client_w(i) && ly < wnd_client_h(i)) { button_click(c, i); return; } RET(0); return; }
    case 0x203: { n->pushed = 1; g_capture = i; RET(0); return; }
    case 0x200: { int lx = (s16)(u16)lp, ly = (s16)(u16)(lp >> 16); int in = lx >= 0 && ly >= 0 && lx < wnd_client_w(i) && ly < wnd_client_h(i); if (in != n->hot) { n->hot = in; wnd_invalidate(i, 1); } RET(0); return; }
    case 0x2A3: n->hot = 0; wnd_invalidate(i, 1); RET(0); return;
    case 0x100: { if (wp == 0x20) { n->pushed = 1; wnd_invalidate(i, 1); } RET(0); return; }
    case 0x101: { if (wp == 0x20 && n->pushed) { n->pushed = 0; wnd_invalidate(i, 1); button_click(c, i); return; } RET(0); return; }
    case 0xF0: RET(n->checked); return;                                                                      /* BM_GETCHECK */
    case 0xF1: n->checked = (int)wp; wnd_invalidate(i, 1); RET(0); return;                                   /* BM_SETCHECK */
    case 0xF2: RET((n->pushed ? 4 : 0) | (n->focus_draw ? 8 : 0) | (n->checked & 3) | (n->hot ? 0x200 : 0)); return;   /* BM_GETSTATE */
    case 0xF3: n->pushed = wp != 0; wnd_invalidate(i, 1); RET(0); return;
    case 0xF4: n->style = (n->style & ~0xFu) | ((u32)wp & 0xF); wnd_invalidate(i, 1); RET(0); return;        /* BM_SETSTYLE */
    case 0xF5: button_click(c, i); return;                                                                   /* BM_CLICK */
    case 0xF6: RET(n->icon); return; case 0xF7: { u64 old = n->icon; n->icon = lp; RET(old); return; }
    case 0x1601: { /* BCM_SETNOTE */ RET(1); return; } case 0x1605: RET(1); return;
    case 0x0C: { char t[1024]; text_in(c, lp, 1, t, sizeof t); wnd_set_text(n, t); wnd_invalidate(i, 1); RET(1); return; }
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }

/* Static: SS_LEFT 0 CENTER 1 RIGHT 2 ICON 3 BLACKRECT 4 GRAYRECT 5 WHITERECT 6 BLACKFRAME 7 GRAYFRAME 8 WHITEFRAME 9 SIMPLE 0xB LEFTNOWORDWRAP 0xC OWNERDRAW 0xD BITMAP 0xE ENHMETAFILE 0xF ETCHEDHORZ 0x10 ETCHEDVERT 0x11 ETCHEDFRAME 0x12; SS_NOPREFIX 0x80 SS_CENTERIMAGE 0x200 SS_SUNKEN 0x1000 */
static void static_paint(Cpu *c, int i, u64 hdc, u64 hbr) { Win32 *w = W; Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; u32 ss = n->style & 0x1F; int cw = wnd_client_w(i), ch = wnd_client_h(i); int en = wnd_enabled_chain(i);
  u32 face = brush_px(w, hbr, 0xf0f0f0); GdiObj *bo = gdi_get(w, hbr); int transparent = bo && bo->kind == K_BRUSH && bo->pen_width == -1; if (!transparent) fill_rect_px(&s, 0, 0, cw, ch, face);
  if (ss == 4 || ss == 5 || ss == 6) { fill_rect_px(&s, 0, 0, cw, ch, ss == 4 ? 0 : ss == 5 ? 0x808080 : 0xffffff); return; } if (ss == 7 || ss == 8 || ss == 9) { u32 col = ss == 7 ? 0 : ss == 8 ? 0x808080 : 0xffffff; edge_rect(&s, 0, 0, cw, ch, col, col); return; }
  if (ss == 0x10) { hline(&s, 0, cw - 1, ch / 2, 0xa0a0a0); hline(&s, 0, cw - 1, ch / 2 + 1, 0xffffff); return; } if (ss == 0x11) { line_px(&s, cw / 2, 0, cw / 2, ch - 1, 0xa0a0a0); line_px(&s, cw / 2 + 1, 0, cw / 2 + 1, ch - 1, 0xffffff); return; } if (ss == 0x12) { edge_rect(&s, 0, 0, cw, ch, 0xa0a0a0, 0xffffff); edge_rect(&s, 1, 1, cw - 1, ch - 1, 0xffffff, 0xa0a0a0); return; }
  if (ss == 3 || ss == 0xE || ss == 0xF) { /* icon / bitmap: a real bitmap handle is blitted, icons get a placeholder glyph */ GdiObj *bm = gdi_get(w, n->icon); if (bm && bm->kind == K_BITMAP) { u64 mdc = make_dc(w); GdiObj *d = gdi_get(w, mdc); if (d) { d->bitmap = n->icon; Surf src; if (dc_surf(c, mdc, &src)) { for (int y = 0; y < bm->bh && y < ch; y++) { u32 *dr = srow(&s, y), *sr = srow(&src, y); for (int x = 0; x < bm->bw && x < cw; x++) dr[x] = sr[x]; } } d->kind = 0; } }
    else if (ss == 3 && n->icon) { int d = cw < ch ? cw : ch; if (d > 32) d = 32; int ox = (cw - d) / 2, oy = (ch - d) / 2; fill_ellipse(&s, ox, oy, ox + d, oy + d, 0x0078d7, 1, 0x005499, 1); GdiObj *dc = gdi_get(w, hdc); if (dc) { dc->text_color = px2cr(0xffffff); dc->bk_mode = 1; draw_text(&s, dc, w, ox + d / 2 - 4, oy + d / 2 - 8, "i", 1); } } return; }
  if (n->style & 0x1000) edge_rect(&s, 0, 0, cw, ch, 0xa0a0a0, 0xffffff);
  u32 fmt = (ss == 1 ? 1 : ss == 2 ? 2 : 0) | (ss == 0xC || ss == 0xB ? 0x20 : 0x10) | (n->style & 0x80 ? 0x800 : 0) | (n->style & 0x200 ? 4 | 0x20 : 0) | (n->style & 0x4000 ? 0x8000 : 0);
  u32 col = en ? (n->builtin == BI_LINK ? 0x0066cc : 0x000000) : 0x6d6d6d; char plain[1024]; const char *txt = n->text;
  if (n->builtin == BI_LINK) { /* strip <a ...> tags */ int o = 0; for (const char *p = n->text; *p && o < 1000; p++) { if (*p == '<') { while (*p && *p != '>') p++; continue; } plain[o++] = *p; } plain[o] = 0; txt = plain; }
  ctl_text(c, hdc, i, 0, 0, cw, ch, fmt, col, txt); if (n->builtin == BI_LINK) { int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); hline(&s, 0, utf8_len_chars(txt, (int)e_strlen(txt)) * fcw - 1, fh - 1, col); } }
static void s_NebulaStaticProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_STATIC); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x87: RET(0x0100 /* DLGC_STATIC */); return;
    case 0x170: { u64 old = n->icon; n->icon = wp; wnd_invalidate(i, 1); RET(old); return; }   /* STM_SETICON */
    case 0x171: RET(n->icon); return;
    case 0x172: { u64 old = n->icon; n->icon = lp; wnd_invalidate(i, 1); RET(old); return; }   /* STM_SETIMAGE */
    case 0x173: RET(n->icon); return;
    case 0x201: { if (n->style & 0x100 /* SS_NOTIFY */) { notify_parent(c, i, 0 /* STN_CLICKED */, 0); return; } if (n->builtin == BI_LINK) { /* NM_CLICK through WM_NOTIFY: NMLINK { hwndFrom, idFrom, code=-2, LITEM } */ Win32 *w = W; u64 nm = w32_heap_alloc(w, 64 + 2 * 48 * 2 + 48 * 4 + 2600, 1); u64 P = PSZ_OF(c); wrp(c, nm, HWND_OF(i)); wrp(c, nm + P, n->id); wr32(c, nm + 2 * P, (u32)-2); notify_parent_wm(c, i, 0x4E, n->id, nm, 0); return; } RET(0); return; }
    case 0x203: { if (n->style & 0x100) { notify_parent(c, i, 1 /* STN_DBLCLK */, 0); return; } RET(0); return; }
    case 0x84: RET((n->style & 0x100) || n->builtin == BI_LINK ? 1 : (u64)-1); return;   /* HTTRANSPARENT unless SS_NOTIFY */
    case 0x0C: { char t[1024]; text_in(c, lp, 1, t, sizeof t); wnd_set_text(n, t); wnd_invalidate(i, 1); RET(1); return; }
    case 0x700: RET(1); return; case 0x702: RET(1); return;   /* LM_HITTEST / LM_SETITEM */
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }
static void s_NebulaLinkProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) SUPER_ADOPT(i, BI_LINK); s_NebulaStaticProc(c); }

/* Edit (single-line editing, multi-line display): ES_LEFT 0 CENTER 1 RIGHT 2 MULTILINE 4 UPPERCASE 8 LOWERCASE 0x10 PASSWORD 0x20 AUTOVSCROLL 0x40 AUTOHSCROLL 0x80 NOHIDESEL 0x100 READONLY 0x800 WANTRETURN 0x1000 NUMBER 0x2000 */

/* ---- multi-line edit: rows are laid out on demand (word wrap unless ES_AUTOHSCROLL), top_index = first visible row, vertical scrollbar when needed ---- */
#define EDIT_SB_W 16
static int edit_is_multi(Wnd *n) { return (n->style & 4) != 0; }
static int edit_layout(Wnd *n, int maxcols, int **starts, int **lens) { const char *t = n->text; int len = (int)e_strlen(t); int cap = 256, nl = 0; int *st = emu_alloc(sizeof(int) * cap), *ln = emu_alloc(sizeof(int) * cap);
  if (maxcols < 1) maxcols = 1; int wrap = !(n->style & 0x80);
#define ROW(a, b) do { if (nl == cap) { cap *= 2; int *s2 = emu_alloc(sizeof(int) * cap), *l2 = emu_alloc(sizeof(int) * cap); e_memcpy(s2, st, sizeof(int) * nl); e_memcpy(l2, ln, sizeof(int) * nl); emu_free(st); emu_free(ln); st = s2; ln = l2; } st[nl] = (a); ln[nl] = (b); nl++; } while (0)
  int i = 0; while (i <= len) { int e = i; while (e < len && t[e] != '\n') e++; int seg = i;
    if (seg == e) ROW(seg, 0);
    while (seg < e) { int cut = seg, chars = 0, last_space = -1; while (cut < e && chars < maxcols) { if (t[cut] == ' ') last_space = cut; if (t[cut] == '\r') { cut++; continue; } int cl = ((u8)t[cut] >= 0xF0) ? 4 : ((u8)t[cut] >= 0xE0) ? 3 : ((u8)t[cut] >= 0xC0) ? 2 : 1; cut += cl; chars++; }
      int endl = cut; if (wrap && cut < e && last_space > seg) endl = last_space; if (!wrap) endl = e; ROW(seg, endl - seg); seg = endl; if (wrap) while (seg < e && t[seg] == ' ') seg++; }
    i = e + 1; }
#undef ROW
  *starts = st; *lens = ln; return nl; }
static int edit_vis_rows(Win32 *w, Wnd *n, int i, int *fh_out) { int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); int ch = wnd_client_h(i) - 6; int vis = ch / (fh ? fh : 1); if (vis < 1) vis = 1; if (fh_out) *fh_out = fh; return vis; }
static int edit_nrows(Win32 *w, Wnd *n, int i, int sb) { int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); int cw = wnd_client_w(i) - 6 - (sb ? EDIT_SB_W : 0); int *st, *ln; int nl = edit_layout(n, cw / (fcw ? fcw : 1), &st, &ln); emu_free(st); emu_free(ln); return nl; }
static void edit_scroll_to(Win32 *w, Wnd *n, int i, int top) { int nl = edit_nrows(w, n, i, 1); int vis = edit_vis_rows(w, n, i, 0); int max = nl - vis; if (max < 0) max = 0; if (top > max) top = max; if (top < 0) top = 0; if (top != n->top_index) { n->top_index = top; wnd_invalidate(i, 1); } }
static void edit_paint_multi(Cpu *c, int i, u64 hdc, Surf *s, int pad, int en) { Win32 *w = W; Wnd *n = &g_wnd[i]; int cw = wnd_client_w(i), ch = wnd_client_h(i); int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); if (fcw < 1) fcw = 1;
  int *st, *ln; int nl = edit_layout(n, (cw - 2 * pad) / fcw, &st, &ln); int vis = (ch - 2 * pad) / (fh ? fh : 1); if (vis < 1) vis = 1; int sb = nl > vis || (n->style & 0x200000);
  if (sb) { emu_free(st); emu_free(ln); nl = edit_layout(n, (cw - 2 * pad - EDIT_SB_W) / fcw, &st, &ln); }
  int max = nl - vis; if (max < 0) max = 0; if (n->top_index > max) n->top_index = max; if (n->top_index < 0) n->top_index = 0;
  GdiObj *dc = gdi_get(w, hdc); u32 col = en ? 0x000000 : 0x6d6d6d; if (dc) { dc->text_color = px2cr(col); dc->bk_mode = 1; }
  int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0;
  for (int r = 0; r < vis && n->top_index + r < nl; r++) { int k = n->top_index + r; int y = pad + r * fh; if (y + fh > ch - pad + fh) break;
    if (a != b && n->focus_draw) { /* selection highlight on this row */ int rs = st[k], re = st[k] + ln[k]; int ha = a > rs ? a : rs, hb = b < re ? b : re; if (ha < hb) { int x0 = pad + utf8_len_chars(n->text + rs, ha - rs) * fcw, x1 = x0 + utf8_len_chars(n->text + ha, hb - ha) * fcw; fill_rect_px(s, x0, y, x1, y + fh, 0x0078d7); } }
    if (dc) draw_text(s, dc, w, pad, y, n->text + st[k], ln[k]); }
  if (sb) { /* vertical scrollbar: track, arrows, proportional thumb */ int x0 = cw - EDIT_SB_W, y0 = 0; fill_rect_px(s, x0, y0, cw, ch, 0xf0f0f0); edge_rect(s, x0, y0, cw, EDIT_SB_W, 0xffffff, 0xa0a0a0); edge_rect(s, x0, ch - EDIT_SB_W, cw, ch, 0xffffff, 0xa0a0a0);
    int tx = x0 + EDIT_SB_W / 2; for (int d = 0; d < 4; d++) { hline(s, tx - d, tx + d, 5 + d, 0x606060); hline(s, tx - d, tx + d, ch - 6 - d, 0x606060); }
    int track = ch - 2 * EDIT_SB_W; if (track > 8 && nl > 0) { int th = (int)((long long)track * vis / nl); if (th < 8) th = 8; if (th > track) th = track; int ty = EDIT_SB_W + (max ? (int)((long long)(track - th) * n->top_index / max) : 0); fill_rect_px(s, x0 + 2, ty, cw - 2, ty + th, 0xcdcdcd); } }
  emu_free(st); emu_free(ln); }
/* a click in the scrollbar area of a multi-line edit: 1 = handled */
static int edit_sb_click(Win32 *w, Wnd *n, int i, int lx, int ly) { int cw = wnd_client_w(i), ch = wnd_client_h(i); int vis = edit_vis_rows(w, n, i, 0); int nl = edit_nrows(w, n, i, 1); if (!(nl > vis || (n->style & 0x200000)) || lx < cw - EDIT_SB_W) return 0;
  int max = nl - vis; if (max < 0) max = 0; int track = ch - 2 * EDIT_SB_W; int th = nl ? (int)((long long)track * vis / nl) : track; if (th < 8) th = 8; int ty = EDIT_SB_W + (max ? (int)((long long)(track - th) * n->top_index / max) : 0);
  int top = n->top_index; if (ly < EDIT_SB_W) top--; else if (ly >= ch - EDIT_SB_W) top++; else if (ly < ty) top -= vis; else if (ly >= ty + th) top += vis; else return 1; edit_scroll_to(w, n, i, top); return 1; }
/* RTF -> plain UTF-8 text: groups, destinations, \par/\line/\tab, \'hh (cp1252), \uN, the usual symbol words; formatting words are dropped */
static void rtf_put(char **o, size_t *ol, size_t *oc, u32 cp) { if (*ol + 8 > *oc) { size_t nc = *oc * 2 + 64; char *nb = emu_alloc(nc); e_memcpy(nb, *o, *ol); emu_free(*o); *o = nb; *oc = nc; } if (cp < 0x80) (*o)[(*ol)++] = (char)cp; else if (cp < 0x800) { (*o)[(*ol)++] = (char)(0xC0 | cp >> 6); (*o)[(*ol)++] = (char)(0x80 | (cp & 0x3f)); } else if (cp < 0x10000) { (*o)[(*ol)++] = (char)(0xE0 | cp >> 12); (*o)[(*ol)++] = (char)(0x80 | ((cp >> 6) & 0x3f)); (*o)[(*ol)++] = (char)(0x80 | (cp & 0x3f)); } else { (*o)[(*ol)++] = (char)(0xF0 | cp >> 18); (*o)[(*ol)++] = (char)(0x80 | ((cp >> 12) & 0x3f)); (*o)[(*ol)++] = (char)(0x80 | ((cp >> 6) & 0x3f)); (*o)[(*ol)++] = (char)(0x80 | (cp & 0x3f)); } }
static const u16 cp1252_hi[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
static char *rtf_to_text(const char *r, size_t n) { size_t oc = n + 64, ol = 0; char *o = emu_alloc(oc); int depth = 0; int skip_depth = 0; /* >0: inside an ignored destination until depth drops below it */ int uc = 1; int uc_stack[64]; int skip_after_u = 0;
  for (size_t i = 0; i < n;) { char ch = r[i];
    if (ch == '{') { if (depth < 64) uc_stack[depth] = uc; depth++; i++; if (!skip_depth && i + 1 < n && r[i] == '\\' && r[i + 1] == '*') { skip_depth = depth; } continue; }
    if (ch == '}') { depth--; if (depth < 0) depth = 0; if (depth < 64) uc = uc_stack[depth]; if (skip_depth && depth < skip_depth) skip_depth = 0; i++; continue; }
    if (ch == '\\') { i++; if (i >= n) break; char k = r[i];
      if (k == '\'') { u32 v = 0; for (int d = 0; d < 2 && i + 1 < n; d++) { i++; char h = r[i]; v = v * 16 + (u32)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0); } i++; if (skip_after_u > 0) { skip_after_u--; continue; } if (!skip_depth) rtf_put(&o, &ol, &oc, v >= 0x80 && v < 0xA0 ? (cp1252_hi[v - 0x80] ? cp1252_hi[v - 0x80] : '?') : v); continue; }
      if (!((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z'))) { i++; if (skip_depth) continue; if (k == '\\' || k == '{' || k == '}') rtf_put(&o, &ol, &oc, (u32)(u8)k); else if (k == '~') rtf_put(&o, &ol, &oc, ' '); else if (k == '_') rtf_put(&o, &ol, &oc, '-'); else if (k == '\n' || k == '\r') rtf_put(&o, &ol, &oc, '\n'); continue; }
      char word[32]; int wl = 0; while (i < n && ((r[i] >= 'a' && r[i] <= 'z') || (r[i] >= 'A' && r[i] <= 'Z'))) { if (wl < 31) word[wl++] = r[i]; i++; } word[wl] = 0;
      int neg = 0, has = 0; long val = 0; if (i < n && r[i] == '-') { neg = 1; i++; } while (i < n && r[i] >= '0' && r[i] <= '9') { val = val * 10 + (r[i] - '0'); has = 1; i++; } if (neg) val = -val; if (i < n && r[i] == ' ') i++;
      if (!e_strcmp(word, "fonttbl") || !e_strcmp(word, "colortbl") || !e_strcmp(word, "stylesheet") || !e_strcmp(word, "info") || !e_strcmp(word, "pict") || !e_strcmp(word, "object") || !e_strcmp(word, "header") || !e_strcmp(word, "footer") || !e_strcmp(word, "listtable") || !e_strcmp(word, "listoverridetable") || !e_strcmp(word, "generator") || !e_strcmp(word, "themedata") || !e_strcmp(word, "colorschememapping") || !e_strcmp(word, "datastore") || !e_strcmp(word, "latentstyles") || !e_strcmp(word, "rsidtbl") || !e_strcmp(word, "xmlnstbl") || !e_strcmp(word, "pntxta") || !e_strcmp(word, "pntxtb") || !e_strcmp(word, "fldinst") || !e_strcmp(word, "revtbl") || !e_strcmp(word, "mmathPr")) { if (!skip_depth) skip_depth = depth ? depth : 1; continue; }
      if (skip_depth) continue;
      if (!e_strcmp(word, "par") || !e_strcmp(word, "line") || !e_strcmp(word, "page") || !e_strcmp(word, "sect")) rtf_put(&o, &ol, &oc, '\n');
      else if (!e_strcmp(word, "tab")) rtf_put(&o, &ol, &oc, '\t');
      else if (!e_strcmp(word, "uc")) uc = (int)val;
      else if (!e_strcmp(word, "u")) { u32 cp = (u32)(val < 0 ? val + 65536 : val); rtf_put(&o, &ol, &oc, cp); skip_after_u = uc; if (skip_after_u > 0) { /* skip the ANSI fallback characters */ size_t j = i; int left = skip_after_u; while (left > 0 && j < n) { if (r[j] == '\\' && j + 1 < n && r[j + 1] == '\'') { j += 4; left--; } else if (r[j] == '{' || r[j] == '}' || r[j] == '\\') break; else { j++; left--; } } i = j; skip_after_u = 0; } }
      else if (!e_strcmp(word, "emdash")) rtf_put(&o, &ol, &oc, 0x2014); else if (!e_strcmp(word, "endash")) rtf_put(&o, &ol, &oc, 0x2013); else if (!e_strcmp(word, "bullet")) rtf_put(&o, &ol, &oc, 0x2022);
      else if (!e_strcmp(word, "lquote")) rtf_put(&o, &ol, &oc, 0x2018); else if (!e_strcmp(word, "rquote")) rtf_put(&o, &ol, &oc, 0x2019); else if (!e_strcmp(word, "ldblquote")) rtf_put(&o, &ol, &oc, 0x201C); else if (!e_strcmp(word, "rdblquote")) rtf_put(&o, &ol, &oc, 0x201D);
      else if (!e_strcmp(word, "emspace") || !e_strcmp(word, "enspace") || !e_strcmp(word, "qmspace")) rtf_put(&o, &ol, &oc, ' ');
      (void)has; continue; }
    i++; if (skip_depth || ch == '\r' || ch == '\n') continue; if (skip_after_u > 0) { skip_after_u--; continue; } rtf_put(&o, &ol, &oc, (u32)(u8)ch); }
  /* bytes >= 0x80 that arrived raw are cp1252 too: re-encode */ char *fin = emu_alloc(ol * 3 + 1); size_t fl = 0; for (size_t k = 0; k < ol; k++) { u8 b = (u8)o[k]; if (b < 0x80) fin[fl++] = (char)b; else if ((b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0 || (b & 0xF8) == 0xF0) { int cl = (b & 0xF8) == 0xF0 ? 4 : (b & 0xF0) == 0xE0 ? 3 : 2; int okc = k + cl <= ol; for (int q = 1; okc && q < cl; q++) if (((u8)o[k + q] & 0xC0) != 0x80) okc = 0; if (okc) { e_memcpy(fin + fl, o + k, (size_t)cl); fl += cl; k += cl - 1; } else { fin[fl++] = '?'; } } else { u32 cp = b >= 0x80 && b < 0xA0 ? (cp1252_hi[b - 0x80] ? cp1252_hi[b - 0x80] : '?') : b; fin[fl++] = (char)(0xC0 | cp >> 6); fin[fl++] = (char)(0x80 | (cp & 0x3f)); } } fin[fl] = 0; emu_free(o); return fin; }
static int looks_like_rtf(const char *t) { return t[0] == '{' && t[1] == '\\' && t[2] == 'r' && t[3] == 't' && t[4] == 'f'; }
static void edit_set_text_maybe_rtf(Wnd *n, const char *t) { if (looks_like_rtf(t)) { char *pl = rtf_to_text(t, e_strlen(t)); wnd_set_text(n, pl); emu_free(pl); } else wnd_set_text(n, t); }
/* EM_STREAMIN: the application's callback is called until it reports 0 bytes; the data is collected and then converted (SF_TEXT / SF_RTF, optionally SF_UNICODE) */
static void streamin_step(Cpu *c, int i) { Win32 *w = W; Wnd *n = &g_wnd[i]; u64 es = w->cont[w->ncont - 1].data[1]; u64 P = PSZ_OF(c); u64 cb = rdp(c, es + 2 * P); u64 buf = n->stream_buf; wr32(c, buf + 4096, 0); w32_guest_call(c, cb, rdp(c, es), buf, 4096, buf + 4096); }
static void streamin_finish(Cpu *c, int i, u64 es) { Wnd *n = &g_wnd[i]; u32 fl = n->stream_flags; char *txt; size_t L = (size_t)n->stream_len; if (!n->stream) { n->stream = emu_alloc(1); n->stream[0] = 0; }
  if (fl & 0x10) { /* UTF-16 */ size_t ch = L / 2; txt = emu_alloc(ch * 3 + 1); size_t o = 0; for (size_t k = 0; k < ch; k++) { u32 cp = (u16)((u8)n->stream[2 * k] | ((u8)n->stream[2 * k + 1] << 8)); if (cp >= 0xD800 && cp < 0xDC00 && k + 1 < ch) { u32 lo = (u16)((u8)n->stream[2 * k + 2] | ((u8)n->stream[2 * k + 3] << 8)); if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); k++; } } if (!cp) break; if (cp < 0x80) txt[o++] = (char)cp; else if (cp < 0x800) { txt[o++] = (char)(0xC0 | cp >> 6); txt[o++] = (char)(0x80 | (cp & 0x3f)); } else if (cp < 0x10000) { txt[o++] = (char)(0xE0 | cp >> 12); txt[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); txt[o++] = (char)(0x80 | (cp & 0x3f)); } else { txt[o++] = (char)(0xF0 | cp >> 18); txt[o++] = (char)(0x80 | ((cp >> 12) & 0x3f)); txt[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); txt[o++] = (char)(0x80 | (cp & 0x3f)); } } txt[o] = 0; }
  else { txt = emu_alloc(L + 1); e_memcpy(txt, n->stream, L); txt[L] = 0; }
  if (looks_like_rtf(txt)) { /* SF_RTF is also used for plain text by some apps (Inno Setup's licence viewer): decide by content */ char *pl = rtf_to_text(txt, e_strlen(txt)); emu_free(txt); txt = pl; }
  if (fl & 0x8000 /* SFF_SELECTION */) { int cur = n->sel0 < n->sel1 ? n->sel0 : n->sel1; (void)cur; }
  wnd_set_text(n, txt); size_t tl = e_strlen(txt); emu_free(txt); emu_free(n->stream); n->stream = 0; n->stream_len = n->stream_cap = 0; n->sel0 = n->sel1 = 0; n->top_index = 0; n->modified = 0; wnd_invalidate(i, 1); wr32(c, es + PSZ_OF(c), 0); w32_cont_finish(c, (u64)tl); }
static void streamin_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; u64 es = w->cont[ci].data[1]; if (!wnd_alive(i)) { w32_cont_finish(c, 0); return; } Wnd *n = &g_wnd[i]; u32 got = rd32(c, n->stream_buf + 4096);
  if ((u32)rax != 0 || got == 0 || got > 4096 || n->stream_len > (1 << 24)) { streamin_finish(c, i, es); return; }
  if (n->stream_len + (int)got + 1 > n->stream_cap) { int nc = n->stream_cap ? n->stream_cap * 2 : 65536; while (nc < n->stream_len + (int)got + 1) nc *= 2; char *nb = emu_alloc((size_t)nc); if (n->stream) { e_memcpy(nb, n->stream, (size_t)n->stream_len); emu_free(n->stream); } n->stream = nb; n->stream_cap = nc; }
  const u8 *p = mem_ptr(c->mem, n->stream_buf, got, 0); if (p) e_memcpy(n->stream + n->stream_len, p, got); n->stream_len += (int)got; streamin_step(c, i); }
static void edit_paint(Cpu *c, int i, u64 hdc, u64 hbr) { Win32 *w = W; Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; int cw = wnd_client_w(i), ch = wnd_client_h(i); int en = wnd_enabled_chain(i); int ro = (n->style & 0x800) != 0; int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh);
  u32 bg = n->bgcol_set ? n->bgcol : brush_px(w, hbr, en && !ro ? 0xffffff : 0xf0f0f0); fill_rect_px(&s, 0, 0, cw, ch, bg); int border = !(n->style & 0x40000000u && n->parent >= 0 && !(n->exstyle & 0x200) && !(n->style & 0x800000)); if (n->exstyle & 0x200 || n->style & 0x800000) border = 1;
  if (border) { u32 bc = !en ? 0xcccccc : n->focus_draw ? 0x0078d7 : 0x7a7a7a; edge_rect(&s, 0, 0, cw, ch, bc, bc); }
  int pad = border ? 3 : 1; char shown[1024]; const char *t = n->text; if (n->style & 0x20) { int l = (int)e_strlen(t); for (int k = 0; k < l && k < 1000; k++) shown[k] = '*'; shown[l < 1000 ? l : 1000] = 0; t = shown; }
  int len = (int)e_strlen(t); u32 col = en ? 0x000000 : 0x6d6d6d; GdiObj *dc = gdi_get(w, hdc); if (!dc) return; dc->text_color = px2cr(col); dc->bk_mode = 1;
  if (n->style & 4) { edit_paint_multi(c, i, hdc, &s, pad, en); return; }
  /* positions below are in columns (code points); sel0/sel1/scroll_x are byte offsets into the UTF-8 text */
  int vis = (cw - 2 * pad) / fcw; if (vis < 1) vis = 1; int caret = n->sel1; if (caret > len) caret = len; int lencols = utf8_len_chars(t, len), caretc = utf8_len_chars(t, caret), fromc = utf8_len_chars(t, n->scroll_x < len ? n->scroll_x : len);
  if (caretc < fromc) fromc = caretc; if (caretc - fromc >= vis) fromc = caretc - vis + 1; if (fromc < 0) fromc = 0; if (!(n->style & 0x80)) fromc = 0; n->scroll_x = utf8_off(t, fromc);
  int from = fromc; int cnt = lencols - from; if (cnt > vis) cnt = vis; if (cnt < 0) cnt = 0; int ty = (ch - fh) / 2; if (ty < pad) ty = pad; int tx = pad; if ((n->style & 3) == 1) tx = (cw - cnt * fcw) / 2; else if ((n->style & 3) == 2) tx = cw - pad - cnt * fcw;
  int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0; int ac = utf8_len_chars(t, a < len ? a : len), bc = utf8_len_chars(t, b < len ? b : len);
  if (n->focus_draw && a != b) { int sa = ac - from, sb = bc - from; if (sa < 0) sa = 0; if (sb > cnt) sb = cnt; if (sb > sa) { fill_rect_px(&s, tx + sa * fcw, ty, tx + sb * fcw, ty + fh, 0x0078d7); } }
  int fb = utf8_off(t, from), eb = utf8_off(t, from + cnt); draw_text(&s, dc, w, tx, ty, t + fb, eb - fb);
  if (n->focus_draw && a != b) { /* selected text in white */ int sa = ac - from, sb = bc - from; if (sa < 0) sa = 0; if (sb > cnt) sb = cnt; if (sb > sa) { dc->text_color = px2cr(0xffffff); int s0 = utf8_off(t, from + sa), s1 = utf8_off(t, from + sb); draw_text(&s, dc, w, tx + sa * fcw, ty, t + s0, s1 - s0); } }
  if (n->focus_draw && en && !ro) { int cx = tx + (caretc - from) * fcw; line_px(&s, cx, ty, cx, ty + fh - 1, 0); } }
static void edit_replace_sel(Cpu *c, int i, const char *ins) { Wnd *n = &g_wnd[i]; int len = (int)e_strlen(n->text); int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0; if (a < 0) a = 0; if (b > len) b = len; if (a > b) a = b;
  int il = (int)e_strlen(ins); int room = n->limit > 0 ? n->limit : 0x7fffffff; if (room > (1 << 22)) room = 1 << 22; int newlen = len - (b - a) + il; if (newlen > room) { il -= newlen - room; if (il < 0) il = 0; }
  wnd_text_reserve(n, (size_t)(a + il + len - b)); e_memmove(n->text + a + il, n->text + b, (size_t)(len - b) + 1); e_memcpy(n->text + a, ins, (size_t)il); n->sel0 = n->sel1 = a + il; n->modified = 1; wnd_invalidate(i, 1); (void)c; }
static void edit_notify_change(Cpu *c, int i) { notify_parent(c, i, 0x300 /* EN_CHANGE */, 0); }
static void s_NebulaEditProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_EDIT); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i]; int len = (int)e_strlen(n->text); int ro = (n->style & 0x800) != 0;
  switch (msg) {
    case 0x01: n->sel0 = n->sel1 = 0; n->limit = 30000; RET(0); return;
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x87: RET(0x0001 | 0x0002 | 0x0080 | ((n->style & 4) ? 0x0004 : 0) /* DLGC_WANTARROWS|WANTTAB? no: WANTARROWS|WANTCHARS|HASSETSEL */); return;
    case 0x0C: { wnd_text_from_guest(c, n, lp, 1); if (looks_like_rtf(n->text)) { char *pl = rtf_to_text(n->text, e_strlen(n->text)); wnd_set_text(n, pl); emu_free(pl); } n->sel0 = n->sel1 = (int)e_strlen(n->text); n->scroll_x = 0; n->top_index = 0; wnd_invalidate(i, 1); edit_notify_change(c, i); c->r[RAX] = 1; return; }
    case 0x201: { if (!wnd_enabled_chain(i)) { RET(0); return; } set_focus(c, i); if (edit_is_multi(n)) { if (edit_sb_click(W, n, i, (s16)(u16)lp, (s16)(u16)(lp >> 16))) { RET(0); return; } RET(0); return; } int fcw, fh; font_dims(W, n->font ? n->font : g_dlg_font, &fcw, &fh); int lx = (s16)(u16)lp; int pos = n->scroll_x + (lx - 3 + fcw / 2) / fcw; if (pos < 0) pos = 0; if (pos > len) pos = len; if (W->key_state[0x10]) n->sel1 = pos; else n->sel0 = n->sel1 = pos; g_capture = i; wnd_invalidate(i, 1); RET(0); return; }
    case 0x200: { if (g_capture == i && (wp & 1)) { int fcw, fh; font_dims(W, n->font ? n->font : g_dlg_font, &fcw, &fh); int lx = (s16)(u16)lp; int pos = n->scroll_x + (lx - 3 + fcw / 2) / fcw; if (pos < 0) pos = 0; if (pos > len) pos = len; if (pos != n->sel1) { n->sel1 = pos; wnd_invalidate(i, 1); } } RET(0); return; }
    case 0x202: g_capture = -1; RET(0); return;
    case 0x203: n->sel0 = 0; n->sel1 = len; wnd_invalidate(i, 1); RET(0); return;
    case 0x20A: { if (!edit_is_multi(n)) { RET(0); return; } int d = (s16)(u16)(wp >> 16); edit_scroll_to(W, n, i, n->top_index + (d < 0 ? 3 : -3)); RET(0); return; }
    case 0x115: { if (!edit_is_multi(n)) { RET(0); return; } int code = (int)(wp & 0xffff), pos = (int)((wp >> 16) & 0xffff); int vis = edit_vis_rows(W, n, i, 0); int top = n->top_index; switch (code) { case 0: top--; break; case 1: top++; break; case 2: top -= vis; break; case 3: top += vis; break; case 4: case 5: top = pos; break; case 6: top = 0; break; case 7: top = 1 << 30; break; default: break; } edit_scroll_to(W, n, i, top); RET(0); return; }
    case 0xB6: { if (edit_is_multi(n)) edit_scroll_to(W, n, i, n->top_index + (int)(s32)(u32)lp); RET(1); return; }   /* EM_LINESCROLL */
    case 0xCE: RET(n->top_index); return;   /* EM_GETFIRSTVISIBLELINE */
    case 0x443: { n->bgcol_set = !(u32)wp; n->bgcol = (u32)lp; u32 px = ((n->bgcol & 0xff) << 16) | (n->bgcol & 0xff00) | ((n->bgcol >> 16) & 0xff); n->bgcol = px; wnd_invalidate(i, 1); RET(0); return; }   /* EM_SETBKGNDCOLOR */
    case 0x449: { /* EM_STREAMIN */ u64 es = lp; if (!es) { RET(0); return; } if (n->stream) { emu_free(n->stream); n->stream = 0; } n->stream_len = n->stream_cap = 0; n->stream_flags = (u32)wp; if (!n->stream_buf) n->stream_buf = w32_heap_alloc(W, 4096 + 16, 0); w32_push_cont(c, CONT_STREAMIN, (u64)i); W->cont[W->ncont - 1].data[1] = es; streamin_step(c, i); return; }
    case 0x44A: RET(0); return;   /* EM_STREAMOUT: nothing written */
    case 0x461: { /* EM_SETTEXTEX {flags, codepage} */ u32 cp = wp ? rd32(c, wp + 4) : 1200; if (cp == 1200) wnd_text_from_guest(c, n, lp, 1); else wnd_text_from_guest(c, n, lp, 0); if (looks_like_rtf(n->text)) { char *pl = rtf_to_text(n->text, e_strlen(n->text)); wnd_set_text(n, pl); emu_free(pl); } n->sel0 = n->sel1 = 0; n->top_index = 0; wnd_invalidate(i, 1); RET(1); return; }
    case 0x434: { if (lp) { int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0; wr32(c, lp, (u32)a); wr32(c, lp + 4, (u32)b); } RET(0); return; }   /* EM_EXGETSEL */
    case 0x437: { if (lp) { int a = (s32)rd32(c, lp), b = (s32)rd32(c, lp + 4); if (a < 0) a = 0; if (b < 0 || b > len) b = len; if (a > len) a = len; n->sel0 = a; n->sel1 = b; wnd_invalidate(i, 1); } RET(0); return; }   /* EM_EXSETSEL */
    case 0x435: n->limit = lp ? (int)lp : 0; RET(0); return;   /* EM_EXLIMITTEXT */
    case 0x45F: RET(len); return;   /* EM_GETTEXTLENGTHEX */
    case 0x44B: { /* EM_GETTEXTRANGE {min,max,lpstrText} */ u64 P = PSZ_OF(c); int a = (s32)rd32(c, lp), b = (s32)rd32(c, lp + 4); if (b < 0 || b > len) b = len; if (a < 0) a = 0; if (a > b) a = b; char tmp[2048]; int l = b - a; if (l > 2047) l = 2047; e_memcpy(tmp, n->text + a, (size_t)l); tmp[l] = 0; RET(text_out_cap(c, rdp(c, lp + 8), tmp, (u32)l + 1, 1)); (void)P; return; }
    case 0x444: case 0x447: case 0x446: case 0x4E1: case 0x450: RET(1); return;   /* EM_SETCHARFORMAT / EM_SETPARAFORMAT / EM_SETOLECALLBACK / EM_SETZOOM / EM_SETPALETTE */
    case 0x445: case 0x43B: case 0x44D: case 0x45B: case 0x459: case 0x478: case 0x4CC: case 0x4CA: case 0x460: case 0x43D: case 0x43A: case 0x452: case 0x43F: case 0x441: case 0x4DE: case 0x4DD: case 0x442: case 0x432: case 0x43C: case 0x448: case 0x44C: case 0x45E: case 0x4D6: case 0x4D9: RET(0); return;   /* event mask, options, URL detect, text mode, language/edit/typography options, scrollbar, formats, undo limit, hide selection, request resize, scroll pos, selection type, can paste, OLE, display band, ... */
    case 0x44F: case 0x438: RET((u64)(u32)-1); return;   /* EM_FINDTEXTEX / EM_FINDTEXT */
    case 0x100: { int shift = W->key_state[0x10], ctrl = W->key_state[0x11]; if (edit_is_multi(n)) { int vis = edit_vis_rows(W, n, i, 0); if (wp == 0x26) { edit_scroll_to(W, n, i, n->top_index - 1); RET(0); return; } if (wp == 0x28) { edit_scroll_to(W, n, i, n->top_index + 1); RET(0); return; } if (wp == 0x21) { edit_scroll_to(W, n, i, n->top_index - vis); RET(0); return; } if (wp == 0x22) { edit_scroll_to(W, n, i, n->top_index + vis); RET(0); return; } if (ctrl && wp == 0x24) { edit_scroll_to(W, n, i, 0); RET(0); return; } if (ctrl && wp == 0x23) { edit_scroll_to(W, n, i, 1 << 30); RET(0); return; } } int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0;
      switch (wp) {
        case 0x25: { int p = ctrl ? 0 : (a != b && !shift ? a : utf8_prev(n->text, n->sel1)); if (p < 0) p = 0; if (shift) n->sel1 = p; else n->sel0 = n->sel1 = p; wnd_invalidate(i, 1); RET(0); return; }
        case 0x27: { int p = ctrl ? len : (a != b && !shift ? b : utf8_nextb(n->text, n->sel1, len)); if (p > len) p = len; if (shift) n->sel1 = p; else n->sel0 = n->sel1 = p; wnd_invalidate(i, 1); RET(0); return; }
        case 0x24: if (shift) n->sel1 = 0; else n->sel0 = n->sel1 = 0; wnd_invalidate(i, 1); RET(0); return;
        case 0x23: if (shift) n->sel1 = len; else n->sel0 = n->sel1 = len; wnd_invalidate(i, 1); RET(0); return;
        case 0x2E: { if (ro) { RET(0); return; } if (a == b) { if (b >= len) { RET(0); return; } n->sel1 = utf8_nextb(n->text, b, len); } edit_replace_sel(c, i, ""); edit_notify_change(c, i); return; }
        case 0x2D: { if (shift && !ro) { if (g_clip[0]) { edit_replace_sel(c, i, g_clip); edit_notify_change(c, i); return; } } else if (ctrl) { if (a != b) { e_memcpy(g_clip, n->text + a, (size_t)(b - a)); g_clip[b - a] = 0; } } RET(0); return; }
        default: RET(0); return; } }
    case 0x102: { u32 ch = (u32)wp; int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0;
      if (ch == 1 /* ^A */) { n->sel0 = 0; n->sel1 = len; wnd_invalidate(i, 1); RET(0); return; }
      if (ch == 3 /* ^C */ || ch == 24 /* ^X */) { if (a != b) { e_memcpy(g_clip, n->text + a, (size_t)(b - a)); g_clip[b - a] = 0; if (ch == 24 && !ro) { edit_replace_sel(c, i, ""); edit_notify_change(c, i); return; } } RET(0); return; }
      if (ch == 22 /* ^V */) { if (!ro && g_clip[0]) { edit_replace_sel(c, i, g_clip); edit_notify_change(c, i); return; } RET(0); return; }
      if (ro) { RET(0); return; }
      if (ch == 8) { if (a == b) { if (a == 0) { RET(0); return; } n->sel0 = utf8_prev(n->text, a); n->sel1 = a; } edit_replace_sel(c, i, ""); edit_notify_change(c, i); return; }
      if (ch == '\r' || ch == '\n') { if (n->style & 4) { edit_replace_sel(c, i, "\n"); edit_notify_change(c, i); return; } RET(0); return; }
      if (ch == '\t' || ch == 27) { RET(0); return; }
      if (ch < 32) { RET(0); return; } if ((n->style & 0x2000) && !(ch >= '0' && ch <= '9')) { RET(0); return; }
      if (n->style & 8 && ch >= 'a' && ch <= 'z') ch -= 32; if (n->style & 0x10 && ch >= 'A' && ch <= 'Z') ch += 32;
      char ins[8]; int o = 0; if (ch < 0x80) ins[o++] = (char)ch; else if (ch < 0x800) { ins[o++] = (char)(0xC0 | ch >> 6); ins[o++] = (char)(0x80 | (ch & 0x3f)); } else { ins[o++] = (char)(0xE0 | ch >> 12); ins[o++] = (char)(0x80 | ((ch >> 6) & 0x3f)); ins[o++] = (char)(0x80 | (ch & 0x3f)); } ins[o] = 0;
      edit_replace_sel(c, i, ins); edit_notify_change(c, i); return; }
    case 0xB0: { int a = n->sel0 < n->sel1 ? n->sel0 : n->sel1, b = n->sel0 < n->sel1 ? n->sel1 : n->sel0; if (wp) wr32(c, wp, (u32)a); if (lp) wr32(c, lp, (u32)b); RET((u64)(u32)a | ((u64)(u32)b << 16)); return; }   /* EM_GETSEL */
    case 0xB1: { int a = (int)(s32)(u32)wp, b = (int)(s32)(u32)lp; if (a == -1) { n->sel0 = n->sel1 = n->sel1; } else { if (b == -1 || b > len) b = len; if (a > len) a = len; n->sel0 = a; n->sel1 = b; } wnd_invalidate(i, 1); RET(0); return; }   /* EM_SETSEL */
    case 0xB7: wnd_invalidate(i, 1); RET(1); return;   /* EM_SCROLLCARET */
    case 0xB8: RET(n->modified); return; case 0xB9: n->modified = (int)wp; RET(0); return;
    case 0xBA: { int lines = 1; for (const char *p = n->text; *p; p++) if (*p == '\n') lines++; RET(lines); return; }   /* EM_GETLINECOUNT */
    case 0xBB: RET(0); return; case 0xC1: RET(len); return; case 0xC9: RET(0); return;   /* EM_LINEINDEX / EM_LINELENGTH / EM_LINEFROMCHAR */
    case 0xC2: { char t[1024]; text_in(c, lp, 1, t, sizeof t); edit_replace_sel(c, i, t); edit_notify_change(c, i); return; }   /* EM_REPLACESEL */
    case 0xC5: n->limit = (int)wp; RET(0); return; case 0xD5: RET(n->limit); return;   /* EM_SETLIMITTEXT / EM_GETLIMITTEXT */
    case 0xCF: { if (wp) n->style |= 0x800; else n->style &= ~0x800u; wnd_invalidate(i, 1); RET(1); return; }   /* EM_SETREADONLY */
    case 0xCC: n->icon = wp; RET(0); return; case 0xD2: RET(n->icon); return;   /* EM_SETPASSWORDCHAR */
    case 0xC4: { int o = 0; for (const char *p = n->text; *p && o < 1000; p++) o++; RET(0); return; }   /* EM_GETLINE: not implemented */
    case 0xD3: RET(0); return; case 0xD4: RET(0); return; case 0xB2: RET(0); return; case 0xB3: RET(1); return; case 0xB4: RET(0); return; case 0xB5: RET(0); return; case 0xBE: RET(1); return; case 0xC7: RET(0); return; case 0xCB: RET(1); return; case 0xCD: RET(0); return; case 0xD0: RET(0); return; case 0xD1: RET(0); return; case 0x1501: RET(1); return; case 0x1502: RET(1); return;
    case 0x203 + 0x1000: RET(0); return;
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }

/* ListBox / ComboBox item storage */
static int items_add(Wnd *n, const char *s, int at) { if (!n->items) { n->items = emu_alloc(sizeof(char[128]) * MAX_ITEMS); n->itemdata = emu_alloc(sizeof(u64) * MAX_ITEMS); n->nitems = 0; } if (n->nitems >= MAX_ITEMS) return -1; if (at < 0 || at > n->nitems) at = n->nitems; for (int k = n->nitems; k > at; k--) { e_memcpy(n->items[k], n->items[k - 1], 128); n->itemdata[k] = n->itemdata[k - 1]; } e_snprintf(n->items[at], 128, "%s", s); n->itemdata[at] = 0; n->nitems++; if (n->cursel >= at && at < n->nitems - 1) n->cursel++; return at; }
static void items_del(Wnd *n, int at) { if (at < 0 || at >= n->nitems) return; for (int k = at; k + 1 < n->nitems; k++) { e_memcpy(n->items[k], n->items[k + 1], 128); n->itemdata[k] = n->itemdata[k + 1]; } n->nitems--; if (n->cursel >= n->nitems) n->cursel = n->nitems - 1; }
static int list_item_h(Win32 *w, Wnd *n) { int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); return fh + 2; }
static void listbox_paint(Cpu *c, int i, u64 hdc, u64 hbr) { Win32 *w = W; Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; int cw = wnd_client_w(i), ch = wnd_client_h(i); int en = wnd_enabled_chain(i); u32 bg = brush_px(w, hbr, en ? 0xffffff : 0xf0f0f0); fill_rect_px(&s, 0, 0, cw, ch, bg);
  int ih = list_item_h(w, n); int vis = (ch - 2) / (ih ? ih : 1); if (n->cursel >= 0) { if (n->cursel < n->top_index) n->top_index = n->cursel; if (n->cursel >= n->top_index + vis) n->top_index = n->cursel - vis + 1; } if (n->top_index < 0) n->top_index = 0;
  GdiObj *dc = gdi_get(w, hdc); for (int k = n->top_index, row = 0; k < n->nitems && row * ih < ch; k++, row++) { int y = 1 + row * ih; int sel = k == n->cursel; if (sel) fill_rect_px(&s, 1, y, cw - 1, y + ih, n->focus_draw || n->builtin == BI_COMBOLIST ? 0x0078d7 : 0xcccccc); if (dc) { dc->text_color = px2cr(sel && (n->focus_draw || n->builtin == BI_COMBOLIST) ? 0xffffff : en ? 0 : 0x6d6d6d); dc->bk_mode = 1; draw_text(&s, dc, w, 3, y + 1, n->items[k], (int)e_strlen(n->items[k])); } }
  u32 bc = !en ? 0xcccccc : n->focus_draw ? 0x0078d7 : 0x7a7a7a; edge_rect(&s, 0, 0, cw, ch, bc, bc); if (n->nitems > vis) { /* scrollbar hint */ int bar_h = ch - 2, th = vis * bar_h / (n->nitems ? n->nitems : 1); if (th < 8) th = 8; int ty = 1 + n->top_index * (bar_h - th) / (n->nitems - vis > 0 ? n->nitems - vis : 1); fill_rect_px(&s, cw - 8, 1, cw - 1, ch - 1, 0xf0f0f0); fill_rect_px(&s, cw - 7, ty, cw - 2, ty + th, 0xcdcdcd); } }
static void listbox_select_notify(Cpu *c, int i) { Wnd *n = &g_wnd[i]; if (n->builtin == BI_COMBOLIST) { /* a dropped combo list: the owner combo takes the selection */ int combo = n->owner; if (combo >= 0 && wnd_alive(combo)) { Wnd *cb = &g_wnd[combo]; cb->cursel = n->cursel; if (cb->cursel >= 0 && cb->cursel < cb->nitems) wnd_set_text(cb, cb->items[cb->cursel]); cb->dropped = 0; cb->drop_wnd = -1; wnd_invalidate(combo, 1); n->visible = 0; g_capture = -1; wnd_invalidate_all(); notify_parent(c, combo, 1 /* CBN_SELCHANGE */, 0); /* the list window is freed lazily */ wnd_free(i); return; } RET(0); return; }
  notify_parent(c, i, 1 /* LBN_SELCHANGE */, 0); }
static void s_NebulaListBoxProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_LISTBOX); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i]; Win32 *w = W;
  switch (msg) {
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x87: RET(0x0001 | 0x0002); return;
    case 0x201: case 0x203: { if (!wnd_enabled_chain(i)) { RET(0); return; } if (n->builtin != BI_COMBOLIST) set_focus(c, i); int ly = (s16)(u16)(lp >> 16), lx = (s16)(u16)lp; if (n->builtin == BI_COMBOLIST && (lx < 0 || ly < 0 || lx >= wnd_client_w(i) || ly >= wnd_client_h(i))) { /* click outside closes the list */ int combo = n->owner; if (combo >= 0 && wnd_alive(combo)) { g_wnd[combo].dropped = 0; g_wnd[combo].drop_wnd = -1; wnd_invalidate(combo, 1); } n->visible = 0; g_capture = -1; wnd_invalidate_all(); wnd_free(i); RET(0); return; }
      int ih = list_item_h(w, n); int k = n->top_index + (ly - 1) / (ih ? ih : 1); if (k >= 0 && k < n->nitems) { n->cursel = k; wnd_invalidate(i, 1); if (msg == 0x203 && n->builtin != BI_COMBOLIST) { notify_parent(c, i, 2 /* LBN_DBLCLK */, 0); return; } listbox_select_notify(c, i); return; } RET(0); return; }
    case 0x100: { int k = n->cursel; if (wp == 0x26 && k > 0) k--; else if (wp == 0x28 && k + 1 < n->nitems) k++; else if (wp == 0x24) k = 0; else if (wp == 0x23) k = n->nitems - 1; else if (wp == 0x21) k -= 8; else if (wp == 0x22) k += 8; else if (wp == 0x0D && n->builtin == BI_COMBOLIST) { listbox_select_notify(c, i); return; } else if (wp == 0x1B && n->builtin == BI_COMBOLIST) { int combo = n->owner; if (combo >= 0 && wnd_alive(combo)) { g_wnd[combo].dropped = 0; g_wnd[combo].drop_wnd = -1; wnd_invalidate(combo, 1); } n->visible = 0; g_capture = -1; wnd_invalidate_all(); wnd_free(i); RET(0); return; } else { RET(0); return; }
      if (k < 0) k = 0; if (k >= n->nitems) k = n->nitems - 1; if (k != n->cursel) { n->cursel = k; wnd_invalidate(i, 1); if (n->builtin == BI_COMBOLIST) { int combo = n->owner; if (combo >= 0 && wnd_alive(combo)) { g_wnd[combo].cursel = k; wnd_set_text(&g_wnd[combo], n->items[k]); wnd_invalidate(combo, 1); } RET(0); return; } notify_parent(c, i, 1, 0); return; } RET(0); return; }
    case 0x20A: { int d = (s16)(u16)(wp >> 16); n->top_index += d < 0 ? 3 : -3; if (n->top_index < 0) n->top_index = 0; if (n->top_index >= n->nitems) n->top_index = n->nitems - 1; wnd_invalidate(i, 1); RET(0); return; }
    case 0x180: { char t[128]; if (n->style & 0x10 /* LBS_HASSTRINGS off / OWNERDRAW */ && lp < 0x10000) e_snprintf(t, sizeof t, "%llu", (unsigned long long)lp); else text_in(c, lp, 1, t, sizeof t); int at = -1; if (n->style & 2 /* LBS_SORT */) { items_add(n, t, -1); at = n->nitems - 1; for (int k = 0; k < n->nitems - 1; k++) if (e_stricmp(t, n->items[k]) < 0) { items_del(n, n->nitems - 1); at = items_add(n, t, k); break; } } else at = items_add(n, t, -1); if (lp < 0x10000 && at >= 0) n->itemdata[at] = lp; wnd_invalidate(i, 1); RET(at < 0 ? (u64)-2 : (u64)at); return; }   /* LB_ADDSTRING */
    case 0x181: { char t[128]; text_in(c, lp, 1, t, sizeof t); int at = items_add(n, t, (int)(s32)(u32)wp); wnd_invalidate(i, 1); RET(at < 0 ? (u64)-2 : (u64)at); return; }   /* LB_INSERTSTRING */
    case 0x182: { items_del(n, (int)wp); wnd_invalidate(i, 1); RET(n->nitems); return; }   /* LB_DELETESTRING */
    case 0x184: items_free(n); n->cursel = -1; n->top_index = 0; wnd_invalidate(i, 1); RET(0); return;   /* LB_RESETCONTENT */
    case 0x186: { if (n->builtin == BI_COMBOLIST) { RET(n->cursel >= 0 ? 1 : 0); return; } int sel = (int)(s32)(u32)wp; int old = n->cursel; n->cursel = sel < n->nitems ? sel : -1; if (n->cursel != old) wnd_invalidate(i, 1); RET(n->cursel < 0 && sel >= 0 ? (u64)-1 : (u64)(u32)n->cursel); return; }   /* LB_SETCURSEL */
    case 0x188: RET(n->cursel < 0 ? (u64)-1 : (u64)(u32)n->cursel); return;   /* LB_GETCURSEL */
    case 0x189: { int k = (int)wp; if (k < 0 || k >= n->nitems) { RET((u64)-1); return; } RET(put_w8(c, lp, n->items[k], 0)); return; }   /* LB_GETTEXT */
    case 0x18A: { int k = (int)wp; if (k < 0 || k >= n->nitems) { RET((u64)-1); return; } RET(utf8_u16len(n->items[k])); return; }   /* LB_GETTEXTLEN */
    case 0x18B: RET(n->nitems); return;   /* LB_GETCOUNT */
    case 0x18E: RET(n->top_index); return; case 0x197: n->top_index = (int)wp; wnd_invalidate(i, 1); RET(0); return;
    case 0x199: { int k = (int)wp; RET(k >= 0 && k < n->nitems ? n->itemdata[k] : (u64)-1); return; }   /* LB_GETITEMDATA */
    case 0x19A: { int k = (int)wp; if (k >= 0 && k < n->nitems) n->itemdata[k] = lp; RET(k >= 0 && k < n->nitems ? 0 : (u64)-1); return; }   /* LB_SETITEMDATA */
    case 0x18F: case 0x19C: { char t[128]; text_in(c, lp, 1, t, sizeof t); int start = (int)(s32)(u32)wp; for (int q = 0; q < n->nitems; q++) { int k = (start + 1 + q) % n->nitems; if (msg == 0x18F ? !e_strnicmp(n->items[k], t, e_strlen(t)) : !e_stricmp(n->items[k], t)) { RET(k); return; } } RET((u64)-1); return; }   /* LB_FINDSTRING / LB_FINDSTRINGEXACT */
    case 0x1A1: RET(list_item_h(w, n)); return; case 0x1A0: RET(0); return;   /* LB_GETITEMHEIGHT / LB_SETITEMHEIGHT */
    case 0x198: { int k = (int)wp; u64 r = lp; int ih = list_item_h(w, n); wr32(c, r, 1); wr32(c, r + 4, (u32)(1 + (k - n->top_index) * ih)); wr32(c, r + 8, (u32)(wnd_client_w(i) - 1)); wr32(c, r + 12, (u32)(1 + (k - n->top_index + 1) * ih)); RET(0); return; }   /* LB_GETITEMRECT */
    case 0x187: RET(n->cursel >= 0 && (int)wp == n->cursel); return;   /* LB_GETSEL */
    case 0x185: { if ((int)(s32)(u32)lp == -1) { n->cursel = wp ? 0 : -1; } else n->cursel = wp ? (int)lp : (n->cursel == (int)lp ? -1 : n->cursel); wnd_invalidate(i, 1); RET(0); return; }   /* LB_SETSEL */
    case 0x190: RET(n->cursel >= 0 ? 1 : 0); return; case 0x191: { if (wp && n->cursel >= 0) wr32(c, lp, (u32)n->cursel); RET(n->cursel >= 0 ? 1 : 0); return; }   /* LB_GETSELCOUNT / LB_GETSELITEMS */
    case 0x194: RET(0); return; case 0x195: RET(0); return; case 0x196: RET(0); return; case 0x19F: RET(0); return; case 0x1A4: RET(0); return; case 0x1A5: RET(0); return;
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }
static void s_NebulaComboListProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i >= 0) SUPER_ADOPT(i, BI_COMBOLIST); s_NebulaListBoxProc(c); }

/* ComboBox: CBS_SIMPLE 1 DROPDOWN 2 DROPDOWNLIST 3; the dropped list is a popup ComboLBox owned by the combo */
static void combo_paint(Cpu *c, int i, u64 hdc, u64 hbr) { Win32 *w = W; Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; int cw = wnd_client_w(i), ch = wnd_client_h(i); int en = wnd_enabled_chain(i); int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh);
  u32 bg = brush_px(w, hbr, en ? 0xffffff : 0xf0f0f0); fill_rect_px(&s, 0, 0, cw, ch, bg); u32 bc = !en ? 0xcccccc : n->focus_draw || n->dropped ? 0x0078d7 : 0x7a7a7a; edge_rect(&s, 0, 0, cw, ch, bc, bc);
  int bw = 17; fill_rect_px(&s, cw - bw - 1, 1, cw - 1, ch - 1, n->dropped ? 0xcce4f7 : 0xe1e1e1); int ax = cw - bw / 2 - 1, ay = ch / 2 - 1; for (int k = 0; k < 4; k++) hline(&s, ax - 3 + k, ax + 3 - k, ay - 1 + k, en ? 0x404040 : 0xa0a0a0);
  int sel = n->focus_draw && (n->style & 3) == 3 && !n->dropped; if (sel) fill_rect_px(&s, 2, 2, cw - bw - 2, ch - 2, 0x0078d7); GdiObj *dc = gdi_get(w, hdc); if (dc) { dc->text_color = px2cr(sel ? 0xffffff : en ? 0 : 0x6d6d6d); dc->bk_mode = 1; int maxc = (cw - bw - 6) / fcw; int ln = utf8_len_chars(n->text, (int)e_strlen(n->text)); if (ln > maxc) ln = maxc; if (ln < 0) ln = 0; int lb = utf8_off(n->text, ln); draw_text(&s, dc, w, 4, (ch - fh) / 2, n->text, lb); if (n->focus_draw && (n->style & 3) == 2 && en) { int cx = 4 + ln * fcw; line_px(&s, cx, (ch - fh) / 2, cx, (ch + fh) / 2 - 1, 0); } } }
static void combo_drop(Cpu *c, int i) { Win32 *w = W; Wnd *n = &g_wnd[i]; if (n->dropped) { if (n->drop_wnd >= 0 && wnd_alive(n->drop_wnd)) { g_wnd[n->drop_wnd].visible = 0; wnd_free(n->drop_wnd); } n->dropped = 0; n->drop_wnd = -1; g_capture = -1; wnd_invalidate_all(); return; }
  int ox, oy; wnd_window_origin(i, &ox, &oy); int ih = list_item_h(w, n); int rows = n->nitems < 8 ? n->nitems : 8; if (rows < 1) rows = 1; int lh = rows * ih + 2; int li = wnd_create_obj(c, "ComboLBox", "", 0x80000000u | 0x10000000u, 0, ox + SCREEN_X0, oy + n->h + SCREEN_Y0, n->w, lh, HWND_OF(i), 0, n->inst, 0, 1, 0); if (li < 0) return;
  Wnd *l = &g_wnd[li]; l->owner = i; l->font = n->font; l->zseq = ++g_zseq; if (l->y + l->h > w->screen_h) l->y = oy - lh; if (l->y < 0) l->y = 0;
  /* share the items */ l->items = emu_alloc(sizeof(char[128]) * MAX_ITEMS); l->itemdata = emu_alloc(sizeof(u64) * MAX_ITEMS); for (int k = 0; k < n->nitems; k++) { e_memcpy(l->items[k], n->items[k], 128); l->itemdata[k] = n->itemdata[k]; } l->nitems = n->nitems; l->cursel = n->cursel; l->top_index = n->cursel > 0 ? n->cursel : 0;
  l->visible = 1; l->invalid = 1; n->dropped = 1; n->drop_wnd = li; g_capture = li; wnd_invalidate(i, 1); w->dirty = 1; }
static void s_NebulaComboProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_COMBO); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i]; Win32 *w = W;
  switch (msg) {
    case 0x01: { /* the template height covers the dropped list; the control itself is one line tall */ int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); if ((n->style & 3) != 1 && n->h > fh + 10) n->h = fh + 8; RET(0); return; }
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x87: RET(0x0001 | 0x0002); return;
    case 0x201: { if (!wnd_enabled_chain(i)) { RET(0); return; } set_focus(c, i); combo_drop(c, i); notify_parent(c, i, n->dropped ? 7 /* CBN_DROPDOWN */ : 8, 0); return; }
    case 0x100: { if (wp == 0x28 && !n->dropped && n->cursel + 1 < n->nitems) { n->cursel++; wnd_set_text(n, n->items[n->cursel]); wnd_invalidate(i, 1); notify_parent(c, i, 1, 0); return; } if (wp == 0x26 && !n->dropped && n->cursel > 0) { n->cursel--; wnd_set_text(n, n->items[n->cursel]); wnd_invalidate(i, 1); notify_parent(c, i, 1, 0); return; } if (wp == 0x73 /* F4 */) { combo_drop(c, i); RET(0); return; } RET(0); return; }
    case 0x102: { if ((n->style & 3) == 2) { /* editable: type into the text */ u32 ch = (u32)wp; if (ch == 8) { int l = (int)e_strlen(n->text); if (l) n->text[l - 1] = 0; } else if (ch >= 32 && ch < 0x80) { int l = (int)e_strlen(n->text); if (l < 1000) { n->text[l] = (char)ch; n->text[l + 1] = 0; } } wnd_invalidate(i, 1); notify_parent(c, i, 5 /* CBN_EDITCHANGE */, 0); return; } RET(0); return; }
    case 0x0C: { char t[1024]; text_in(c, lp, 1, t, sizeof t); wnd_set_text(n, t); wnd_invalidate(i, 1); RET(1); return; }
    case 0x143: { char t[128]; text_in(c, lp, 1, t, sizeof t); int at; if (n->style & 0x100 /* CBS_SORT */) { items_add(n, t, -1); at = n->nitems - 1; for (int k = 0; k < n->nitems - 1; k++) if (e_stricmp(t, n->items[k]) < 0) { items_del(n, n->nitems - 1); at = items_add(n, t, k); break; } } else at = items_add(n, t, -1); RET(at < 0 ? (u64)-2 : (u64)at); return; }   /* CB_ADDSTRING */
    case 0x14A: { char t[128]; text_in(c, lp, 1, t, sizeof t); int at = items_add(n, t, (int)(s32)(u32)wp); RET(at < 0 ? (u64)-2 : (u64)at); return; }   /* CB_INSERTSTRING */
    case 0x144: items_del(n, (int)wp); RET(n->nitems); return;   /* CB_DELETESTRING */
    case 0x14B: items_free(n); n->cursel = -1; n->text[0] = 0; wnd_invalidate(i, 1); RET(0); return;   /* CB_RESETCONTENT */
    case 0x146: RET(n->nitems); return;   /* CB_GETCOUNT */
    case 0x147: RET(n->cursel < 0 ? (u64)-1 : (u64)(u32)n->cursel); return;   /* CB_GETCURSEL */
    case 0x14E: { int sel = (int)(s32)(u32)wp; if (sel >= 0 && sel < n->nitems) { n->cursel = sel; wnd_set_text(n, n->items[sel]); } else { n->cursel = -1; n->text[0] = 0; } wnd_invalidate(i, 1); RET(n->cursel < 0 ? (u64)-1 : (u64)(u32)n->cursel); return; }   /* CB_SETCURSEL */
    case 0x148: { int k = (int)wp; if (k < 0 || k >= n->nitems) { RET((u64)-1); return; } RET(put_w8(c, lp, n->items[k], 0)); return; }   /* CB_GETLBTEXT */
    case 0x149: { int k = (int)wp; if (k < 0 || k >= n->nitems) { RET((u64)-1); return; } RET(utf8_u16len(n->items[k])); return; }   /* CB_GETLBTEXTLEN */
    case 0x150: { int k = (int)wp; RET(k >= 0 && k < n->nitems ? n->itemdata[k] : (u64)-1); return; }   /* CB_GETITEMDATA */
    case 0x151: { int k = (int)wp; if (k >= 0 && k < n->nitems) n->itemdata[k] = lp; RET(k >= 0 && k < n->nitems ? 0 : (u64)-1); return; }   /* CB_SETITEMDATA */
    case 0x14C: case 0x158: { char t[128]; text_in(c, lp, 1, t, sizeof t); int start = (int)(s32)(u32)wp; for (int q = 0; q < n->nitems; q++) { int k = (start + 1 + q) % (n->nitems ? n->nitems : 1); if (n->nitems && (msg == 0x14C ? !e_strnicmp(n->items[k], t, e_strlen(t)) : !e_stricmp(n->items[k], t))) { RET(k); return; } } RET((u64)-1); return; }   /* CB_FINDSTRING / CB_FINDSTRINGEXACT */
    case 0x14D: { char t[128]; text_in(c, lp, 1, t, sizeof t); for (int k = 0; k < n->nitems; k++) if (!e_strnicmp(n->items[k], t, e_strlen(t))) { n->cursel = k; wnd_set_text(n, n->items[k]); wnd_invalidate(i, 1); RET(k); return; } RET((u64)-1); return; }   /* CB_SELECTSTRING */
    case 0x14F: { if (wp && !n->dropped) combo_drop(c, i); else if (!wp && n->dropped) combo_drop(c, i); RET(1); return; }   /* CB_SHOWDROPDOWN */
    case 0x157: RET(n->dropped); return;   /* CB_GETDROPPEDSTATE */
    case 0x152: { int fcw, fh; font_dims(w, n->font ? n->font : g_dlg_font, &fcw, &fh); u64 r = lp; int ox, oy; wnd_window_origin(i, &ox, &oy); wr32(c, r, (u32)(ox + SCREEN_X0)); wr32(c, r + 4, (u32)(oy + n->h + SCREEN_Y0)); wr32(c, r + 8, (u32)(ox + n->w + SCREEN_X0)); wr32(c, r + 12, (u32)(oy + n->h + 8 * (fh + 2) + SCREEN_Y0)); RET(1); return; }   /* CB_GETDROPPEDCONTROLRECT */
    case 0x153: RET(list_item_h(w, n)); return; case 0x154: RET(0); return; case 0x155: RET(1); return; case 0x15F: RET(1); return; case 0x160: RET(0); return; case 0x161: RET(1); return; case 0x162: RET(8); return; case 0x15B: RET(0); return; case 0x15D: RET(1); return;
    case 0x140: { if (wp) wr32(c, wp, 0); if (lp) wr32(c, lp, (u32)e_strlen(n->text)); RET(0); return; } case 0x141: RET(0); return; case 0x142: RET(0); return; case 0x145: RET(0); return; case 0x156: RET(1); return;   /* CB_GETEDITSEL / LIMITTEXT / SETEDITSEL / DIR / SETEXTENDEDUI */
    case 0x164: { /* CB_GETCOMBOBOXINFO */ u64 p = lp; u32 sz = rd32(c, p); if (sz >= 4) { wr32(c, p + 36, 0); wrp(c, p + 40, HWND_OF(i)); wrp(c, p + 40 + PSZ_OF(c), HWND_OF(i)); wrp(c, p + 40 + 2 * PSZ_OF(c), n->drop_wnd >= 0 ? HWND_OF(n->drop_wnd) : 0); } RET(1); return; }
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }

/* progress bar: PBM_SETRANGE 0x401 SETPOS 0x402 DELTAPOS 0x403 SETSTEP 0x404 STEPIT 0x405 SETRANGE32 0x406 GETRANGE 0x407 GETPOS 0x408 SETBARCOLOR 0x409 SETBKCOLOR 0x2001 SETMARQUEE 0x40A SETSTATE 0x410 */
static void progress_paint(Cpu *c, int i, u64 hdc) { Wnd *n = &g_wnd[i]; Surf s; if (!dc_surf(c, hdc, &s)) return; int cw = wnd_client_w(i), ch = wnd_client_h(i); fill_rect_px(&s, 0, 0, cw, ch, 0xe6e6e6); edge_rect(&s, 0, 0, cw, ch, 0xbcbcbc, 0xbcbcbc); int range = n->pmax - n->pmin; if (range <= 0) range = 1; int pos = n->ppos - n->pmin; if (pos < 0) pos = 0; if (pos > range) pos = range; int fw = (int)((long long)(cw - 2) * pos / range); u32 col = n->icon == 2 ? 0xda3a3a : n->icon == 3 ? 0xd9b22a : 0x06b025; if (n->style & 4 /* PBS_VERTICAL */) { int fhh = (int)((long long)(ch - 2) * pos / range); fill_rect_px(&s, 1, ch - 1 - fhh, cw - 1, ch - 1, col); } else fill_rect_px(&s, 1, 1, 1 + fw, ch - 1, col); (void)c; }
static void s_NebulaProgressProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_PROGRESS); u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x01: n->pmin = 0; n->pmax = 100; n->ppos = 0; n->pstep = 10; RET(0); return;
    case 0x0F: ctl_do_paint(c, i); return;
    case 0x401: { u32 old = ((u32)n->pmin & 0xffff) | ((u32)n->pmax << 16); n->pmin = (s16)(u16)lp; n->pmax = (s16)(u16)(lp >> 16); wnd_invalidate(i, 0); RET(old); return; }
    case 0x402: { int old = n->ppos; n->ppos = (int)(s32)(u32)wp; if (n->ppos != old) wnd_invalidate(i, 0); RET((u32)old); return; }
    case 0x403: { int old = n->ppos; n->ppos += (int)(s32)(u32)wp; wnd_invalidate(i, 0); RET((u32)old); return; }
    case 0x404: { int old = n->pstep; n->pstep = (int)(s32)(u32)wp; RET((u32)old); return; }
    case 0x405: { int old = n->ppos; n->ppos += n->pstep; if (n->ppos > n->pmax) n->ppos = n->pmin + (n->ppos - n->pmax) % ((n->pmax - n->pmin) ? (n->pmax - n->pmin) : 1); wnd_invalidate(i, 0); RET((u32)old); return; }
    case 0x406: { n->pmin = (int)(s32)(u32)wp; n->pmax = (int)(s32)(u32)lp; wnd_invalidate(i, 0); RET(0); return; }
    case 0x407: { if (lp) { wr32(c, lp, (u32)n->pmin); wr32(c, lp + 4, (u32)n->pmax); } RET(wp ? (u32)n->pmin : (u32)n->pmax); return; }
    case 0x408: RET((u32)n->ppos); return;
    case 0x409: case 0x2001: RET(0xffffffff); return; case 0x40A: wnd_invalidate(i, 0); RET(1); return; case 0x40D: RET(n->pstep); return;
    case 0x410: { u64 old = n->icon ? n->icon : 1; n->icon = wp; wnd_invalidate(i, 0); RET(old); return; } case 0x411: RET(n->icon ? n->icon : 1); return;
    default: if (ctl_common(c, i, msg, wp, lp)) return; def_window_proc(c, i, msg, wp, lp); return; } }
static void s_NebulaScrollBarProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } SUPER_ADOPT(i, BI_SCROLLBAR); u32 msg = (u32)ARG(1); if (msg == 0x0F) { u64 hdc = ctl_paint_begin(c, i); Surf s; if (hdc && dc_surf(c, hdc, &s)) { fill_rect_px(&s, 0, 0, s.w, s.h, 0xf0f0f0); edge_rect(&s, 0, 0, s.w, s.h, 0xdcdcdc, 0xdcdcdc); } if (hdc) ctl_paint_end(c, i, hdc); RET(0); return; } if (ctl_common(c, i, msg, ARG(2), ARG(3))) return; def_window_proc(c, i, msg, ARG(2), ARG(3)); }
static int g_unsupported_reported;
static void s_NebulaUnsupportedProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1); Wnd *n = &g_wnd[i]; Win32 *w = W;
  if (msg == 0x01) { if (g_unsupported_reported++ < 6) w32_report(w, "control class \"%s\" is not implemented yet — shown as an empty box", n->clsname); RET(0); return; }
  if (msg == 0x0F) { u64 hdc = ctl_paint_begin(c, i); Surf s; if (hdc && dc_surf(c, hdc, &s)) { fill_rect_px(&s, 0, 0, s.w, s.h, 0xffffff); edge_rect(&s, 0, 0, s.w, s.h, 0xa0a0a0, 0xa0a0a0); GdiObj *dc = gdi_get(w, hdc); if (dc && s.h >= 16) { dc->text_color = px2cr(0x9a9a9a); dc->bk_mode = 1; draw_text(&s, dc, w, 4, 3, n->clsname, (int)e_strlen(n->clsname)); } } if (hdc) ctl_paint_end(c, i, hdc); RET(0); return; }
  if (msg == 0x1002 || msg == 0x1032 || msg == 0x1104) { RET(0); return; }   /* LVM_GETITEMCOUNT ... */
  if (ctl_common(c, i, msg, ARG(2), ARG(3))) return; def_window_proc(c, i, msg, ARG(2), ARG(3)); }
/* host-side direct invocation of a builtin class procedure (no guest round trip; used for WM_CREATE/WM_DESTROY/focus of builtins) */
static void builtin_proc_direct(Cpu *c, int i, u32 msg, u64 wp, u64 lp) { switch (g_wnd[i].builtin) { case BI_MENU: call_shim_tail(c, s_NebulaMenuProc, HWND_OF(i), msg, wp, lp); break; case BI_BUTTON: call_shim_tail(c, s_NebulaButtonProc, HWND_OF(i), msg, wp, lp); break; case BI_STATIC: case BI_LINK: call_shim_tail(c, s_NebulaStaticProc, HWND_OF(i), msg, wp, lp); break; case BI_EDIT: call_shim_tail(c, s_NebulaEditProc, HWND_OF(i), msg, wp, lp); break; case BI_COMBO: call_shim_tail(c, s_NebulaComboProc, HWND_OF(i), msg, wp, lp); break; case BI_LISTBOX: case BI_COMBOLIST: call_shim_tail(c, s_NebulaListBoxProc, HWND_OF(i), msg, wp, lp); break; case BI_PROGRESS: call_shim_tail(c, s_NebulaProgressProc, HWND_OF(i), msg, wp, lp); break; case BI_STATUS: call_shim_tail(c, s_NebulaStatusProc, HWND_OF(i), msg, wp, lp); break; case BI_TOOLBAR: call_shim_tail(c, s_NebulaToolbarProc, HWND_OF(i), msg, wp, lp); break; case BI_TAB: call_shim_tail(c, s_NebulaTabProc, HWND_OF(i), msg, wp, lp); break; case BI_SCROLLBAR: call_shim_tail(c, s_NebulaScrollBarProc, HWND_OF(i), msg, wp, lp); break; case BI_DIALOG: case BI_MSGBOX: break; default: call_shim_tail(c, s_NebulaUnsupportedProc, HWND_OF(i), msg, wp, lp); break; } }

/* ---------- dialogs ---------- */
static int dlg_find_id(int d, u64 id) { for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == d && g_wnd[k].id == id) return k; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].id == id && wnd_is_child_of(k, d)) return k; return -1; }
static void s_GetDlgItem(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); RET(k >= 0 ? HWND_OF(k) : 0); }
static void s_GetDlgCtrlID(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 ? (u32)g_wnd[i].id : 0); }
static void s_SendDlgItemMessageW(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { RET(0); return; } dispatch_to_wndproc(c, HWND_OF(k), (u32)ARG(2), ARG(3), ARG(4)); }
static void s_SendDlgItemMessageA(Cpu *c) { s_SendDlgItemMessageW(c); }
static void s_SetDlgItemTextW(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { RET(0); return; } call_shim_tail(c, s_SetWindowTextW, HWND_OF(k), ARG(2), 0, 0); }
static void s_SetDlgItemTextA(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { RET(0); return; } call_shim_tail(c, s_SetWindowTextA, HWND_OF(k), ARG(2), 0, 0); }
static void s_GetDlgItemTextW(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { if (ARG(3)) wr16(c, ARG(2), 0); RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { if (ARG(3)) wr16(c, ARG(2), 0); RET(0); return; } RET(text_out_cap(c, ARG(2), g_wnd[k].text, (u32)ARG(3), 1)); }
static void s_GetDlgItemTextA(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { if (ARG(3)) wr8(c, ARG(2), 0); RET(0); return; } RET(text_out_cap(c, ARG(2), g_wnd[k].text, (u32)ARG(3), 0)); }
static void s_SetDlgItemInt(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { RET(0); return; } char t[32]; if (ARG(3)) e_snprintf(t, sizeof t, "%d", (int)(s32)(u32)ARG(2)); else e_snprintf(t, sizeof t, "%u", (u32)ARG(2)); wnd_set_text(&g_wnd[k], t); g_wnd[k].sel0 = g_wnd[k].sel1 = (int)e_strlen(t); wnd_invalidate(k, 1); RET(1); }
static void s_GetDlgItemInt(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { if (ARG(2)) wr32(c, ARG(2), 0); RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { if (ARG(2)) wr32(c, ARG(2), 0); RET(0); return; } const char *p = g_wnd[k].text; while (*p == ' ') p++; int neg = 0; if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++; if (*p < '0' || *p > '9') { if (ARG(2)) wr32(c, ARG(2), 0); RET(0); return; } u32 v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0'); if (ARG(2)) wr32(c, ARG(2), 1); RET(neg ? (u32)-(s32)v : v); }
static void s_CheckDlgButton(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); if (k < 0) { RET(0); return; } g_wnd[k].checked = (int)ARG(2); wnd_invalidate(k, 1); RET(1); }
static void s_IsDlgButtonChecked(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_find_id(d, (u32)ARG(1)); RET(k >= 0 ? (u32)g_wnd[k].checked : 0); }
static void s_CheckRadioButton(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } u32 first = (u32)ARG(1), last = (u32)ARG(2), sel = (u32)ARG(3); for (u32 id = first; id <= last; id++) { int k = dlg_find_id(d, id); if (k >= 0) { g_wnd[k].checked = id == sel; wnd_invalidate(k, 1); } } RET(1); }
static void s_MapDialogRect(Cpu *c) { int d = wnd_of(ARG(0)); u64 r = ARG(1); int bx = d >= 0 && g_wnd[d].base_x ? g_wnd[d].base_x : 7, by = d >= 0 && g_wnd[d].base_y ? g_wnd[d].base_y : 13; wr32(c, r, (u32)((s32)rd32(c, r) * bx / 4)); wr32(c, r + 4, (u32)((s32)rd32(c, r + 4) * by / 8)); wr32(c, r + 8, (u32)((s32)rd32(c, r + 8) * bx / 4)); wr32(c, r + 12, (u32)((s32)rd32(c, r + 12) * by / 8)); RET(1); }
static void s_GetDialogBaseUnits(Cpu *c) { RET(7 | (13 << 16)); }
/* tab order: children of the dialog in creation order with WS_TABSTOP, visible and enabled */
static int dlg_next_tab(int d, int from, int back) { int order[MAX_WND]; int n = 0; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == d) order[n++] = k; for (int a = 1; a < n; a++) { int v = order[a], b = a - 1; while (b >= 0 && g_wnd[order[b]].zseq > g_wnd[v].zseq) { order[b + 1] = order[b]; b--; } order[b + 1] = v; }
  int cur = -1; for (int a = 0; a < n; a++) if (order[a] == from) cur = a; for (int step = 1; step <= n; step++) { int a = cur < 0 ? (back ? n - step : step - 1) : ((cur + (back ? n - step : step)) % n); int k = order[a]; if ((g_wnd[k].style & 0x10000) && g_wnd[k].visible && g_wnd[k].enabled && g_wnd[k].builtin != BI_STATIC) return k; } return -1; }
static void s_GetNextDlgTabItem(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int k = dlg_next_tab(d, wnd_of(ARG(1)), ARG(2) != 0); RET(k >= 0 ? HWND_OF(k) : 0); }
static void s_GetNextDlgGroupItem(Cpu *c) { int d = wnd_of(ARG(0)); if (d < 0) { RET(0); return; } int from = wnd_of(ARG(1)); int k = dlg_next_tab(d, from, ARG(2) != 0); RET(k >= 0 ? HWND_OF(k) : (from >= 0 ? ARG(1) : 0)); }
/* DefDlgProc: run the dialog procedure, then default handling when it returns FALSE */
static void dlg_default(Cpu *c, int i, u32 msg, u64 wp, u64 lp) { Win32 *w = W; Wnd *n = &g_wnd[i];
  switch (msg) {
    case 0x14: { Surf s; if (dc_surf(c, wp, &s)) { fill_rect_px(&s, 0, 0, s.w, s.h, 0xf0f0f0); mark_screen(w, wp); } RET(1); return; }
    case 0x10: post_msg_to(w, HWND_OF(i), 0x111, 2 /* IDCANCEL */, 0); RET(0); return;
    case 0x07: { int f = -1; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == i && g_wnd[k].focus_draw) { f = k; break; } if (f < 0) f = dlg_next_tab(i, -1, 0); if (f >= 0) set_focus(c, f); RET(0); return; }
    case 0x110: RET(0); return;
    case 0x111: { /* an unhandled IDOK/IDCANCEL in a modal dialog with no dlgproc: end it */ if (!n->dlgproc && n->modal && ((wp & 0xffff) == 1 || (wp & 0xffff) == 2)) { n->ended = 1; n->result = wp & 0xffff; } RET(0); return; }
    case 0x400 + 1: /* DM_SETDEFID */ n->def_id = (int)wp; wnd_invalidate(i, 1); RET(1); return;
    case 0x400 + 0: RET(n->def_id ? (0x534B0000u | (u32)n->def_id) : 0); return;   /* DM_GETDEFID */
    case 0x402: RET(0); return;   /* DM_REPOSITION */
    default: def_window_proc(c, i, msg, wp, lp); return; } }
static void s_DefDlgProcW(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1); Wnd *n = &g_wnd[i];
  if (msg == 0x400 || msg == 0x401 || msg == 0x402) { dlg_default(c, i, msg, ARG(2), ARG(3)); return; }
  if (!n->dlgproc) { dlg_default(c, i, msg, ARG(2), ARG(3)); return; }
  n->msgresult = 0; n->extra[0] = 0; Win32 *w = W; w32_push_cont(c, CONT_DLGPROC, (u64)i); w->cont[w->ncont - 1].data[1] = msg; w->cont[w->ncont - 1].data[2] = ARG(2); w->cont[w->ncont - 1].data[3] = ARG(3); w32_guest_call(c, n->dlgproc, HWND_OF(i), msg, ARG(2), ARG(3)); }
static void s_DefDlgProcA(Cpu *c) { s_DefDlgProcW(c); }
static void dlgproc_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; int i = (int)w->cont[ci].data[0]; u32 msg = (u32)w->cont[ci].data[1]; u64 wp = w->cont[ci].data[2], lp = w->cont[ci].data[3];
  if (!wnd_alive(i)) { w32_cont_finish(c, 0); return; } Wnd *n = &g_wnd[i]; u64 r = c->mode32 ? (u32)rax : rax;
  if (r) { /* handled: WM_CTLCOLOR* / compare / init return the value itself, everything else returns DWLP_MSGRESULT */ if ((msg >= 0x132 && msg <= 0x138) || msg == 0x39 || msg == 0x2E || msg == 0x2F || msg == 0x37 || msg == 0x110) { w32_cont_finish(c, r); return; } w32_cont_finish(c, n->extra[0]); return; }
  /* not handled: default processing runs as if DefDlgProc were still executing (pop the frame, keep its return address) */
  u64 rsp = w->cont[ci].rsp, ret = w->cont[ci].ret_addr; u16 cl = w->cont[ci].clean32; w->ncont--; c->r[RSP] = rsp; c->rip = ret;
  dlg_default(c, i, msg, wp, lp);
  if (w->ncont > ci) { w->cont[ci].clean32 = cl; return; }   /* a nested guest call was started: its frame now carries the cleanup */
  if (c->status == RUN_OK) { c->r[RSP] = rsp + cl; if (c->mode32) c->r[RDX] = c->r[RAX] >> 32; } }
/* dialog templates */
typedef struct { u32 style, exstyle; int x, y, cx, cy; u32 id; char cls[64]; char title[512]; u16 ordinal; u32 helpid; } DlgItem;
static u64 tmpl_sz_or_ord(Cpu *c, u64 p, char *out, size_t cap, u16 *ord) { *ord = 0; out[0] = 0; u16 w0 = rd16(c, p); if (w0 == 0) return p + 2; if (w0 == 0xFFFF) { *ord = rd16(c, p + 2); e_snprintf(out, cap, "#%u", *ord); return p + 4; } int n = 0; while (rd16(c, p) && n < 4000) { p += 2; n++; } wstr_to_utf8(c, p - (u64)n * 2, out, cap, n); return p + 2; }
static int dlg_parse_header(Cpu *c, u64 t, int *ex, u32 *style, u32 *exstyle, int *cdit, int *x, int *y, int *cx, int *cy, char *cls, char *title, int *fontpt, char *face, u64 *items) {
  *ex = rd16(c, t) == 1 && rd16(c, t + 2) == 0xFFFF; u64 p = t;
  if (*ex) { *exstyle = rd32(c, t + 8); *style = rd32(c, t + 12); *cdit = rd16(c, t + 16); *x = (s16)rd16(c, t + 18); *y = (s16)rd16(c, t + 20); *cx = (s16)rd16(c, t + 22); *cy = (s16)rd16(c, t + 24); p = t + 26; }
  else { *style = rd32(c, t); *exstyle = rd32(c, t + 4); *cdit = rd16(c, t + 8); *x = (s16)rd16(c, t + 10); *y = (s16)rd16(c, t + 12); *cx = (s16)rd16(c, t + 14); *cy = (s16)rd16(c, t + 16); p = t + 18; }
  u16 ord; char menu[64]; p = tmpl_sz_or_ord(c, p, menu, sizeof menu, &ord); p = tmpl_sz_or_ord(c, p, cls, 64, &ord); char tmp[512]; int n = 0; while (rd16(c, p + n * 2) && n < 500) n++; wstr_to_utf8(c, p, title, 512, n); p += (u64)(n + 1) * 2; (void)tmp;
  *fontpt = 0; face[0] = 0; if (*style & 0x40 /* DS_SETFONT */ || (*ex && (*style & 0x48))) { *fontpt = rd16(c, p); p += 2; if (*ex) p += 4; /* weight, italic, charset */ n = 0; while (rd16(c, p + n * 2) && n < 60) n++; wstr_to_utf8(c, p, face, 64, n); p += (u64)(n + 1) * 2; }
  *items = (p + 3) & ~3ull; return 1; }
static u64 dlg_parse_item(Cpu *c, u64 p, int ex, DlgItem *it) { p = (p + 3) & ~3ull;
  if (ex) { it->helpid = rd32(c, p); it->exstyle = rd32(c, p + 4); it->style = rd32(c, p + 8); it->x = (s16)rd16(c, p + 12); it->y = (s16)rd16(c, p + 14); it->cx = (s16)rd16(c, p + 16); it->cy = (s16)rd16(c, p + 18); it->id = rd32(c, p + 20); p += 24; }
  else { it->style = rd32(c, p); it->exstyle = rd32(c, p + 4); it->x = (s16)rd16(c, p + 8); it->y = (s16)rd16(c, p + 10); it->cx = (s16)rd16(c, p + 12); it->cy = (s16)rd16(c, p + 14); it->id = rd16(c, p + 16); p += 18; }
  u16 ord; p = tmpl_sz_or_ord(c, p, it->cls, sizeof it->cls, &ord); if (ord) { static const char *names[] = { "Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox" }; if (ord >= 0x80 && ord <= 0x85) e_snprintf(it->cls, sizeof it->cls, "%s", names[ord - 0x80]); }
  p = tmpl_sz_or_ord(c, p, it->title, sizeof it->title, &ord); it->ordinal = ord; u16 extra = rd16(c, p); p += 2 + extra; return p; }
/* CONT_DLGINIT chain: data[0] dialog idx, data[1] template items ptr, data[2] remaining count, data[3] ex flag, data[4] param, data[5] modal flag, data[6] slot (for the modal re-entry), idx = step */
static void dlg_create_next(Cpu *c);
static int dlg_begin(Cpu *c, u64 hinst, u64 tmpl, u64 owner_h, u64 dlgproc, u64 param, int modal) {
  Win32 *w = W; builtin_init_slots(w); int ex; u32 style, exstyle; int cdit, x, y, cx, cy, fontpt; char cls[64], title[512], face[64]; u64 items;
  if (!dlg_parse_header(c, tmpl, &ex, &style, &exstyle, &cdit, &x, &y, &cx, &cy, cls, title, &fontpt, face, &items)) return -1;
  if (w->api_trace) { char msg[400]; e_snprintf(msg, sizeof msg, "[user] dialog%s style=%08x ex=%08x items=%d at %d,%d %dx%d DLU font %dpt \"%s\" \"%.40s\"\n", ex ? "ex" : "", style, exstyle, cdit, x, y, cx, cy, fontpt, face, title); host_write(3, msg, e_strlen(msg)); }
  int fh = fontpt ? (fontpt * 96 + 36) / 72 : 13; if (fh < 8) fh = 8; if (fh > 40) fh = 40; int fcw = (fh * 11 + 10) / 20; if (fcw < 3) fcw = 3; int bx = fcw, by = (fh * 6 + 2) / 5;   /* dialog base units: average char width × cell height (tmHeight ≈ 1.2 em: 8 pt → 6×13 like Windows) */
  int pw = cx * bx / 4, ph = cy * by / 8; const char *wcls = cls[0] && cls[0] != '#' ? cls : "#32770"; int owner = wnd_of(owner_h);
  u32 wstyle = (style & ~0x10000000u) | (owner >= 0 || !(style & 0x40000000u) ? 0 : 0); if (!(wstyle & 0x40000000u)) wstyle |= 0x80000000u;   /* WS_POPUP unless WS_CHILD */
  int fx = 0, fy = 0; if (!g_wnd[0].used && !(wstyle & 0x40000000u) && (style & 0x00C00000u) == 0x00C00000u) { fx = 16; fy = 39; }   /* template cx/cy are CLIENT units; the main-window path takes a full window size */
  int i = wnd_create_obj(c, wcls, title, wstyle | 0x00C00000u * ((style & 0x00C00000u) == 0x00C00000u ? 1 : 0), exstyle, (style & 0x0800) || (style & 0x40000000u) == 0 ? 0x80000000 : x * bx / 4 + SCREEN_X0, (style & 0x0800) || (style & 0x40000000u) == 0 ? 0x80000000 : y * by / 8 + SCREEN_Y0, pw + fx, ph + fy, owner_h, 0, hinst, param, 1, 0);
  if (i < 0) return -1; Wnd *n = &g_wnd[i]; n->is_dlg = 1; n->dlgproc = dlgproc; n->modal = modal; n->base_x = bx; n->base_y = by; n->cls_brush = 0; n->builtin = n->builtin ? n->builtin : BI_DIALOG; if (!cls[0] || cls[0] == '#') n->wndproc = g_slot_dialog;
  GdiObj *f = gdi_new(w, K_FONT); if (f) { f->font_h = by; f->font_em = fh; n->font = f->handle; } else n->font = g_dlg_font;   /* line height = cell height (13 for 8 pt), char width from the em (6) */
  if (modal && owner >= 0) { g_wnd[owner].enabled = 0; wnd_invalidate(owner, 1); }
  n->style = (n->style & ~0x10000000u) | (style & 0x10000000u); n->visible = 0;
  w32_push_cont(c, CONT_DLGINIT, (u64)i); u64 *d = w->cont[w->ncont - 1].data; d[1] = items; d[2] = (u64)cdit; d[3] = (u64)ex; d[4] = param; d[5] = (u64)modal; d[6] = w32_cur_slot(); d[7] = 0; return i; }
static void dlg_create_next(Cpu *c) {
  Win32 *w = W; int ci = w->ncont - 1; u64 *d = w->cont[ci].data; int i = (int)d[0]; Wnd *dn = &g_wnd[i];
  while (d[2] > 0) {   /* controls: builtin classes are created here, guest classes go through their own WM_CREATE chain */
    DlgItem it; d[1] = dlg_parse_item(c, d[1], (int)d[3], &it); d[2]--;
    char title[512]; if (it.ordinal && !it.title[0]) e_snprintf(title, sizeof title, "#%u", it.ordinal); else e_snprintf(title, sizeof title, "%s", it.title);
    int px = it.x * dn->base_x / 4, py = it.y * dn->base_y / 8, pcx = it.cx * dn->base_x / 4, pcy = it.cy * dn->base_y / 8;
    int k = wnd_create_obj(c, it.cls, title, it.style | 0x40000000u, it.exstyle, px, py, pcx, pcy, HWND_OF(i), it.id, dn->inst, 0, 1, 0); if (k < 0) continue;
    Wnd *kn = &g_wnd[k]; kn->font = dn->font; if (it.ordinal && kn->builtin == BI_STATIC) kn->icon = 0x7B001 + it.ordinal * 0x10;
    if (kn->builtin) { builtin_proc_direct(c, k, 0x01, 0, 0); if (c->status) return; if (kn->style & 0x10000000u) { kn->visible = 1; kn->invalid = 1; } }
    else { u64 cs = make_createstruct(c, k, 0, 0, 0); w32_push_cont(c, CONT_WNDCREATE, (u64)k); w->cont[w->ncont - 1].data[1] = cs; w32_guest_call(c, kn->wndproc, HWND_OF(k), 0x0081, 0, cs); return; } }
  if (w->cont[ci].idx == 0) {   /* all controls exist: WM_INITDIALOG with the first tab stop as focus candidate */
    w->cont[ci].idx = 1; int f = dlg_next_tab(i, -1, 0); d[7] = f >= 0 ? (u64)f + 1 : 0; for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == i && g_wnd[k].builtin == BI_BUTTON && (g_wnd[k].style & 0xF) == 1 && !dn->def_id) dn->def_id = (int)g_wnd[k].id;
    w32_guest_call(c, dn->wndproc, HWND_OF(i), 0x110, f >= 0 ? HWND_OF(f) : 0, d[4]); return; }
  /* after WM_INITDIALOG: focus, show, done */
  if (!wnd_alive(i)) { w32_cont_finish(c, 0); return; }
  if (w->cont[ci].idx <= 2) {   /* step 2: show + initial focus; the focus change may notify the dialog procedure (EN_SETFOCUS …) = a nested guest call, after which we come back here at step 3 */
    w->cont[ci].idx = 3;
    if ((dn->style & 0x10000000u) || d[5]) { /* modal dialogs are shown regardless of WS_VISIBLE */ dn->style |= 0x10000000u; wnd_show_now(c, i); for (int k = 0; k < MAX_WND; k++) if (wnd_alive(k) && g_wnd[k].parent == i && (g_wnd[k].style & 0x10000000u)) g_wnd[k].visible = 1; }
    if (!wnd_alive(g_focus) || wnd_root(g_focus) != i) { if (d[7]) { set_focus(c, (int)d[7] - 1); if (w->ncont > ci + 1 || c->status != RUN_OK) return; } } }
  if (d[5]) { /* modal: hand over to the DialogBox loop by re-executing its shim */ u64 slot = d[6]; w->ncont--; c->r[RSP] = w->cont[w->ncont].rsp; cpu_push(c, w->cont[w->ncont].ret_addr); c->rip = slot; return; }
  w32_cont_finish(c, HWND_OF(i)); }
static void dlginit_resume(Cpu *c, u64 rax) { Win32 *w = W; int ci = w->ncont - 1; if (w->cont[ci].idx == 1) { /* WM_INITDIALOG returned: FALSE means the dialog set focus itself */ int i = (int)w->cont[ci].data[0]; if (!(u32)rax) w->cont[ci].data[7] = 0; (void)i; } dlg_create_next(c); }
static void s_CreateDialogIndirectParamW(Cpu *c) { int i = dlg_begin(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0); if (i < 0) { RET(0); return; } dlg_create_next(c); }
static void s_CreateDialogIndirectParamA(Cpu *c) { s_CreateDialogIndirectParamW(c); }
static void s_CreateDialogParamW(Cpu *c) { u64 e = rsrc_lookup(c, ARG(0), ARG(1), 5, 1, 0); if (!e) { char nm[64]; if (ARG(1) >= 0x10000) text_in(c, ARG(1), 1, nm, sizeof nm); else e_snprintf(nm, sizeof nm, "#%u", (u32)ARG(1)); w32_report(W, "CreateDialogParam: dialog resource %s not found", nm); W->last_error = 1814; RET(0); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; int i = dlg_begin(c, ARG(0), base + rd32(c, e), ARG(2), ARG(3), ARG(4), 0); if (i < 0) { RET(0); return; } dlg_create_next(c); }
static void s_CreateDialogParamA(Cpu *c) { u64 e = rsrc_lookup(c, ARG(0), ARG(1), 5, 0, 0); if (!e) { W->last_error = 1814; RET(0); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; int i = dlg_begin(c, ARG(0), base + rd32(c, e), ARG(2), ARG(3), ARG(4), 0); if (i < 0) { RET(0); return; } dlg_create_next(c); }
/* modal loop: DialogBoxParam / MessageBox / SHBrowseForFolder re-execute their shim after every dispatched message until EndDialog.
   g_modal[].rip/rsp identify the re-entry; phase 1 = running, 2 = ending (window being destroyed) */
static struct { u64 rip, rsp, slot; int wnd; int phase; } g_modal[8]; static int g_nmodal;
static void modal_reexec(Cpu *c) { int m = g_nmodal - 1; if (m < 0) { RET(0); return; } cpu_push(c, c->rip); c->rip = g_modal[m].slot; }
static int modal_reentry(Cpu *c) { int m = g_nmodal - 1; return m >= 0 && g_modal[m].rip == c->rip && g_modal[m].rsp == c->r[RSP]; }
static void modal_resume(Cpu *c, u64 rax) { (void)rax; Win32 *w = W; int ci = w->ncont - 1; w->ncont--; c->r[RSP] = w->cont[ci].rsp; c->rip = w->cont[ci].ret_addr; modal_reexec(c); }
static u64 modal_result(Win32 *w, int m) { int i = g_modal[m].wnd; u64 res = g_wnd[i].result; if (g_wnd[i].dlgproc == w32_shim_slot(w, "user32", "NebulaBrowseProc")) res = res ? pidl_make(w, g_wnd[i].text) : 0; return res; }
static void modal_loop(Cpu *c, int i) {
  Win32 *w = W; int m = g_nmodal - 1; Wnd *n = &g_wnd[i];
  if (!wnd_alive(i)) { u64 res = modal_result(w, m); g_nmodal--; RET(res); return; }
  if (n->ended) {
    int ow = n->owner; if (ow >= 0 && wnd_alive(ow)) { g_wnd[ow].enabled = 1; wnd_invalidate(ow, 1); g_active = ow; if (!wnd_alive(g_focus) || wnd_root(g_focus) == i) g_focus = ow; }
    g_modal[m].phase = 2; n->visible = 0; wnd_invalidate_all(); w->dirty = 1;
    w32_push_cont(c, CONT_MODAL, (u64)i); destroy_window(c, i); return; }   /* destroy finishes → chained CONT_MODAL (phase 2) returns the result */
  GuestMsg msg; if (!next_message(c, &msg, 1, 0)) { w->blits_since_yield = 0; w32_retry_yield(c); return; }
  if (msg.msg == 0x12) { w->quit_posted = 1; n->ended = 1; n->result = 0; modal_loop(c, i); return; }
  if (dlg_key_message(c, i, &msg)) return;
  translate_msg(c, msg.hwnd, msg.msg, msg.wparam, msg.lparam);
  int t = wnd_of(msg.hwnd); if (t < 0 || !g_wnd[t].wndproc) { modal_reexec(c); return; }
  if (msg.msg == 0x113 && msg.lparam > 0x10000) { w32_push_cont(c, CONT_MODAL, (u64)i); w32_guest_call(c, msg.lparam, msg.hwnd, msg.msg, msg.wparam, (u64)(u32)(host_now_ms() - w->start_ms)); return; }
  w32_push_cont(c, CONT_MODAL, (u64)i); w32_guest_call(c, g_wnd[t].wndproc, msg.hwnd, msg.msg, msg.wparam, msg.lparam); }
static void dialog_box(Cpu *c, u64 hinst, u64 tmpl, u64 owner_h, u64 dlgproc, u64 param) {
  int m = g_nmodal - 1; if (modal_reentry(c)) { modal_loop(c, g_modal[m].wnd); return; }
  if (g_nmodal >= 8) { RET((u64)-1); return; } int i = dlg_begin(c, hinst, tmpl, owner_h, dlgproc, param, 1); if (i < 0) { RET((u64)-1); return; }
  m = g_nmodal++; g_modal[m].rip = c->rip; g_modal[m].rsp = c->r[RSP]; g_modal[m].slot = w32_cur_slot(); g_modal[m].wnd = i; g_modal[m].phase = 1; dlg_create_next(c); }
static void s_DialogBoxIndirectParamW(Cpu *c) { dialog_box(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }
static void s_DialogBoxIndirectParamA(Cpu *c) { dialog_box(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }
static void s_DialogBoxParamW(Cpu *c) { if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; } u64 e = rsrc_lookup(c, ARG(0), ARG(1), 5, 1, 0); if (!e) { W->last_error = 1814; RET((u64)-1); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; dialog_box(c, ARG(0), base + rd32(c, e), ARG(2), ARG(3), ARG(4)); }
static void s_DialogBoxParamA(Cpu *c) { if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; } u64 e = rsrc_lookup(c, ARG(0), ARG(1), 5, 0, 0); if (!e) { W->last_error = 1814; RET((u64)-1); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; dialog_box(c, ARG(0), base + rd32(c, e), ARG(2), ARG(3), ARG(4)); }
static void s_EndDialog(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; n->ended = 1; n->result = ARG(1); if (!n->modal) { n->visible = 0; wnd_invalidate_all(); } RET(1); }
/* IsDialogMessage: Tab / Shift+Tab, Enter (default button or the focused button), Esc (IDCANCEL); other messages for the dialog's windows are translated + dispatched */
/* after a dialog key was consumed: a modal loop re-executes its shim, IsDialogMessage returns TRUE */
static int dlg_modal_of(int d) { int m = g_nmodal - 1; return m >= 0 && g_modal[m].wnd == d; }
static void dlg_key_done(Cpu *c, int d) { if (dlg_modal_of(d)) modal_reexec(c); else RET(1); }
/* run `fn` (which may start a guest call) and make sure the dialog-key epilogue happens afterwards: a continuation is pushed first
   so that a nested call chains into it; if nothing was deferred it is settled immediately */
static int dlg_key_mark(Cpu *c, int d) { Win32 *w = W; w32_push_cont(c, dlg_modal_of(d) ? CONT_MODAL : CONT_RET_VAL_, dlg_modal_of(d) ? (u64)d : 1); return w->ncont - 1; }
static void dlg_key_settle(Cpu *c, int idx, int d) { Win32 *w = W; if (w->ncont - 1 == idx && c->r[RSP] == w->cont[idx].rsp && c->status == RUN_OK) { w->ncont--; dlg_key_done(c, d); } }   /* a started guest call lowered RSP: the mark stays and receives its result */
static int dlg_key_message(Cpu *c, int d, GuestMsg *m) {
  Win32 *w = W; if (m->msg != 0x100 && m->msg != 0x102) return 0; if (g_wnd[d].builtin == BI_MENU) return 0; int t = wnd_of(m->hwnd); if (t < 0 || (t != d && !wnd_is_child_of(t, d))) return 0; Wnd *dn = &g_wnd[d];
  u64 vk = m->wparam;
  if (m->msg == 0x102) { if (vk == '\t' || vk == '\r' || vk == 27) { dlg_key_done(c, d); return 1; } return 0; }
  int in_dlg_focus = wnd_alive(g_focus) && (g_focus == d || wnd_is_child_of(g_focus, d));
  if (vk == 0x09) { if (in_dlg_focus && g_wnd[g_focus].builtin == BI_EDIT && (g_wnd[g_focus].style & 4) && !w->key_state[0x11]) return 0; int f = dlg_next_tab(d, in_dlg_focus ? g_focus : -1, w->key_state[0x10]); int idx = dlg_key_mark(c, d); if (f >= 0) set_focus(c, f); dlg_key_settle(c, idx, d); return 1; }
  if (vk == 0x0D) { int target = -1; if (in_dlg_focus && g_wnd[g_focus].parent == d && g_wnd[g_focus].builtin == BI_BUTTON && ((g_wnd[g_focus].style & 0xF) <= 1 || (g_wnd[g_focus].style & 0xF) == 8)) target = g_focus; else if (dn->def_id) target = dlg_find_id(d, (u64)dn->def_id); if (target < 0) target = dlg_find_id(d, 1);
    if (target >= 0 && wnd_enabled_chain(target) && g_wnd[target].visible) { int idx = dlg_key_mark(c, d); if (g_wnd[target].builtin == BI_BUTTON) button_click(c, target); else w32_guest_call(c, dn->wndproc, HWND_OF(d), 0x111, g_wnd[target].id & 0xffff, HWND_OF(target)); dlg_key_settle(c, idx, d); return 1; }
    dlg_key_done(c, d); return 1; }
  if (vk == 0x1B) { int idx = dlg_key_mark(c, d); w32_guest_call(c, dn->wndproc, HWND_OF(d), 0x111, 2, 0); dlg_key_settle(c, idx, d); return 1; }
  if ((vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28) && in_dlg_focus && g_wnd[g_focus].parent == d && g_wnd[g_focus].builtin == BI_BUTTON && ((g_wnd[g_focus].style & 0xF) == 4 || (g_wnd[g_focus].style & 0xF) == 9)) {
    int f = dlg_next_tab(d, g_focus, vk == 0x25 || vk == 0x26); if (f >= 0 && g_wnd[f].builtin == BI_BUTTON && ((g_wnd[f].style & 0xF) == 4 || (g_wnd[f].style & 0xF) == 9)) { set_focus(c, f); int idx = dlg_key_mark(c, d); button_click(c, f); dlg_key_settle(c, idx, d); return 1; } return 0; }
  return 0; }
static void s_IsDialogMessageW(Cpu *c) { int d = wnd_of(ARG(0)); u64 a = ARG(1); if (d < 0) { RET(0); return; } GuestMsg m; m.hwnd = rdp(c, a); m.msg = rd32(c, MSG_MSG(a)); m.wparam = rdp(c, MSG_WP(a)); m.lparam = rdp(c, MSG_LP(a)); int t = wnd_of(m.hwnd); if (t < 0 || (t != d && !wnd_is_child_of(t, d))) { RET(0); return; }
  if (dlg_key_message(c, d, &m)) return; translate_msg(c, m.hwnd, m.msg, m.wparam, m.lparam); if (!g_wnd[t].wndproc) { RET(1); return; }
  if (m.msg == 0x113 && m.lparam > 0x10000) { w32_push_cont(c, CONT_RET_VAL_, 1); w32_guest_call(c, m.lparam, m.hwnd, m.msg, m.wparam, (u64)(u32)(host_now_ms() - W->start_ms)); return; }
  send_to(c, t, m.msg, m.wparam, m.lparam, CONT_RET_VAL_, 1); }
static void s_IsDialogMessageA(Cpu *c) { s_IsDialogMessageW(c); }
static void s_IsDialogMessage(Cpu *c) { s_IsDialogMessageW(c); }
/* MessageBox: a dialog built in memory (static text + buttons), run through the modal loop; the native runner auto-dismisses unless X64_MSGBOX_MODAL=1 */
static u16 *g_mb_tmpl; static size_t g_mb_cap;
static void mb_put16(size_t *n, u16 v) { if (*n + 1 >= g_mb_cap) { size_t nc = g_mb_cap ? g_mb_cap * 2 : 4096; u16 *nb = emu_alloc(nc * 2); if (g_mb_tmpl) { e_memcpy(nb, g_mb_tmpl, *n * 2); emu_free(g_mb_tmpl); } g_mb_tmpl = nb; g_mb_cap = nc; } g_mb_tmpl[(*n)++] = v; }
static void mb_put32(size_t *n, u32 v) { mb_put16(n, (u16)v); mb_put16(n, (u16)(v >> 16)); }
static void mb_puts(size_t *n, const char *s) { const u8 *p = (const u8 *)s; while (*p) { u32 cp = *p, extra = 0; if (cp >= 0xF0) { cp &= 7; extra = 3; } else if (cp >= 0xE0) { cp &= 15; extra = 2; } else if (cp >= 0xC0) { cp &= 31; extra = 1; } p++; for (u32 k = 0; k < extra && (*p & 0xC0) == 0x80; k++) cp = (cp << 6) | (*p++ & 0x3f); if (cp >= 0x10000) { cp -= 0x10000; mb_put16(n, (u16)(0xD800 | (cp >> 10))); mb_put16(n, (u16)(0xDC00 | (cp & 0x3ff))); } else mb_put16(n, (u16)cp); } mb_put16(n, 0); }
static void mb_align(size_t *n) { while (*n & 1) mb_put16(n, 0); }
static void mb_item(size_t *n, u32 style, int x, int y, int cx, int cy, u16 id, u16 cls, const char *title) { mb_align(n); mb_put32(n, style | 0x50000000u); mb_put32(n, 0); mb_put16(n, (u16)x); mb_put16(n, (u16)y); mb_put16(n, (u16)cx); mb_put16(n, (u16)cy); mb_put16(n, id); mb_put16(n, 0xFFFF); mb_put16(n, cls); mb_puts(n, title); mb_put16(n, 0); }
static void s_NebulaMsgBoxProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1); u64 wp = ARG(2); if (msg == 0x111) { u32 id = (u32)(wp & 0xffff); if (id >= 1 && id <= 11) { g_wnd[i].ended = 1; g_wnd[i].result = id; } RET(1); return; } if (msg == 0x110) { RET(1); return; } RET(0); }
static void message_box(Cpu *c, u64 owner_h, const char *text, const char *caption, u32 type) {
  Win32 *w = W; int m;
  w32_report(w, "MessageBox [%s]: %s", caption, text); e_log("MessageBox [%s]: %s\n", caption, text);
  u32 kind = type & 0xF; static const int defbtn[] = { 1, 1, 6, 6, 6, 4, 2 }; static const char *labels[] = { "OK", "Cancel", "&Abort", "&Retry", "&Ignore", "&Yes", "&No", 0, 0, 0, "&Try Again", "&Continue" };
  if (g_msgbox_auto) { int r = defbtn[kind < 7 ? kind : 0]; if ((type & 0x300) == 0x100) r = kind == 1 ? 2 : kind == 3 ? 7 : kind == 4 ? 7 : kind == 2 ? 4 : kind == 5 ? 2 : kind == 6 ? 11 : r; RET(r); return; }
  int btns[3]; int nb = 0; switch (kind) { case 0: btns[nb++] = 1; break; case 1: btns[nb++] = 1; btns[nb++] = 2; break; case 2: btns[nb++] = 3; btns[nb++] = 4; btns[nb++] = 5; break; case 3: btns[nb++] = 6; btns[nb++] = 7; btns[nb++] = 2; break; case 4: btns[nb++] = 6; btns[nb++] = 7; break; case 5: btns[nb++] = 4; btns[nb++] = 2; break; case 6: btns[nb++] = 2; btns[nb++] = 10; btns[nb++] = 11; break; default: btns[nb++] = 1; }
  /* layout in dialog units (base 7x13 px): text block up to 60 columns */ int cols = 0, lines = 1, cur = 0; for (const char *p = text; *p; p++) { if (*p == '\n') { lines++; if (cur > cols) cols = cur; cur = 0; } else if (((u8)*p & 0xC0) != 0x80) { cur++; if (cur >= 60) { lines++; cur = 0; } } } if (cur > cols) cols = cur; if (cols < 16) cols = 16; if (cols > 60) cols = 60;
  int tw = cols * 4 + 8, th = lines * 8 + 4; int bw = 50, bh = 14, gap = 6; int bwtotal = nb * bw + (nb - 1) * gap; int dw = (tw + 24 > bwtotal + 24 ? tw + 24 : bwtotal + 24); if (type & 0xF0) dw += 24; int dh = th + bh + 32;
  size_t n = 0; mb_put32(&n, 0x80C800C0u | 0x40 | 0x04 | 0x80); mb_put32(&n, 0); mb_put16(&n, (u16)(nb + 1 + ((type & 0xF0) ? 1 : 0))); mb_put16(&n, 0); mb_put16(&n, 0); mb_put16(&n, (u16)dw); mb_put16(&n, (u16)dh); mb_put16(&n, 0); mb_put16(&n, 0); mb_puts(&n, caption); mb_put16(&n, 8); mb_puts(&n, "MS Shell Dlg");
  int tx = 12; if (type & 0xF0) { mb_item(&n, 0x3 /* SS_ICON */, 10, 10, 20, 20, 0xFFFF, 0x82, "#1"); tx = 36; } mb_item(&n, 0x0 | 0x80 /* SS_NOPREFIX */, tx, 12, tw, th, 0xFFFF, 0x82, text);
  int bx = (dw - bwtotal) / 2; int def = (type >> 8) & 3; for (int k = 0; k < nb; k++) { mb_item(&n, (k == def ? 1 : 0) | 0x10000, bx + k * (bw + gap), th + 20, bw, bh, (u16)btns[k], 0x80, labels[btns[k] - 1]); }
  u64 tmpl = w32_heap_alloc(w, n * 2 + 16, 1); u8 *p = mem_ptr(w->mem, tmpl, n * 2, 1); if (!p) { RET(0); return; } e_memcpy(p, g_mb_tmpl, n * 2);
  if (g_nmodal >= 8) { RET(0); return; } int i = dlg_begin(c, 0, tmpl, owner_h, g_slot_msgbox, 0, 1); if (i < 0) { RET(0); return; } g_wnd[i].builtin = BI_MSGBOX; g_wnd[i].wndproc = g_slot_dialog;
  m = g_nmodal++; g_modal[m].rip = c->rip; g_modal[m].rsp = c->r[RSP]; g_modal[m].slot = w32_cur_slot(); g_modal[m].wnd = i; g_modal[m].phase = 1; dlg_create_next(c); }
static void s_MessageBoxW(Cpu *c) { if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; } char t[2048], cap[256]; text_in(c, ARG(1), 1, t, sizeof t); text_in(c, ARG(2), 1, cap, sizeof cap); if (!cap[0]) e_snprintf(cap, sizeof cap, "Error"); message_box(c, ARG(0), t, cap, (u32)ARG(3)); }
static void s_MessageBoxA(Cpu *c) { if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; } char t[2048], cap[256]; text_in(c, ARG(1), 0, t, sizeof t); text_in(c, ARG(2), 0, cap, sizeof cap); if (!cap[0]) e_snprintf(cap, sizeof cap, "Error"); message_box(c, ARG(0), t, cap, (u32)ARG(3)); }
static void s_MessageBoxExW(Cpu *c) { s_MessageBoxW(c); }
static void s_MessageBoxExA(Cpu *c) { s_MessageBoxA(c); }
static void s_MessageBoxIndirectW(Cpu *c) { if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; } u64 p = ARG(0); u64 P = PSZ_OF(c); char t[2048], cap[256]; text_in(c, rdp(c, p + 4 + 2 * P), 1, t, sizeof t); text_in(c, rdp(c, p + 4 + 3 * P), 1, cap, sizeof cap); message_box(c, rdp(c, p + 4), t, cap, rd32(c, p + 4 + 4 * P)); }
static void s_MessageBeep(Cpu *c) { RET(1); }
static void s_GetDlgItemTextLength(Cpu *c) { RET(0); }
/* SHBrowseForFolder: a folder picker built from a list box over the virtual disk */
static void s_NebulaBrowseProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1); u64 wp = ARG(2); Wnd *n = &g_wnd[i]; Win32 *w = W;
  if (msg == 0x110) { /* fill the list with the subdirectories of the current path (stored in the dialog text) */ int lb = dlg_find_id(i, 100); int ed = dlg_find_id(i, 101); if (ed >= 0) wnd_set_text(&g_wnd[ed], n->text); if (lb >= 0) { Wnd *l = &g_wnd[lb]; items_free(l); items_add(l, "[..]", -1); char pat[600]; e_snprintf(pat, sizeof pat, "%s\\*", n->text); int h = vfs_find_first(pat); char nm[520]; u64 sz; int is_dir; while (h >= 0 && vfs_find_next(h, nm, sizeof nm, &sz, &is_dir)) { if (!is_dir || !e_strcmp(nm, ".") || !e_strcmp(nm, "..")) continue; char e[140]; e_snprintf(e, sizeof e, "[%s]", nm); items_add(l, e, -1); } if (h >= 0) vfs_find_close(h); l->cursel = -1; l->top_index = 0; wnd_invalidate(lb, 1); } RET(1); return; }
  if (msg == 0x111) { u32 id = (u32)(wp & 0xffff), code = (u32)(wp >> 16);
    if (id == 100 && code == 2 /* LBN_DBLCLK */) { int lb = dlg_find_id(i, 100); if (lb >= 0 && g_wnd[lb].cursel >= 0) { const char *it = g_wnd[lb].items[g_wnd[lb].cursel]; char path[600]; if (!e_strcmp(it, "[..]")) { e_snprintf(path, sizeof path, "%s", n->text); size_t l = e_strlen(path); while (l > 3 && path[l - 1] != '\\') l--; if (l > 3) path[l - 1] = 0; else path[3] = 0; } else { char nm[140]; e_snprintf(nm, sizeof nm, "%s", it + 1); size_t l = e_strlen(nm); if (l) nm[l - 1] = 0; if (n->text[e_strlen(n->text) - 1] == '\\') e_snprintf(path, sizeof path, "%s%s", n->text, nm); else e_snprintf(path, sizeof path, "%s\\%s", n->text, nm); } wnd_set_text(n, path); call_shim_tail(c, s_NebulaBrowseProc, HWND_OF(i), 0x110, 0, 0); } RET(1); return; }
    if (id == 100 && code == 1) { int lb = dlg_find_id(i, 100), ed = dlg_find_id(i, 101); if (lb >= 0 && ed >= 0 && g_wnd[lb].cursel >= 0) { const char *it = g_wnd[lb].items[g_wnd[lb].cursel]; if (e_strcmp(it, "[..]")) { char nm[140]; e_snprintf(nm, sizeof nm, "%s", it + 1); size_t l = e_strlen(nm); if (l) nm[l - 1] = 0; if (n->text[e_strlen(n->text) - 1] == '\\') { char pth[1024]; e_snprintf(pth, sizeof pth, "%s%s", n->text, nm); wnd_set_text(&g_wnd[ed], pth); } else { char pth[1024]; e_snprintf(pth, sizeof pth, "%s\\%s", n->text, nm); wnd_set_text(&g_wnd[ed], pth); } } else wnd_set_text(&g_wnd[ed], n->text); wnd_invalidate(ed, 1); } RET(1); return; }
    if (id == 1) { int ed = dlg_find_id(i, 101); if (ed >= 0) wnd_set_text(n, g_wnd[ed].text); n->ended = 1; n->result = 1; RET(1); return; } if (id == 2) { n->ended = 1; n->result = 0; RET(1); return; } if (id == 102) { /* new folder */ int ed = dlg_find_id(i, 101); if (ed >= 0) { char p[600]; e_snprintf(p, sizeof p, "%s", g_wnd[ed].text); shell_mkdirs(p); wnd_set_text(n, p); call_shim_tail(c, s_NebulaBrowseProc, HWND_OF(i), 0x110, 0, 0); } RET(1); return; } RET(0); return; }
  (void)w; RET(0); }
static void s_SHBrowseForFolderW(Cpu *c) { Win32 *w = W; int m; if (modal_reentry(c)) { modal_loop(c, g_modal[g_nmodal - 1].wnd); return; }
  u64 bi = ARG(0); u64 P = PSZ_OF(c); char title[256]; text_in(c, rdp(c, bi + 2 * P), 1, title, sizeof title); if (!title[0]) e_snprintf(title, sizeof title, "Browse For Folder"); const char *start = "C:\\"; u64 root = rdp(c, bi + P); char sroot[520]; if (root) { e_snprintf(sroot, sizeof sroot, "%s", pidl_path(c, root)); if (sroot[0]) start = sroot; }
  if (g_msgbox_auto) { /* headless: pick the start folder */ u64 pidl = pidl_make(w, start); w32_report(w, "SHBrowseForFolder: headless run — returned %s", start); RET(pidl); return; }
  size_t n = 0; mb_put32(&n, 0x80C800C0u | 0x40 | 0x04); mb_put32(&n, 0); mb_put16(&n, 6); mb_put16(&n, 0); mb_put16(&n, 0); mb_put16(&n, 220); mb_put16(&n, 180); mb_put16(&n, 0); mb_put16(&n, 0); mb_puts(&n, title); mb_put16(&n, 8); mb_puts(&n, "MS Shell Dlg");
  mb_item(&n, 0, 8, 6, 200, 10, 0xFFFF, 0x82, "Double-click a folder to open it; select it and press OK:"); mb_item(&n, 0x1 | 0x800000 /* WS_BORDER */ | 0x10000 | 0x200000, 8, 18, 204, 110, 100, 0x83, ""); mb_item(&n, 0x80 | 0x10000 | 0x800000, 8, 132, 204, 13, 101, 0x81, "");
  mb_item(&n, 0x10000, 8, 158, 60, 14, 102, 0x80, "Make New Folder"); mb_item(&n, 1 | 0x10000, 106, 158, 50, 14, 1, 0x80, "OK"); mb_item(&n, 0x10000, 162, 158, 50, 14, 2, 0x80, "Cancel");
  u64 tmpl = w32_heap_alloc(w, n * 2 + 16, 1); u8 *p = mem_ptr(w->mem, tmpl, n * 2, 1); if (!p) { RET(0); return; } e_memcpy(p, g_mb_tmpl, n * 2);
  if (g_nmodal >= 8) { RET(0); return; } u64 proc = w32_shim_slot(w, "user32", "NebulaBrowseProc"); int i = dlg_begin(c, 0, tmpl, rdp(c, bi), proc, 0, 1); if (i < 0) { RET(0); return; } wnd_set_text(&g_wnd[i], start); g_wnd[i].builtin = BI_MSGBOX;
  m = g_nmodal++; g_modal[m].rip = c->rip; g_modal[m].rsp = c->r[RSP]; g_modal[m].slot = w32_cur_slot(); g_modal[m].wnd = i; g_modal[m].phase = 1; dlg_create_next(c); }
static void s_SHBrowseForFolderA(Cpu *c) { s_SHBrowseForFolderW(c); }
/* ---------- continuation dispatch for this module ---------- */
void gdi_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int ci = w->ncont - 1;
  switch (kind) {
    case CONT_WNDCREATE: wndcreate_resume(c, rax); return;
    case CONT_WNDDESTROY: destroy_step(c); return;
    case CONT_CTLPAINT: ctlpaint_resume(c, rax); return;
    case CONT_ERASEBG: erasebg_resume(c, rax); return;
    case CONT_SETPOS: setpos_resume(c, rax); return;
    case CONT_STREAMIN: streamin_resume(c, rax); return;
    case CONT_ILREAD: ilr_resume(c, rax); return;
    case CONT_WICREAD: wicread_resume(c, rax); return;
    case CONT_MENUOPEN: menuopen_resume(c, rax); return;
    case CONT_MENUDRAW: menudraw_resume(c, rax); return;
    case CONT_TABSEL: tabsel_resume(c, rax); return;
    case CONT_SUBCLASS: subclass_resume(c, rax); return;
    case CONT_DIENUM: dienum_resume(c, rax); return;
    case CONT_DLGINIT: dlginit_resume(c, rax); return;
    case CONT_DLGPROC: dlgproc_resume(c, rax); return;
    case CONT_MODAL: { int m = g_nmodal - 1; if (m >= 0 && g_modal[m].phase == 2) { u64 res = modal_result(w, m); g_nmodal--; w32_cont_finish(c, res); return; } modal_resume(c, rax); return; }
    case CONT_ENUMWND: { if (!(u32)rax) { w32_cont_finish(c, 0); return; } enum_wnd_step(c); return; }
    default: (void)ci; cpu_fault(c, "bad continuation kind %d", kind); return;
  }
}
void gdi_set_msgbox_auto(int on) { g_msgbox_auto = on; }

#define USER_SHIMS \
  S(SendMessageTimeoutW), S(SendNotifyMessageW), S(PostThreadMessageW), S(MsgWaitForMultipleObjectsEx), S(GetQueueStatus), S(GetInputState), S(InSendMessage), S(ReplyMessage), S(GetMessagePos), S(GetMessageTime), S(GetMessageExtraInfo), \
  S(UnregisterClassW), S(GetClassInfoExW), S(GetClassInfoW), S(GetClassInfoA), S(GetClassNameW), S(RealGetWindowClassW), S(GetClassLongPtrW), SA("GetClassLongPtrA", GetClassLongPtrW), S(GetClassLongA), SA("GetClassLongW", GetClassLongA), S(SetClassLongPtrW), SA("SetClassLongPtrA", SetClassLongPtrW), SA("SetClassLongW", SetClassLongPtrW), SA("SetClassLongA", SetClassLongPtrW), S(AdjustWindowRectExForDpi), \
  S(CallWindowProcW), S(CallWindowProcA), S(ShowWindowAsync), S(ValidateRgn), S(GetWindowInfo), S(BeginDeferWindowPos), S(DeferWindowPos), S(EndDeferWindowPos), S(GetWindowPlacement), S(SetWindowPlacement), S(IsWindowEnabled), S(IsChild), S(GetShellWindow), S(GetAncestor), S(SetParent), S(GetWindow), S(GetTopWindow), S(GetNextWindow), S(GetLastActivePopup), \
  S(EnumChildWindows), S(EnumWindows), S(EnumThreadWindows), S(FindWindowW), S(FindWindowExW), S(FindWindowExA), S(WindowFromPoint), S(ChildWindowFromPoint), S(ChildWindowFromPointEx), S(GetWindowThreadProcessId), S(IsHungAppWindow), S(IsWindowUnicode), S(IsGUIThread), S(FlashWindowEx), S(FlashWindow), S(SetLayeredWindowAttributes), S(UpdateLayeredWindow), S(AnimateWindow), S(SetWindowRgn), S(GetWindowRgn), S(LockWindowUpdate), S(SetWindowDisplayAffinity), S(GetWindowDisplayAffinity), \
  S(GetWindowTextW), S(GetWindowTextLengthW), S(GetWindowTextLengthA), S(InternalGetWindowText), S(SetWindowLongW), S(GetWindowLongW), S(GetPropW), S(SetPropW), S(RemovePropW), S(GetPropA), S(SetPropA), S(RemovePropA), S(GetCapture), S(GetCursor), S(GetClipCursor), S(LoadCursorFromFileW), S(LoadImageW), S(LoadBitmapW), S(LoadBitmapA), S(CopyImage), S(DestroyIcon), S(CreateIconIndirect), S(CreateIconFromResourceEx), S(LookupIconIdFromDirectoryEx), S(DrawIconEx), S(DrawIcon), S(GetIconInfo), \
  S(MapVirtualKeyExW), SA("MapVirtualKeyW", MapVirtualKeyA), SA("MapVirtualKeyExA", MapVirtualKeyA), S(GetKeyboardLayout), S(GetKeyboardLayoutList), S(ActivateKeyboardLayout), S(GetKeyNameTextW), SA("GetKeyNameTextA", GetKeyNameTextW), S(ToUnicode), S(ToUnicodeEx), S(ToAscii), SA("ToAsciiEx", ToAscii), S(VkKeyScanW), S(VkKeyScanA), SA("VkKeyScanExW", VkKeyScanW), S(keybd_event), S(mouse_event), S(SendInput), S(BlockInput), S(GetLastInputInfo), S(GetDoubleClickTime), S(SetDoubleClickTime), S(GetSystemMetricsForDpi), S(SystemParametersInfoW), S(SystemParametersInfoForDpi), \
  S(GetMonitorInfoW), S(GetMonitorInfoA), S(MonitorFromWindow), S(MonitorFromPoint), S(MonitorFromRect), S(EnumDisplayMonitors), S(GetDpiForMonitor), \
  S(DrawTextExW), S(DrawTextExA), S(TabbedTextOutW), S(DrawEdge), S(DrawFocusRect), S(DrawFrameControl), S(DrawStateW), SA("DrawStateA", DrawStateW), S(FrameRect), S(InvertRect), S(DrawCaption), S(GrayStringW), SA("GrayStringA", GrayStringW), \
  S(OpenClipboard), S(CloseClipboard), S(EmptyClipboard), S(SetClipboardData), S(GetClipboardData), S(IsClipboardFormatAvailable), S(GetClipboardSequenceNumber), S(CountClipboardFormats), S(EnumClipboardFormats), S(GetClipboardOwner), S(SetClipboardViewer), S(ChangeClipboardChain), S(AddClipboardFormatListener), S(RemoveClipboardFormatListener), S(GetClipboardFormatNameW), SA("GetClipboardFormatNameA", GetClipboardFormatNameW), S(RegisterClipboardFormatW), S(RegisterWindowMessageW), S(RegisterWindowMessageA), \
  S(SetScrollInfo), S(GetScrollInfo), S(SetScrollPos), S(GetScrollPos), S(SetScrollRange), S(GetScrollRange), S(ShowScrollBar), S(EnableScrollBar), S(ScrollWindowEx), S(ScrollWindow), S(LoadAcceleratorsW), S(LoadAcceleratorsA), S(CreateAcceleratorTableW), SA("CreateAcceleratorTableA", CreateAcceleratorTableW), S(DestroyAcceleratorTable), S(TranslateAcceleratorW), S(TranslateAcceleratorA), SA("TranslateAccelerator", TranslateAcceleratorA), \
  S(CreateCaret), S(DestroyCaret), S(ShowCaret), S(HideCaret), S(SetCaretPos), S(GetCaretPos), S(SetCaretBlinkTime), S(GetCaretBlinkTime), S(InitCommonControls), S(InitCommonControlsEx), \
  S(CreateStatusWindowW), S(CreateToolbarEx), S(LoadIconWithScaleDown), S(LoadIconMetric), S(SetWindowSubclass), S(RemoveWindowSubclass), S(NebulaSubclassProc), S(DefSubclassProc), S(GetWindowSubclass), S(TaskDialogIndirect), S(TaskDialog), S(SetSysColors), \
  S(NebulaButtonProc), S(NebulaStaticProc), S(NebulaLinkProc), S(NebulaEditProc), S(NebulaListBoxProc), S(NebulaComboListProc), S(NebulaComboProc), S(NebulaProgressProc), S(NebulaScrollBarProc), S(NebulaUnsupportedProc), S(NebulaMsgBoxProc), S(NebulaBrowseProc), \
  S(GetDlgCtrlID), S(SendDlgItemMessageW), S(SendDlgItemMessageA), S(SetDlgItemTextW), S(SetDlgItemTextA), S(GetDlgItemTextW), S(GetDlgItemTextA), S(SetDlgItemInt), S(GetDlgItemInt), S(CheckDlgButton), S(IsDlgButtonChecked), S(CheckRadioButton), S(MapDialogRect), S(GetDialogBaseUnits), S(GetNextDlgTabItem), S(GetNextDlgGroupItem), \
  S(DefDlgProcW), S(DefDlgProcA), S(CreateDialogIndirectParamW), S(CreateDialogIndirectParamA), S(CreateDialogParamW), S(CreateDialogParamA), S(DialogBoxIndirectParamW), S(DialogBoxIndirectParamA), S(DialogBoxParamW), S(DialogBoxParamA), S(EndDialog), S(IsDialogMessageW), S(IsDialogMessageA), S(IsDialogMessage), \
  S(GetDCEx), S(WindowFromDC), S(GetUpdateRgn), S(ExcludeUpdateRgn), S(PaintDesktop), S(MessageBoxExW), S(MessageBoxExA), S(MessageBoxIndirectW), SA("MessageBoxIndirectA", MessageBoxIndirectW), S(MessageBeep), S(SHBrowseForFolderW), S(SHBrowseForFolderA),
