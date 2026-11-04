/* user32 + gdi32 personality: one top-level window, message queue, software GDI on a BGRA framebuffer. */
#include "emu.h"
#include "font8x8_basic.h"
#include "font8x8_ext.h"
/* UTF-8 → code point (advances *i); glyph lookup falls back to '?' */
static unsigned utf8_next(const char *str, int len, int *i) { unsigned char c0 = (unsigned char)str[*i]; (*i)++; if (c0 < 0x80) return c0; int extra = c0 >= 0xF0 ? 3 : c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : 0; unsigned cp = c0 & (0x3F >> extra); for (int k = 0; k < extra; k++) { if (*i >= len || ((unsigned char)str[*i] & 0xC0) != 0x80) return '?'; cp = (cp << 6) | ((unsigned char)str[*i] & 0x3F); (*i)++; } return cp; }
static int utf8_cols(const char *str, int len) { int i = 0, n = 0; while (i < len) { utf8_next(str, len, &i); n++; } return n; }
static const unsigned char *glyph_of(unsigned cp) { if (cp < 128) return font8x8_basic[cp]; const unsigned char *g = font8x8_ext_lookup(cp); return g ? g : font8x8_basic[(int)'?']; }

#define ARG(n) w32_arg(c, n)
#define RET(v) (c->r[RAX] = (u64)(v))
#define W (c->w)
#define GDI_TAG   0x01000000ull
#define CONT_RET_RAX 4
#define CONT_RET_VAL 5
#define CONT_CREATEWND 6
#define SLOT_CONT_ADDR (G_SHIM_BASE + 16)

int w32_format_args(Cpu *c, char *out, size_t n, u64 fmt, int wide, int first_arg);
int wstr_to_utf8(Cpu *c, u64 addr, char *out, size_t cap, int maxchars);
void w32_retry_yield(Cpu *c);
void w32_push_cont(Cpu *c, int kind, u64 val);
void w32_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3);
void w32_guest_call6(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
void w32_cont_finish(Cpu *c, u64 rax);

/* ---------- GDI objects ---------- */
static GdiObj *gdi_new(Win32 *w, int kind) {
  for (int i = 1; i < MAX_GDI; i++) if (!w->gdi[i].kind) { GdiObj *o = &w->gdi[i]; e_memset(o, 0, sizeof *o); o->kind = kind; o->handle = GDI_TAG | (u64)i; if (i >= w->ngdi) w->ngdi = i + 1; return o; }
  return 0;
}
static GdiObj *gdi_get(Win32 *w, u64 h) { if ((h & ~0xffffull) != GDI_TAG) return 0; u64 i = h & 0xffff; if (i >= MAX_GDI || !w->gdi[i].kind) return 0; return &w->gdi[i]; }
enum { K_DC = 1, K_BITMAP, K_BRUSH, K_PEN, K_FONT, K_RGN, K_PALETTE };
static u64 stock[20];
static u64 window_dc;

static u32 cr2px(u32 cr) { return ((cr & 0xff) << 16) | (cr & 0xff00) | ((cr >> 16) & 0xff); }
static u32 px2cr(u32 px) { return ((px & 0xff) << 16) | (px & 0xff00) | ((px >> 16) & 0xff); }

typedef struct { u32 *px; int w, h; int bottom_up; int stride; int x0, y0; } Surf;   /* valid pixels: x0 ≤ x < w, y0 ≤ y < h (x0/y0 > 0 when the DC origin is shifted or clipped) */
static inline u32 *srow(Surf *s, int y) { return s->bottom_up ? s->px + (size_t)(s->h - 1 - y) * s->stride : s->px + (size_t)y * s->stride; }
static void dib_sync(Win32 *w, GdiObj *bm);
static int dc_surf_raw(Cpu *c, u64 hdc, Surf *s) {
  Win32 *w = W; GdiObj *dc = gdi_get(w, hdc); if (!dc || dc->kind != K_DC) return 0; s->x0 = s->y0 = 0;
  if (hdc == window_dc || dc->wnd < 0) { s->px = w->screen; s->w = w->screen_w; s->h = w->screen_h; s->stride = w->screen_w; s->bottom_up = 0; return s->px != 0; }
  if (dc->wnd > 0) { if (!w->screen) return 0; int ox = dc->ox, oy = dc->oy, vw = dc->vw, vh = dc->vh; if (ox + vw > w->screen_w) vw = w->screen_w - ox; if (oy + vh > w->screen_h) vh = w->screen_h - oy; if (vw < 0) vw = 0; if (vh < 0) vh = 0;
    s->px = w->screen + (size_t)oy * w->screen_w + ox; s->w = vw; s->h = vh; s->stride = w->screen_w; s->bottom_up = 0; return 1; }
  GdiObj *bm = gdi_get(w, dc->bitmap); if (!bm || bm->kind != K_BITMAP) return 0; if (bm->dib_section && bm->dib_bpp != 32) dib_sync(w, bm);
  s->px = (u32 *)mem_ptr(w->mem, bm->bits, (u64)bm->bw * bm->bh * 4, 1); s->w = bm->bw; s->h = bm->bh; s->stride = bm->bw; s->bottom_up = bm->font_bold; /* font_bold reused as bottom-up flag for bitmaps */
  return s->px != 0;
}
/* the surface in LOGICAL coordinates of the DC: device = logical + (lox, loy); the valid rectangle is the device area ∩ clip, expressed in logical coordinates.
   Bottom-up DIBs are addressed through srow(), so the shift is folded into x0/y0/w/h and the row pointer instead of moving px. */
static int dc_surf(Cpu *c, u64 hdc, Surf *s) {
  if (!dc_surf_raw(c, hdc, s)) return 0; GdiObj *dc = gdi_get(W, hdc); if (!dc) return 0; int lox = dc->lox, loy = dc->loy;
  int dx0 = 0, dy0 = 0, dx1 = s->w, dy1 = s->h; if (dc->clip_on) { if (dc->cl > dx0) dx0 = dc->cl; if (dc->ct > dy0) dy0 = dc->ct; if (dc->cr < dx1) dx1 = dc->cr; if (dc->cb < dy1) dy1 = dc->cb; }
  if (!lox && !loy && dx0 == 0 && dy0 == 0 && dx1 == s->w && dy1 == s->h) return 1;
  if (dx1 < dx0) dx1 = dx0; if (dy1 < dy0) dy1 = dy0;
  /* logical (x, y) → device (x + lox, y + loy): shift the pixel origin so that logical (0,0) addresses device (lox, loy) */
  if (s->bottom_up) { /* srow(y) = px + (h-1-y)*stride: a logical y maps to device y+loy, i.e. srow'(y) = px + (h-1-(y+loy))*stride = px' + (h'-1-y)*stride with h' = h - loy and px' = px */ s->h -= loy; s->px += lox; }
  else { s->px += (size_t)loy * s->stride + lox; s->h = dy1 - loy; }
  s->w = dx1 - lox; s->x0 = dx0 - lox; s->y0 = dy0 - loy; if (!s->bottom_up) { } else { s->y0 = dy0 - loy; if (dy1 - loy < s->h) s->h = dy1 - loy; }
  return 1;
}
/* character cell width of a font: derived from the em height (negative lfHeight) when known, else from the cell height */
static int font_cw(GdiObj *f, int fh) { int em = f && f->kind == K_FONT && f->font_em ? f->font_em : fh; int cw = (em * 11 + 10) / 20; return cw < 3 ? 3 : cw; }
static void mark_screen(Win32 *w, u64 hdc) { if (hdc == window_dc) { w->dirty = 1; w->blits_since_yield++; return; } GdiObj *dc = gdi_get(w, hdc); if (dc && dc->wnd) { w->dirty = 1; w->blits_since_yield++; } }
static u32 dc_brush_px(Win32 *w, GdiObj *dc, int *null_brush) { GdiObj *b = gdi_get(w, dc->brush); *null_brush = 0; if (!b) return 0xffffff; if (b->kind == K_BRUSH && b->pen_width == -1) { *null_brush = 1; return 0; } return b->color; }
static u32 dc_pen_px(Win32 *w, GdiObj *dc, int *null_pen) { GdiObj *p = gdi_get(w, dc->pen); *null_pen = 0; if (!p) return 0; if (p->pen_width == -1) { *null_pen = 1; return 0; } return p->color; }

static void fill_rect_px(Surf *s, int x0, int y0, int x1, int y1, u32 px) {
  if (x0 < s->x0) x0 = s->x0; if (y0 < s->y0) y0 = s->y0; if (x1 > s->w) x1 = s->w; if (y1 > s->h) y1 = s->h;
  for (int y = y0; y < y1; y++) { u32 *r = srow(s, y); for (int x = x0; x < x1; x++) r[x] = px; }
}
static void hline(Surf *s, int x0, int x1, int y, u32 px) { if (y < s->y0 || y >= s->h) return; if (x0 > x1) { int t = x0; x0 = x1; x1 = t; } if (x0 < s->x0) x0 = s->x0; if (x1 >= s->w) x1 = s->w - 1; u32 *r = srow(s, y); for (int x = x0; x <= x1; x++) r[x] = px; }
static void put_px(Surf *s, int x, int y, u32 px) { if (x < s->x0 || y < s->y0 || x >= s->w || y >= s->h) return; srow(s, y)[x] = px; }
static void line_px(Surf *s, int x0, int y0, int x1, int y1, u32 px) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
  for (int n = 0; n < 100000; n++) { put_px(s, x0, y0, px); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 > -dy) { err -= dy; x0 += sx; } if (e2 < dx) { err += dx; y0 += sy; } }
}
/* scanline polygon fill (even-odd) */
static void fill_poly(Surf *s, const s32 *pts, int n, u32 px) {
  if (n < 3) return; int ymin = pts[1], ymax = pts[1];
  for (int i = 1; i < n; i++) { if (pts[i * 2 + 1] < ymin) ymin = pts[i * 2 + 1]; if (pts[i * 2 + 1] > ymax) ymax = pts[i * 2 + 1]; }
  if (ymin < 0) ymin = 0; if (ymax > s->h) ymax = s->h;
  for (int y = ymin; y < ymax; y++) {
    float yc = (float)y + 0.5f; float xs[64]; int nx = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; float x0 = (float)pts[i * 2], y0 = (float)pts[i * 2 + 1], x1 = (float)pts[j * 2], y1 = (float)pts[j * 2 + 1];
      if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) { float x = x0 + (yc - y0) * (x1 - x0) / (y1 - y0); if (nx < 64) xs[nx++] = x; } }
    for (int i = 1; i < nx; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
    for (int i = 0; i + 1 < nx; i += 2) { int xa = (int)(xs[i] + 0.5f), xb = (int)(xs[i + 1] + 0.5f); if (xb > xa) hline(s, xa, xb - 1, y, px); }
  }
}
static void fill_ellipse(Surf *s, int l, int t, int r, int b, u32 px, int fill, u32 pen, int outline) {
  int w = r - l, h = b - t; if (w <= 0 || h <= 0) return; float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f, rx = w * 0.5f, ry = h * 0.5f;
  for (int y = t; y < b; y++) { float dy = (y + 0.5f - cy) / ry; float v = 1 - dy * dy; if (v < 0) continue; float hw = rx * e_sqrt(v); int xa = (int)(cx - hw + 0.5f), xb = (int)(cx + hw - 0.5f);
    if (fill) hline(s, xa, xb, y, px); if (outline) { put_px(s, xa, y, pen); put_px(s, xb, y, pen); if (y == t || y == b - 1) hline(s, xa, xb, y, pen); } }
}
static void draw_text(Surf *s, GdiObj *dc, Win32 *w, int x, int y, const char *str, int len) {
  GdiObj *f = gdi_get(w, dc->font); int fh = f && f->kind == K_FONT ? f->font_h : 16; int bold = f && f->kind == K_FONT ? f->font_bold : 0;
  if (fh < 4) fh = 4; int cw = font_cw(f, fh); int gh = f && f->kind == K_FONT && f->font_em && f->font_em < fh ? f->font_em : fh;   /* glyphs fill the em; the internal leading (fh - gh) stays above them like a real TrueType cell */
  u32 fg = cr2px(dc->text_color), bg = cr2px(dc->bk_color); int opaque = dc->bk_mode != 1;
  if (opaque) fill_rect_px(s, x, y, x + cw * utf8_cols(str, len), y + fh, bg);
  for (int i = 0, col = 0; i < len; col++) { unsigned cp = utf8_next(str, len, &i); const unsigned char *g = glyph_of(cp); int gx0 = x + col * cw;
    for (int py = 0; py < gh; py++) { int gy = py * 8 / gh; int yy = y + (fh - gh) + py; if (yy < s->y0 || yy >= s->h) continue; u32 *row = srow(s, yy);
      for (int px = 0; px < cw; px++) { int gx = px * 8 / cw; int on = (g[gy] >> gx) & 1; if (!on && bold && gx > 0) on = (g[gy] >> (gx - 1)) & 1; if (on) { int xx = gx0 + px; if (xx >= s->x0 && xx < s->w) row[xx] = fg; } } } }
}

/* UTF-8 → UTF-16 into guest memory (returns the number of UTF-16 units written, excluding the terminator) */
static int put_w_utf8(Cpu *c, u64 dst, const char *s, int cap) { int i = 0, o = 0, n = (int)e_strlen(s);
  while (i < n && o + 2 < cap) { u32 b = (u8)s[i++], cp = b; int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0; if (extra) cp = b & (0x3f >> extra);
    for (int k = 0; k < extra && i < n && ((u8)s[i] & 0xC0) == 0x80; k++) cp = (cp << 6) | ((u8)s[i++] & 0x3f);
    if (cp >= 0x10000) { cp -= 0x10000; wr16(c, dst + o++ * 2, (u16)(0xD800 | (cp >> 10))); wr16(c, dst + o++ * 2, (u16)(0xDC00 | (cp & 0x3ff))); } else wr16(c, dst + o++ * 2, (u16)cp); }
  wr16(c, dst + o * 2, 0); return o; }
static void s_wsprintfW(Cpu *c) { char out[1024]; w32_format_args(c, out, sizeof out, ARG(1), 1, 2); if (c->status) return; RET(put_w_utf8(c, ARG(0), out, 1024)); }
static void s_wsprintfA(Cpu *c) { char out[1024]; int n = w32_format_args(c, out, sizeof out, ARG(1), 0, 2); if (c->status) return; u8 *p = mem_ptr(c->mem, ARG(0), n + 1, 1); if (p) e_memcpy(p, out, n + 1); RET(n); }
static void s_SetProcessDPIAware(Cpu *c) { RET(1); }
static void s_SetProcessDpiAwarenessContext(Cpu *c) { RET(1); }
static void s_GetDpiForWindow(Cpu *c) { RET(96); }
static void s_GetDpiForSystem(Cpu *c) { RET(96); }
static void s_EnumDisplaySettingsA(Cpu *c) { RET(0); }
static void s_ChangeDisplaySettingsA(Cpu *c) { RET(0); }
static void s_SetRect(Cpu *c) { u64 r = ARG(0); wr32(c, r, (u32)ARG(1)); wr32(c, r + 4, (u32)ARG(2)); wr32(c, r + 8, (u32)ARG(3)); wr32(c, r + 12, (u32)ARG(4)); RET(1); }
static void s_PtInRect(Cpu *c) { u64 r = ARG(0); s32 x = (s32)(u32)ARG(1), y = (s32)(u32)(ARG(1) >> 32); RET(x >= (s32)rd32(c, r) && x < (s32)rd32(c, r + 8) && y >= (s32)rd32(c, r + 4) && y < (s32)rd32(c, r + 12)); }
static void s_FillRect(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 r = ARG(1); u64 hb = ARG(2); u32 px = 0;
  GdiObj *b = gdi_get(w, hb); if (b) { if (b->kind == K_BRUSH && b->pen_width == -1) { RET(1); return; } px = b->color; } else if (hb >= 1 && hb <= 31) { extern u32 sys_color_px(u32); px = sys_color_px((u32)hb - 1); }
  fill_rect_px(&s, (s32)rd32(c, r), (s32)rd32(c, r + 4), (s32)rd32(c, r + 8), (s32)rd32(c, r + 12), px); mark_screen(w, ARG(0)); RET(1); }
static u64 make_dc(Win32 *w) { GdiObj *dc = gdi_new(w, K_DC); if (!dc) return 0; dc->text_color = 0; dc->bk_color = 0xffffff; dc->bk_mode = 2; dc->pen = stock[7]; dc->brush = stock[0]; dc->font = stock[13]; return dc->handle; }
static void s_CreateCompatibleDC(Cpu *c) { RET(make_dc(W)); }
static void s_CreateDCA(Cpu *c) { RET(window_dc); }
static void dcsave_drop(u64 hdc);
static void s_DeleteDC(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (o && o->kind == K_DC && ARG(0) != window_dc) { o->kind = 0; dcsave_drop(ARG(0)); RET(1); } else RET(0); }
static u64 make_bitmap(Cpu *c, int bw, int bh, int bottom_up, u64 *bits_out) {
  Win32 *w = W; if (bw <= 0) bw = 1; if (bh <= 0) bh = 1; if ((u64)bw * bh > 64ull << 20) { w32_report(w, "bitmap %dx%d too large", bw, bh); return 0; }
  u64 sz = (u64)bw * bh * 4; u64 addr = w->fb_cur; if (!mem_map(w->mem, addr, sz, "dib")) return 0; w->fb_cur += (sz + 0xffff) & ~0xffffull;
  GdiObj *bm = gdi_new(w, K_BITMAP); if (!bm) return 0; bm->bw = bw; bm->bh = bh; bm->bits = addr; bm->font_bold = bottom_up; if (bits_out) *bits_out = addr; return bm->handle;
}
/* ---- DIB sections below 32 bpp: the guest sees (and may write) the bits in their native format at dib_native; GDI draws into a 32-bpp shadow at `bits`.
   dib_sync() reconciles both before every use: guest writes to the native bits are expanded into the shadow, GDI drawing into the shadow is packed back. ---- */
static u32 dib_nearest(GdiObj *bm, u32 v) { int best = 0; u32 bd = 0xffffffffu; v &= 0xffffff; for (int i = 0; i < bm->dib_ncol; i++) { u32 p = bm->dib_pal[i]; if (p == v) return (u32)i; int dr = (int)((p >> 16) & 255) - (int)((v >> 16) & 255), dg = (int)((p >> 8) & 255) - (int)((v >> 8) & 255), db = (int)(p & 255) - (int)(v & 255); u32 d = (u32)(dr * dr + dg * dg + db * db); if (d < bd) { bd = d; best = i; } } return (u32)best; }
static u32 dib_expand_px(GdiObj *bm, const u8 *row, int x) { switch (bm->dib_bpp) { case 1: return bm->dib_pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; case 4: return bm->dib_pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; case 8: return bm->dib_pal[row[x]];
  case 16: { u32 s = row[x * 2] | (row[x * 2 + 1] << 8); u32 r = (s >> 10) & 31, g = (s >> 5) & 31, b = s & 31; return (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | (b * 255 / 31); } case 24: return row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16; default: return ((const u32 *)row)[x]; } }
static void dib_pack_px(GdiObj *bm, u8 *row, int x, u32 v) { switch (bm->dib_bpp) { case 1: { u32 i = dib_nearest(bm, v); u8 m = (u8)(0x80 >> (x & 7)); if (i) row[x >> 3] |= m; else row[x >> 3] &= (u8)~m; break; } case 4: { u32 i = dib_nearest(bm, v); if (x & 1) row[x >> 1] = (u8)((row[x >> 1] & 0xf0) | i); else row[x >> 1] = (u8)((row[x >> 1] & 0x0f) | (i << 4)); break; } case 8: row[x] = (u8)dib_nearest(bm, v); break;
  case 16: { u32 s = ((v >> 19) & 31) << 10 | ((v >> 11) & 31) << 5 | ((v >> 3) & 31); row[x * 2] = (u8)s; row[x * 2 + 1] = (u8)(s >> 8); break; } case 24: row[x * 3] = (u8)v; row[x * 3 + 1] = (u8)(v >> 8); row[x * 3 + 2] = (u8)(v >> 16); break; default: ((u32 *)row)[x] = v; } }
static void dib_sync(Win32 *w, GdiObj *bm) {
  if (!bm->dib_section || bm->dib_bpp == 32) return; size_t nb = (size_t)bm->dib_stride * bm->bh, npx = (size_t)bm->bw * bm->bh;
  u8 *nat = mem_ptr(w->mem, bm->dib_native, nb, 1); u32 *sh = (u32 *)mem_ptr(w->mem, bm->bits, npx * 4, 1); if (!nat || !sh) return;
  if (!bm->dib_native_prev) { bm->dib_native_prev = emu_alloc(nb); bm->dib_shadow_prev = emu_alloc(npx * 4); if (!bm->dib_native_prev || !bm->dib_shadow_prev) return; e_memcpy(bm->dib_native_prev, nat, nb); e_memset(bm->dib_native_prev, nat[0] ^ 1, 1); /* force the first expand */ }
  if (e_memcmp(nat, bm->dib_native_prev, nb) != 0) { /* guest wrote the native bits (or first use): expand everything */
    for (int y = 0; y < bm->bh; y++) { const u8 *row = nat + (size_t)y * bm->dib_stride; u32 *d = sh + (size_t)y * bm->bw; for (int x = 0; x < bm->bw; x++) d[x] = dib_expand_px(bm, row, x); }
  } else if (e_memcmp(sh, bm->dib_shadow_prev, npx * 4) != 0) { /* GDI drew into the shadow: pack the changed pixels */
    for (int y = 0; y < bm->bh; y++) { u8 *row = nat + (size_t)y * bm->dib_stride; const u32 *s2 = sh + (size_t)y * bm->bw, *pv = bm->dib_shadow_prev + (size_t)y * bm->bw; for (int x = 0; x < bm->bw; x++) if (s2[x] != pv[x]) dib_pack_px(bm, row, x, s2[x]); }
  } else return;
  e_memcpy(bm->dib_native_prev, nat, nb); e_memcpy(bm->dib_shadow_prev, sh, npx * 4);
}
static void dib_free(GdiObj *bm) { if (bm->dib_pal) emu_free(bm->dib_pal); if (bm->dib_native_prev) emu_free(bm->dib_native_prev); if (bm->dib_shadow_prev) emu_free(bm->dib_shadow_prev); bm->dib_pal = 0; bm->dib_native_prev = 0; bm->dib_shadow_prev = 0; bm->dib_section = 0; bm->dib_bpp = 0; }
static void s_CreateDIBSection(Cpu *c) { Win32 *w = W; u64 bmi = ARG(1); u32 hs = rd32(c, bmi); s32 bw = (s32)rd32(c, bmi + 4), bh = (s32)rd32(c, bmi + 8); u16 bpp = rd16(c, bmi + 14); u32 comp = rd32(c, bmi + 16); u32 clr = rd32(c, bmi + 32); u32 usage = (u32)ARG(2);
  if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) || (comp != 0 && comp != 3)) { w32_report(w, "CreateDIBSection: %u bpp (compression %u) treated as 32 bpp — colours will be wrong", bpp, comp); bpp = 32; }
  int bottom_up = bh > 0; if (bh < 0) bh = -bh; u64 bits = 0; u64 h = make_bitmap(c, bw, bh, bottom_up, &bits); if (!h) { RET(0); return; } GdiObj *bm = gdi_get(w, h); bm->dib_section = 1; bm->dib_bpp = bpp; bm->dib_stride = (int)((((u64)(bw > 0 ? bw : 1) * bpp + 31) / 32) * 4); bm->dib_native = bits;
  if (bpp != 32) { u64 nb = (u64)bm->dib_stride * bm->bh; u64 addr = w->fb_cur; if (!mem_map(w->mem, addr, nb, "dib8")) { RET(0); return; } w->fb_cur += (nb + 0xffff) & ~0xffffull; bm->dib_native = addr;
    if (bpp <= 8) { int n = 1 << bpp; if (clr && (int)clr < n) n = (int)clr; bm->dib_pal = emu_alloc(256 * 4); bm->dib_ncol = n; e_memset(bm->dib_pal, 0, 1024);
      for (int i = 0; i < n; i++) { if (usage == 1) { u32 idx = rd16(c, bmi + hs + i * 2); GdiObj *pl = gdi_get(w, stock[15]); u8 *p = pl ? mem_ptr(w->mem, pl->bits + idx * 4, 4, 0) : 0; bm->dib_pal[i] = p ? (u32)p[0] << 16 | (u32)p[1] << 8 | p[2] : 0; } else bm->dib_pal[i] = rd32(c, bmi + hs + i * 4) & 0xffffff; }
      if (bpp == 1 && !clr && n == 2 && !bm->dib_pal[0] && !bm->dib_pal[1]) bm->dib_pal[1] = 0xffffff; } }
  if (ARG(3)) wrp(c, ARG(3), bm->dib_native); RET(h); }
static void s_CreateCompatibleBitmap(Cpu *c) { RET(make_bitmap(c, (int)ARG(1), (int)ARG(2), 0, 0)); }
static void s_CreateBitmap(Cpu *c) { u64 bits; u64 h = make_bitmap(c, (int)ARG(0), (int)ARG(1), 0, &bits); if (ARG(4) && h) { u32 bpp = (u32)ARG(3) * (u32)ARG(2); if (bpp == 32) { u64 n = (u64)(int)ARG(0) * (int)ARG(1) * 4; u8 *s = mem_ptr(c->mem, ARG(4), n, 0), *d = mem_ptr(c->mem, bits, n, 1); if (s && d) e_memcpy(d, s, (size_t)n); } } RET(h); }
static void s_DeleteObject(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (!o) { RET(0); return; } for (int i = 0; i < 20; i++) if (stock[i] == ARG(0)) { RET(1); return; } if (o->kind == K_BITMAP) dib_free(o); o->kind = 0; RET(1); }
static void s_SelectObject(Cpu *c) { Win32 *w = W; GdiObj *dc = gdi_get(w, ARG(0)), *o = gdi_get(w, ARG(1)); if (!dc || dc->kind != K_DC || !o) { RET(0); return; } u64 prev = 0;
  switch (o->kind) { case K_BITMAP: prev = dc->bitmap ? dc->bitmap : stock[19]; dc->bitmap = ARG(1); break; case K_BRUSH: prev = dc->brush; dc->brush = ARG(1); break; case K_PEN: prev = dc->pen; dc->pen = ARG(1); break; case K_FONT: prev = dc->font; dc->font = ARG(1); break; case K_RGN: prev = 1; break; default: prev = 0; }
  RET(prev); }
/* palettes: a 32-bit display never needs them, but Delphi/VCL and old games create and select one; the entries are kept
   so GetPaletteEntries / GetNearestPaletteIndex answer consistently */
static void s_CreatePalette(Cpu *c) { u64 lp = ARG(0); u32 n = rd16(c, lp + 2); if (n > 256) n = 256; GdiObj *o = gdi_new(W, K_PALETTE); if (!o) { RET(0); return; } o->font_h = (int)n; o->bits = w32_heap_alloc(W, 1024, 0); for (u32 i = 0; i < n; i++) wr32(c, o->bits + i * 4, rd32(c, lp + 4 + i * 4)); RET(o->handle); }
static void s_GetPaletteEntries(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (!o || o->kind != K_PALETTE) { RET(0); return; } u32 st = (u32)ARG(1), n = (u32)ARG(2), tot = (u32)o->font_h; if (!ARG(3)) { RET(tot); return; } u32 k = 0; for (; k < n && st + k < tot; k++) wr32(c, ARG(3) + k * 4, rd32(c, o->bits + (st + k) * 4)); RET(k); }
static void s_SetPaletteEntries(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (!o || o->kind != K_PALETTE) { RET(0); return; } u32 st = (u32)ARG(1), n = (u32)ARG(2), tot = (u32)o->font_h; u32 k = 0; for (; k < n && st + k < tot; k++) wr32(c, o->bits + (st + k) * 4, rd32(c, ARG(3) + k * 4)); RET(k); }
static void s_SelectPalette(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } RET(stock[15]); }   /* DEFAULT_PALETTE comes back as the previous one */
static void s_RealizePalette(Cpu *c) { RET(0); }
static void s_GetSystemPaletteEntries(Cpu *c) { u32 st = (u32)ARG(1), n = (u32)ARG(2); if (!ARG(3)) { RET(256); return; } u32 k = 0; for (; k < n && st + k < 256; k++) { u32 i = st + k; u32 r = (i & 7) * 36, g = ((i >> 3) & 7) * 36, b = ((i >> 6) & 3) * 85; wr32(c, ARG(3) + k * 4, r | (g << 8) | (b << 16)); } RET(k); }
static void s_GetNearestPaletteIndex(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); u32 col = (u32)ARG(1); if (!o || o->kind != K_PALETTE) { RET(0); return; } u32 best = 0, bd = 0xffffffffu; for (u32 i = 0; i < (u32)o->font_h; i++) { u32 e = rd32(c, o->bits + i * 4); int dr = (int)(e & 255) - (int)(col & 255), dg = (int)((e >> 8) & 255) - (int)((col >> 8) & 255), db = (int)((e >> 16) & 255) - (int)((col >> 16) & 255); u32 d = (u32)(dr * dr + dg * dg + db * db); if (d < bd) { bd = d; best = i; } } RET(best); }
static void s_ResizePalette(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (!o || o->kind != K_PALETTE) { RET(0); return; } u32 n = (u32)ARG(1); if (n > 256) n = 256; o->font_h = (int)n; RET(1); }
static void s_GetSystemPaletteUse(Cpu *c) { RET(1); }   /* SYSPAL_STATIC */
static void s_SetSystemPaletteUse(Cpu *c) { RET(1); }
static void s_UpdateColors(Cpu *c) { RET(1); }
static void s_AnimatePalette(Cpu *c) { RET(1); }
static void s_UnrealizeObject(Cpu *c) { RET(1); }
static void s_GetStockObject(Cpu *c) { u32 i = (u32)ARG(0); RET(i < 20 ? stock[i] : 0); }
static void s_GetCurrentObject(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } u32 t = (u32)ARG(1); RET(t == 1 ? dc->pen : t == 2 ? dc->brush : t == 6 ? dc->font : t == 7 ? dc->bitmap : 0); }
static void s_CreateSolidBrush(Cpu *c) { GdiObj *b = gdi_new(W, K_BRUSH); if (!b) { RET(0); return; } b->color = cr2px((u32)ARG(0)); RET(b->handle); }
static void s_CreateHatchBrush(Cpu *c) { GdiObj *b = gdi_new(W, K_BRUSH); if (!b) { RET(0); return; } b->color = cr2px((u32)ARG(1)); RET(b->handle); }
static void s_CreatePatternBrush(Cpu *c) { GdiObj *b = gdi_new(W, K_BRUSH); if (!b) { RET(0); return; } b->color = 0x808080; RET(b->handle); }
static void s_CreateBrushIndirect(Cpu *c) { GdiObj *b = gdi_new(W, K_BRUSH); if (!b) { RET(0); return; } b->color = cr2px(rd32(c, ARG(0) + 4)); if (rd32(c, ARG(0)) == 1) b->pen_width = -1; RET(b->handle); }
static void s_CreatePen(Cpu *c) { GdiObj *p = gdi_new(W, K_PEN); if (!p) { RET(0); return; } p->color = cr2px((u32)ARG(2)); p->pen_width = (u32)ARG(0) == 5 ? -1 : ((int)ARG(1) > 0 ? (int)ARG(1) : 1); RET(p->handle); }
static void s_ExtCreatePen(Cpu *c) { GdiObj *p = gdi_new(W, K_PEN); if (!p) { RET(0); return; } p->color = cr2px(rd32(c, ARG(2) + 4)); p->pen_width = (int)ARG(1) > 0 ? (int)ARG(1) : 1; RET(p->handle); }
static void s_CreateFontA(Cpu *c) { GdiObj *f = gdi_new(W, K_FONT); if (!f) { RET(0); return; } int h = (int)(s32)ARG(0); int em = 0; if (h < 0) { em = -h; h = (em * 6 + 2) / 5; } /* negative lfHeight = em height without internal leading: the cell (line) height is ~1.2x, like tmHeight of an 8 pt Tahoma = 13 */ if (!h) h = 16; f->font_h = h; f->font_em = em; f->font_bold = (int)ARG(4) >= 600; RET(f->handle); }
static void s_CreateFontIndirectA(Cpu *c) { GdiObj *f = gdi_new(W, K_FONT); if (!f) { RET(0); return; } int h = (s32)rd32(c, ARG(0)); int em = 0; if (h < 0) { em = -h; h = (em * 6 + 2) / 5; } /* negative lfHeight = em height without internal leading: the cell (line) height is ~1.2x, like tmHeight of an 8 pt Tahoma = 13 */ if (!h) h = 16; f->font_h = h; f->font_em = em; f->font_bold = (s32)rd32(c, ARG(0) + 16) >= 600; RET(f->handle); }
static void s_SetTextColor(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0xffffffffu); return; } u32 old = dc->text_color; dc->text_color = (u32)ARG(1); RET(old); }
static void s_SetBkColor(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0xffffffffu); return; } u32 old = dc->bk_color; dc->bk_color = (u32)ARG(1); RET(old); }
static void s_SetBkMode(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } int old = dc->bk_mode; dc->bk_mode = (int)ARG(1); RET(old); }
static void s_GetTextColor(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); RET(dc ? dc->text_color : 0); }
static void s_GetBkColor(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); RET(dc ? dc->bk_color : 0xffffff); }
static void s_SetTextAlign(Cpu *c) { RET(0); }
static void s_SetROP2(Cpu *c) { RET(13); }
static void s_SetStretchBltMode(Cpu *c) { RET(1); }
static void s_SetBrushOrgEx(Cpu *c) { RET(1); }
static void s_GdiFlush(Cpu *c) { RET(1); }
/* DC state stack: SaveDC pushes origin/clip/selected objects/colours; RestoreDC(-1) pops one level, RestoreDC(n) restores level n */
typedef struct { u64 hdc; int level; int lox, loy, clip_on, cl, ct, cr, cb; u64 font, brush, pen, bitmap; u32 text_color, bk_color; int bk_mode; } DcSave;
#define DCSAVE_MAX 512
static DcSave g_dcsave[DCSAVE_MAX];
static void s_SaveDC(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc || dc->kind != K_DC) { RET(0); return; } int k; for (k = 0; k < DCSAVE_MAX && g_dcsave[k].hdc; k++) ; if (k == DCSAVE_MAX) { RET(0); return; }
  DcSave *d = &g_dcsave[k]; d->hdc = ARG(0); d->level = ++dc->save_depth; d->lox = dc->lox; d->loy = dc->loy; d->clip_on = dc->clip_on; d->cl = dc->cl; d->ct = dc->ct; d->cr = dc->cr; d->cb = dc->cb; d->font = dc->font; d->brush = dc->brush; d->pen = dc->pen; d->bitmap = dc->bitmap; d->text_color = dc->text_color; d->bk_color = dc->bk_color; d->bk_mode = dc->bk_mode; RET(d->level); }
static void s_RestoreDC(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc || dc->kind != K_DC || !dc->save_depth) { RET(0); return; } int lv = (int)(s32)ARG(1); if (lv < 0) lv = dc->save_depth + 1 + lv; if (lv <= 0 || lv > dc->save_depth) { RET(0); return; }
  DcSave *d = 0; for (int k = 0; k < DCSAVE_MAX; k++) if (g_dcsave[k].hdc == ARG(0)) { if (g_dcsave[k].level == lv) d = &g_dcsave[k]; if (g_dcsave[k].level > lv) g_dcsave[k].hdc = 0; }
  if (!d) { RET(0); return; } dc->lox = d->lox; dc->loy = d->loy; dc->clip_on = d->clip_on; dc->cl = d->cl; dc->ct = d->ct; dc->cr = d->cr; dc->cb = d->cb; dc->font = d->font; dc->brush = d->brush; dc->pen = d->pen; if (d->bitmap) dc->bitmap = d->bitmap; dc->text_color = d->text_color; dc->bk_color = d->bk_color; dc->bk_mode = d->bk_mode; d->hdc = 0; dc->save_depth = lv - 1; RET(1); }
static void dcsave_drop(u64 hdc) { for (int k = 0; k < DCSAVE_MAX; k++) if (g_dcsave[k].hdc == hdc) g_dcsave[k].hdc = 0; }
/* window / viewport origins: device = logical − window_org + viewport_org; we keep only the difference (lox, loy) and report window org as −(lox, loy) (viewport org reads 0) */
static void s_SetWindowOrgEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(3)) { wr32(c, ARG(3), (u32)-dc->lox); wr32(c, ARG(3) + 4, (u32)-dc->loy); } dc->lox = -(int)(s32)ARG(1); dc->loy = -(int)(s32)ARG(2); RET(1); }
static void s_GetWindowOrgEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(1)) { wr32(c, ARG(1), (u32)-dc->lox); wr32(c, ARG(1) + 4, (u32)-dc->loy); } RET(1); }
static void s_OffsetWindowOrgEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(3)) { wr32(c, ARG(3), (u32)-dc->lox); wr32(c, ARG(3) + 4, (u32)-dc->loy); } dc->lox -= (int)(s32)ARG(1); dc->loy -= (int)(s32)ARG(2); RET(1); }
static void s_SetViewportOrgEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(3)) { wr32(c, ARG(3), 0); wr32(c, ARG(3) + 4, 0); } dc->lox = (int)(s32)ARG(1); dc->loy = (int)(s32)ARG(2); RET(1); }
static void s_GetViewportOrgEx(Cpu *c) { if (ARG(1)) { wr32(c, ARG(1), 0); wr32(c, ARG(1) + 4, 0); } RET(1); }
static void s_OffsetViewportOrgEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(3)) { wr32(c, ARG(3), 0); wr32(c, ARG(3) + 4, 0); } dc->lox += (int)(s32)ARG(1); dc->loy += (int)(s32)ARG(2); RET(1); }
static void s_DPtoLP(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } for (u32 i = 0; i < (u32)ARG(2); i++) { u64 p = ARG(1) + i * 8; wr32(c, p, (u32)((s32)rd32(c, p) - dc->lox)); wr32(c, p + 4, (u32)((s32)rd32(c, p + 4) - dc->loy)); } RET(1); }
static void s_LPtoDP(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } for (u32 i = 0; i < (u32)ARG(2); i++) { u64 p = ARG(1) + i * 8; wr32(c, p, (u32)((s32)rd32(c, p) + dc->lox)); wr32(c, p + 4, (u32)((s32)rd32(c, p + 4) + dc->loy)); } RET(1); }
/* clipping: a single rectangle in device coordinates (regions are reduced to their bounding box) */
static int dc_clip_set(Cpu *c, GdiObj *dc, int l, int t, int r, int b, int intersect) { if (intersect && dc->clip_on) { if (dc->cl > l) l = dc->cl; if (dc->ct > t) t = dc->ct; if (dc->cr < r) r = dc->cr; if (dc->cb < b) b = dc->cb; } dc->clip_on = 1; dc->cl = l; dc->ct = t; dc->cr = r; dc->cb = b; (void)c; return (r <= l || b <= t) ? 1 /* NULLREGION */ : 2 /* SIMPLEREGION */; }
static void s_IntersectClipRect(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } int l = (s32)ARG(1) + dc->lox, t = (s32)ARG(2) + dc->loy, r = (s32)ARG(3) + dc->lox, b = (s32)ARG(4) + dc->loy; RET(dc_clip_set(c, dc, l, t, r, b, 1)); }
static void s_ExcludeClipRect(Cpu *c) { RET(2); }   /* excluding a hole from a rectangle cannot be expressed with one rectangle: left as is (over-draw) */
static void s_SelectClipRgn(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } GdiObj *rg = ARG(1) ? gdi_get(W, ARG(1)) : 0; if (!rg || rg->kind != K_RGN) { dc->clip_on = 0; RET(2); return; } RET(dc_clip_set(c, dc, rg->rx, rg->ry, rg->rx + rg->rw, rg->ry + rg->rh, 0)); }
static void s_ExtSelectClipRgn(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } GdiObj *rg = ARG(1) ? gdi_get(W, ARG(1)) : 0; u32 mode = (u32)ARG(2); if (!rg || rg->kind != K_RGN) { dc->clip_on = 0; RET(2); return; } if (mode == 1 /* RGN_AND */) RET(dc_clip_set(c, dc, rg->rx, rg->ry, rg->rx + rg->rw, rg->ry + rg->rh, 1)); else if (mode == 5 /* RGN_COPY */) RET(dc_clip_set(c, dc, rg->rx, rg->ry, rg->rx + rg->rw, rg->ry + rg->rh, 0)); else RET(2); }
static void s_GetClipBox(Cpu *c) { Surf s; GdiObj *dc = gdi_get(W, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } if (ARG(1)) { wr32(c, ARG(1), (u32)s.x0); wr32(c, ARG(1) + 4, (u32)s.y0); wr32(c, ARG(1) + 8, (u32)s.w); wr32(c, ARG(1) + 12, (u32)s.h); } RET(s.w > s.x0 && s.h > s.y0 ? 2 : 1); }
static void s_GetClipRgn(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(-1); return; } if (!dc->clip_on) { RET(0); return; } GdiObj *rg = ARG(1) ? gdi_get(W, ARG(1)) : 0; if (rg && rg->kind == K_RGN) { rg->rx = dc->cl; rg->ry = dc->ct; rg->rw = dc->cr - dc->cl; rg->rh = dc->cb - dc->ct; } RET(1); }
static void s_RectVisible(Cpu *c) { Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } u64 r = ARG(1); int l = (s32)rd32(c, r), t = (s32)rd32(c, r + 4), rr = (s32)rd32(c, r + 8), b = (s32)rd32(c, r + 12); RET(rr > s.x0 && b > s.y0 && l < s.w && t < s.h); }
static void s_PtVisible(Cpu *c) { Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } int x = (s32)ARG(1), y = (s32)ARG(2); RET(x >= s.x0 && y >= s.y0 && x < s.w && y < s.h); }

static void s_SetMapMode(Cpu *c) { RET(1); }
static void s_GetDeviceCaps(Cpu *c) { int i = (int)ARG(1); RET(i == 8 ? W->screen_w : i == 10 ? W->screen_h : i == 12 ? 32 : i == 14 ? 1 : i == 88 ? 96 : i == 90 ? 96 : i == 116 ? 60 : i == 4 ? 1 : i == 24 ? 0x4000000 : i == 2 ? 0x4000 : 0); }
static void s_GetObjectA(Cpu *c) { GdiObj *o = gdi_get(W, ARG(0)); if (!o) { RET(0); return; } u64 a = ARG(2);
  if (o->kind == K_BITMAP) { int bsz = c->mode32 ? 24 : 32, dsz = c->mode32 ? 84 : 104; int bpp = o->dib_section ? o->dib_bpp : 32; u32 stride = o->dib_section ? (u32)o->dib_stride : (u32)o->bw * 4; u64 bits = o->dib_section ? o->dib_native : o->bits; u32 cb = (u32)ARG(1);
    if (!a) { RET(o->dib_section ? dsz : bsz); return; } if (cb < (u32)bsz) { RET(0); return; } wr32(c, a, 0); wr32(c, a + 4, (u32)o->bw); wr32(c, a + 8, (u32)o->bh); wr32(c, a + 12, stride); wr16(c, a + 16, 1); wr16(c, a + 18, (u16)bpp); if (c->mode32) wr32(c, a + 20, (u32)bits); else wr64(c, a + 24, bits);
    if (!o->dib_section || cb < (u32)dsz) { RET(bsz); return; } u64 ih = a + bsz; wr32(c, ih, 40); wr32(c, ih + 4, (u32)o->bw); wr32(c, ih + 8, (u32)(o->font_bold ? o->bh : -o->bh)); wr16(c, ih + 12, 1); wr16(c, ih + 14, (u16)bpp); wr32(c, ih + 16, 0); wr32(c, ih + 20, stride * (u32)o->bh); wr32(c, ih + 24, 3780); wr32(c, ih + 28, 3780); wr32(c, ih + 32, (u32)(bpp <= 8 ? o->dib_ncol : 0)); wr32(c, ih + 36, 0);
    u64 bf = ih + 40; wr32(c, bf, 0xff0000); wr32(c, bf + 4, 0xff00); wr32(c, bf + 8, 0xff); if (c->mode32) { wr32(c, bf + 12, 0); wr32(c, bf + 16, 0); } else { wr64(c, bf + 16, 0); wr32(c, bf + 24, 0); } RET(dsz); return; }
  if (o->kind == K_BRUSH) { if (a) { wr32(c, a, 0); wr32(c, a + 4, px2cr(o->color)); wrp(c, a + 8, 0); } RET(c->mode32 ? 12 : 16); return; }
  if (o->kind == K_PEN) { if (a) { wr32(c, a, 0); wr32(c, a + 4, (u32)o->pen_width); wr32(c, a + 12, px2cr(o->color)); } RET(16); return; }
  if (o->kind == K_FONT) { if (a) { u8 *p = mem_ptr(c->mem, a, 92, 1); if (p) e_memset(p, 0, 92); wr32(c, a, (u32)-(o->font_em ? o->font_em : o->font_h)); wr32(c, a + 16, o->font_bold ? 700 : 400); } RET(92); return; }
  RET(0); }
static void s_GetTextExtentPoint32A(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); GdiObj *f = dc ? gdi_get(W, dc->font) : 0; int fh = f ? f->font_h : 16; int cw = font_cw(f, fh); wr32(c, ARG(3), (u32)(cw * (int)ARG(2))); wr32(c, ARG(3) + 4, (u32)fh); RET(1); }
static void s_GetTextMetricsA(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); GdiObj *f = dc ? gdi_get(W, dc->font) : 0; int fh = f ? f->font_h : 16; int cw = font_cw(f, fh); u64 a = ARG(1); u8 *p = mem_ptr(c->mem, a, 56, 1); if (p) e_memset(p, 0, 56); wr32(c, a, (u32)fh); wr32(c, a + 4, (u32)(fh - 3)); wr32(c, a + 8, 3); wr32(c, a + 20, (u32)cw); wr32(c, a + 24, (u32)cw); wr32(c, a + 28, f && f->font_bold ? 700 : 400); RET(1); }
static void text_out(Cpu *c, u64 hdc, int x, int y, u64 str, int len, int wide) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, hdc); if (!dc || !dc_surf(c, hdc, &s)) { RET(0); return; } char buf[4100]; int n = 0;
  if (len < 0) len = 0; if (len > 1000) len = 1000;
  for (int i = 0; i < len && n < 1000; i++) { u32 ch = wide ? rd16(c, str + i * 2) : rd8(c, str + i); if (c->status) return; if (wide && ch >= 0xD800 && ch < 0xDC00 && i + 1 < len) { u32 lo = rd16(c, str + (i + 1) * 2); if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); i++; } }
    if (ch < 0x80) buf[n++] = (char)ch; else if (ch < 0x800) { buf[n++] = (char)(0xC0 | ch >> 6); buf[n++] = (char)(0x80 | (ch & 0x3f)); } else if (ch < 0x10000) { buf[n++] = (char)(0xE0 | ch >> 12); buf[n++] = (char)(0x80 | ((ch >> 6) & 0x3f)); buf[n++] = (char)(0x80 | (ch & 0x3f)); } else { buf[n++] = (char)(0xF0 | ch >> 18); buf[n++] = (char)(0x80 | ((ch >> 12) & 0x3f)); buf[n++] = (char)(0x80 | ((ch >> 6) & 0x3f)); buf[n++] = (char)(0x80 | (ch & 0x3f)); } }   /* ANSI bytes ≥ 0x80 are taken as Latin-1 */
  draw_text(&s, dc, w, x, y, buf, n); mark_screen(w, hdc); RET(1); }
static void s_TextOutA(Cpu *c) { text_out(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), ARG(3), (int)(s32)ARG(4), 0); }
static void s_TextOutW(Cpu *c) { text_out(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), ARG(3), (int)(s32)ARG(4), 1); }
static void s_ExtTextOutA(Cpu *c) { u64 rc = ARG(4); if ((ARG(3) & 2) && rc) { Surf s; GdiObj *dc = gdi_get(W, ARG(0)); if (dc && dc_surf(c, ARG(0), &s)) fill_rect_px(&s, (s32)rd32(c, rc), (s32)rd32(c, rc + 4), (s32)rd32(c, rc + 8), (s32)rd32(c, rc + 12), cr2px(dc->bk_color)); } text_out(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), ARG(5), (int)(s32)ARG(6), 0); }
static void s_ExtTextOutW(Cpu *c) { u64 rc = ARG(4); if ((ARG(3) & 2) && rc) { Surf s; GdiObj *dc = gdi_get(W, ARG(0)); if (dc && dc_surf(c, ARG(0), &s)) fill_rect_px(&s, (s32)rd32(c, rc), (s32)rd32(c, rc + 4), (s32)rd32(c, rc + 8), (s32)rd32(c, rc + 12), cr2px(dc->bk_color)); } text_out(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), ARG(5), (int)(s32)ARG(6), 1); }

static u32 rop_apply(u32 rop, u32 d, u32 s, u32 p) {
  switch (rop) { case 0x00CC0020: return s; case 0x00EE0086: return d | s; case 0x008800C6: return d & s; case 0x00660046: return d ^ s; case 0x00330008: return ~s & 0xffffff; case 0x00550009: return ~d & 0xffffff; case 0x00000042: return 0; case 0x00FF0062: return 0xffffff; case 0x00F00021: return p; case 0x005A0049: return d ^ p; case 0x00A000C9: return d & p; case 0x00FA0089: return d | p; case 0x00440328: return s & ~d; default: return s; }
}
static void blit(Cpu *c, u64 hdst, int dx, int dy, int dw, int dh, u64 hsrc, int sx, int sy, int sw, int sh, u32 rop) {
  Win32 *w = W; Surf d, s; if (!dc_surf(c, hdst, &d)) { RET(0); return; }
  int has_src = hsrc && dc_surf_raw(c, hsrc, &s); GdiObj *ddc = gdi_get(w, hdst); int nb; u32 pat = ddc ? dc_brush_px(w, ddc, &nb) : 0;
  if (rop != 0x00CC0020 && rop != 0x00000042 && rop != 0x00FF0062 && rop != 0x00F00021 && rop != 0x00EE0086 && rop != 0x008800C6 && rop != 0x00660046 && rop != 0x00330008 && rop != 0x00550009) { static int warned; if (!warned++) w32_report(w, "BitBlt: raster op 0x%08x approximated as SRCCOPY", rop); }
  int flipx = 0, flipy = 0; if (dw < 0) { dw = -dw; dx -= dw; flipx ^= 1; } if (dh < 0) { dh = -dh; dy -= dh; flipy ^= 1; } if (sw < 0) { sw = -sw; sx -= sw; flipx ^= 1; } if (sh < 0) { sh = -sh; sy -= sh; flipy ^= 1; }
  if (dw <= 0 || dh <= 0) { RET(1); return; }
  for (int y = 0; y < dh; y++) { int yy = dy + y; if (yy < d.y0 || yy >= d.h) continue; u32 *drow = srow(&d, yy); int syy = sy + (flipy ? dh - 1 - y : y) * sh / dh; u32 *srw = 0; if (has_src && syy >= 0 && syy < s.h) srw = srow(&s, syy);
    if (dw == sw && !flipx && srw && rop == 0x00CC0020) { int x0 = dx < d.x0 ? d.x0 - dx : 0, x1 = dw; if (dx + x1 > d.w) x1 = d.w - dx; int sx0 = sx + x0; if (sx0 < 0) { x0 += -sx0; sx0 = 0; } int cnt = x1 - x0; if (sx0 + cnt > s.w) cnt = s.w - sx0; if (cnt > 0) e_memcpy(drow + dx + x0, srw + sx0, (size_t)cnt * 4); continue; }
    for (int x = 0; x < dw; x++) { int xx = dx + x; if (xx < d.x0 || xx >= d.w) continue; u32 sv = 0; if (srw) { int sxx = sx + (flipx ? dw - 1 - x : x) * sw / dw; if (sxx >= 0 && sxx < s.w) sv = srw[sxx]; } drow[xx] = rop_apply(rop, drow[xx], sv, pat); } }
  mark_screen(w, hdst); RET(1);
}
/* msimg32: AlphaBlend (per-pixel alpha when AC_SRC_ALPHA, premultiplied as GDI specifies, times SourceConstantAlpha), TransparentBlt (colour key), GradientFill (H/V rects, triangles as flat fill) */
static void s_AlphaBlend(Cpu *c) { Win32 *w = W; Surf d, s; if (!dc_surf(c, ARG(0), &d) || !dc_surf_raw(c, ARG(5), &s)) { RET(0); return; }
  int dx = (int)(s32)ARG(1), dy = (int)(s32)ARG(2), dw = (int)(s32)ARG(3), dh = (int)(s32)ARG(4), sx = (int)(s32)ARG(6), sy = (int)(s32)ARG(7), sw = (int)(s32)ARG(8), sh = (int)(s32)ARG(9);
  u32 bf = (u32)ARG(10); int ca = (bf >> 16) & 0xff, per_pixel = (bf >> 24) & 1; if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) { RET(1); return; }
  for (int y = 0; y < dh; y++) { int yy = dy + y; if (yy < d.y0 || yy >= d.h) continue; int syy = sy + y * sh / dh; if (syy < 0 || syy >= s.h) continue; u32 *drow = srow(&d, yy), *srw = srow(&s, syy);
    for (int x = 0; x < dw; x++) { int xx = dx + x; if (xx < d.x0 || xx >= d.w) continue; int sxx = sx + x * sw / dw; if (sxx < 0 || sxx >= s.w) continue; u32 sv = srw[sxx], dv = drow[xx];
      int sa = per_pixel ? (int)(sv >> 24) : 255; /* premultiplied source: out = src*ca/255 + dst*(255 - sa*ca/255)/255 */
      int sr = (sv >> 16) & 0xff, sg = (sv >> 8) & 0xff, sb = sv & 0xff; if (!per_pixel) { sr = sr * ca / 255; sg = sg * ca / 255; sb = sb * ca / 255; } else { sr = sr * ca / 255; sg = sg * ca / 255; sb = sb * ca / 255; }
      int da = 255 - sa * ca / 255; int r = sr + ((dv >> 16) & 0xff) * da / 255, g = sg + ((dv >> 8) & 0xff) * da / 255, b = sb + (dv & 0xff) * da / 255;
      if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; drow[xx] = 0xff000000u | (u32)r << 16 | (u32)g << 8 | (u32)b; } }
  mark_screen(w, ARG(0)); RET(1); }
static void s_TransparentBlt(Cpu *c) { Win32 *w = W; Surf d, s; if (!dc_surf(c, ARG(0), &d) || !dc_surf_raw(c, ARG(5), &s)) { RET(0); return; }
  int dx = (int)(s32)ARG(1), dy = (int)(s32)ARG(2), dw = (int)(s32)ARG(3), dh = (int)(s32)ARG(4), sx = (int)(s32)ARG(6), sy = (int)(s32)ARG(7), sw = (int)(s32)ARG(8), sh = (int)(s32)ARG(9);
  u32 key = (u32)ARG(10); u32 keypx = (key & 0xff) << 16 | (key & 0xff00) | (key >> 16 & 0xff); if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) { RET(1); return; }
  for (int y = 0; y < dh; y++) { int yy = dy + y; if (yy < d.y0 || yy >= d.h) continue; int syy = sy + y * sh / dh; if (syy < 0 || syy >= s.h) continue; u32 *drow = srow(&d, yy), *srw = srow(&s, syy);
    for (int x = 0; x < dw; x++) { int xx = dx + x; if (xx < d.x0 || xx >= d.w) continue; int sxx = sx + x * sw / dw; if (sxx < 0 || sxx >= s.w) continue; u32 sv = srw[sxx]; if ((sv & 0xffffff) == keypx) continue; drow[xx] = sv | 0xff000000u; } }
  mark_screen(w, ARG(0)); RET(1); }
static void s_GradientFill(Cpu *c) { /* (hdc, TRIVERTEX *v, nv, void *mesh, nmesh, mode) — TRIVERTEX 16 B: x, y, R, G, B, A (16-bit colour channels) */ Win32 *w = W; Surf d; if (!dc_surf(c, ARG(0), &d)) { RET(0); return; }
  u64 vtx = ARG(1); u32 nv = (u32)ARG(2); u64 mesh = ARG(3); u32 nm = (u32)ARG(4); u32 mode = (u32)ARG(5); if (nm > 4096) nm = 4096;
  int ox = 0, oy = 0;
  #define TV(i, f) rd32(c, vtx + (u64)(i) * 16 + (f))
  #define TVC(i) (((TV(i, 8) >> 8) & 0xff) << 16 | ((TV(i, 12) >> 8) & 0xff) << 8 | ((TV(i, 16) >> 8) & 0xff))
  for (u32 m = 0; m < nm; m++) {
    if (mode == 2) { /* GRADIENT_FILL_TRIANGLE: 3 indices — flat fill with the average colour inside the bounding triangle */ u32 i0 = rd32(c, mesh + m * 12), i1 = rd32(c, mesh + m * 12 + 4), i2 = rd32(c, mesh + m * 12 + 8); if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
      int x0 = (int)TV(i0, 0), y0 = (int)TV(i0, 4), x1 = (int)TV(i1, 0), y1 = (int)TV(i1, 4), x2 = (int)TV(i2, 0), y2 = (int)TV(i2, 4); u32 c0 = TVC(i0), c1 = TVC(i1), c2 = TVC(i2);
      u32 avg = 0xff000000u | ((((c0 >> 16) & 0xff) + ((c1 >> 16) & 0xff) + ((c2 >> 16) & 0xff)) / 3) << 16 | ((((c0 >> 8) & 0xff) + ((c1 >> 8) & 0xff) + ((c2 >> 8) & 0xff)) / 3) << 8 | (((c0 & 0xff) + (c1 & 0xff) + (c2 & 0xff)) / 3);
      int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
      for (int y = miny; y < maxy; y++) { /* scanline: intersect the three edges */ int xs[3], n = 0; int ex[3][4] = { { x0, y0, x1, y1 }, { x1, y1, x2, y2 }, { x2, y2, x0, y0 } };
        for (int e = 0; e < 3; e++) { int ax = ex[e][0], ay = ex[e][1], bx = ex[e][2], by = ex[e][3]; if (ay == by) continue; if ((y >= ay && y < by) || (y >= by && y < ay)) xs[n++] = ax + (int)((long long)(y - ay) * (bx - ax) / (by - ay)); }
        if (n >= 2) { int xa = xs[0], xb = xs[1]; if (n == 3) { if (xs[2] < xa) xa = xs[2]; if (xs[2] > xb) xb = xs[2]; } if (xa > xb) { int t = xa; xa = xb; xb = t; } fill_rect_px(&d, xa + ox, y + oy, xb + ox, y + oy + 1, avg); } }
      continue; }
    u32 iul = rd32(c, mesh + m * 8), ilr = rd32(c, mesh + m * 8 + 4); if (iul >= nv || ilr >= nv) continue;
    int x0 = (int)TV(iul, 0), y0 = (int)TV(iul, 4), x1 = (int)TV(ilr, 0), y1 = (int)TV(ilr, 4); u32 c0 = TVC(iul), c1 = TVC(ilr); if (x1 < x0) { int t = x0; x0 = x1; x1 = t; } if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int len = mode == 1 ? y1 - y0 : x1 - x0; if (len <= 0) continue;
    for (int i = 0; i < len; i++) { u32 px = 0xff000000u; for (int sh = 0; sh <= 16; sh += 8) { int a = (c0 >> sh) & 0xff, b = (c1 >> sh) & 0xff; px |= (u32)(a + (b - a) * i / len) << sh; }
      if (mode == 1) fill_rect_px(&d, x0 + ox, y0 + i + oy, x1 + ox, y0 + i + 1 + oy, px); else fill_rect_px(&d, x0 + i + ox, y0 + oy, x0 + i + 1 + ox, y1 + oy, px); } }
  #undef TV
  #undef TVC
  mark_screen(w, ARG(0)); RET(1); }
static void s_BitBlt(Cpu *c) { int wdt = (int)(s32)ARG(3), hgt = (int)(s32)ARG(4); blit(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), wdt, hgt, ARG(5), (int)(s32)ARG(6), (int)(s32)ARG(7), wdt, hgt, (u32)ARG(8)); }
static void s_StretchBlt(Cpu *c) { blit(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), ARG(5), (int)(s32)ARG(6), (int)(s32)ARG(7), (int)(s32)ARG(8), (int)(s32)ARG(9), (u32)ARG(10)); }
static void s_PatBlt(Cpu *c) { blit(c, ARG(0), (int)(s32)ARG(1), (int)(s32)ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), 0, 0, 0, (int)(s32)ARG(3), (int)(s32)ARG(4), (u32)ARG(5)); }
/* StretchDIBits(hdc, xd, yd, wd, hd, xs, ys, ws, hs, bits, bmi, usage, rop): copy from guest 32-bpp DIB memory */
static void s_StretchDIBits(Cpu *c) { Win32 *w = W; Surf d; if (!dc_surf(c, ARG(0), &d)) { RET(0); return; } u64 bmi = ARG(10); s32 bw = (s32)rd32(c, bmi + 4), bh = (s32)rd32(c, bmi + 8); u16 bpp = rd16(c, bmi + 14); int bottom_up = bh > 0; if (bh < 0) bh = -bh;
  if (bpp != 32 && bpp != 24) { w32_report(w, "StretchDIBits: %u bpp not supported", bpp); RET(0); return; }
  int dx = (s32)ARG(1), dy = (s32)ARG(2), dw = (s32)ARG(3), dh = (s32)ARG(4), sx = (s32)ARG(5), sy = (s32)ARG(6), sw = (s32)ARG(7), sh = (s32)ARG(8); if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) { RET(0); return; }
  int stride = bpp == 32 ? bw * 4 : ((bw * 3 + 3) & ~3); const u8 *bits = mem_ptr(c->mem, ARG(9), (u64)stride * bh, 0); if (!bits) { RET(0); return; }
  for (int y = 0; y < dh; y++) { int yy = dy + y; if (yy < d.y0 || yy >= d.h) continue; int syy = sy + y * sh / dh; if (syy < 0 || syy >= bh) continue; const u8 *row = bits + (size_t)(bottom_up ? bh - 1 - syy : syy) * stride; u32 *drow = srow(&d, yy);
    for (int x = 0; x < dw; x++) { int xx = dx + x; if (xx < d.x0 || xx >= d.w) continue; int sxx = sx + x * sw / dw; if (sxx < 0 || sxx >= bw) continue; drow[xx] = bpp == 32 ? *(const u32 *)(row + sxx * 4) : (u32)row[sxx * 3] | (u32)row[sxx * 3 + 1] << 8 | (u32)row[sxx * 3 + 2] << 16; } }
  mark_screen(w, ARG(0)); RET(dh); }
static void s_SetDIBitsToDevice(Cpu *c) { /* (hdc, xd, yd, w, h, xs, ys, start, lines, bits, bmi, usage) */ u64 a9 = ARG(9), a10 = ARG(10); int wdt = (s32)ARG(3), hgt = (s32)ARG(4);
  /* reuse StretchDIBits by rewriting args: place onto stack is messy — emulate directly */ Win32 *w = W; Surf d; if (!dc_surf(c, ARG(0), &d)) { RET(0); return; } s32 bw = (s32)rd32(c, a10 + 4), bh = (s32)rd32(c, a10 + 8); u16 bpp = rd16(c, a10 + 14); int bottom_up = bh > 0; if (bh < 0) bh = -bh; if (bpp != 32) { w32_report(w, "SetDIBitsToDevice: %u bpp not supported", bpp); RET(0); return; }
  const u32 *bits = (const u32 *)mem_ptr(c->mem, a9, (u64)bw * bh * 4, 0); if (!bits) { RET(0); return; } int dx = (s32)ARG(1), dy = (s32)ARG(2), sx = (s32)ARG(5), sy = (s32)ARG(6);
  for (int y = 0; y < hgt; y++) { int yy = dy + y, syy = sy + y; if (yy < d.y0 || yy >= d.h || syy < 0 || syy >= bh) continue; const u32 *row = bits + (size_t)(bottom_up ? bh - 1 - syy : syy) * bw; u32 *drow = srow(&d, yy); for (int x = 0; x < wdt; x++) { int xx = dx + x, sxx = sx + x; if (xx < d.x0 || xx >= d.w || sxx < 0 || sxx >= bw) continue; drow[xx] = row[sxx]; } }
  mark_screen(w, ARG(0)); RET(hgt); }
static void s_Rectangle(Cpu *c) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } int nb, np; u32 b = dc_brush_px(w, dc, &nb), p = dc_pen_px(w, dc, &np); int l = (s32)ARG(1), t = (s32)ARG(2), r = (s32)ARG(3), bt = (s32)ARG(4);
  if (!nb) fill_rect_px(&s, l, t, r, bt, b); if (!np) { hline(&s, l, r - 1, t, p); hline(&s, l, r - 1, bt - 1, p); line_px(&s, l, t, l, bt - 1, p); line_px(&s, r - 1, t, r - 1, bt - 1, p); } mark_screen(w, ARG(0)); RET(1); }
static void s_RoundRect(Cpu *c) { s_Rectangle(c); }
static void s_Ellipse(Cpu *c) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } int nb, np; u32 b = dc_brush_px(w, dc, &nb), p = dc_pen_px(w, dc, &np); fill_ellipse(&s, (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), b, !nb, p, !np); mark_screen(w, ARG(0)); RET(1); }
static int cur_x, cur_y;
static void s_MoveToEx(Cpu *c) { if (ARG(3)) { wr32(c, ARG(3), (u32)cur_x); wr32(c, ARG(3) + 4, (u32)cur_y); } cur_x = (s32)ARG(1); cur_y = (s32)ARG(2); RET(1); }
static void s_LineTo(Cpu *c) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } int np; u32 p = dc_pen_px(w, dc, &np); int x = (s32)ARG(1), y = (s32)ARG(2); if (!np) line_px(&s, cur_x, cur_y, x, y, p); cur_x = x; cur_y = y; mark_screen(w, ARG(0)); RET(1); }
static void s_Polyline(Cpu *c) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } int np; u32 p = dc_pen_px(w, dc, &np); int n = (int)ARG(2); u64 a = ARG(1); for (int i = 1; i < n && !np; i++) line_px(&s, (s32)rd32(c, a + (i - 1) * 8), (s32)rd32(c, a + (i - 1) * 8 + 4), (s32)rd32(c, a + i * 8), (s32)rd32(c, a + i * 8 + 4), p); mark_screen(w, ARG(0)); RET(1); }
static void s_Polygon(Cpu *c) { Win32 *w = W; Surf s; GdiObj *dc = gdi_get(w, ARG(0)); if (!dc || !dc_surf(c, ARG(0), &s)) { RET(0); return; } int nb, np; u32 b = dc_brush_px(w, dc, &nb), p = dc_pen_px(w, dc, &np); int n = (int)ARG(2); if (n > 64) n = 64; s32 pts[128]; for (int i = 0; i < n; i++) { pts[i * 2] = (s32)rd32(c, ARG(1) + i * 8); pts[i * 2 + 1] = (s32)rd32(c, ARG(1) + i * 8 + 4); }
  if (!nb) fill_poly(&s, pts, n, b); if (!np) for (int i = 0; i < n; i++) { int j = (i + 1) % n; line_px(&s, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], p); } mark_screen(w, ARG(0)); RET(1); }
static void s_SetPixel(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0xffffffffu); return; } put_px(&s, (s32)ARG(1), (s32)ARG(2), cr2px((u32)ARG(3))); mark_screen(w, ARG(0)); RET(ARG(3)); }
static void s_SetPixelV(Cpu *c) { s_SetPixel(c); RET(1); }
static void s_GetPixel(Cpu *c) { Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0xffffffffu); return; } int x = (s32)ARG(1), y = (s32)ARG(2); if (x < s.x0 || y < s.y0 || x >= s.w || y >= s.h) { RET(0xffffffffu); return; } RET(px2cr(srow(&s, y)[x])); }
static void s_CreatePolygonRgn(Cpu *c) { GdiObj *r = gdi_new(W, K_RGN); if (!r) { RET(0); return; } int n = (int)ARG(1); if (n > 32) n = 32; r->npts = n; for (int i = 0; i < n; i++) { r->pts[i * 2] = (s32)rd32(c, ARG(0) + i * 8); r->pts[i * 2 + 1] = (s32)rd32(c, ARG(0) + i * 8 + 4); } RET(r->handle); }
static void s_CreateRectRgn(Cpu *c) { GdiObj *r = gdi_new(W, K_RGN); if (!r) { RET(0); return; } r->npts = 0; r->rx = (s32)ARG(0); r->ry = (s32)ARG(1); r->rw = (s32)ARG(2) - r->rx; r->rh = (s32)ARG(3) - r->ry; RET(r->handle); }
static void s_CreateRectRgnIndirect(Cpu *c) { GdiObj *r = gdi_new(W, K_RGN); if (!r) { RET(0); return; } u64 a = ARG(0); r->npts = 0; r->rx = (s32)rd32(c, a); r->ry = (s32)rd32(c, a + 4); r->rw = (s32)rd32(c, a + 8) - r->rx; r->rh = (s32)rd32(c, a + 12) - r->ry; RET(r->handle); }
static void s_CreateEllipticRgn(Cpu *c) { GdiObj *r = gdi_new(W, K_RGN); if (!r) { RET(0); return; } r->npts = -1; r->rx = (s32)ARG(0); r->ry = (s32)ARG(1); r->rw = (s32)ARG(2) - r->rx; r->rh = (s32)ARG(3) - r->ry; RET(r->handle); }
static void s_CombineRgn(Cpu *c) { GdiObj *d = gdi_get(W, ARG(0)), *a = gdi_get(W, ARG(1)); if (d && a) { u64 h = d->handle; *d = *a; d->handle = h; } RET(2); }
static void s_FillRgn(Cpu *c) { Win32 *w = W; Surf s; if (!dc_surf(c, ARG(0), &s)) { RET(0); return; } GdiObj *r = gdi_get(w, ARG(1)), *b = gdi_get(w, ARG(2)); if (!r || !b) { RET(0); return; }
  if (r->npts > 0) fill_poly(&s, r->pts, r->npts, b->color); else if (r->npts < 0) fill_ellipse(&s, r->rx, r->ry, r->rx + r->rw, r->ry + r->rh, b->color, 1, 0, 0); else fill_rect_px(&s, r->rx, r->ry, r->rx + r->rw, r->ry + r->rh, b->color); mark_screen(w, ARG(0)); RET(1); }
static void s_PaintRgn(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } call_shim(c, s_FillRgn, ARG(0), ARG(1), dc->brush, 0); }
static void s_OffsetRgn(Cpu *c) { GdiObj *r = gdi_get(W, ARG(0)); if (!r) { RET(0); return; } int dx = (s32)ARG(1), dy = (s32)ARG(2); r->rx += dx; r->ry += dy; for (int i = 0; i < r->npts; i++) { r->pts[i * 2] += dx; r->pts[i * 2 + 1] += dy; } RET(2); }
static void s_PtInRegion(Cpu *c) { GdiObj *r = gdi_get(W, ARG(0)); if (!r) { RET(0); return; } int x = (s32)ARG(1), y = (s32)ARG(2); if (r->npts <= 0) { RET(x >= r->rx && x < r->rx + r->rw && y >= r->ry && y < r->ry + r->rh); return; } int in = 0; for (int i = 0, j = r->npts - 1; i < r->npts; j = i++) { int xi = r->pts[i * 2], yi = r->pts[i * 2 + 1], xj = r->pts[j * 2], yj = r->pts[j * 2 + 1]; if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi + (yj == yi)) + xi)) in = !in; } RET(in); }
static void s_GetRgnBox(Cpu *c) { GdiObj *r = gdi_get(W, ARG(0)); if (!r) { RET(0); return; } u64 a = ARG(1); if (r->npts > 0) { int x0 = r->pts[0], y0 = r->pts[1], x1 = x0, y1 = y0; for (int i = 1; i < r->npts; i++) { if (r->pts[i * 2] < x0) x0 = r->pts[i * 2]; if (r->pts[i * 2] > x1) x1 = r->pts[i * 2]; if (r->pts[i * 2 + 1] < y0) y0 = r->pts[i * 2 + 1]; if (r->pts[i * 2 + 1] > y1) y1 = r->pts[i * 2 + 1]; } wr32(c, a, (u32)x0); wr32(c, a + 4, (u32)y0); wr32(c, a + 8, (u32)x1); wr32(c, a + 12, (u32)y1); } else { wr32(c, a, (u32)r->rx); wr32(c, a + 4, (u32)r->ry); wr32(c, a + 8, (u32)(r->rx + r->rw)); wr32(c, a + 12, (u32)(r->ry + r->rh)); } RET(2); }
static void s_GetNearestColor(Cpu *c) { RET(ARG(1)); }
static void s_GetTextFaceA(Cpu *c) { const char *n = "Nebula Mono"; size_t l = e_strlen(n) + 1; if ((u32)ARG(1) >= l && ARG(2)) { u8 *p = mem_ptr(c->mem, ARG(2), l, 1); if (p) e_memcpy(p, n, l); } RET(l); }
static void s_GetCharWidth32A(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); GdiObj *f = dc ? gdi_get(W, dc->font) : 0; int fh = f ? f->font_h : 16; int cw = font_cw(f, fh); for (u64 i = ARG(1); i <= ARG(2) && i - ARG(1) < 256; i++) wr32(c, ARG(3) + (i - ARG(1)) * 4, (u32)cw); RET(1); }
static void s_CreateFontW(Cpu *c) { s_CreateFontA(c); }
static void s_CreateFontIndirectW(Cpu *c) { s_CreateFontIndirectA(c); }
static void s_GetTextMetricsW(Cpu *c) { s_GetTextMetricsA(c); }
static void s_GetTextExtentPoint32W(Cpu *c) { s_GetTextExtentPoint32A(c); }
/* GetTextExtentExPoint(hdc, str, n, maxExtent, *fit, *dx, *size): fixed-pitch cells */
static void s_GetTextExtentExPointW(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); GdiObj *f = dc ? gdi_get(W, dc->font) : 0; int fh = f ? f->font_h : 16; int cw = font_cw(f, fh); int n = (int)(s32)ARG(2); if (n < 0) n = 0; int maxx = (int)(s32)ARG(3); int fit = n; if (ARG(4) && maxx >= 0) { fit = cw ? maxx / cw : n; if (fit > n) fit = n; wr32(c, ARG(4), (u32)fit); }
  if (ARG(5)) for (int k = 0; k < n; k++) wr32(c, ARG(5) + 4 * k, (u32)(cw * (k + 1))); if (ARG(6)) { wr32(c, ARG(6), (u32)(cw * n)); wr32(c, ARG(6) + 4, (u32)fh); } RET(1); }
static void s_GetTextExtentExPointA(Cpu *c) { s_GetTextExtentExPointW(c); }
static void s_GetObjectW(Cpu *c) { s_GetObjectA(c); }
static void s_timeGetTime(Cpu *c) { RET((u32)(host_now_ms() - W->start_ms + 100000.0)); }
static void s_timeBeginPeriod(Cpu *c) { RET(0); }
static void s_timeEndPeriod(Cpu *c) { RET(0); }
static void s_PlaySoundA(Cpu *c) { RET(1); }
static void s_waveOutGetNumDevs(Cpu *c) { RET(0); }
static void s_CoInitialize(Cpu *c) { RET(0); }
static void s_CoInitializeEx(Cpu *c) { RET(0); }
static void s_CoUninitialize(Cpu *c) { }
static void s_OleInitialize(Cpu *c) { RET(0); }
static void s_RegisterDragDrop(Cpu *c) { RET(0); }   /* OLE drag-and-drop targets are accepted but never receive drops (no host DnD bridge yet) */
static void s_RevokeDragDrop(Cpu *c) { RET(0); }
static void s_OleSetClipboard(Cpu *c) { RET(0); }
static void s_OleGetClipboard(Cpu *c) { if (ARG(0)) wrp(c, ARG(0), 0); RET(0x800401D0u); }
static void s_OleFlushClipboard(Cpu *c) { RET(0); }
static void s_OleIsCurrentClipboard(Cpu *c) { RET(1); }
static void s_DoDragDrop(Cpu *c) { if (ARG(3)) wr32(c, ARG(3), 0); RET(0x00040101u); }   /* DRAGDROP_S_CANCEL */
static void s_OleUninitialize(Cpu *c) { }
static void s_CoCreateInstance(Cpu *c) { w32_report(W, "CoCreateInstance: COM objects are not available"); RET(0x80040154u); }
static void s_CommandLineToArgvW(Cpu *c) { wr32(c, ARG(1), rd32(c, W->ubuf + 0x200)); RET(rd64(c, W->ubuf + 0x218)); }

#define PSZ_OF(cpu) ((cpu)->mode32 ? 4 : 8)
#include "user.inc"
#include "menu.inc"
#include "win32_vcl.inc"
#include "imagelist.inc"
#include "win32_wic.inc"
#include "comctl.inc"
#include "input.inc"
static void s_PeekMessageW(Cpu *c) { s_PeekMessageA(c); }
static void s_GetMessageW(Cpu *c) { s_GetMessageA(c); }
static void s_DispatchMessageW(Cpu *c) { s_DispatchMessageA(c); }
static void s_SendMessageW(Cpu *c) { s_SendMessageA(c); }
static void s_PostMessageW(Cpu *c) { s_PostMessageA(c); }
static void s_LoadCursorW(Cpu *c) { s_LoadCursorA(c); }
static void s_LoadIconW(Cpu *c) { s_LoadIconA(c); }
#define S(n) { "", #n, s_##n }
#define SA(api, fn) { "", api, s_##fn }
static const ShimDef gdi_shims[] = {
  S(PeekMessageA), S(PeekMessageW), S(GetMessageA), S(GetMessageW), S(TranslateMessage), S(DispatchMessageA), S(DispatchMessageW), S(SendMessageA), S(SendMessageW), S(PostMessageA), S(PostMessageW), S(PostQuitMessage), S(SetTimer), S(KillTimer), S(MsgWaitForMultipleObjects), S(WaitMessage),
  S(RegisterClassA), S(RegisterClassExA), S(RegisterClassW), S(RegisterClassExW), S(UnregisterClassA), S(GetClassInfoExA), S(AdjustWindowRect), S(AdjustWindowRectEx), S(CreateWindowExA), S(CreateWindowExW), S(DefWindowProcA), S(DefWindowProcW), S(DestroyWindow), S(ShowWindow), S(UpdateWindow), S(InvalidateRect), S(InvalidateRgn), S(ValidateRect), S(RedrawWindow), S(GetClientRect), S(GetWindowRect), S(BeginPaint), S(EndPaint), S(GetDC), S(GetWindowDC), S(ReleaseDC),
  S(SetWindowTextA), S(SetWindowTextW), S(GetWindowTextA), S(SetWindowPos), S(MoveWindow), S(SetWindowLongPtrA), S(GetWindowLongPtrA), S(SetWindowLongPtrW), S(GetWindowLongPtrW), S(SetWindowLongA), S(GetWindowLongA), S(SetFocus), S(GetFocus), S(SetForegroundWindow), S(GetForegroundWindow), S(GetActiveWindow), S(SetActiveWindow), S(IsWindow), S(IsWindowVisible), S(IsIconic), S(IsZoomed), S(GetDesktopWindow), S(GetParent), S(FindWindowA), S(EnableWindow), S(BringWindowToTop), S(SetCapture), S(ReleaseCapture), S(ClientToScreen), S(ScreenToClient), S(MapWindowPoints),
  S(GetCursorPos), S(SetCursorPos), S(ShowCursor), S(SetCursor), S(ClipCursor), S(LoadCursorA), S(LoadCursorW), S(LoadIconA), S(LoadIconW), S(LoadImageA), S(DestroyCursor), S(GetAsyncKeyState), S(GetKeyState), S(GetKeyboardState), S(MapVirtualKeyA), S(GetSystemMetrics), S(SystemParametersInfoA), S(MessageBoxA), S(MessageBoxW), S(wsprintfA), S(wsprintfW), S(SetProcessDPIAware), S(SetProcessDpiAwarenessContext), S(GetDpiForWindow), S(GetDpiForSystem), S(EnumDisplaySettingsA), S(ChangeDisplaySettingsA), S(RegisterClipboardFormatA), S(GetDlgItem), S(SetRect), S(PtInRect), S(FillRect), S(DrawTextA), S(DrawTextW), S(GetClassNameA), S(GetUpdateRect), S(GetSysColor), S(GetSysColorBrush),
  S(CreateCompatibleDC), S(CreateDCA), S(DeleteDC), S(CreateDIBSection), S(CreateCompatibleBitmap), S(CreateBitmap), S(DeleteObject), S(SelectObject), S(GetStockObject), VCL_SHIMS, MENU_SHIMS, COMCTL_SHIMS, INPUT_SHIMS, IMAGELIST_SHIMS, WIC_SHIMS, THEME_SHIMS, DPI_SHIMS, IMM_SHIMS, S(CreatePalette), S(GetPaletteEntries), S(SetPaletteEntries), S(SelectPalette), S(RealizePalette), S(GetSystemPaletteEntries), S(GetNearestPaletteIndex), S(ResizePalette), S(GetSystemPaletteUse), S(SetSystemPaletteUse), S(UpdateColors), S(AnimatePalette), S(UnrealizeObject), S(GetCurrentObject), S(CreateSolidBrush), S(CreateHatchBrush), S(CreatePatternBrush), S(CreateBrushIndirect), S(CreatePen), S(ExtCreatePen), S(CreateFontA), S(CreateFontW), S(CreateFontIndirectA), S(CreateFontIndirectW), S(SetTextColor), S(SetBkColor), S(SetBkMode), S(GetTextColor), S(GetBkColor), S(SetTextAlign), S(SetROP2), S(SetStretchBltMode), S(SetBrushOrgEx), S(GdiFlush), S(SaveDC), S(RestoreDC), S(SelectClipRgn), S(IntersectClipRect), S(SetMapMode), S(SetViewportOrgEx), S(SetWindowOrgEx), S(GetDeviceCaps), S(GetObjectA), S(GetObjectW),
  S(GetTextExtentPoint32A), S(GetTextExtentPoint32W), S(GetTextExtentExPointA), S(GetTextExtentExPointW), S(GetTextMetricsA), S(GetTextMetricsW), S(TextOutA), S(TextOutW), S(ExtTextOutA), S(ExtTextOutW), S(BitBlt), S(StretchBlt), S(ExtSelectClipRgn), S(GetViewportOrgEx), S(OffsetWindowOrgEx), S(OffsetViewportOrgEx), S(DPtoLP), S(LPtoDP), S(ExcludeClipRect), S(RectVisible), S(PtVisible), S(GetClipBox), S(GetClipRgn), S(GetWindowOrgEx), S(AlphaBlend), SA("GdiAlphaBlend", AlphaBlend), S(TransparentBlt), SA("GdiTransparentBlt", TransparentBlt), S(GradientFill), SA("GdiGradientFill", GradientFill), S(PatBlt), S(StretchDIBits), S(SetDIBitsToDevice), S(Rectangle), S(RoundRect), S(Ellipse), S(MoveToEx), S(LineTo), S(Polyline), S(Polygon), S(SetPixel), S(SetPixelV), S(GetPixel),
  S(CreatePolygonRgn), S(CreateRectRgn), S(CreateRectRgnIndirect), S(CreateEllipticRgn), S(CombineRgn), S(FillRgn), S(PaintRgn), S(OffsetRgn), S(PtInRegion), S(GetRgnBox), S(GetNearestColor), S(GetTextFaceA), S(GetCharWidth32A),
  S(timeGetTime), S(timeBeginPeriod), S(timeEndPeriod), S(PlaySoundA), S(waveOutGetNumDevs), S(CoInitialize), S(CoInitializeEx), S(CoUninitialize), S(OleInitialize), S(OleUninitialize), S(RegisterDragDrop), S(RevokeDragDrop), S(OleSetClipboard), S(OleGetClipboard), S(OleFlushClipboard), S(OleIsCurrentClipboard), S(DoDragDrop), S(CoCreateInstance), S(CommandLineToArgvW),
  USER_SHIMS
};
ShimFn gdi_find_shim(const char *name) { for (size_t i = 0; i < sizeof gdi_shims / sizeof gdi_shims[0]; i++) if (!e_strcmp(gdi_shims[i].name, name)) return gdi_shims[i].fn; return 0; }

void gdi_init(Win32 *w) {
  /* stock objects */
  static const u32 brush_cols[] = { 0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000 };
  for (int i = 0; i < 5; i++) { GdiObj *b = gdi_new(w, K_BRUSH); b->color = brush_cols[i]; stock[i] = b->handle; }
  { GdiObj *b = gdi_new(w, K_BRUSH); b->pen_width = -1; stock[5] = b->handle; }
  { GdiObj *p = gdi_new(w, K_PEN); p->color = 0xffffff; p->pen_width = 1; stock[6] = p->handle; }
  { GdiObj *p = gdi_new(w, K_PEN); p->color = 0; p->pen_width = 1; stock[7] = p->handle; }
  { GdiObj *p = gdi_new(w, K_PEN); p->pen_width = -1; stock[8] = p->handle; }
  for (int i = 10; i <= 17; i++) { GdiObj *f = gdi_new(w, K_FONT); f->font_h = 16; stock[i] = f->handle; }
  { GdiObj *pl = gdi_get(w, stock[15]); pl->kind = K_PALETTE; pl->font_h = 20; pl->bits = w32_heap_alloc(w, 1024, 0); static const u32 sys20[20] = { 0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xc0c0c0, 0xc0dcc0, 0xf0caa6, 0xf0fbff, 0xa4a0a0, 0x808080, 0x0000ff, 0x00ff00, 0x00ffff, 0xff0000, 0xff00ff, 0xffff00, 0xffffff }; u8 *p = mem_ptr(w->mem, pl->bits, 80, 1); if (p) for (int i = 0; i < 20; i++) { u32 v = sys20[i]; p[i * 4] = (u8)(v >> 16); p[i * 4 + 1] = (u8)(v >> 8); p[i * 4 + 2] = (u8)v; p[i * 4 + 3] = 0; } }   /* DEFAULT_PALETTE: the 20 system colours */
  { GdiObj *b = gdi_new(w, K_BRUSH); b->color = 0xffffff; stock[18] = b->handle; }
  { GdiObj *b = gdi_new(w, K_BITMAP); b->bw = b->bh = 1; b->bits = 0; stock[19] = b->handle; }   /* default 1x1 bitmap handle */
  { GdiObj *p = gdi_new(w, K_PEN); p->color = 0; p->pen_width = 1; stock[9] = p->handle; }
  window_dc = make_dc(w);
}
