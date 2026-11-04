/* ---------- menus: real menu tables, a menu bar drawn in the non-client area of framed top-level windows, popup menus as
   built-in top-level windows ("#32768") driven with the mouse and the keyboard; WM_INITMENUPOPUP before a popup opens,
   WM_COMMAND / WM_SYSCOMMAND when an item is chosen, owner-drawn items through WM_MEASUREITEM / WM_DRAWITEM, TrackPopupMenu as a
   modal loop (TPM_RETURNCMD), menu templates (MENU and MENUEX resources) for LoadMenu ---------- */
#define MENU_MAX 512
#define MITEM_MAX 96
typedef struct { u32 id; u32 state; u32 ftype; u64 sub; u64 data; u64 bmp; u16 ow, oh; char text[96]; } MItem;
typedef struct { int used; int pid; int n; MItem *it; } Menu;
static Menu g_menu[MENU_MAX];
static u64 menu_handle(int k) { return 0x6E001 + (u64)k * 16; }
static int menu_of(u64 h) { if (h < 0x6E001 || (h - 0x6E001) % 16) return -1; u64 k = (h - 0x6E001) / 16; if (k == 0 || k >= MENU_MAX || !g_menu[k].used) return -1; return (int)k; }
static int menu_new(Win32 *w) { for (int k = 1; k < MENU_MAX; k++) if (!g_menu[k].used) { Menu *m = &g_menu[k]; if (!m->it) m->it = emu_alloc(sizeof(MItem) * MITEM_MAX); if (!m->it) return -1; e_memset(m->it, 0, sizeof(MItem) * MITEM_MAX); m->used = 1; m->pid = w->pid; m->n = 0; return k; } return -1; }
/* position of the item with command `id` in menu m or (recursively) in its submenus; returns the menu that holds it */
static int menu_find_cmd(int m, u32 id, int *idx, int depth) { if (m < 0 || depth > 8) return -1; Menu *mm = &g_menu[m]; for (int k = 0; k < mm->n; k++) if (mm->it[k].id == id && !mm->it[k].sub) { *idx = k; return m; } for (int k = 0; k < mm->n; k++) if (mm->it[k].sub) { int s = menu_of(mm->it[k].sub); if (s >= 0) { int r = menu_find_cmd(s, id, idx, depth + 1); if (r >= 0) return r; } } for (int k = 0; k < mm->n; k++) if (mm->it[k].id == id) { *idx = k; return m; } return -1; }
/* resolve (hmenu, item, flags): MF_BYPOSITION → the item at that position, otherwise the item with that command id */
static int menu_item_at(u64 hmenu, u32 item, u32 flags, int *idx) { int m = menu_of(hmenu); if (m < 0) return -1; if (flags & 0x400) { if ((int)item < 0 || (int)item >= g_menu[m].n) return -1; *idx = (int)item; return m; } return menu_find_cmd(m, item, idx, 0); }
static int menu_insert_at(int m, int pos) { Menu *mm = &g_menu[m]; if (mm->n >= MITEM_MAX) return -1; if (pos < 0 || pos > mm->n) pos = mm->n; for (int k = mm->n; k > pos; k--) mm->it[k] = mm->it[k - 1]; e_memset(&mm->it[pos], 0, sizeof(MItem)); mm->n++; return pos; }
static void menu_remove_at(int m, int pos) { Menu *mm = &g_menu[m]; if (pos < 0 || pos >= mm->n) return; for (int k = pos; k + 1 < mm->n; k++) mm->it[k] = mm->it[k + 1]; mm->n--; }
static void menu_destroy(int m, int depth) { if (m < 0 || depth > 8) return; Menu *mm = &g_menu[m]; for (int k = 0; k < mm->n; k++) if (mm->it[k].sub) menu_destroy(menu_of(mm->it[k].sub), depth + 1); mm->used = 0; mm->n = 0; }
/* MF_* flags of AppendMenu/InsertMenu/ModifyMenu → item */
static void menu_item_from_flags(Cpu *c, MItem *it, u32 flags, u64 idnew, u64 newitem, int wide) {
  it->ftype = (flags & 0x800) ? 0x800 : 0; if (flags & 0x100) it->ftype |= 0x100; if (flags & 0x4) it->ftype |= 0x4; if (flags & 0x20) it->ftype |= 0x20; if (flags & 0x40) it->ftype |= 0x40; if (flags & 0x200) it->ftype |= 0x200;
  it->state = ((flags & 3) ? 3 : 0) | ((flags & 8) ? 8 : 0) | ((flags & 0x1000) ? 0x1000 : 0);
  if (flags & 0x10) { it->sub = idnew; it->id = (u32)idnew; } else { it->id = (u32)idnew; it->sub = 0; }
  it->text[0] = 0; if (flags & 0x100) it->data = newitem; else if (flags & 0x4) it->bmp = newitem; else if (!(flags & 0x800)) text_in(c, newitem, wide, it->text, sizeof it->text); }
/* MENUITEMINFO: x86 cbSize@0 fMask@4 fType@8 fState@12 wID@16 hSubMenu@20 hbmpChecked@24 hbmpUnchecked@28 dwItemData@32 dwTypeData@36 cch@40 hbmpItem@44;
   x64 … wID@16 hSubMenu@24 hbmpChecked@32 hbmpUnchecked@40 dwItemData@48 dwTypeData@56 cch@64 hbmpItem@72 */
#define MII_SUB(a) ((a) + (c->mode32 ? 20 : 24))
#define MII_DATA(a) ((a) + (c->mode32 ? 32 : 48))
#define MII_TYPEDATA(a) ((a) + (c->mode32 ? 36 : 56))
#define MII_CCH(a) ((a) + (c->mode32 ? 40 : 64))
#define MII_BMP(a) ((a) + (c->mode32 ? 44 : 72))
static void mii_read(Cpu *c, u64 mi, MItem *it, int wide) { u32 mask = rd32(c, mi + 4), cb = rd32(c, mi);
  if (mask & 0x100) it->ftype = rd32(c, mi + 8) & ~0x4u; if (mask & 0x10) it->ftype = rd32(c, mi + 8);
  if (mask & 0x1) it->state = rd32(c, mi + 12); if (mask & 0x2) it->id = rd32(c, mi + 16); if (mask & 0x4) it->sub = rdp(c, MII_SUB(mi)); if (mask & 0x20) it->data = rdp(c, MII_DATA(mi));
  if (mask & 0x80 && cb >= (u32)(c->mode32 ? 48 : 80)) it->bmp = rdp(c, MII_BMP(mi));
  if ((mask & 0x40) || ((mask & 0x10) && !(it->ftype & 0x904))) { u64 td = rdp(c, MII_TYPEDATA(mi)); if (td) text_in(c, td, wide, it->text, sizeof it->text); else it->text[0] = 0; }
  else if ((mask & 0x10) && (it->ftype & 0x4)) it->bmp = rdp(c, MII_TYPEDATA(mi)); }
static void mii_write(Cpu *c, u64 mi, MItem *it, int wide, int m) { u32 mask = rd32(c, mi + 4), cb = rd32(c, mi); (void)m;
  if (mask & 0x110) wr32(c, mi + 8, it->ftype); if (mask & 0x1) wr32(c, mi + 12, it->state); if (mask & 0x2) wr32(c, mi + 16, it->id); if (mask & 0x4) wrp(c, MII_SUB(mi), it->sub); if (mask & 0x20) wrp(c, MII_DATA(mi), it->data);
  if (mask & 0x8) { wrp(c, MII_SUB(mi) + (c->mode32 ? 4 : 8), 0); wrp(c, MII_SUB(mi) + (c->mode32 ? 8 : 16), 0); } if (mask & 0x80 && cb >= (u32)(c->mode32 ? 48 : 80)) wrp(c, MII_BMP(mi), it->bmp);
  if ((mask & 0x40) || ((mask & 0x10) && !(it->ftype & 0x904))) { u64 td = rdp(c, MII_TYPEDATA(mi)); u32 cch = rd32(c, MII_CCH(mi)); u32 len = wide ? utf8_u16len(it->text) : (u32)e_strlen(it->text); if (!td || !cch) wr32(c, MII_CCH(mi), len); else { u32 n = text_out_cap(c, td, it->text, cch, wide); wr32(c, MII_CCH(mi), n); } }
  else if ((mask & 0x10) && (it->ftype & 0x4)) wrp(c, MII_TYPEDATA(mi), it->bmp); }

/* ---------- menu label helpers: "&File\tCtrl+O" → label "File" with the mnemonic column, accelerator "Ctrl+O" ---------- */
static int menu_label(const char *t, char *out, int cap, int *mn_col) { int o = 0, col = 0; *mn_col = -1; for (int i = 0; t[i] && t[i] != '\t' && o < cap - 1; i++) { if (t[i] == '&' && t[i + 1] != '&') { if (*mn_col < 0 && t[i + 1]) *mn_col = col; continue; } if (t[i] == '&') i++; out[o++] = t[i]; if (((u8)t[i] & 0xC0) != 0x80) col++; } out[o] = 0; return o; }
static const char *menu_accel(const char *t) { for (int i = 0; t[i]; i++) if (t[i] == '\t') return t + i + 1; return 0; }
static int menu_mnemonic(const char *t) { for (int i = 0; t[i]; i++) { if (t[i] == '&' && t[i + 1] && t[i + 1] != '&') { char ch = t[i + 1]; return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; } if (t[i] == '&') i++; } return 0; }
static void menu_draw_label(Surf *s, GdiObj *d, Win32 *w, int x, int y, const char *t, u32 color) { char lab[96]; int mn; int n = menu_label(t, lab, sizeof lab, &mn); d->text_color = px2cr(color); d->bk_mode = 1; draw_text(s, d, w, x, y, lab, n); if (mn >= 0) { int cw, fh; font_dims(w, d->font, &cw, &fh); hline(s, x + mn * cw, x + (mn + 1) * cw - 1, y + fh - 1, color); } }
static int menu_label_w(Win32 *w, const char *t) { char lab[96]; int mn; int n = menu_label(t, lab, sizeof lab, &mn); return text_width(w, g_dlg_font, lab, n); }

/* ---------- the menu bar of a framed top-level window ---------- */
#define BAR_TOP(i) ((i) == 0 ? 0 : CAPTION_H)   /* the main window has no caption: its bar sits at the top of the framebuffer */
static int wnd_bar_menu(int i) { if (i < 0 || !wnd_alive(i) || g_wnd[i].parent >= 0 || (i != 0 && !g_wnd[i].ncy)) return -1; int m = menu_of(g_wnd[i].menu); return m >= 0 && g_menu[m].n > 0 ? m : -1; }
/* the caption/bar height follows the menu: the window grows so that the client area keeps its size (the size CreateWindow asked for) */
static void menu_apply_bar(Win32 *w, int i) { Wnd *n = &g_wnd[i]; if (i < 0 || n->parent >= 0 || (i != 0 && !n->ncy)) return; int want = BAR_TOP(i) + (wnd_bar_menu(i) >= 0 ? MENU_H : 0); if (want != n->ncy) { n->h += want - n->ncy; n->ncy = want; if (i == 0) { screen_ensure(w, n->w, n->h); sync_main(w); wnd_invalidate(0, 1); w->dirty = 1; return; } if (n->y + n->h > w->screen_h && n->y > 0) { n->y = w->screen_h - n->h; if (n->y < 0) n->y = 0; } wnd_invalidate(i, 1); w->dirty = 1; } }
/* x ranges of the bar items (window-relative); returns the count */
static int menu_bar_layout(Win32 *w, int i, int *x0, int *x1) { int m = wnd_bar_menu(i); if (m < 0) return 0; int x = g_wnd[i].ncx + 2; Menu *mm = &g_menu[m]; for (int k = 0; k < mm->n; k++) { int tw = (mm->it[k].ftype & 0x100) ? (mm->it[k].ow ? mm->it[k].ow : 40) : menu_label_w(w, mm->it[k].text); x0[k] = x; x1[k] = x + tw + 14; x = x1[k]; } return mm->n; }
static int menu_bar_hit(Win32 *w, int i, int fx, int fy) { int m = wnd_bar_menu(i); if (m < 0) return -1; int wx, wy; wnd_window_origin(i, &wx, &wy); if (fy < wy + BAR_TOP(i) || fy >= wy + g_wnd[i].ncy || fx < wx || fx >= wx + g_wnd[i].w) return -1; int x0[MITEM_MAX], x1[MITEM_MAX]; int n = menu_bar_layout(w, i, x0, x1); for (int k = 0; k < n; k++) if (fx - wx >= x0[k] && fx - wx < x1[k]) return k; return -2; }
static void menu_bar_draw(Win32 *w, Surf *f, GdiObj *d, int i, int wx, int wy) { int m = wnd_bar_menu(i); if (m < 0) return; Wnd *n = &g_wnd[i]; int by = wy + BAR_TOP(i); fill_rect_px(f, wx + n->ncx, by, wx + n->w - n->ncx, by + MENU_H, 0xf0f0f0);
  int x0[MITEM_MAX], x1[MITEM_MAX]; int cnt = menu_bar_layout(w, i, x0, x1); int cw, fh; font_dims(w, d->font, &cw, &fh); Menu *mm = &g_menu[m];
  for (int k = 0; k < cnt; k++) { int hot = n->bar_hot1 == k + 1; if (hot) { fill_rect_px(f, wx + x0[k], by + 1, wx + x1[k], by + MENU_H - 1, 0x91c9f7); edge_rect(f, wx + x0[k], by + 1, wx + x1[k], by + MENU_H - 1, 0x3399ff, 0x3399ff); }
    if (mm->it[k].ftype & 0x100) continue; menu_draw_label(f, d, w, wx + x0[k] + 7, by + (MENU_H - fh) / 2, mm->it[k].text, (mm->it[k].state & 3) ? 0x6d6d6d : 0x000000); } }

/* ---------- popup menus ---------- */
#define MENU_POP_MAX 8
static struct { int wnd, menu, owner, hot, bar_item, sys, pressed; } g_pop[MENU_POP_MAX]; static int g_npop;
static int g_menu_track_owner = -1; static u32 g_menu_track_flags; static int g_menu_track_root = -1;
static int menu_top_wnd(void) { return g_npop > 0 && wnd_alive(g_pop[g_npop - 1].wnd) ? g_pop[g_npop - 1].wnd : -1; }
static int menu_item_h(Win32 *w, MItem *it) { if (it->ftype & 0x800) return 7; if ((it->ftype & 0x100) && it->oh) return it->oh; int cw, fh; font_dims(w, g_dlg_font, &cw, &fh); return fh + 6; }
static void menu_popup_size(Win32 *w, int m, int *pw, int *ph) { Menu *mm = &g_menu[m]; int maxw = 60, maxa = 0, h = 2; int cw, fh; font_dims(w, g_dlg_font, &cw, &fh); for (int k = 0; k < mm->n; k++) { MItem *it = &mm->it[k]; h += menu_item_h(w, it); if (it->ftype & 0x100) { if (it->ow > maxw) maxw = it->ow; continue; } int tw = menu_label_w(w, it->text); if (tw > maxw) maxw = tw; const char *a = menu_accel(it->text); if (a) { int aw = text_width(w, g_dlg_font, a, (int)e_strlen(a)); if (aw > maxa) maxa = aw; } } *pw = 22 + maxw + (maxa ? 16 + maxa : 0) + 20; *ph = h + 2; }
static int menu_item_at_y(Win32 *w, int m, int y, int *top) { Menu *mm = &g_menu[m]; int yy = 1; for (int k = 0; k < mm->n; k++) { int ih = menu_item_h(w, &mm->it[k]); if (y >= yy && y < yy + ih) { if (top) *top = yy; return k; } yy += ih; } return -1; }
static int menu_item_top(Win32 *w, int m, int idx) { Menu *mm = &g_menu[m]; int yy = 1; for (int k = 0; k < idx && k < mm->n; k++) yy += menu_item_h(w, &mm->it[k]); return yy; }
static void menu_bar_refresh(Cpu *c, int owner) { if (wnd_alive(owner)) wnd_frame(c, owner); }
/* close the popups above level `keep` (keep = 0 closes everything) */
static void menu_close_to(Cpu *c, int keep) { Win32 *w = W; while (g_npop > keep) { int p = --g_npop; int pw = g_pop[p].wnd; if (wnd_alive(pw)) { g_wnd[pw].visible = 0; desktop_clear(w, pw); if (p == 0 && pw == g_menu_track_root) { g_wnd[pw].ended = 1; g_menu_track_root = -1; } else wnd_free(pw); } }
  if (g_npop == 0) { int ow = g_pop[0].owner; g_capture = -1; if (wnd_alive(ow)) { g_wnd[ow].bar_hot1 = 0; if (g_wnd[ow].pid == w->pid && g_wnd[ow].wndproc) post_msg_to(w, HWND_OF(ow), 0x11F, 0xFFFF0000ull, 0); } wnd_invalidate_all(); w->dirty = 1; menu_bar_refresh(c, ow); } else { int t = menu_top_wnd(); if (t >= 0) g_capture = t; wnd_invalidate_all(); w->dirty = 1; } }
/* an item was chosen (or the menu dismissed: id = 0, cancel = 1) */
static void menu_finish(Cpu *c, u32 id, int sys, int cancel) { Win32 *w = W; int ow = g_npop ? g_pop[0].owner : -1; int track = g_npop && g_pop[0].wnd == g_menu_track_root;
  if (track) { int root = g_menu_track_root; g_wnd[root].result = (g_menu_track_flags & 0x100) ? (cancel ? 0 : id) : 1; if (!cancel && !(g_menu_track_flags & 0x100) && wnd_alive(ow)) post_msg_to(w, HWND_OF(ow), sys ? 0x112 : 0x111, id, 0); menu_close_to(c, 0); return; }
  menu_close_to(c, 0); if (!cancel && wnd_alive(ow)) post_msg_to(w, HWND_OF(ow), sys ? 0x112 : 0x111, sys ? id : (u64)id, 0); }

/* opening a popup: WM_INITMENUPOPUP to the owner, WM_MEASUREITEM for owner-drawn items, then the popup window; `done` = 0 → finish the
   current shim with 0 (DefWindowProc / the popup procedure), 1 → re-enter the TrackPopupMenu modal loop */
static struct { int owner, menu, sx, sy, bar_item, sys, done, step, idx; u64 buf; int active; } g_mo;
static void menuopen_step(Cpu *c);
static void menu_popup_create(Cpu *c) { Win32 *w = W; int m = g_mo.menu, ow = g_mo.owner; if (g_npop >= MENU_POP_MAX || m < 0) { g_mo.active = 0; return; }
  int pw, ph; menu_popup_size(w, m, &pw, &ph); int sx = g_mo.sx, sy = g_mo.sy; if (sx + pw > w->screen_w) sx = w->screen_w - pw; if (sy + ph > w->screen_h) { int alt = (g_npop ? sy : g_mo.sy - MENU_H) - ph; sy = alt >= 0 ? alt : w->screen_h - ph; } if (sx < 0) sx = 0; if (sy < 0) sy = 0;
  int li = wnd_create_obj(c, "#32768", "", 0x80000000u, 0x88, sx + SCREEN_X0, sy + SCREEN_Y0, pw, ph, wnd_alive(ow) ? HWND_OF(ow) : 0, 0, wnd_alive(ow) ? g_wnd[ow].inst : 0, 0, 1, 0); if (li < 0) { g_mo.active = 0; return; }
  Wnd *l = &g_wnd[li]; l->owner = ow; l->font = g_dlg_font; l->zseq = ++g_zseq; l->x = sx; l->y = sy; l->w = pw; l->h = ph; l->ncx = l->ncy = 0; l->visible = 1; l->invalid = 1; l->erase = 0; l->enabled = 1;
  int p = g_npop++; g_pop[p].wnd = li; g_pop[p].menu = m; g_pop[p].owner = ow; g_pop[p].hot = -1; g_pop[p].bar_item = g_mo.bar_item; g_pop[p].sys = g_mo.sys; g_pop[p].pressed = 0; g_capture = li; w->dirty = 1;
  if (p == 0 && g_mo.done == 1) { g_menu_track_root = li; if (g_nmodal > 0) g_modal[g_nmodal - 1].wnd = li; }
  if (wnd_alive(ow) && g_mo.bar_item >= 0) { g_wnd[ow].bar_hot1 = g_mo.bar_item + 1; menu_bar_refresh(c, ow); } g_mo.active = 0; }
static void menuopen_finish(Cpu *c) { Win32 *w = W; int ci = w->ncont - 1; int done = g_mo.done; if (g_mo.buf) { w32_heap_free(w, g_mo.buf); g_mo.buf = 0; } menu_popup_create(c);
  if (done == 1) { if (g_nmodal > 0 && g_modal[g_nmodal - 1].wnd < 0) { g_nmodal--; w32_cont_finish(c, 0); return; } w->ncont--; c->r[RSP] = w->cont[ci].rsp; c->rip = w->cont[ci].ret_addr; modal_reexec(c); return; } w32_cont_finish(c, 0); }
static void menuopen_step(Cpu *c) { Win32 *w = W; int ow = g_mo.owner, m = g_mo.menu; int can_call = wnd_alive(ow) && g_wnd[ow].pid == w->pid && g_wnd[ow].wndproc;
  if (g_mo.step == 0) { g_mo.step = 1; if (can_call) { w32_guest_call(c, g_wnd[ow].wndproc, HWND_OF(ow), 0x117, menu_handle(m), (u64)(u32)(g_mo.bar_item < 0 ? 0 : g_mo.bar_item) | ((u64)(g_mo.sys ? 1 : 0) << 16)); return; } }
  if (g_mo.step == 1 && m >= 0) { Menu *mm = &g_menu[m]; for (; g_mo.idx < mm->n; g_mo.idx++) { MItem *it = &mm->it[g_mo.idx]; if (!(it->ftype & 0x100) || !can_call) continue;
      if (!g_mo.buf) g_mo.buf = w32_heap_alloc(w, 48, 1); u64 b = g_mo.buf; wr32(c, b, 1); wr32(c, b + 4, 0); wr32(c, b + 8, it->id); wr32(c, b + 12, 0); wr32(c, b + 16, 0); wrp(c, b + (c->mode32 ? 20 : 24), it->data); g_mo.step = 2; w32_guest_call(c, g_wnd[ow].wndproc, HWND_OF(ow), 0x2C, 0, b); return; } }
  menuopen_finish(c); }
static void menuopen_resume(Cpu *c, u64 rax) { (void)rax; if (g_mo.step == 2) { int m = g_mo.menu; if (m >= 0 && g_mo.idx < g_menu[m].n && g_mo.buf) { MItem *it = &g_menu[m].it[g_mo.idx]; u32 iw = rd32(c, g_mo.buf + 12), ih = rd32(c, g_mo.buf + 16); it->ow = (u16)(iw > 1000 ? 1000 : iw); it->oh = (u16)(ih > 200 ? 200 : ih); } g_mo.idx++; g_mo.step = 1; } menuopen_step(c); }
/* start opening menu m for owner `ow` at screen point (sx, sy) */
static void menu_popup_open(Cpu *c, int ow, int m, int sx, int sy, int bar_item, int sys, int done) { if (g_mo.active || m < 0 || g_npop >= MENU_POP_MAX) { if (done == 1) { if (g_nmodal > 0) g_nmodal--; RET(0); } else RET(0); return; }
  g_mo.active = 1; g_mo.owner = ow; g_mo.menu = m; g_mo.sx = sx; g_mo.sy = sy; g_mo.bar_item = bar_item; g_mo.sys = sys; g_mo.done = done; g_mo.step = 0; g_mo.idx = 0; g_mo.buf = 0;
  w32_push_cont(c, CONT_MENUOPEN, (u64)m); menuopen_step(c); }
/* open bar item k of window i (closing whatever is open) */
static void menu_bar_open(Cpu *c, int i, int k, int done) { Win32 *w = W; int m = wnd_bar_menu(i); if (m < 0 || k < 0 || k >= g_menu[m].n) { RET(0); return; } MItem *it = &g_menu[m].it[k];
  if (!it->sub || menu_of(it->sub) < 0) { /* a bar item without a popup is a command */ menu_close_to(c, 0); if (!(it->state & 3)) post_msg_to(w, HWND_OF(i), 0x111, it->id, 0); RET(0); return; }
  if (g_npop) menu_close_to(c, 0); int x0[MITEM_MAX], x1[MITEM_MAX]; menu_bar_layout(w, i, x0, x1); int wx, wy; wnd_window_origin(i, &wx, &wy); menu_popup_open(c, i, menu_of(it->sub), wx + x0[k], wy + g_wnd[i].ncy, k, 0, done); }
/* WM_NCLBUTTONDOWN(HTMENU) reached DefWindowProc: lParam = screen point */
static int menu_bar_press(Cpu *c, int i, u64 lp) { int fx = (int)(s16)(u16)lp - SCREEN_X0, fy = (int)(s16)(u16)(lp >> 16) - SCREEN_Y0; int k = menu_bar_hit(W, i, fx, fy); if (k < 0) return 0; if (g_npop && g_pop[0].owner == i && g_pop[0].bar_item == k) { menu_close_to(c, 0); RET(0); return 1; } menu_bar_open(c, i, k, 0); return 1; }
/* Alt+letter (WM_SYSCHAR) / F10 */
static int menu_bar_mnemonic(Cpu *c, int i, u32 ch) { int m = wnd_bar_menu(i); if (m < 0) return 0; if (ch >= 'a' && ch <= 'z') ch -= 32; for (int k = 0; k < g_menu[m].n; k++) if ((u32)menu_mnemonic(g_menu[m].it[k].text) == ch) { menu_bar_open(c, i, k, 0); return 1; } return 0; }

/* ---------- popup window procedure ---------- */
static int menu_pop_of_wnd(int i) { for (int p = 0; p < g_npop; p++) if (g_pop[p].wnd == i) return p; return -1; }
static void menu_set_hot(Cpu *c, int p, int k) { if (g_pop[p].hot == k) return; g_pop[p].hot = k; wnd_invalidate(g_pop[p].wnd, 0); W->dirty = 1; Win32 *w = W; int ow = g_pop[p].owner; if (wnd_alive(ow) && g_wnd[ow].pid == w->pid && g_wnd[ow].wndproc) { MItem *it = k >= 0 ? &g_menu[g_pop[p].menu].it[k] : 0; u64 wp = it ? ((it->sub ? (u64)k : (u64)it->id) | ((u64)((it->sub ? 0x10 : 0) | ((it->state & 3) ? 3 : 0)) << 16)) : 0xFFFF0000ull; post_msg_to(w, HWND_OF(ow), 0x11F, wp, menu_handle(g_pop[p].menu)); } (void)c; }
/* open the submenu of the hot item of popup p (cascade to the right) */
static void menu_open_sub(Cpu *c, int p, int k) { Win32 *w = W; MItem *it = &g_menu[g_pop[p].menu].it[k]; int sm = menu_of(it->sub); if (sm < 0 || (it->state & 3)) { RET(0); return; } menu_close_to(c, p + 1); int pw = g_pop[p].wnd; int wx, wy; wnd_window_origin(pw, &wx, &wy); int top = menu_item_top(w, g_pop[p].menu, k); menu_popup_open(c, g_pop[p].owner, sm, wx + g_wnd[pw].w - 3, wy + top - 1, -1, g_pop[p].sys, 0); }
static void menu_choose(Cpu *c, int p, int k) { MItem *it = &g_menu[g_pop[p].menu].it[k]; if (it->ftype & 0x800) { RET(0); return; } if (it->sub) { menu_open_sub(c, p, k); return; } if (it->state & 3) { RET(0); return; } u32 id = it->id; int sys = g_pop[p].sys; menu_finish(c, id, sys, 0); RET(0); }
static int menu_next_item(int m, int from, int dir) { Menu *mm = &g_menu[m]; if (mm->n == 0) return -1; int k = from; for (int tries = 0; tries < mm->n; tries++) { k += dir; if (k < 0) k = mm->n - 1; if (k >= mm->n) k = 0; if (!(mm->it[k].ftype & 0x800)) return k; } return -1; }
static void menu_popup_paint(Cpu *c, int i);
static void s_NebulaMenuProc(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } u32 msg = (u32)ARG(1); u64 wp = ARG(2), lp = ARG(3); Win32 *w = W; int p = menu_pop_of_wnd(i);
  switch (msg) {
    case 0x0F: menu_popup_paint(c, i); return;
    case 0x14: RET(1); return;
    case 0x200: case 0x201: case 0x202: case 0x203: { if (p < 0) { RET(0); return; } int cx, cy; wnd_client_origin(i, &cx, &cy); int fx = (int)(s16)(u16)lp + cx, fy = (int)(s16)(u16)(lp >> 16) + cy;
      /* which popup is under the point? (topmost first) */ int hp = -1; for (int q = g_npop - 1; q >= 0; q--) { int qw = g_pop[q].wnd; int wx, wy; wnd_window_origin(qw, &wx, &wy); if (fx >= wx && fy >= wy && fx < wx + g_wnd[qw].w && fy < wy + g_wnd[qw].h) { hp = q; break; } }
      if (hp >= 0) { int qw = g_pop[hp].wnd; int wx, wy; wnd_window_origin(qw, &wx, &wy); int k = menu_item_at_y(w, g_pop[hp].menu, fy - wy, 0); if (k >= 0 && (g_menu[g_pop[hp].menu].it[k].ftype & 0x800)) k = -1;
        if (msg == 0x200) { if (k >= 0 && hp < g_npop - 1 && g_pop[hp].hot != k) menu_close_to(c, hp + 1); if (k >= 0) { int was = g_pop[hp].hot; menu_set_hot(c, hp, k); if (was != k && g_menu[g_pop[hp].menu].it[k].sub && !(g_menu[g_pop[hp].menu].it[k].state & 3) && hp == g_npop - 1) { menu_open_sub(c, hp, k); return; } } RET(0); return; }
        if (msg == 0x201 || msg == 0x203) { if (k >= 0) { g_pop[hp].pressed = 1; menu_set_hot(c, hp, k); if (g_menu[g_pop[hp].menu].it[k].sub) { menu_open_sub(c, hp, k); return; } } RET(0); return; }
        if (k >= 0) { menu_choose(c, hp, k); return; } RET(0); return; }
      /* over the owner's menu bar? */ int ow = g_pop[0].owner; if (wnd_alive(ow) && g_pop[0].bar_item >= 0) { int k = menu_bar_hit(W, ow, fx, fy); if (k >= 0) { if (msg == 0x200) { if (k != g_pop[0].bar_item) { menu_bar_open(c, ow, k, 0); return; } RET(0); return; } if (msg == 0x201) { if (k == g_pop[0].bar_item) menu_close_to(c, 0); else { menu_bar_open(c, ow, k, 0); return; } } RET(0); return; } }
      if (msg == 0x201 || msg == 0x204) { /* click outside: dismiss */ menu_finish(c, 0, 0, 1); } RET(0); return; }
    case 0x100: { if (p < 0) { RET(0); return; } int top = g_npop - 1; int m = g_pop[top].menu; int ow = g_pop[0].owner;
      if (wp == 27) { if (g_npop > 1) menu_close_to(c, g_npop - 1); else menu_finish(c, 0, 0, 1); RET(0); return; }
      if (wp == 0x28 || wp == 0x26) { int k = menu_next_item(m, g_pop[top].hot < 0 ? (wp == 0x28 ? -1 : g_menu[m].n) : g_pop[top].hot, wp == 0x28 ? 1 : -1); if (k >= 0) menu_set_hot(c, top, k); RET(0); return; }
      if (wp == 0x27) { int k = g_pop[top].hot; if (k >= 0 && g_menu[m].it[k].sub) { menu_open_sub(c, top, k); if (g_npop > top + 1) { int nk = menu_next_item(g_pop[top + 1].menu, -1, 1); if (nk >= 0) menu_set_hot(c, top + 1, nk); } return; } if (g_pop[0].bar_item >= 0 && wnd_alive(ow)) { int bm = wnd_bar_menu(ow); if (bm >= 0) { menu_bar_open(c, ow, (g_pop[0].bar_item + 1) % g_menu[bm].n, 0); return; } } RET(0); return; }
      if (wp == 0x25) { if (g_npop > 1) { menu_close_to(c, g_npop - 1); RET(0); return; } if (g_pop[0].bar_item >= 0 && wnd_alive(ow)) { int bm = wnd_bar_menu(ow); if (bm >= 0) { menu_bar_open(c, ow, (g_pop[0].bar_item + g_menu[bm].n - 1) % g_menu[bm].n, 0); return; } } RET(0); return; }
      if (wp == 0x0D || wp == 0x20) { int k = g_pop[top].hot; if (k >= 0) { menu_choose(c, top, k); return; } RET(0); return; }
      RET(0); return; }
    case 0x102: { if (p < 0) { RET(0); return; } int top = g_npop - 1; int m = g_pop[top].menu; u32 ch = (u32)wp; if (ch >= 'a' && ch <= 'z') ch -= 32; for (int k = 0; k < g_menu[m].n; k++) if ((u32)menu_mnemonic(g_menu[m].it[k].text) == ch) { menu_set_hot(c, top, k); menu_choose(c, top, k); return; } RET(0); return; }
    case 0x02: { int q = menu_pop_of_wnd(i); if (q >= 0 && q == g_npop - 1 && g_pop[q].wnd == g_menu_track_root) { g_npop = 0; g_menu_track_root = -1; g_capture = -1; } RET(0); return; }
    default: def_window_proc(c, i, msg, wp, lp); return; } }
/* painting: background, items, check marks, submenu arrows, accelerators; owner-drawn items through WM_DRAWITEM (chained guest calls) */
static struct { int wnd, idx; u64 hdc, buf; } g_md;
static void menu_draw_items(Cpu *c, int i, u64 hdc, int p) { Win32 *w = W; Surf s; if (!dc_surf(c, hdc, &s)) return; GdiObj *d = gdi_get(w, hdc); if (!d) return; GdiObj *fo = gdi_get(w, g_dlg_font); if (fo) d->font = g_dlg_font; int cw, fh; font_dims(w, d->font, &cw, &fh);
  int W_ = wnd_client_w(i), H_ = wnd_client_h(i); fill_rect_px(&s, 0, 0, W_, H_, 0xf2f2f2); edge_rect(&s, 0, 0, W_, H_, 0xcccccc, 0xcccccc); if (p < 0) return; Menu *mm = &g_menu[g_pop[p].menu];
  int y = 1; for (int k = 0; k < mm->n; k++) { MItem *it = &mm->it[k]; int ih = menu_item_h(w, it); if (it->ftype & 0x800) { hline(&s, 4, W_ - 5, y + ih / 2, 0xd7d7d7); y += ih; continue; } int hot = g_pop[p].hot == k; int dis = (it->state & 3) != 0;
    if (hot) { fill_rect_px(&s, 2, y, W_ - 2, y + ih, dis ? 0xe0e0e0 : 0x91c9f7); edge_rect(&s, 2, y, W_ - 2, y + ih, dis ? 0xc8c8c8 : 0x3399ff, dis ? 0xc8c8c8 : 0x3399ff); }
    if (it->ftype & 0x100) { y += ih; continue; } u32 col = dis ? 0x6d6d6d : 0x000000; int ty = y + (ih - fh) / 2;
    if (it->state & 8) { if (it->ftype & 0x200) { fill_rect_px(&s, 9, y + ih / 2 - 2, 14, y + ih / 2 + 3, col); } else { line_px(&s, 7, y + ih / 2, 10, y + ih / 2 + 3, col); line_px(&s, 10, y + ih / 2 + 3, 15, y + ih / 2 - 3, col); line_px(&s, 7, y + ih / 2 + 1, 10, y + ih / 2 + 4, col); line_px(&s, 10, y + ih / 2 + 4, 15, y + ih / 2 - 2, col); } }
    menu_draw_label(&s, d, w, 22, ty, it->text, col); const char *a = menu_accel(it->text); if (a) { int aw = text_width(w, d->font, a, (int)e_strlen(a)); d->text_color = px2cr(col); draw_text(&s, d, w, W_ - 18 - aw, ty, a, (int)e_strlen(a)); }
    if (it->sub) { int ax = W_ - 12, ay = y + ih / 2; for (int q = 0; q < 4; q++) line_px(&s, ax + q, ay - 3 + q, ax + q, ay + 3 - q, col); }
    y += ih; }
  mark_screen(w, hdc); }
static void menudraw_step(Cpu *c) { Win32 *w = W; int i = g_md.wnd; int p = menu_pop_of_wnd(i); int ow = p >= 0 ? g_pop[p].owner : -1; int can = p >= 0 && wnd_alive(ow) && g_wnd[ow].pid == w->pid && g_wnd[ow].wndproc;
  if (can) { Menu *mm = &g_menu[g_pop[p].menu]; for (; g_md.idx < mm->n; g_md.idx++) { MItem *it = &mm->it[g_md.idx]; if (!(it->ftype & 0x100)) continue; if (!g_md.buf) g_md.buf = w32_heap_alloc(w, 64, 1); u64 b = g_md.buf; int top = menu_item_top(w, g_pop[p].menu, g_md.idx), ih = menu_item_h(w, it); u32 st = (g_pop[p].hot == g_md.idx ? 1 : 0) | ((it->state & 3) ? 6 : 0) | ((it->state & 8) ? 8 : 0);
      wr32(c, b, 1); wr32(c, b + 4, 0); wr32(c, b + 8, it->id); wr32(c, b + 12, 1); wr32(c, b + 16, st); if (c->mode32) { wr32(c, b + 20, (u32)menu_handle(g_pop[p].menu)); wr32(c, b + 24, (u32)g_md.hdc); wr32(c, b + 28, 2); wr32(c, b + 32, (u32)top); wr32(c, b + 36, (u32)(wnd_client_w(i) - 2)); wr32(c, b + 40, (u32)(top + ih)); wr32(c, b + 44, (u32)it->data); }
      else { wr64(c, b + 24, menu_handle(g_pop[p].menu)); wr64(c, b + 32, g_md.hdc); wr32(c, b + 40, 2); wr32(c, b + 44, (u32)top); wr32(c, b + 48, (u32)(wnd_client_w(i) - 2)); wr32(c, b + 52, (u32)(top + ih)); wr64(c, b + 56, it->data); }
      g_md.idx++; w32_guest_call(c, g_wnd[ow].wndproc, HWND_OF(ow), 0x2B, 0, b); return; } }
  if (g_md.buf) { w32_heap_free(w, g_md.buf); g_md.buf = 0; } if (wnd_alive(i)) ctl_paint_end(c, i, g_md.hdc); else wnd_dc_free(w, g_md.hdc); w32_cont_finish(c, 0); }
static void menudraw_resume(Cpu *c, u64 rax) { (void)rax; menudraw_step(c); }
static void menu_popup_paint(Cpu *c, int i) { Win32 *w = W; int p = menu_pop_of_wnd(i); u64 hdc = ctl_paint_begin(c, i); if (!hdc) { RET(0); return; } menu_draw_items(c, i, hdc, p);
  int owner_drawn = 0; if (p >= 0) { Menu *mm = &g_menu[g_pop[p].menu]; for (int k = 0; k < mm->n; k++) if (mm->it[k].ftype & 0x100) owner_drawn = 1; }
  if (!owner_drawn) { ctl_paint_end(c, i, hdc); RET(0); return; } g_md.wnd = i; g_md.idx = 0; g_md.hdc = hdc; g_md.buf = 0; w32_push_cont(c, CONT_MENUDRAW, (u64)i); (void)w; menudraw_step(c); }

/* ---------- menu templates (RT_MENU): MENUHEADER version 0 = classic, 1 = MENUEX ---------- */
static u64 menu_tmpl_items(Cpu *c, u64 p, int m, int ex, int depth) { if (depth > 8) return p; for (int guard = 0; guard < 512; guard++) {
    if (ex) { p = (p + 3) & ~3ull; u32 type = rd32(c, p), state = rd32(c, p + 4), id = rd32(c, p + 8); u16 res = rd16(c, p + 12); p += 14; char t[96]; wstr_to_utf8(c, p, t, sizeof t, -1); while (rd16(c, p)) p += 2; p += 2; p = (p + 3) & ~3ull;
      int pos = menu_insert_at(m, -1); if (pos >= 0) { MItem *it = &g_menu[m].it[pos]; it->ftype = type; it->state = state; it->id = id; e_snprintf(it->text, sizeof it->text, "%s", t); }
      if (res & 1) { p += 4; int sm = menu_new(W); if (sm < 0) return p; if (pos >= 0) g_menu[m].it[pos].sub = menu_handle(sm); p = menu_tmpl_items(c, p, sm, 1, depth + 1); } if (res & 0x80) return p; }
    else { u16 opt = rd16(c, p); p += 2; u32 id = 0; if (!(opt & 0x10)) { id = rd16(c, p); p += 2; } char t[96]; wstr_to_utf8(c, p, t, sizeof t, -1); while (rd16(c, p)) p += 2; p += 2;
      int pos = menu_insert_at(m, -1); if (pos >= 0) { MItem *it = &g_menu[m].it[pos]; it->id = id; it->ftype = (opt & 0x800) ? 0x800 : 0; if (opt & 0x20) it->ftype |= 0x20; if (opt & 0x40) it->ftype |= 0x40; it->state = ((opt & 3) ? 3 : 0) | ((opt & 8) ? 8 : 0); e_snprintf(it->text, sizeof it->text, "%s", t); if (!t[0] && !id) it->ftype |= 0x800; }
      if (opt & 0x10) { int sm = menu_new(W); if (sm < 0) return p; if (pos >= 0) g_menu[m].it[pos].sub = menu_handle(sm); p = menu_tmpl_items(c, p, sm, 0, depth + 1); } if (opt & 0x80) return p; } }
  return p; }
static u64 menu_load_indirect(Cpu *c, u64 t) { if (!t) return 0; u16 ver = rd16(c, t), off = rd16(c, t + 2); int m = menu_new(W); if (m < 0) return 0; if (ver == 1) { u64 p = t + 4 + off; menu_tmpl_items(c, p, m, 1, 0); } else menu_tmpl_items(c, t + 4 + off, m, 0, 0); return menu_handle(m); }
static void s_LoadMenuIndirectW(Cpu *c) { RET(menu_load_indirect(c, ARG(0))); }
static void s_LoadMenuIndirectA(Cpu *c) { RET(menu_load_indirect(c, ARG(0))); }
static void s_LoadMenuW(Cpu *c) { u64 e = rsrc_lookup(c, ARG(0), ARG(1), 4, 1, 0); if (!e) { W->last_error = 1814; RET(0); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; RET(menu_load_indirect(c, base + rd32(c, e))); }
static void s_LoadMenuA(Cpu *c) { u64 e = rsrc_lookup(c, ARG(0), ARG(1), 4, 0, 0); if (!e) { W->last_error = 1814; RET(0); return; } u64 base = ARG(0) ? ARG(0) : W->image_base; RET(menu_load_indirect(c, base + rd32(c, e))); }

/* ---------- the API ---------- */
static void s_CreateMenu(Cpu *c) { int m = menu_new(W); RET(m < 0 ? 0 : menu_handle(m)); }
static void s_CreatePopupMenu(Cpu *c) { s_CreateMenu(c); }
static void s_DestroyMenu(Cpu *c) { int m = menu_of(ARG(0)); if (m < 0) { RET(0); return; } for (int p = 0; p < g_npop; p++) if (g_pop[p].menu == m) { menu_close_to(c, p); break; } for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && g_wnd[k].menu == ARG(0)) { g_wnd[k].menu = 0; menu_apply_bar(W, k); } menu_destroy(m, 0); RET(1); }
static void s_IsMenu(Cpu *c) { RET(menu_of(ARG(0)) >= 0); }
static void menu_changed(Win32 *w, int m) { for (int k = 0; k < MAX_WND; k++) if (g_wnd[k].used && !g_wnd[k].destroyed && menu_of(g_wnd[k].menu) == m) menu_apply_bar(w, k); for (int p = 0; p < g_npop; p++) if (g_pop[p].menu == m) { wnd_invalidate(g_pop[p].wnd, 0); w->dirty = 1; } }
static void append_menu(Cpu *c, int wide) { int m = menu_of(ARG(0)); if (m < 0) { RET(0); return; } int pos = menu_insert_at(m, -1); if (pos < 0) { RET(0); return; } menu_item_from_flags(c, &g_menu[m].it[pos], (u32)ARG(1), ARG(2), ARG(3), wide); menu_changed(W, m); RET(1); }
static void s_AppendMenuW(Cpu *c) { append_menu(c, 1); }
static void s_AppendMenuA(Cpu *c) { append_menu(c, 0); }
static void insert_menu(Cpu *c, int wide) { u32 flags = (u32)ARG(2); int m = menu_of(ARG(0)); if (m < 0) { RET(0); return; } int pos; if (flags & 0x400) { pos = (int)(s32)(u32)ARG(1); if (pos < 0 || pos > g_menu[m].n) pos = g_menu[m].n; } else { int idx; int mm = menu_item_at(ARG(0), (u32)ARG(1), 0, &idx); if (mm < 0) { pos = g_menu[m].n; } else { m = mm; pos = idx; } }
  pos = menu_insert_at(m, pos); if (pos < 0) { RET(0); return; } menu_item_from_flags(c, &g_menu[m].it[pos], flags, ARG(3), ARG(4), wide); menu_changed(W, m); RET(1); }
static void s_InsertMenuW(Cpu *c) { insert_menu(c, 1); }
static void s_InsertMenuA(Cpu *c) { insert_menu(c, 0); }
static void modify_menu(Cpu *c, int wide) { int idx; u32 flags = (u32)ARG(2); int m = menu_item_at(ARG(0), (u32)ARG(1), flags, &idx); if (m < 0) { RET(0); return; } MItem *it = &g_menu[m].it[idx]; MItem old = *it; menu_item_from_flags(c, it, flags, ARG(3), ARG(4), wide); if (!(flags & 0x10) && old.sub && !(flags & 0x100)) { it->sub = old.sub; } if (!(flags & 0x1000)) it->state = (it->state & ~0x1000u) | (old.state & 0x1000); menu_changed(W, m); RET(1); }
static void s_ModifyMenuW(Cpu *c) { modify_menu(c, 1); }
static void s_ModifyMenuA(Cpu *c) { modify_menu(c, 0); }
static void insert_menu_item(Cpu *c, int wide) { int m = menu_of(ARG(0)); if (m < 0) { W->last_error = 1401; RET(0); return; } int pos; if (ARG(2)) { pos = (int)(s32)(u32)ARG(1); if (pos < 0 || pos > g_menu[m].n) pos = g_menu[m].n; } else { int idx; int mm = menu_item_at(ARG(0), (u32)ARG(1), 0, &idx); if (mm < 0) pos = g_menu[m].n; else { m = mm; pos = idx; } }
  pos = menu_insert_at(m, pos); if (pos < 0) { RET(0); return; } mii_read(c, ARG(3), &g_menu[m].it[pos], wide); menu_changed(W, m); RET(1); }
static void s_InsertMenuItemW(Cpu *c) { insert_menu_item(c, 1); }
static void s_InsertMenuItemA(Cpu *c) { insert_menu_item(c, 0); }
static void set_menu_item_info(Cpu *c, int wide) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), ARG(2) ? 0x400 : 0, &idx); if (m < 0) { W->last_error = 1401; RET(0); return; } mii_read(c, ARG(3), &g_menu[m].it[idx], wide); menu_changed(W, m); RET(1); }
static void s_SetMenuItemInfoW(Cpu *c) { set_menu_item_info(c, 1); }
static void s_SetMenuItemInfoA(Cpu *c) { set_menu_item_info(c, 0); }
static void get_menu_item_info(Cpu *c, int wide) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), ARG(2) ? 0x400 : 0, &idx); if (m < 0) { W->last_error = 1401; RET(0); return; } mii_write(c, ARG(3), &g_menu[m].it[idx], wide, m); RET(1); }
static void s_GetMenuItemInfoW(Cpu *c) { get_menu_item_info(c, 1); }
static void s_GetMenuItemInfoA(Cpu *c) { get_menu_item_info(c, 0); }
static void s_RemoveMenu(Cpu *c) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), (u32)ARG(2), &idx); if (m < 0) { RET(0); return; } menu_remove_at(m, idx); menu_changed(W, m); RET(1); }
static void s_DeleteMenu(Cpu *c) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), (u32)ARG(2), &idx); if (m < 0) { RET(0); return; } u64 sub = g_menu[m].it[idx].sub; menu_remove_at(m, idx); if (sub) menu_destroy(menu_of(sub), 0); menu_changed(W, m); RET(1); }
static void s_SetMenu(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } if (ARG(1) && menu_of(ARG(1)) < 0) { W->last_error = 1401; RET(0); return; } g_wnd[i].menu = ARG(1); menu_apply_bar(W, i); RET(1); }
static void s_GetMenu(Cpu *c) { int i = wnd_of(ARG(0)); RET(i >= 0 && g_wnd[i].parent < 0 ? g_wnd[i].menu : 0); }
static void s_DrawMenuBar(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } menu_apply_bar(W, i); if (g_wnd[i].visible) menu_bar_refresh(c, i); RET(1); }
static void s_GetSystemMenu(Cpu *c) { int i = wnd_of(ARG(0)); if (i < 0) { RET(0); return; } Wnd *n = &g_wnd[i]; if (ARG(1) && n->sysmenu) { menu_destroy(menu_of(n->sysmenu), 0); n->sysmenu = 0; RET(0); return; }
  if (!n->sysmenu) { int m = menu_new(W); if (m < 0) { RET(0); return; } static const struct { u32 id; const char *t; } sysit[] = { { 0xF120, "&Restore" }, { 0xF010, "&Move" }, { 0xF000, "&Size" }, { 0xF020, "Mi&nimize" }, { 0xF030, "Ma&ximize" }, { 0, "-" }, { 0xF060, "&Close\tAlt+F4" } };
    for (int k = 0; k < 7; k++) { int pos = menu_insert_at(m, -1); if (pos < 0) break; MItem *it = &g_menu[m].it[pos]; it->id = sysit[k].id; it->ftype = sysit[k].id ? 0 : 0x800; e_snprintf(it->text, sizeof it->text, "%s", sysit[k].t); if (sysit[k].id == 0xF120) it->state = 3; } n->sysmenu = menu_handle(m); }
  RET(n->sysmenu); }
static void s_GetSubMenu(Cpu *c) { int m = menu_of(ARG(0)); int k = (int)(s32)(u32)ARG(1); if (m < 0 || k < 0 || k >= g_menu[m].n) { RET(0); return; } RET(menu_of(g_menu[m].it[k].sub) >= 0 ? g_menu[m].it[k].sub : 0); }
static void s_GetMenuItemCount(Cpu *c) { int m = menu_of(ARG(0)); RET(m < 0 ? (u64)(u32)-1 : (u64)g_menu[m].n); }
static void s_GetMenuItemID(Cpu *c) { int m = menu_of(ARG(0)); int k = (int)(s32)(u32)ARG(1); if (m < 0 || k < 0 || k >= g_menu[m].n || g_menu[m].it[k].sub) { RET((u64)(u32)-1); return; } RET(g_menu[m].it[k].id); }
static u32 menu_mf_state(MItem *it) { u32 f = 0; if (it->state & 3) f |= 3; if (it->state & 8) f |= 8; if (it->ftype & 0x800) f |= 0x800; if (it->ftype & 0x100) f |= 0x100; if (it->ftype & 0x20) f |= 0x20; if (it->ftype & 0x40) f |= 0x40; if (it->state & 0x80) f |= 0x80; if (it->state & 0x1000) f |= 0x1000; if (it->sub) { int s = menu_of(it->sub); f |= 0x10 | ((u32)(s >= 0 ? g_menu[s].n : 0) << 8); } return f; }
static void s_GetMenuState(Cpu *c) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), (u32)ARG(2), &idx); if (m < 0) { RET((u64)(u32)-1); return; } RET(menu_mf_state(&g_menu[m].it[idx])); }
static void get_menu_string(Cpu *c, int wide) { int idx; int m = menu_item_at(ARG(0), (u32)ARG(1), (u32)ARG(4), &idx); if (m < 0) { RET(0); return; } MItem *it = &g_menu[m].it[idx]; if (!ARG(2) || !ARG(3)) { RET(wide ? utf8_u16len(it->text) : (u32)e_strlen(it->text)); return; } RET(text_out_cap(c, ARG(2), it->text, (u32)ARG(3), wide)); }
static void s_GetMenuStringW(Cpu *c) { get_menu_string(c, 1); }
static void s_GetMenuStringA(Cpu *c) { get_menu_string(c, 0); }
static void s_EnableMenuItem(Cpu *c) { int idx; u32 flags = (u32)ARG(2); int m = menu_item_at(ARG(0), (u32)ARG(1), flags, &idx); if (m < 0) { RET((u64)(u32)-1); return; } MItem *it = &g_menu[m].it[idx]; u32 prev = it->state & 3; it->state = (it->state & ~3u) | (flags & 3); if (prev != (it->state & 3)) menu_changed(W, m); RET(prev); }
static void s_CheckMenuItem(Cpu *c) { int idx; u32 flags = (u32)ARG(2); int m = menu_item_at(ARG(0), (u32)ARG(1), flags, &idx); if (m < 0) { RET((u64)(u32)-1); return; } MItem *it = &g_menu[m].it[idx]; u32 prev = it->state & 8; it->state = (it->state & ~8u) | (flags & 8); if (prev != (it->state & 8)) menu_changed(W, m); RET(prev); }
static void s_CheckMenuRadioItem(Cpu *c) { int m = menu_of(ARG(0)); if (m < 0) { RET(0); return; } u32 first = (u32)ARG(1), last = (u32)ARG(2), check = (u32)ARG(3), flags = (u32)ARG(4); int ok = 0;
  if (flags & 0x400) { for (u32 k = first; k <= last && k < (u32)g_menu[m].n; k++) { MItem *it = &g_menu[m].it[k]; it->ftype |= 0x200; if (k == check) { it->state |= 8; ok = 1; } else it->state &= ~8u; } }
  else { for (int k = 0; k < g_menu[m].n; k++) { MItem *it = &g_menu[m].it[k]; if (it->id >= first && it->id <= last) { it->ftype |= 0x200; if (it->id == check) { it->state |= 8; ok = 1; } else it->state &= ~8u; } } }
  menu_changed(W, m); RET(ok); }
static void s_SetMenuDefaultItem(Cpu *c) { int m = menu_of(ARG(0)); if (m < 0) { RET(0); return; } u32 item = (u32)ARG(1); for (int k = 0; k < g_menu[m].n; k++) { MItem *it = &g_menu[m].it[k]; int hit = item != 0xFFFFFFFFu && (ARG(2) ? (u32)k == item : it->id == item); it->state = (it->state & ~0x1000u) | (hit ? 0x1000 : 0); } RET(1); }
static void s_GetMenuDefaultItem(Cpu *c) { int m = menu_of(ARG(0)); if (m < 0) { RET((u64)(u32)-1); return; } for (int k = 0; k < g_menu[m].n; k++) if (g_menu[m].it[k].state & 0x1000) { RET(ARG(1) ? (u64)k : (u64)g_menu[m].it[k].id); return; } RET((u64)(u32)-1); }
static void s_SetMenuItemBitmaps(Cpu *c) { RET(menu_of(ARG(0)) >= 0); }
static void s_GetMenuCheckMarkDimensions(Cpu *c) { RET(13 | (13 << 16)); }
static void s_SetMenuInfo(Cpu *c) { RET(1); }
static void s_GetMenuInfo(Cpu *c) { RET(1); }
static void s_SetMenuContextHelpId(Cpu *c) { RET(1); }
static void s_GetMenuContextHelpId(Cpu *c) { RET(0); }
static void s_HiliteMenuItem(Cpu *c) { RET(1); }
static void s_GetMenuBarInfo(Cpu *c) { RET(0); }
static void s_GetMenuItemRect(Cpu *c) { int m = menu_of(ARG(1)); int k = (int)(s32)(u32)ARG(2); u64 r = ARG(3); if (m < 0 || k < 0 || k >= g_menu[m].n || !r) { RET(0); return; }
  for (int p = 0; p < g_npop; p++) if (g_pop[p].menu == m) { int pw = g_pop[p].wnd; int wx, wy; wnd_window_origin(pw, &wx, &wy); int top = menu_item_top(W, m, k); wr32(c, r, (u32)(wx + 2 + SCREEN_X0)); wr32(c, r + 4, (u32)(wy + top + SCREEN_Y0)); wr32(c, r + 8, (u32)(wx + g_wnd[pw].w - 2 + SCREEN_X0)); wr32(c, r + 12, (u32)(wy + top + menu_item_h(W, &g_menu[m].it[k]) + SCREEN_Y0)); RET(1); return; }
  for (int i = 0; i < MAX_WND; i++) if (wnd_alive(i) && menu_of(g_wnd[i].menu) == m) { int x0[MITEM_MAX], x1[MITEM_MAX]; menu_bar_layout(W, i, x0, x1); int wx, wy; wnd_window_origin(i, &wx, &wy); wr32(c, r, (u32)(wx + x0[k] + SCREEN_X0)); wr32(c, r + 4, (u32)(wy + BAR_TOP(i) + SCREEN_Y0)); wr32(c, r + 8, (u32)(wx + x1[k] + SCREEN_X0)); wr32(c, r + 12, (u32)(wy + BAR_TOP(i) + MENU_H + SCREEN_Y0)); RET(1); return; }
  wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, 0); wr32(c, r + 12, 0); RET(1); }
static void s_MenuItemFromPoint(Cpu *c) { int m = menu_of(ARG(1)); if (m < 0) { RET((u64)(u32)-1); return; } int fx = (int)(s32)(u32)ARG(2) - SCREEN_X0, fy = (int)(s32)(u32)ARG(3) - SCREEN_Y0; for (int p = 0; p < g_npop; p++) if (g_pop[p].menu == m) { int pw = g_pop[p].wnd; int wx, wy; wnd_window_origin(pw, &wx, &wy); if (fx >= wx && fx < wx + g_wnd[pw].w) { int k = menu_item_at_y(W, m, fy - wy, 0); RET(k < 0 ? (u64)(u32)-1 : (u64)k); return; } } RET((u64)(u32)-1); }
static void s_EndMenu(Cpu *c) { if (g_npop) menu_finish(c, 0, 0, 1); RET(1); }
/* TrackPopupMenu(hMenu, flags, x, y, reserved, hwnd, prcRect) / TrackPopupMenuEx(hMenu, flags, x, y, hwnd, lptpm): a modal loop like DialogBox */
static void track_popup(Cpu *c, u64 hmenu, u32 flags, int x, int y, u64 hwnd) {
  if (modal_reentry(c)) { int m = g_nmodal - 1; if (g_modal[m].wnd < 0) { w32_retry_yield(c); return; } modal_loop(c, g_modal[m].wnd); return; }
  int m = menu_of(hmenu); int ow = wnd_of(hwnd); if (m < 0 || g_menu[m].n == 0 || g_nmodal >= 8 || g_mo.active) { W->last_error = 1401; RET(0); return; } if (ow < 0) ow = wnd_alive(g_active) ? g_active : 0;
  if (g_npop) menu_close_to(c, 0); g_menu_track_owner = ow; g_menu_track_flags = flags; int pw, ph; menu_popup_size(W, m, &pw, &ph); int sx = x - SCREEN_X0, sy = y - SCREEN_Y0; if (flags & 0x4) sx -= pw / 2; else if (flags & 0x8) sx -= pw; if (flags & 0x10) sy -= ph / 2; else if (flags & 0x20) sy -= ph;
  int mi = g_nmodal++; g_modal[mi].rip = c->rip; g_modal[mi].rsp = c->r[RSP]; g_modal[mi].slot = w32_cur_slot(); g_modal[mi].wnd = -1; g_modal[mi].phase = 1; g_menu_track_root = -1;
  menu_popup_open(c, ow, m, sx, sy, -1, 0, 1); }
static void s_TrackPopupMenu(Cpu *c) { track_popup(c, ARG(0), (u32)ARG(1), (int)(s32)ARG(2), (int)(s32)ARG(3), ARG(5)); }
static void s_TrackPopupMenuEx(Cpu *c) { track_popup(c, ARG(0), (u32)ARG(1), (int)(s32)ARG(2), (int)(s32)ARG(3), ARG(4)); }

#define MENU_SHIMS \
  S(CreateMenu), S(CreatePopupMenu), S(LoadMenuW), S(LoadMenuA), S(LoadMenuIndirectW), S(LoadMenuIndirectA), S(DestroyMenu), S(AppendMenuW), S(AppendMenuA), S(InsertMenuW), S(InsertMenuA), S(InsertMenuItemW), S(InsertMenuItemA), \
  S(SetMenuItemInfoW), S(SetMenuItemInfoA), S(GetMenuItemInfoW), S(GetMenuItemInfoA), S(ModifyMenuW), S(ModifyMenuA), S(RemoveMenu), S(DeleteMenu), S(SetMenu), S(GetMenu), S(GetSystemMenu), S(GetSubMenu), S(GetMenuItemCount), \
  S(GetMenuItemID), S(GetMenuState), S(GetMenuStringW), S(GetMenuStringA), S(EnableMenuItem), S(CheckMenuItem), S(CheckMenuRadioItem), S(SetMenuDefaultItem), S(GetMenuDefaultItem), S(SetMenuItemBitmaps), S(GetMenuCheckMarkDimensions), \
  S(DrawMenuBar), S(TrackPopupMenu), S(TrackPopupMenuEx), S(IsMenu), S(SetMenuInfo), S(GetMenuInfo), S(SetMenuContextHelpId), S(GetMenuContextHelpId), S(HiliteMenuItem), S(GetMenuBarInfo), S(GetMenuItemRect), S(MenuItemFromPoint), S(EndMenu), S(NebulaMenuProc)
