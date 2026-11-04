/* comctl32 image lists: real pixel storage (BGRA, alpha in the top byte), Add/AddMasked/ReplaceIcon/Remove, Draw/DrawEx/DrawIndirect
   with per-pixel alpha onto any DC surface, Copy/Duplicate, and ImageList_Read (the "IL" v1.01 stream written by ImageList_Write —
   what Delphi/VCL TImageList embeds in DFM resources) read through the guest's IStream::Read. */
#define MAX_IML 160
#define IML_BASE 0x7D001
typedef struct { int used; u32 pid; int cx, cy, n, cap; u32 flags; u32 bk; u32 *px; } ImgList;   /* px: n images, each cx*cy pixels, 0xAARRGGBB in the surface's byte order (B,G,R,A) */
static ImgList g_iml[MAX_IML];
static ImgList *iml_of(u64 h) { if (h < IML_BASE || h >= IML_BASE + MAX_IML) return 0; ImgList *il = &g_iml[h - IML_BASE]; return il->used ? il : 0; }
static u64 iml_handle(ImgList *il) { return IML_BASE + (u64)(il - g_iml); }
static void iml_reserve(ImgList *il, int n) { if (n <= il->cap) return; int nc = il->cap ? il->cap : 8; while (nc < n) nc *= 2; size_t one = (size_t)il->cx * il->cy * 4; u32 *np = emu_alloc(one * nc); e_memset(np, 0, one * nc); if (il->px) { e_memcpy(np, il->px, one * il->n); emu_free(il->px); } il->px = np; il->cap = nc; }
static ImgList *iml_new(Win32 *w, int cx, int cy, u32 flags, int init) { if (cx <= 0 || cx > 256 || cy <= 0 || cy > 256) return 0; for (int k = 0; k < MAX_IML; k++) if (!g_iml[k].used) { ImgList *il = &g_iml[k]; e_memset(il, 0, sizeof *il); il->used = 1; il->pid = w->pid; il->cx = cx; il->cy = cy; il->flags = flags; il->bk = 0xffffffffu; if (init > 0 && init < 4096) iml_reserve(il, init); return il; } return 0; }
static void iml_free(ImgList *il) { if (il->px) emu_free(il->px); e_memset(il, 0, sizeof *il); }
static u32 *iml_img(ImgList *il, int i) { return il->px + (size_t)i * il->cx * il->cy; }
/* pixel rows of a GDI bitmap (DIB sections are synced first) */
static u32 *bm_pixels(Win32 *w, u64 hbm, int *bw, int *bh, int *bottom_up) { GdiObj *bm = gdi_get(w, hbm); if (!bm || bm->kind != K_BITMAP || !bm->bits) return 0; dib_sync(w, bm); u32 *px = (u32 *)mem_ptr(w->mem, bm->bits, (u64)bm->bw * bm->bh * 4, 1); if (!px) return 0; *bw = bm->bw; *bh = bm->bh; *bottom_up = bm->font_bold; return px; }
static inline u32 *bm_row(u32 *px, int bw, int bh, int bottom_up, int y) { return px + (size_t)(bottom_up ? bh - 1 - y : y) * bw; }
/* append the images of a cx-wide strip held in host memory (stride in pixels, rows top-down); mask: 1 = transparent (may be 0); returns the first new index */
static int iml_add_strip(ImgList *il, const u32 *src, int sw, int sh, int sstride, const u8 *mask, int mstride, u32 key, int use_key) {
  int cnt = sw / il->cx; if (cnt <= 0) return -1; int first = il->n; iml_reserve(il, il->n + cnt);
  int has_alpha = 0; for (int y = 0; y < sh && !has_alpha; y++) for (int x = 0; x < sw; x++) if (src[(size_t)y * sstride + x] >> 24) { has_alpha = 1; break; }
  for (int j = 0; j < cnt; j++) { u32 *d = iml_img(il, il->n + j); for (int y = 0; y < il->cy; y++) for (int x = 0; x < il->cx; x++) { u32 v = 0; int sx = j * il->cx + x; if (y < sh) { v = src[(size_t)y * sstride + sx]; u32 a = has_alpha ? v >> 24 : 0xff; if (mask && ((mask[(size_t)y * mstride + (sx >> 3)] >> (7 - (sx & 7))) & 1)) a = 0; if (use_key && (v & 0xffffff) == (key & 0xffffff)) a = 0; v = (v & 0xffffff) | a << 24; } d[y * il->cx + x] = v; } }
  il->n += cnt; return first;
}
static int iml_add_bitmap(Win32 *w, ImgList *il, u64 hbm, u64 hmask, u32 key, int use_key) {
  int bw, bh, bu; u32 *px = bm_pixels(w, hbm, &bw, &bh, &bu); if (!px) return -1; int cnt = bw / il->cx; if (cnt <= 0) return -1;
  size_t rows = (size_t)(bh < il->cy ? bh : il->cy); u32 *tmp = emu_alloc((size_t)bw * rows * 4); for (size_t y = 0; y < rows; y++) e_memcpy(tmp + y * bw, bm_row(px, bw, bh, bu, (int)y), (size_t)bw * 4);
  u8 *mk = 0; int mstride = (bw + 7) / 8; if (hmask) { int mw, mh, mbu; u32 *mp = bm_pixels(w, hmask, &mw, &mh, &mbu); if (mp) { mk = emu_alloc((size_t)mstride * rows); e_memset(mk, 0, (size_t)mstride * rows); for (size_t y = 0; y < rows && (int)y < mh; y++) { u32 *r = bm_row(mp, mw, mh, mbu, (int)y); for (int x = 0; x < bw && x < mw; x++) if (r[x] & 0xffffff) mk[y * mstride + (x >> 3)] |= (u8)(0x80 >> (x & 7)); } } }
  if (use_key && key == 0xFF000000u /* CLR_DEFAULT */) key = tmp[0];
  int r = iml_add_strip(il, tmp, bw, (int)rows, bw, mk, mstride, key, use_key); emu_free(tmp); if (mk) emu_free(mk); return r;
}
/* generic 16x16-ish glyph for pseudo icons (icons are not decoded yet): a framed window */
static void iml_generic_icon(ImgList *il, u32 *d) { for (int y = 0; y < il->cy; y++) for (int x = 0; x < il->cx; x++) { int bx = x < 1 || y < 1 || x >= il->cx - 1 || y >= il->cy - 1; int title = y < il->cy / 4; d[y * il->cx + x] = (x > 0 && y > 0 && x < il->cx - 1 && y < il->cy - 1) || bx ? (bx ? 0xff404040u : title ? 0xff3060c0u : 0xfff4f4f4u) : 0; } }
static void iml_draw(Cpu *c, ImgList *il, int i, u64 hdc, int x, int y, int dx, int dy, u32 style) {
  Win32 *w = W; if (i < 0 || i >= il->n) return; Surf s; if (!dc_surf(c, hdc, &s)) return; if (dx <= 0) dx = il->cx; if (dy <= 0) dy = il->cy; const u32 *img = iml_img(il, i); int sel = (style & 0x4) != 0; int mask_only = (style & 0x10) != 0;
  for (int yy = 0; yy < dy && yy < il->cy; yy++) { int py = y + yy; if (py < s.y0 || py >= s.h) continue; u32 *row = srow(&s, py); for (int xx = 0; xx < dx && xx < il->cx; xx++) { int px = x + xx; if (px < s.x0 || px >= s.w) continue; u32 v = img[yy * il->cx + xx]; u32 a = v >> 24; if (!a) continue;
      if (mask_only) { row[px] = 0; continue; } u32 d = row[px]; u32 r = ((v >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255, g = ((v >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255, b = ((v & 255) * a + (d & 255) * (255 - a)) / 255; if (sel) { r = (r + 0x33) / 2; g = (g + 0x99) / 2; b = (b + 0xff) / 2; } row[px] = r << 16 | g << 8 | b; } }
  mark_screen(w, hdc);
}
static void s_ImageList_Create(Cpu *c) { ImgList *il = iml_new(W, (int)ARG(0), (int)ARG(1), (u32)ARG(2), (int)ARG(3)); RET(il ? iml_handle(il) : 0); }
static void s_ImageList_Destroy(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(0); return; } iml_free(il); RET(1); }
static void s_ImageList_Add(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(-1); return; } RET(iml_add_bitmap(W, il, ARG(1), ARG(2), 0, 0)); }
static void s_ImageList_AddMasked(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(-1); return; } u32 cr = (u32)ARG(2); u32 key = cr == 0xFF000000u ? cr : ((cr & 0xff) << 16 | (cr & 0xff00) | (cr >> 16 & 0xff)); RET(iml_add_bitmap(W, il, ARG(1), 0, key, 1)); }
static void s_ImageList_Replace(Cpu *c) { ImgList *il = iml_of(ARG(0)); int i = (int)ARG(1); if (!il || i < 0 || i >= il->n) { RET(0); return; } int k = iml_add_bitmap(W, il, ARG(2), ARG(3), 0, 0); if (k < 0) { RET(0); return; } e_memcpy(iml_img(il, i), iml_img(il, k), (size_t)il->cx * il->cy * 4); il->n = k; RET(1); }
static void s_ImageList_ReplaceIcon(Cpu *c) { ImgList *il = iml_of(ARG(0)); int i = (int)ARG(1); if (!il) { RET(-1); return; } if (i < 0) { iml_reserve(il, il->n + 1); i = il->n++; } if (i >= il->n) { RET(-1); return; } iml_generic_icon(il, iml_img(il, i)); RET(i); }
static void s_ImageList_Remove(Cpu *c) { ImgList *il = iml_of(ARG(0)); int i = (int)ARG(1); if (!il) { RET(0); return; } if (i < 0) { il->n = 0; RET(1); return; } if (i >= il->n) { RET(0); return; } size_t one = (size_t)il->cx * il->cy * 4; e_memmove(iml_img(il, i), iml_img(il, i + 1), one * (size_t)(il->n - 1 - i)); il->n--; RET(1); }
static void s_ImageList_GetImageCount(Cpu *c) { ImgList *il = iml_of(ARG(0)); RET(il ? il->n : 0); }
static void s_ImageList_SetImageCount(Cpu *c) { ImgList *il = iml_of(ARG(0)); int n = (int)ARG(1); if (!il || n < 0 || n > 4096) { RET(0); return; } iml_reserve(il, n); if (n > il->n) e_memset(iml_img(il, il->n), 0, (size_t)il->cx * il->cy * 4 * (size_t)(n - il->n)); il->n = n; RET(1); }
static void s_ImageList_GetIconSize(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (ARG(1)) wr32(c, ARG(1), il ? il->cx : 16); if (ARG(2)) wr32(c, ARG(2), il ? il->cy : 16); RET(il ? 1 : 0); }
static void s_ImageList_SetIconSize(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(0); return; } int cx = (int)ARG(1), cy = (int)ARG(2); if (cx <= 0 || cy <= 0 || cx > 256 || cy > 256) { RET(0); return; } if (il->px) emu_free(il->px); il->px = 0; il->n = il->cap = 0; il->cx = cx; il->cy = cy; RET(1); }
static void s_ImageList_SetBkColor(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(0xffffffffu); return; } u32 old = il->bk; il->bk = (u32)ARG(1); RET(old); }
static void s_ImageList_GetBkColor(Cpu *c) { ImgList *il = iml_of(ARG(0)); RET(il ? il->bk : 0xffffffffu); }
static void s_ImageList_Draw(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(0); return; } iml_draw(c, il, (int)ARG(1), ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), 0, 0, (u32)ARG(5)); RET(1); }
static void s_ImageList_DrawEx(Cpu *c) { ImgList *il = iml_of(ARG(0)); if (!il) { RET(0); return; } iml_draw(c, il, (int)ARG(1), ARG(2), (int)(s32)ARG(3), (int)(s32)ARG(4), (int)(s32)ARG(5), (int)(s32)ARG(6), (u32)ARG(9)); RET(1); }
static void s_ImageList_DrawIndirect(Cpu *c) { u64 p = ARG(0); if (!p) { RET(0); return; } u64 P = PSZ_OF(c); u64 himl = rdp(c, p + P), hdc; int i; int base; if (c->mode32) { i = (int)rd32(c, p + 8); hdc = rd32(c, p + 12); base = 16; } else { i = (int)rd32(c, p + 16); hdc = rd64(c, p + 24); base = 32; }
  ImgList *il = iml_of(himl); if (!il) { RET(0); return; } int x = (int)(s32)rd32(c, p + base), y = (int)(s32)rd32(c, p + base + 4), cx = (int)(s32)rd32(c, p + base + 8), cy = (int)(s32)rd32(c, p + base + 12); u32 style = rd32(c, p + base + 32); iml_draw(c, il, i, hdc, x, y, cx, cy, style); RET(1); }
static void s_ImageList_GetIcon(Cpu *c) { ImgList *il = iml_of(ARG(0)); RET(il && (int)ARG(1) < il->n ? 0x7B801 : 0); }
static void s_ImageList_Duplicate(Cpu *c) { ImgList *src = iml_of(ARG(0)); if (!src) { RET(0); return; } ImgList *il = iml_new(W, src->cx, src->cy, src->flags, src->n); if (!il) { RET(0); return; } if (src->n) { iml_reserve(il, src->n); e_memcpy(il->px, src->px, (size_t)src->cx * src->cy * 4 * src->n); } il->n = src->n; il->bk = src->bk; RET(iml_handle(il)); }
static void s_ImageList_Copy(Cpu *c) { /* (dst, idst, src, isrc, flags: 0 copy / 1 swap) */ ImgList *d = iml_of(ARG(0)), *s = iml_of(ARG(2)); int id = (int)ARG(1), is = (int)ARG(3); if (!d || !s || id < 0 || id >= d->n || is < 0 || is >= s->n || d->cx != s->cx || d->cy != s->cy) { RET(0); return; } size_t one = (size_t)d->cx * d->cy * 4; if (ARG(4) & 1) { u32 *t = emu_alloc(one); e_memcpy(t, iml_img(d, id), one); e_memcpy(iml_img(d, id), iml_img(s, is), one); e_memcpy(iml_img(s, is), t, one); emu_free(t); } else e_memcpy(iml_img(d, id), iml_img(s, is), one); RET(1); }
static void s_ImageList_GetImageInfo(Cpu *c) { ImgList *il = iml_of(ARG(0)); int i = (int)ARG(1); u64 p = ARG(2); if (!il || i < 0 || i >= il->n || !p) { RET(0); return; } u64 P = PSZ_OF(c); wrp(c, p, 0); wrp(c, p + P, 0); u64 r = p + 2 * P + 8; wr32(c, r, (u32)(i * il->cx)); wr32(c, r + 4, 0); wr32(c, r + 8, (u32)((i + 1) * il->cx)); wr32(c, r + 12, (u32)il->cy); RET(1); }
static void s_ImageList_Merge(Cpu *c) { ImgList *a = iml_of(ARG(0)); if (!a) { RET(0); return; } ImgList *il = iml_new(W, a->cx, a->cy, a->flags, 1); if (!il) { RET(0); return; } iml_reserve(il, 1); int i = (int)ARG(1); if (i >= 0 && i < a->n) e_memcpy(il->px, iml_img(a, i), (size_t)a->cx * a->cy * 4); il->n = 1; RET(iml_handle(il)); }
static void s_ImageList_LoadImageW(Cpu *c) { /* (hinst, name, cx, grow, crMask, type, flags) */ int cx = (int)ARG(2); u32 cr = (u32)ARG(4); call_shim(c, s_LoadImageW, ARG(0), ARG(1), ARG(5), 0); u64 hbm = c->r[RAX]; if (!hbm) { RET(0); return; } GdiObj *bm = gdi_get(W, hbm); if (!bm || bm->kind != K_BITMAP) { RET(0); return; } if (cx <= 0) cx = bm->bh; ImgList *il = iml_new(W, cx, bm->bh, (u32)ARG(6), bm->bw / cx); if (!il) { RET(0); return; } if (cr == 0xFFFFFFFFu /* CLR_NONE */) iml_add_bitmap(W, il, hbm, 0, 0, 0); else { u32 key = cr == 0xFF000000u ? cr : ((cr & 0xff) << 16 | (cr & 0xff00) | (cr >> 16 & 0xff)); iml_add_bitmap(W, il, hbm, 0, key, 1); } call_shim(c, s_DeleteObject, hbm, 0, 0, 0); RET(iml_handle(il)); }
static void s_ImageList_LoadImageA(Cpu *c) { if (ARG(1) >= 0x10000) { RET(0); return; } call_shim_tail(c, s_ImageList_LoadImageW, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_ImageList_BeginDrag(Cpu *c) { RET(0); }
static void s_ImageList_EndDrag(Cpu *c) { (void)c; }
static void s_ImageList_SetDragCursorImage(Cpu *c) { RET(0); }
static void s_ImageList_DragEnter(Cpu *c) { RET(0); }
static void s_ImageList_DragLeave(Cpu *c) { RET(0); }
static void s_ImageList_DragMove(Cpu *c) { RET(0); }
static void s_ImageList_DragShowNolock(Cpu *c) { RET(0); }
static void s_ImageList_GetDragImage(Cpu *c) { RET(0); }
static void s_ImageList_Write(Cpu *c) { RET(0); }
static void s_ImageList_SetOverlayImage(Cpu *c) { RET(1); }
static void s_ImageList_SetColorTable(Cpu *c) { RET(0); }
/* ---- ImageList_Read(IStream*): "IL" v1.01 header (28 bytes), image bitmap as BITMAPFILEHEADER + DIB, mask bitmap (1 bpp) when ILC_MASK ---- */
#define ILR_BUF 65536
typedef struct { int active; u32 pid; u64 stream, buf; int step; u8 *acc; size_t have, need, cap; int cx, cy, cur, mx; u32 flags; size_t img_off, img_len, mask_off; } IlRead;
static IlRead g_ilr;
static size_t dib_total(const u8 *p, size_t avail, int *bpp_out, int *bw, int *bh, size_t *bits_off) { /* p → BITMAPINFOHEADER; returns total DIB bytes (header + palette + bits) or 0 */ if (avail < 40) return 0; u32 hs = *(const u32 *)p; s32 w = *(const s32 *)(p + 4), h = *(const s32 *)(p + 8); u16 bpp = *(const u16 *)(p + 14); u32 comp = *(const u32 *)(p + 16), simg = *(const u32 *)(p + 20), clr = *(const u32 *)(p + 32); if (h < 0) h = -h; if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || hs < 40 || hs > 256) return 0; size_t pal = bpp <= 8 ? (size_t)(clr ? clr : 1u << bpp) * 4 : (comp == 3 ? 12 : 0); size_t stride = (((size_t)w * bpp + 31) / 32) * 4, bits = simg && comp ? simg : stride * h; *bpp_out = bpp; *bw = w; *bh = h; *bits_off = hs + pal; return hs + pal + bits; }
static u32 *dib_host_decode(const u8 *p, size_t len, int *bw, int *bh) { int bpp, w, h; size_t bo; size_t tot = dib_total(p, len, &bpp, &w, &h, &bo); if (!tot || tot > len) return 0; s32 sh = *(const s32 *)(p + 8); int bottom_up = sh > 0; u32 hs = *(const u32 *)p; u32 comp = *(const u32 *)(p + 16); if (comp != 0 && comp != 3) return 0; const u8 *pal = p + hs; const u8 *bits = p + bo; size_t stride = (((size_t)w * bpp + 31) / 32) * 4; u32 *out = emu_alloc((size_t)w * h * 4);
  for (int y = 0; y < h; y++) { const u8 *row = bits + (size_t)(bottom_up ? h - 1 - y : y) * stride; u32 *d = out + (size_t)y * w; for (int x = 0; x < w; x++) { u32 v, idx; switch (bpp) { case 1: idx = (row[x >> 3] >> (7 - (x & 7))) & 1; v = *(const u32 *)(pal + idx * 4) & 0xffffff; break; case 4: idx = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15; v = *(const u32 *)(pal + idx * 4) & 0xffffff; break; case 8: v = *(const u32 *)(pal + row[x] * 4) & 0xffffff; break; case 16: { u32 s = row[x * 2] | row[x * 2 + 1] << 8; v = ((s >> 10 & 31) * 255 / 31) << 16 | ((s >> 5 & 31) * 255 / 31) << 8 | (s & 31) * 255 / 31; break; } case 24: v = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16; break; default: v = *(const u32 *)(row + x * 4); break; } d[x] = v; } }
  *bw = w; *bh = h; return out; }
static void ilr_finish(Cpu *c, u64 result) { if (g_ilr.acc) emu_free(g_ilr.acc); if (g_ilr.buf) w32_heap_free(W, g_ilr.buf); e_memset(&g_ilr, 0, sizeof g_ilr); w32_cont_finish(c, result); }
static void ilr_want(size_t n) { g_ilr.need = n; if (n > g_ilr.cap) { size_t nc = g_ilr.cap ? g_ilr.cap : 4096; while (nc < n) nc *= 2; u8 *na = emu_alloc(nc); if (g_ilr.acc) { e_memcpy(na, g_ilr.acc, g_ilr.have); emu_free(g_ilr.acc); } g_ilr.acc = na; g_ilr.cap = nc; } }
static void ilr_read_more(Cpu *c) { u64 P = PSZ_OF(c); u64 vt = rdp(c, g_ilr.stream); u64 fn = rdp(c, vt + 3 * P); size_t chunk = g_ilr.need - g_ilr.have; if (chunk > ILR_BUF) chunk = ILR_BUF; wr32(c, g_ilr.buf + ILR_BUF, 0); w32_guest_call(c, fn, g_ilr.stream, g_ilr.buf, chunk, g_ilr.buf + ILR_BUF); }
static void ilr_build(Cpu *c) { Win32 *w = W; const u8 *p = g_ilr.acc; int iw, ih; u32 *img = dib_host_decode(p + g_ilr.img_off, g_ilr.img_len, &iw, &ih); if (!img) { e_log("[imagelist] ImageList_Read: undecodable image bitmap\n"); ilr_finish(c, 0); return; }
  u8 *mask = 0; int mstride = 0; if (g_ilr.mask_off) { int bpp, mw, mh; size_t bo; const u8 *mp = p + g_ilr.mask_off; size_t tot = dib_total(mp, g_ilr.have - g_ilr.mask_off, &bpp, &mw, &mh, &bo); if (tot && bpp == 1 && mw >= iw) { mstride = (int)((((size_t)mw + 31) / 32) * 4); s32 sh = *(const s32 *)(mp + 8); mask = emu_alloc((size_t)mstride * ih); e_memset(mask, 0, (size_t)mstride * ih); for (int y = 0; y < ih && y < mh; y++) e_memcpy(mask + (size_t)y * mstride, mp + bo + (size_t)(sh > 0 ? mh - 1 - y : y) * mstride, (size_t)mstride); } }
  ImgList *il = iml_new(w, g_ilr.cx, g_ilr.cy, g_ilr.flags, g_ilr.mx); if (!il) { emu_free(img); if (mask) emu_free(mask); ilr_finish(c, 0); return; }
  /* images are tiled `cols` per row in the stored bitmap (4 per row since XP, one long strip before); re-pack them into a single strip */
  int cols = iw / g_ilr.cx, rows = ih / g_ilr.cy; if (cols < 1) cols = 1; if (rows < 1) rows = 1; int cur = g_ilr.cur; if (cur > cols * rows) cur = cols * rows;
  if (cur > 0) { int sw = cur * g_ilr.cx, sh = g_ilr.cy; u32 *strip = emu_alloc((size_t)sw * sh * 4); int ms = (sw + 7) / 8; u8 *smask = mask ? emu_alloc((size_t)ms * sh) : 0; if (smask) e_memset(smask, 0, (size_t)ms * sh);
    for (int i = 0; i < cur; i++) { int tx = (i % cols) * g_ilr.cx, ty = (i / cols) * g_ilr.cy; for (int y = 0; y < sh; y++) { e_memcpy(strip + (size_t)y * sw + i * g_ilr.cx, img + (size_t)(ty + y) * iw + tx, (size_t)g_ilr.cx * 4); if (smask) for (int x = 0; x < g_ilr.cx; x++) { int sx = tx + x, dx = i * g_ilr.cx + x; if ((mask[(size_t)(ty + y) * mstride + (sx >> 3)] >> (7 - (sx & 7))) & 1) smask[(size_t)y * ms + (dx >> 3)] |= (u8)(0x80 >> (dx & 7)); } } }
    iml_add_strip(il, strip, sw, sh, sw, smask, ms, 0, 0); emu_free(strip); if (smask) emu_free(smask); } il->n = cur; emu_free(img); if (mask) emu_free(mask);
  e_log("[imagelist] ImageList_Read: %d images %dx%d (strip %dx%d, flags %x)\n", cur, g_ilr.cx, g_ilr.cy, iw, ih, g_ilr.flags); ilr_finish(c, iml_handle(il)); }
static void ilr_advance(Cpu *c) {
  for (;;) { if (g_ilr.have < g_ilr.need) { ilr_read_more(c); return; }
    const u8 *p = g_ilr.acc;
    if (g_ilr.step == 0) { u16 magic = *(const u16 *)p, ver = *(const u16 *)(p + 2); if (magic != 0x4C49 || (ver != 0x101 && ver != 0x100)) { e_log("[imagelist] ImageList_Read: unsupported stream magic %04x version %04x\n", magic, ver); ilr_finish(c, 0); return; }
      g_ilr.cur = *(const u16 *)(p + 4); g_ilr.mx = *(const u16 *)(p + 6); g_ilr.cx = *(const u16 *)(p + 10); g_ilr.cy = *(const u16 *)(p + 12); g_ilr.flags = *(const u16 *)(p + 18); g_ilr.step = 1; g_ilr.img_off = 28 + 14; ilr_want(28 + 14 + 40); continue; }
    if (g_ilr.step == 1) { int bpp, bw, bh; size_t bo; size_t tot = dib_total(p + g_ilr.img_off, g_ilr.have - g_ilr.img_off, &bpp, &bw, &bh, &bo); if (!tot) { ilr_finish(c, 0); return; } g_ilr.img_len = tot; g_ilr.step = 2; ilr_want(g_ilr.img_off + tot); continue; }
    if (g_ilr.step == 2) { if (!(g_ilr.flags & 1)) { ilr_build(c); return; } g_ilr.mask_off = g_ilr.img_off + g_ilr.img_len + 14; g_ilr.step = 3; ilr_want(g_ilr.mask_off + 40); continue; }
    if (g_ilr.step == 3) { int bpp, bw, bh; size_t bo; size_t tot = dib_total(p + g_ilr.mask_off, g_ilr.have - g_ilr.mask_off, &bpp, &bw, &bh, &bo); if (!tot) { ilr_finish(c, 0); return; } g_ilr.step = 4; ilr_want(g_ilr.mask_off + tot); continue; }
    ilr_build(c); return; }
}
static void ilr_resume(Cpu *c, u64 rax) { u32 got = rd32(c, g_ilr.buf + ILR_BUF); if ((u32)rax > 1 || got == 0 || got > ILR_BUF) { e_log("[imagelist] ImageList_Read: IStream::Read hr=%x got=%u\n", (u32)rax, got); ilr_finish(c, 0); return; } const u8 *src = mem_ptr(c->mem, g_ilr.buf, got, 0); if (!src) { ilr_finish(c, 0); return; } if (g_ilr.have + got > g_ilr.cap) got = (u32)(g_ilr.cap - g_ilr.have); e_memcpy(g_ilr.acc + g_ilr.have, src, got); g_ilr.have += got; ilr_advance(c); }
static void s_ImageList_Read(Cpu *c) { Win32 *w = W; u64 st = ARG(0); if (!st || g_ilr.active) { RET(0); return; } e_memset(&g_ilr, 0, sizeof g_ilr); g_ilr.active = 1; g_ilr.pid = w->pid; g_ilr.stream = st; g_ilr.buf = w32_heap_alloc(w, ILR_BUF + 16, 0); if (!g_ilr.buf) { g_ilr.active = 0; RET(0); return; } ilr_want(28); w32_push_cont(c, CONT_ILREAD, 0); ilr_advance(c); }
#define IMAGELIST_SHIMS S(ImageList_Create), S(ImageList_Destroy), S(ImageList_Add), S(ImageList_AddMasked), S(ImageList_Replace), S(ImageList_ReplaceIcon), S(ImageList_Remove), S(ImageList_GetImageCount), S(ImageList_SetImageCount), S(ImageList_GetIconSize), S(ImageList_SetIconSize), \
  S(ImageList_SetBkColor), S(ImageList_GetBkColor), S(ImageList_Draw), S(ImageList_DrawEx), S(ImageList_DrawIndirect), S(ImageList_GetIcon), S(ImageList_Duplicate), S(ImageList_Copy), S(ImageList_GetImageInfo), S(ImageList_Merge), S(ImageList_LoadImageW), S(ImageList_LoadImageA), \
  S(ImageList_BeginDrag), S(ImageList_EndDrag), S(ImageList_SetDragCursorImage), S(ImageList_DragEnter), S(ImageList_DragLeave), S(ImageList_DragMove), S(ImageList_DragShowNolock), S(ImageList_GetDragImage), S(ImageList_Write), S(ImageList_SetOverlayImage), S(ImageList_SetColorTable), S(ImageList_Read)
