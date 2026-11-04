/* What a Delphi/VCL program (Inno Setup's real Setup, many shareware tools) imports beyond the core: rect helpers, global
   atoms, Windows hooks (accepted, never called), DIB conversions at 1/4/8/16/24/32 bpp, GDI curves/metafiles (accepted,
   drawn as lines or not at all — reported once), ImageList/FlatSB extras, MDI/menu/hook odds and ends, a few COM/OLE helpers. */
/* ---- RECT ---- */
static void s_SetRectEmpty(Cpu *c) { u64 r = ARG(0); if (!r) { RET(0); return; } wr32(c, r, 0); wr32(c, r + 4, 0); wr32(c, r + 8, 0); wr32(c, r + 12, 0); RET(1); }
static void s_OffsetRect(Cpu *c) { u64 r = ARG(0); s32 dx = (s32)ARG(1), dy = (s32)ARG(2); if (!r) { RET(0); return; } wr32(c, r, (u32)((s32)rd32(c, r) + dx)); wr32(c, r + 4, (u32)((s32)rd32(c, r + 4) + dy)); wr32(c, r + 8, (u32)((s32)rd32(c, r + 8) + dx)); wr32(c, r + 12, (u32)((s32)rd32(c, r + 12) + dy)); RET(1); }
static void s_InflateRect(Cpu *c) { u64 r = ARG(0); s32 dx = (s32)ARG(1), dy = (s32)ARG(2); if (!r) { RET(0); return; } wr32(c, r, (u32)((s32)rd32(c, r) - dx)); wr32(c, r + 4, (u32)((s32)rd32(c, r + 4) - dy)); wr32(c, r + 8, (u32)((s32)rd32(c, r + 8) + dx)); wr32(c, r + 12, (u32)((s32)rd32(c, r + 12) + dy)); RET(1); }
static void s_IntersectRect(Cpu *c) { u64 d = ARG(0), a = ARG(1), b = ARG(2); s32 l = (s32)rd32(c, a) > (s32)rd32(c, b) ? (s32)rd32(c, a) : (s32)rd32(c, b), t = (s32)rd32(c, a + 4) > (s32)rd32(c, b + 4) ? (s32)rd32(c, a + 4) : (s32)rd32(c, b + 4), r = (s32)rd32(c, a + 8) < (s32)rd32(c, b + 8) ? (s32)rd32(c, a + 8) : (s32)rd32(c, b + 8), bt = (s32)rd32(c, a + 12) < (s32)rd32(c, b + 12) ? (s32)rd32(c, a + 12) : (s32)rd32(c, b + 12);
  if (l >= r || t >= bt) { if (d) { wr32(c, d, 0); wr32(c, d + 4, 0); wr32(c, d + 8, 0); wr32(c, d + 12, 0); } RET(0); return; } if (d) { wr32(c, d, (u32)l); wr32(c, d + 4, (u32)t); wr32(c, d + 8, (u32)r); wr32(c, d + 12, (u32)bt); } RET(1); }
static void s_UnionRect(Cpu *c) { u64 d = ARG(0), a = ARG(1), b = ARG(2); int ea = (s32)rd32(c, a) >= (s32)rd32(c, a + 8) || (s32)rd32(c, a + 4) >= (s32)rd32(c, a + 12), eb = (s32)rd32(c, b) >= (s32)rd32(c, b + 8) || (s32)rd32(c, b + 4) >= (s32)rd32(c, b + 12);
  if (ea && eb) { wr32(c, d, 0); wr32(c, d + 4, 0); wr32(c, d + 8, 0); wr32(c, d + 12, 0); RET(0); return; } u64 s = ea ? b : eb ? a : 0; if (s) { for (int i = 0; i < 16; i += 4) wr32(c, d + i, rd32(c, s + i)); RET(1); return; }
  s32 l = (s32)rd32(c, a) < (s32)rd32(c, b) ? (s32)rd32(c, a) : (s32)rd32(c, b), t = (s32)rd32(c, a + 4) < (s32)rd32(c, b + 4) ? (s32)rd32(c, a + 4) : (s32)rd32(c, b + 4), r = (s32)rd32(c, a + 8) > (s32)rd32(c, b + 8) ? (s32)rd32(c, a + 8) : (s32)rd32(c, b + 8), bt = (s32)rd32(c, a + 12) > (s32)rd32(c, b + 12) ? (s32)rd32(c, a + 12) : (s32)rd32(c, b + 12);
  wr32(c, d, (u32)l); wr32(c, d + 4, (u32)t); wr32(c, d + 8, (u32)r); wr32(c, d + 12, (u32)bt); RET(1); }
static void s_SubtractRect(Cpu *c) { u64 d = ARG(0), a = ARG(1); for (int i = 0; i < 16; i += 4) wr32(c, d + i, rd32(c, a + i)); RET(1); }
/* ---- global atoms (one table per emulator: they are system-wide on Windows too) ---- */
static char g_atoms[256][256]; static int g_atom_ref[256];
static int atom_find(const char *s) { for (int i = 0; i < 256; i++) if (g_atom_ref[i] && !e_stricmp(g_atoms[i], s)) return i; return -1; }
static void atom_add(Cpu *c, int wide) { u64 a = ARG(0); if (a < 0x10000) { RET(a); return; } char s[256]; if (wide) wstr_to_utf8(c, a, s, sizeof s, -1); else { const char *p = rd_str(c, a, 255); e_snprintf(s, sizeof s, "%s", p ? p : ""); } if (!s[0]) { W->last_error = 87; RET(0); return; }
  int i = atom_find(s); if (i < 0) { for (i = 0; i < 256 && g_atom_ref[i]; i++); if (i == 256) { W->last_error = 8; RET(0); return; } e_snprintf(g_atoms[i], sizeof g_atoms[i], "%s", s); } g_atom_ref[i]++; RET(0xC000 + i); }
static void atom_find_api(Cpu *c, int wide) { u64 a = ARG(0); if (a < 0x10000) { RET(a); return; } char s[256]; if (wide) wstr_to_utf8(c, a, s, sizeof s, -1); else { const char *p = rd_str(c, a, 255); e_snprintf(s, sizeof s, "%s", p ? p : ""); } int i = atom_find(s); if (i < 0) { W->last_error = 2; RET(0); return; } RET(0xC000 + i); }
static void atom_name(Cpu *c, int wide) { u32 at = (u32)ARG(0); u64 buf = ARG(1); u32 cap = (u32)ARG(2); char tmp[32]; const char *s; if (at >= 0xC000 && at < 0xC100 && g_atom_ref[at - 0xC000]) s = g_atoms[at - 0xC000]; else if (at && at < 0xC000) { e_snprintf(tmp, sizeof tmp, "#%u", at); s = tmp; } else { W->last_error = 87; RET(0); return; }
  if (!buf || !cap) { RET(0); return; } if (wide) RET(put_w_utf8(c, buf, s, (int)cap)); else { size_t l = e_strlen(s); if (l + 1 > cap) l = cap - 1; for (size_t i = 0; i < l; i++) wr8(c, buf + i, (u8)s[i]); wr8(c, buf + l, 0); RET((u32)l); } }
static void s_GlobalAddAtomW(Cpu *c) { atom_add(c, 1); }
static void s_GlobalAddAtomA(Cpu *c) { atom_add(c, 0); }
static void s_GlobalFindAtomW(Cpu *c) { atom_find_api(c, 1); }
static void s_GlobalFindAtomA(Cpu *c) { atom_find_api(c, 0); }
static void s_GlobalDeleteAtom(Cpu *c) { u32 at = (u32)ARG(0); if (at >= 0xC000 && at < 0xC100 && g_atom_ref[at - 0xC000]) g_atom_ref[at - 0xC000]--; W->last_error = 0; RET(0); }
static void s_GlobalGetAtomNameW(Cpu *c) { atom_name(c, 1); }
static void s_GlobalGetAtomNameA(Cpu *c) { atom_name(c, 0); }
static void s_AddAtomW(Cpu *c) { atom_add(c, 1); }
static void s_AddAtomA(Cpu *c) { atom_add(c, 0); }
static void s_FindAtomW(Cpu *c) { atom_find_api(c, 1); }
static void s_FindAtomA(Cpu *c) { atom_find_api(c, 0); }
static void s_DeleteAtom(Cpu *c) { s_GlobalDeleteAtom(c); }
static void s_GetAtomNameW(Cpu *c) { atom_name(c, 1); }
static void s_GetAtomNameA(Cpu *c) { atom_name(c, 0); }
/* ---- Windows hooks: accepted and never called (the VCL installs a GetMessage/CBT hook for its own bookkeeping) ---- */
static int g_nhooks;
static void s_SetWindowsHookExW(Cpu *c) { RET(0x48000 + (++g_nhooks)); }
static void s_SetWindowsHookExA(Cpu *c) { RET(0x48000 + (++g_nhooks)); }
static void s_UnhookWindowsHookEx(Cpu *c) { RET(1); }
static void s_CallNextHookEx(Cpu *c) { RET(0); }
/* ---- keyboard layout / OEM / MDI / menus ---- */
static void s_GetKeyboardLayoutNameW(Cpu *c) { const char *s = "00000409"; for (int i = 0; i <= 8; i++) wr16(c, ARG(0) + i * 2, (u8)s[i]); RET(1); }
static void s_GetKeyboardLayoutNameA(Cpu *c) { const char *s = "00000409"; for (int i = 0; i <= 8; i++) wr8(c, ARG(0) + i, (u8)s[i]); RET(1); }
static void s_LoadKeyboardLayoutW(Cpu *c) { RET(0x04090409); }
static void s_LoadKeyboardLayoutA(Cpu *c) { RET(0x04090409); }
static void s_OemToCharBuffA(Cpu *c) { u64 s = ARG(0), d = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) wr8(c, d + i, rd8(c, s + i)); RET(1); }
static void s_CharToOemBuffA(Cpu *c) { u64 s = ARG(0), d = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) wr8(c, d + i, rd8(c, s + i)); RET(1); }
static void s_OemToCharBuffW(Cpu *c) { u64 s = ARG(0), d = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) wr16(c, d + i * 2, rd8(c, s + i)); RET(1); }
static void s_CharToOemBuffW(Cpu *c) { u64 s = ARG(0), d = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 0; i < n; i++) { u16 ch = rd16(c, s + i * 2); wr8(c, d + i, ch < 256 ? (u8)ch : '?'); } RET(1); }
static void s_DefFrameProcW(Cpu *c) { call_shim_tail(c, s_DefWindowProcW, ARG(0), ARG(2), ARG(3), ARG(4)); }
static void s_DefFrameProcA(Cpu *c) { call_shim_tail(c, s_DefWindowProcA, ARG(0), ARG(2), ARG(3), ARG(4)); }
static void s_DefMDIChildProcW(Cpu *c) { call_shim_tail(c, s_DefWindowProcW, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_DefMDIChildProcA(Cpu *c) { call_shim_tail(c, s_DefWindowProcA, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_TranslateMDISysAccel(Cpu *c) { RET(0); }
static void s_ShowOwnedPopups(Cpu *c) { RET(1); }
static void s_GetScrollBarInfo(Cpu *c) { W->last_error = 1400; RET(0); }
static void s_CreateIcon(Cpu *c) { RET(0x7B801); }   /* same pseudo icon as CreateIconIndirect: drawn as the generic app icon */
/* ---- DIB decoding at any BI_RGB depth → BGRA; used by SetDIBits / CreateDIBitmap / GetDIBits ---- */
static u32 dib_pal(Cpu *c, u64 bmi, u32 idx, int core) { u32 hs = rd32(c, bmi); if (core) { u64 e = bmi + 12 + idx * 3; return (u32)rd8(c, e) | (u32)rd8(c, e + 1) << 8 | (u32)rd8(c, e + 2) << 16; } u64 e = bmi + hs + idx * 4; return rd32(c, e) & 0xffffff; }
static int dib_decode(Cpu *c, u64 bmi, u64 bits, u32 *dst, int dw, int dh, int flip_only_rows) {
  u32 hs = rd32(c, bmi); int core = hs == 12; s32 bw = core ? (s16)rd16(c, bmi + 4) : (s32)rd32(c, bmi + 4), bh = core ? (s16)rd16(c, bmi + 6) : (s32)rd32(c, bmi + 8); u16 bpp = core ? rd16(c, bmi + 10) : rd16(c, bmi + 14); u32 comp = core ? 0 : rd32(c, bmi + 16);
  int bottom_up = bh > 0; if (bh < 0) bh = -bh; if (comp != 0 && comp != 3) return 0; (void)flip_only_rows;
  u32 rm = 0x7c00, gm = 0x3e0, bm = 0x1f; if (comp == 3 && bpp == 16) { rm = rd32(c, bmi + hs); gm = rd32(c, bmi + hs + 4); bm = rd32(c, bmi + hs + 8); }
  u64 stride = (((u64)bw * bpp + 31) / 32) * 4; const u8 *p = mem_ptr(c->mem, bits, stride * bh, 0); if (!p) return 0;
  for (int y = 0; y < dh && y < bh; y++) { const u8 *row = p + (size_t)(bottom_up ? bh - 1 - y : y) * stride; u32 *d = dst + (size_t)y * dw;
    for (int x = 0; x < dw && x < bw; x++) { u32 v;
      switch (bpp) { case 1: v = dib_pal(c, bmi, (row[x >> 3] >> (7 - (x & 7))) & 1, core); break; case 4: v = dib_pal(c, bmi, (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15, core); break; case 8: v = dib_pal(c, bmi, row[x], core); break;
        case 16: { u32 s = row[x * 2] | (row[x * 2 + 1] << 8); u32 r = rm ? (s & rm) * 255 / rm : 0, g = gm ? (s & gm) * 255 / gm : 0, b = bm ? (s & bm) * 255 / bm : 0; v = b | (g << 8) | (r << 16); break; }
        case 24: v = row[x * 3] | (row[x * 3 + 1] << 8) | (row[x * 3 + 2] << 16); break; default: v = *(const u32 *)(row + x * 4); break; }
      d[x] = v; } }
  return 1; }
static void s_SetDIBits(Cpu *c) { /* (hdc, hbm, start, lines, bits, bmi, usage) */ GdiObj *bm = gdi_get(W, ARG(1)); u64 bmi = ARG(5); if (!bm || bm->kind != K_BITMAP || !bmi) { RET(0); return; } dib_sync(W, bm);
  u32 *tmp = emu_alloc((size_t)bm->bw * bm->bh * 4); if (!tmp) { RET(0); return; } e_memset(tmp, 0, (size_t)bm->bw * bm->bh * 4);
  if (!dib_decode(c, bmi, ARG(4), tmp, bm->bw, bm->bh, 0)) { emu_free(tmp); w32_report(W, "SetDIBits: compressed DIB not supported"); RET(0); return; }
  u32 *px = (u32 *)mem_ptr(c->mem, bm->bits, (u64)bm->bw * bm->bh * 4, 1); if (px) for (int y = 0; y < bm->bh; y++) e_memcpy(px + (size_t)(bm->font_bold ? bm->bh - 1 - y : y) * bm->bw, tmp + (size_t)y * bm->bw, (size_t)bm->bw * 4);
  emu_free(tmp); RET((u32)ARG(3)); }
static void s_CreateDIBitmap(Cpu *c) { /* (hdc, bmih, flags, bits, bmi, usage) */ u64 h = ARG(1); u32 hs = rd32(c, h); int core = hs == 12; s32 bw = core ? (s16)rd16(c, h + 4) : (s32)rd32(c, h + 4), bh = core ? (s16)rd16(c, h + 6) : (s32)rd32(c, h + 8); if (bh < 0) bh = -bh;
  u64 bits = 0; u64 hb = make_bitmap(c, bw, bh, 0, &bits); if (!hb) { RET(0); return; } if ((ARG(2) & 4) && ARG(3) && ARG(4)) { u32 *px = (u32 *)mem_ptr(c->mem, bits, (u64)bw * bh * 4, 1); if (px && !dib_decode(c, ARG(4), ARG(3), px, bw, bh, 0)) w32_report(W, "CreateDIBitmap: compressed DIB not supported"); } RET(hb); }
static void s_GetDIBits(Cpu *c) { /* (hdc, hbm, start, lines, bits, bmi, usage) */ GdiObj *bm = gdi_get(W, ARG(1)); u64 bmi = ARG(5), bits = ARG(4); if (!bm || bm->kind != K_BITMAP || !bmi) { RET(0); return; } dib_sync(W, bm);
  u32 hs = rd32(c, bmi); u16 bpp = rd16(c, bmi + 14); s32 rh = (s32)rd32(c, bmi + 8);
  if (!bits) { wr32(c, bmi + 4, (u32)bm->bw); wr32(c, bmi + 8, (u32)bm->bh); wr16(c, bmi + 12, 1); if (!bpp) wr16(c, bmi + 14, 32); wr32(c, bmi + 16, 0); wr32(c, bmi + 20, (u32)(((bm->bw * (bpp ? bpp : 32) + 31) / 32) * 4 * bm->bh)); RET(bm->bh); return; }
  if (bpp != 32 && bpp != 24) { w32_report(W, "GetDIBits: %u bpp not supported", bpp); RET(0); return; } (void)hs;
  int bottom_up = rh >= 0; u32 start = (u32)ARG(2), lines = (u32)ARG(3); u64 stride = (((u64)bm->bw * bpp + 31) / 32) * 4; const u32 *px = (const u32 *)mem_ptr(c->mem, bm->bits, (u64)bm->bw * bm->bh * 4, 0); if (!px) { RET(0); return; }
  u32 done = 0; for (u32 l = 0; l < lines && start + l < (u32)bm->bh; l++) { int dibrow = (int)(start + l); int srcy = bottom_up ? bm->bh - 1 - dibrow : dibrow; if (bm->font_bold) srcy = bm->bh - 1 - srcy; const u32 *s = px + (size_t)srcy * bm->bw; u64 drow = bits + (u64)l * stride;
    for (int x = 0; x < bm->bw; x++) { u32 v = s[x]; if (bpp == 32) wr32(c, drow + x * 4, v); else { wr8(c, drow + x * 3, (u8)v); wr8(c, drow + x * 3 + 1, (u8)(v >> 8)); wr8(c, drow + x * 3 + 2, (u8)(v >> 16)); } } done++; }
  RET(done); }
static void s_GetBitmapBits(Cpu *c) { GdiObj *bm = gdi_get(W, ARG(0)); u32 n = (u32)ARG(1); u64 dst = ARG(2); if (!bm || bm->kind != K_BITMAP) { RET(0); return; } u64 total = (u64)bm->bw * bm->bh * 4; if (n > total) n = (u32)total; const u8 *p = mem_ptr(c->mem, bm->bits, total, 0); u8 *d = mem_ptr(c->mem, dst, n, 1); if (p && d) e_memcpy(d, p, n); RET(n); }
static GdiObj *dc_dib(Cpu *c, u64 hdc) { GdiObj *dc = gdi_get(W, hdc); if (!dc || dc->kind != K_DC) return 0; GdiObj *bm = gdi_get(W, dc->bitmap); return bm && bm->kind == K_BITMAP && bm->dib_section && bm->dib_pal ? bm : 0; }
static void s_GetDIBColorTable(Cpu *c) { GdiObj *bm = dc_dib(c, ARG(0)); u32 st = (u32)ARG(1), n = (u32)ARG(2); if (!bm || st >= (u32)bm->dib_ncol) { RET(0); return; } if (st + n > (u32)bm->dib_ncol) n = (u32)bm->dib_ncol - st; for (u32 i = 0; i < n; i++) wr32(c, ARG(3) + i * 4, bm->dib_pal[st + i]); RET(n); }
static void s_SetDIBColorTable(Cpu *c) { GdiObj *bm = dc_dib(c, ARG(0)); u32 st = (u32)ARG(1), n = (u32)ARG(2); if (!bm || st >= (u32)bm->dib_ncol) { RET(0); return; } if (st + n > (u32)bm->dib_ncol) n = (u32)bm->dib_ncol - st; for (u32 i = 0; i < n; i++) bm->dib_pal[st + i] = rd32(c, ARG(3) + i * 4) & 0xffffff; if (bm->dib_native_prev) { e_memset(bm->dib_native_prev, bm->dib_native_prev[0] ^ 1, 1); } RET(n); }
/* ---- GDI: curves drawn as straight segments (good enough for frames/rounded corners), metafiles rejected cleanly ---- */
static void s_Arc(Cpu *c) { RET(1); }
static void s_ArcTo(Cpu *c) { RET(1); }
static void s_AngleArc(Cpu *c) { RET(1); }
static void s_Pie(Cpu *c) { RET(1); }
static void s_Chord(Cpu *c) { RET(1); }
static void s_PolyBezier(Cpu *c) { /* polyline through the control points */ u64 pts = ARG(1); u32 n = (u32)ARG(2); if (n < 2) { RET(1); return; } call_shim(c, s_MoveToEx, ARG(0), (u64)rd32(c, pts), (u64)rd32(c, pts + 4), 0); for (u32 i = 3; i < n; i += 3) { call_shim(c, s_LineTo, ARG(0), (u64)rd32(c, pts + i * 8), (u64)rd32(c, pts + i * 8 + 4), 0); } RET(1); }
static void s_PolyBezierTo(Cpu *c) { u64 pts = ARG(1); u32 n = (u32)ARG(2); for (u32 i = 2; i < n; i += 3) call_shim(c, s_LineTo, ARG(0), (u64)rd32(c, pts + i * 8), (u64)rd32(c, pts + i * 8 + 4), 0); RET(1); }
static void s_LineDDA(Cpu *c) { RET(1); }
static void s_ExtFloodFill(Cpu *c) { RET(1); }
static void s_FloodFill(Cpu *c) { RET(1); }
static void s_MaskBlt(Cpu *c) { /* (hdcDest, x, y, w, h, hdcSrc, xs, ys, mask, xm, ym, rop) → plain BitBlt with the foreground rop */ call_shim_tail(c, s_BitBlt, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_SetRectRgn(Cpu *c) { GdiObj *r = gdi_get(W, ARG(0)); if (!r || r->kind != K_RGN) { RET(0); return; } r->npts = 0; r->rx = (s32)ARG(1); r->ry = (s32)ARG(2); r->rw = (s32)ARG(3) - r->rx; r->rh = (s32)ARG(4) - r->ry; RET(1); }
static void s_FrameRgn(Cpu *c) { RET(1); }
static void s_GetBrushOrgEx(Cpu *c) { if (ARG(1)) { wr32(c, ARG(1), 0); wr32(c, ARG(1) + 4, 0); } RET(1); }
static void s_GetCurrentPositionEx(Cpu *c) { GdiObj *dc = gdi_get(W, ARG(0)); if (!dc) { RET(0); return; } if (ARG(1)) { wr32(c, ARG(1), (u32)cur_x); wr32(c, ARG(1) + 4, (u32)cur_y); } RET(1); }
static void s_GetStretchBltMode(Cpu *c) { RET(3); }
static void s_CreatePenIndirect(Cpu *c) { u64 lp = ARG(0); call_shim_tail(c, s_CreatePen, rd32(c, lp), rd32(c, lp + 4), rd32(c, lp + 12), 0); }
static void s_GetTextExtentPointW(Cpu *c) { call_shim_tail(c, s_GetTextExtentPoint32W, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_GetTextExtentPointA(Cpu *c) { call_shim_tail(c, s_GetTextExtentPoint32A, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_EnumFontsW(Cpu *c) { RET(1); }
static void s_EnumFontsA(Cpu *c) { RET(1); }
static void s_EnumFontFamiliesExW(Cpu *c) { RET(1); }
static void s_EnumFontFamiliesExA(Cpu *c) { RET(1); }
static void s_EnumFontFamiliesW(Cpu *c) { RET(1); }
static void s_EnumFontFamiliesA(Cpu *c) { RET(1); }
static void s_AddFontResourceW(Cpu *c) { RET(1); }
static void s_AddFontResourceA(Cpu *c) { RET(1); }
static void s_RemoveFontResourceW(Cpu *c) { RET(1); }
static void s_RemoveFontResourceA(Cpu *c) { RET(1); }
static void s_CreateHalftonePalette(Cpu *c) { RET(stock[15]); }
static int g_mf_reported;
static void mf_unsupported(Cpu *c) { if (!g_mf_reported++) w32_report(W, "metafiles (EMF/WMF) are not supported — drawn as nothing"); RET(0); }
static void s_GetEnhMetaFileHeader(Cpu *c) { mf_unsupported(c); }
static void s_GetEnhMetaFileBits(Cpu *c) { mf_unsupported(c); }
static void s_SetEnhMetaFileBits(Cpu *c) { mf_unsupported(c); }
static void s_GetWinMetaFileBits(Cpu *c) { mf_unsupported(c); }
static void s_SetWinMetaFileBits(Cpu *c) { mf_unsupported(c); }
static void s_PlayEnhMetaFile(Cpu *c) { mf_unsupported(c); }
static void s_DeleteEnhMetaFile(Cpu *c) { RET(1); }
static void s_CopyEnhMetaFileW(Cpu *c) { mf_unsupported(c); }
static void s_GetEnhMetaFileDescriptionW(Cpu *c) { mf_unsupported(c); }
static void s_GetEnhMetaFilePaletteEntries(Cpu *c) { RET(0); }
static void s_CreateEnhMetaFileW(Cpu *c) { mf_unsupported(c); }
static void s_CloseEnhMetaFile(Cpu *c) { RET(0); }
/* ---- comctl32 extras ---- */
static void s_InitializeFlatSB(Cpu *c) { RET(0); }   /* "not available": the VCL falls back to the normal scroll bar API */
static void s_UninitializeFlatSB(Cpu *c) { RET(0); }
static void s_FlatSB_SetScrollInfo(Cpu *c) { call_shim_tail(c, s_SetScrollInfo, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_FlatSB_GetScrollInfo(Cpu *c) { call_shim_tail(c, s_GetScrollInfo, ARG(0), ARG(1), ARG(2), 0); }
static void s_FlatSB_SetScrollPos(Cpu *c) { call_shim_tail(c, s_SetScrollPos, ARG(0), ARG(1), ARG(2), ARG(3)); }
static void s_FlatSB_GetScrollPos(Cpu *c) { call_shim_tail(c, s_GetScrollPos, ARG(0), ARG(1), 0, 0); }
static void s_FlatSB_SetScrollProp(Cpu *c) { RET(1); }
static void s_FlatSB_SetScrollRange(Cpu *c) { RET(1); }
static void s_FlatSB_ShowScrollBar(Cpu *c) { RET(1); }
static void s_FlatSB_EnableScrollBar(Cpu *c) { RET(1); }
static void s_TrackMouseEvent_(Cpu *c) { RET(1); }   /* WM_MOUSELEAVE/hover are not generated */
/* ---- kernel32 / advapi32 / mpr / wtsapi32 / winhttp / comdlg32 ---- */
static void s_TransactNamedPipe(Cpu *c) { W->last_error = 50; RET(0); }
static void s_InitializeSecurityDescriptor(Cpu *c) { u64 sd = ARG(0); if (sd) { u8 *p = mem_ptr(c->mem, sd, 20, 1); if (p) e_memset(p, 0, 20); wr8(c, sd, 1); } RET(1); }
static void s_SetSecurityDescriptorDacl(Cpu *c) { RET(1); }
static void s_SetSecurityDescriptorOwner(Cpu *c) { RET(1); }
static void s_SetSecurityDescriptorGroup(Cpu *c) { RET(1); }
static void s_WNetOpenEnumW(Cpu *c) { RET(1222); }   /* ERROR_NO_NETWORK */
static void s_WNetEnumResourceW(Cpu *c) { RET(259); }
static void s_WNetCloseEnum(Cpu *c) { RET(0); }
static void s_WNetGetConnectionW(Cpu *c) { RET(2250); }   /* ERROR_NOT_CONNECTED */
static void s_WNetGetUniversalNameW(Cpu *c) { RET(2250); }
static void s_WNetOpenEnumA(Cpu *c) { RET(1222); }
static void s_WNetGetConnectionA(Cpu *c) { RET(2250); }
static void s_WTSRegisterSessionNotification(Cpu *c) { RET(1); }
static void s_WTSUnRegisterSessionNotification(Cpu *c) { RET(1); }
static void s_WinHttpQueryAuthSchemes(Cpu *c) { W->last_error = 12019; RET(0); }
static void s_WinHttpSetCredentials(Cpu *c) { W->last_error = 12019; RET(0); }
static void s_GetOpenFileNameW(Cpu *c) { RET(0); }   /* the file dialog is cancelled */
static void s_GetSaveFileNameW(Cpu *c) { RET(0); }
static void s_GetOpenFileNameA(Cpu *c) { RET(0); }
static void s_GetSaveFileNameA(Cpu *c) { RET(0); }
static void s_CommDlgExtendedError(Cpu *c) { RET(0); }
/* ---- ole32 / oleaut32 helpers ---- */
static void s_IsEqualGUID(Cpu *c) { u64 a = ARG(0), b = ARG(1); for (int i = 0; i < 16; i++) if (rd8(c, a + i) != rd8(c, b + i)) { RET(0); return; } RET(1); }
static void s_StringFromGUID2(Cpu *c) { u64 g = ARG(0), out = ARG(1); u32 cap = (u32)ARG(2); char s[48]; e_snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(c, g), rd16(c, g + 4), rd16(c, g + 6), rd8(c, g + 8), rd8(c, g + 9), rd8(c, g + 10), rd8(c, g + 11), rd8(c, g + 12), rd8(c, g + 13), rd8(c, g + 14), rd8(c, g + 15)); if (cap < 39) { RET(0); return; } for (int i = 0; i <= 38; i++) wr16(c, out + i * 2, (u8)s[i]); RET(39); }
static void s_StringFromCLSID(Cpu *c) { u64 g = ARG(0); char s[48]; e_snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(c, g), rd16(c, g + 4), rd16(c, g + 6), rd8(c, g + 8), rd8(c, g + 9), rd8(c, g + 10), rd8(c, g + 11), rd8(c, g + 12), rd8(c, g + 13), rd8(c, g + 14), rd8(c, g + 15)); u64 m = w32_heap_alloc(W, 80, 0); for (int i = 0; i <= 38; i++) wr16(c, m + i * 2, (u8)s[i]); wrp(c, ARG(1), m); RET(0); }
static void s_CoFreeUnusedLibraries(Cpu *c) { (void)c; }
static void s_CoDisconnectObject(Cpu *c) { RET(0); }
static void s_StgCreateDocfileOnILockBytes(Cpu *c) { RET(0x80004001); }
static void s_CreateILockBytesOnHGlobal(Cpu *c) { RET(0x80004001); }
static void s_LoadTypeLib(Cpu *c) { RET(0x80029C4A); }
static void s_LoadTypeLibEx(Cpu *c) { RET(0x80029C4A); }
static void s_RegisterTypeLib(Cpu *c) { RET(0x80029C4A); }
static void s_GetErrorInfo(Cpu *c) { if (ARG(1)) wrp(c, ARG(1), 0); RET(1); }   /* S_FALSE: no error object */
static void s_SetErrorInfo(Cpu *c) { RET(0); }
static void s_GetActiveObject(Cpu *c) { RET(0x800401E3); }   /* MK_E_UNAVAILABLE */
#define VCL_SHIMS S(SetRectEmpty), S(OffsetRect), S(InflateRect), S(IntersectRect), S(UnionRect), S(SubtractRect), \
  S(GlobalAddAtomW), S(GlobalAddAtomA), S(GlobalFindAtomW), S(GlobalFindAtomA), S(GlobalDeleteAtom), S(GlobalGetAtomNameW), S(GlobalGetAtomNameA), S(AddAtomW), S(AddAtomA), S(FindAtomW), S(FindAtomA), S(DeleteAtom), S(GetAtomNameW), S(GetAtomNameA), \
  S(SetWindowsHookExW), S(SetWindowsHookExA), S(UnhookWindowsHookEx), S(CallNextHookEx), S(GetKeyboardLayoutNameW), S(GetKeyboardLayoutNameA), S(LoadKeyboardLayoutW), S(LoadKeyboardLayoutA), \
  S(OemToCharBuffA), S(CharToOemBuffA), S(OemToCharBuffW), S(CharToOemBuffW), S(DefFrameProcW), S(DefFrameProcA), S(DefMDIChildProcW), S(DefMDIChildProcA), S(TranslateMDISysAccel), S(ShowOwnedPopups), S(GetScrollBarInfo), S(CreateIcon), \
  S(SetDIBits), S(CreateDIBitmap), S(GetDIBits), S(GetBitmapBits), S(GetDIBColorTable), S(SetDIBColorTable), \
  S(Arc), S(ArcTo), S(AngleArc), S(Pie), S(Chord), S(PolyBezier), S(PolyBezierTo), S(LineDDA), S(ExtFloodFill), S(FloodFill), S(MaskBlt), S(ExcludeClipRect), S(RectVisible), S(PtVisible), S(GetClipBox), S(GetClipRgn), S(SetRectRgn), S(FrameRgn), \
  S(GetWindowOrgEx), S(GetBrushOrgEx), S(GetCurrentPositionEx), S(GetStretchBltMode), S(CreatePenIndirect), S(GetTextExtentPointW), S(GetTextExtentPointA), S(EnumFontsW), S(EnumFontsA), S(EnumFontFamiliesExW), S(EnumFontFamiliesExA), S(EnumFontFamiliesW), S(EnumFontFamiliesA), \
  S(AddFontResourceW), S(AddFontResourceA), S(RemoveFontResourceW), S(RemoveFontResourceA), S(CreateHalftonePalette), S(GetEnhMetaFileHeader), S(GetEnhMetaFileBits), S(SetEnhMetaFileBits), S(GetWinMetaFileBits), S(SetWinMetaFileBits), S(PlayEnhMetaFile), S(DeleteEnhMetaFile), S(CopyEnhMetaFileW), S(GetEnhMetaFileDescriptionW), S(GetEnhMetaFilePaletteEntries), S(CreateEnhMetaFileW), S(CloseEnhMetaFile), \
  S(InitializeFlatSB), S(UninitializeFlatSB), S(FlatSB_SetScrollInfo), S(FlatSB_GetScrollInfo), S(FlatSB_SetScrollPos), S(FlatSB_GetScrollPos), S(FlatSB_SetScrollProp), S(FlatSB_SetScrollRange), S(FlatSB_ShowScrollBar), S(FlatSB_EnableScrollBar), SA("_TrackMouseEvent", TrackMouseEvent_), SA("TrackMouseEvent", TrackMouseEvent_), \
  \
  S(TransactNamedPipe), S(InitializeSecurityDescriptor), S(SetSecurityDescriptorDacl), S(SetSecurityDescriptorOwner), S(SetSecurityDescriptorGroup), S(WNetOpenEnumW), S(WNetEnumResourceW), S(WNetCloseEnum), S(WNetGetConnectionW), S(WNetGetUniversalNameW), S(WNetOpenEnumA), S(WNetGetConnectionA), \
  S(WTSRegisterSessionNotification), S(WTSUnRegisterSessionNotification), S(WinHttpQueryAuthSchemes), S(WinHttpSetCredentials), S(GetOpenFileNameW), S(GetSaveFileNameW), S(GetOpenFileNameA), S(GetSaveFileNameA), S(CommDlgExtendedError), \
  S(IsEqualGUID), S(StringFromGUID2), S(StringFromCLSID), S(CoFreeUnusedLibraries), S(CoDisconnectObject), S(StgCreateDocfileOnILockBytes), S(CreateILockBytesOnHGlobal), S(LoadTypeLib), S(LoadTypeLibEx), S(RegisterTypeLib), S(GetErrorInfo), S(SetErrorInfo), S(GetActiveObject)
/* ---- uxtheme: visual styles are OFF (classic look). Every query says "no theme", every draw call fails cleanly so the
   VCL/comctl fall back to classic drawing; BufferedPaint* exists but hands out no buffer (callers paint directly). ---- */
static void s_IsAppThemed(Cpu *c) { RET(0); }
static void s_IsThemeActive(Cpu *c) { RET(0); }
static void s_IsCompositionActive(Cpu *c) { RET(0); }
static void s_OpenThemeData(Cpu *c) { RET(0); }
static void s_OpenThemeDataEx(Cpu *c) { RET(0); }
static void s_OpenThemeDataForDpi(Cpu *c) { RET(0); }
static void s_CloseThemeData(Cpu *c) { RET(0); }
static void s_GetWindowTheme(Cpu *c) { RET(0); }
static void s_SetWindowTheme(Cpu *c) { RET(0); }
static void s_SetWindowThemeAttribute(Cpu *c) { RET(0); }
static void s_EnableThemeDialogTexture(Cpu *c) { RET(0); }
static void s_IsThemeDialogTextureEnabled(Cpu *c) { RET(0); }
static void s_SetThemeAppProperties(Cpu *c) { (void)c; }
static void s_GetThemeAppProperties(Cpu *c) { RET(0); }
static void s_EnableTheming(Cpu *c) { RET(0); }
static void s_IsThemePartDefined(Cpu *c) { RET(0); }
static void s_IsThemeBackgroundPartiallyTransparent(Cpu *c) { RET(0); }
static void s_GetCurrentThemeName(Cpu *c) { RET(0x80004005); }
static void theme_nohandle(Cpu *c) { RET(0x80070006); }   /* E_HANDLE: no theme handle is ever valid */
static void s_DrawThemeBackground(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeBackgroundEx(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeText(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeTextEx(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeEdge(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeIcon(Cpu *c) { theme_nohandle(c); }
static void s_DrawThemeParentBackground(Cpu *c) { RET(0); }
static void s_DrawThemeParentBackgroundEx(Cpu *c) { RET(0); }
static void s_GetThemePartSize(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeTextExtent(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeTextMetrics(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeBackgroundContentRect(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeBackgroundExtent(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeBackgroundRegion(Cpu *c) { theme_nohandle(c); }
static void s_HitTestThemeBackground(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeColor(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeMetric(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeString(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeBool(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeInt(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeEnumValue(Cpu *c) { theme_nohandle(c); }
static void s_GetThemePosition(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeFont(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeRect(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeMargins(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeIntList(Cpu *c) { theme_nohandle(c); }
static void s_GetThemePropertyOrigin(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeFilename(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeBitmap(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeStream(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeTransitionDuration(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeSysColor(Cpu *c) { call_shim_tail(c, s_GetSysColor, ARG(1), 0, 0, 0); }
static void s_GetThemeSysColorBrush(Cpu *c) { call_shim_tail(c, s_GetSysColorBrush, ARG(1), 0, 0, 0); }
static void s_GetThemeSysSize(Cpu *c) { call_shim_tail(c, s_GetSystemMetrics, ARG(1), 0, 0, 0); }
static void s_GetThemeSysBool(Cpu *c) { RET(0); }
static void s_GetThemeSysInt(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeSysFont(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeSysString(Cpu *c) { theme_nohandle(c); }
static void s_GetThemeDocumentationProperty(Cpu *c) { RET(0x80004005); }
static void s_BufferedPaintInit(Cpu *c) { RET(0); }
static void s_BufferedPaintUnInit(Cpu *c) { RET(0); }
static void s_BeginBufferedPaint(Cpu *c) { if (ARG(4)) wrp(c, ARG(4), 0); RET(0); }   /* NULL: paint straight to the target DC */
static void s_EndBufferedPaint(Cpu *c) { RET(0); }
static void s_BufferedPaintClear(Cpu *c) { RET(0); }
static void s_BufferedPaintSetAlpha(Cpu *c) { RET(0); }
static void s_BufferedPaintRenderAnimation(Cpu *c) { RET(0); }
static void s_BufferedPaintStopAllAnimations(Cpu *c) { RET(0); }
static void s_BeginBufferedAnimation(Cpu *c) { RET(0); }
static void s_EndBufferedAnimation(Cpu *c) { RET(0); }
static void s_GetBufferedPaintBits(Cpu *c) { RET(0x80004005); }
static void s_GetBufferedPaintDC(Cpu *c) { RET(0); }
static void s_GetBufferedPaintTargetDC(Cpu *c) { RET(0); }
static void s_GetBufferedPaintTargetRect(Cpu *c) { RET(0x80004005); }
/* dwmapi: no composition */
static void s_DwmIsCompositionEnabled(Cpu *c) { if (ARG(0)) wr32(c, ARG(0), 0); RET(0); }
static void s_DwmExtendFrameIntoClientArea(Cpu *c) { RET(0x80004005); }
static void s_DwmSetWindowAttribute(Cpu *c) { RET(0x80004005); }
static void s_DwmGetWindowAttribute(Cpu *c) { RET(0x80004005); }
static void s_DwmEnableBlurBehindWindow(Cpu *c) { RET(0x80004005); }
static void s_DwmGetColorizationColor(Cpu *c) { RET(0x80004005); }
static void s_DwmFlush(Cpu *c) { RET(0); }
static void s_DwmDefWindowProc(Cpu *c) { if (ARG(4)) wrp(c, ARG(4), 0); RET(0); }
#define THEME_SHIMS S(IsAppThemed), S(IsThemeActive), S(IsCompositionActive), S(OpenThemeData), S(OpenThemeDataEx), S(OpenThemeDataForDpi), S(CloseThemeData), S(GetWindowTheme), S(SetWindowTheme), S(SetWindowThemeAttribute), \
  S(EnableThemeDialogTexture), S(IsThemeDialogTextureEnabled), S(SetThemeAppProperties), S(GetThemeAppProperties), S(EnableTheming), S(IsThemePartDefined), S(IsThemeBackgroundPartiallyTransparent), S(GetCurrentThemeName), \
  S(DrawThemeBackground), S(DrawThemeBackgroundEx), S(DrawThemeText), S(DrawThemeTextEx), S(DrawThemeEdge), S(DrawThemeIcon), S(DrawThemeParentBackground), S(DrawThemeParentBackgroundEx), S(GetThemePartSize), S(GetThemeTextExtent), S(GetThemeTextMetrics), \
  S(GetThemeBackgroundContentRect), S(GetThemeBackgroundExtent), S(GetThemeBackgroundRegion), S(HitTestThemeBackground), S(GetThemeColor), S(GetThemeMetric), S(GetThemeString), S(GetThemeBool), S(GetThemeInt), S(GetThemeEnumValue), S(GetThemePosition), S(GetThemeFont), S(GetThemeRect), S(GetThemeMargins), S(GetThemeIntList), S(GetThemePropertyOrigin), S(GetThemeFilename), S(GetThemeBitmap), S(GetThemeStream), S(GetThemeTransitionDuration), \
  S(GetThemeSysColor), S(GetThemeSysColorBrush), S(GetThemeSysSize), S(GetThemeSysBool), S(GetThemeSysInt), S(GetThemeSysFont), S(GetThemeSysString), S(GetThemeDocumentationProperty), \
  S(BufferedPaintInit), S(BufferedPaintUnInit), S(BeginBufferedPaint), S(EndBufferedPaint), S(BufferedPaintClear), S(BufferedPaintSetAlpha), S(BufferedPaintRenderAnimation), S(BufferedPaintStopAllAnimations), S(BeginBufferedAnimation), S(EndBufferedAnimation), S(GetBufferedPaintBits), S(GetBufferedPaintDC), S(GetBufferedPaintTargetDC), S(GetBufferedPaintTargetRect), \
  S(DwmIsCompositionEnabled), S(DwmExtendFrameIntoClientArea), S(DwmSetWindowAttribute), S(DwmGetWindowAttribute), S(DwmEnableBlurBehindWindow), S(DwmGetColorizationColor), S(DwmFlush), S(DwmDefWindowProc)
/* ---- DPI awareness (everything is 96 dpi, context = "unaware" pseudo handle -1), message filter, touch gestures ---- */
static void s_GetWindowDpiAwarenessContext(Cpu *c) { RET(c->mode32 ? 0xffffffffull : ~0ull); }
static void s_GetThreadDpiAwarenessContext(Cpu *c) { RET(c->mode32 ? 0xffffffffull : ~0ull); }
static void s_SetThreadDpiAwarenessContext(Cpu *c) { RET(c->mode32 ? 0xffffffffull : ~0ull); }
static void s_GetAwarenessFromDpiAwarenessContext(Cpu *c) { RET(0); }
static void s_GetDpiFromDpiAwarenessContext(Cpu *c) { RET(96); }
static void s_AreDpiAwarenessContextsEqual(Cpu *c) { RET((u32)ARG(0) == (u32)ARG(1)); }
static void s_IsValidDpiAwarenessContext(Cpu *c) { RET(1); }
static void s_EnableNonClientDpiScaling(Cpu *c) { RET(1); }
static void s_SetProcessDpiAwareness(Cpu *c) { RET(0); }
static void s_GetProcessDpiAwareness(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(0); }
static void s_IsProcessDPIAware(Cpu *c) { RET(0); }
static void s_ChangeWindowMessageFilter(Cpu *c) { RET(1); }
static void s_ChangeWindowMessageFilterEx(Cpu *c) { RET(1); }
static void s_SetGestureConfig(Cpu *c) { RET(1); }
static void s_GetGestureConfig(Cpu *c) { RET(1); }
static void s_CloseGestureInfoHandle(Cpu *c) { RET(1); }
static void s_GetGestureInfo(Cpu *c) { RET(0); }
static void s_RegisterTouchWindow(Cpu *c) { RET(1); }
static void s_UnregisterTouchWindow(Cpu *c) { RET(1); }
static void s_IsTouchWindow(Cpu *c) { RET(0); }
static void s_GetTouchInputInfo(Cpu *c) { RET(0); }
static void s_CloseTouchInputHandle(Cpu *c) { RET(1); }
static void s_GetPointerType(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 4); RET(1); }   /* PT_MOUSE */
#define DPI_SHIMS S(GetWindowDpiAwarenessContext), S(GetThreadDpiAwarenessContext), S(SetThreadDpiAwarenessContext), S(GetAwarenessFromDpiAwarenessContext), S(GetDpiFromDpiAwarenessContext), S(AreDpiAwarenessContextsEqual), S(IsValidDpiAwarenessContext), S(EnableNonClientDpiScaling), \
  S(SetProcessDpiAwareness), S(GetProcessDpiAwareness), S(IsProcessDPIAware), S(ChangeWindowMessageFilter), S(ChangeWindowMessageFilterEx), S(SetGestureConfig), S(GetGestureConfig), S(CloseGestureInfoHandle), S(GetGestureInfo), S(RegisterTouchWindow), S(UnregisterTouchWindow), S(IsTouchWindow), S(GetTouchInputInfo), S(CloseTouchInputHandle), S(GetPointerType)

/* imm32: no input-method editor — every query says "no IME / no context" (Delphi VCL delay-loads these for TEdit/TMemo IME support) */
static void s_ImmAssociateContextEx(Cpu *c) { RET(1); }
static void s_ImmReleaseContext(Cpu *c) { RET(1); }
static void s_ImmNotifyIME(Cpu *c) { RET(1); }
static void s_ImmSetOpenStatus(Cpu *c) { RET(1); }
static void s_ImmDestroyContext(Cpu *c) { RET(1); }
static void s_ImmDisableIME(Cpu *c) { RET(1); }
static void s_ImmDisableTextFrameService(Cpu *c) { RET(1); }
static void s_ImmDisableLegacyIME(Cpu *c) { RET(1); }
static void s_ImmGetContext(Cpu *c) { RET(0); }
static void s_ImmSetCompositionWindow(Cpu *c) { RET(0); }
static void s_ImmSetCompositionFontW(Cpu *c) { RET(0); }
static void s_ImmSetCompositionFontA(Cpu *c) { RET(0); }
static void s_ImmGetCompositionStringW(Cpu *c) { RET(0); }
static void s_ImmGetCompositionStringA(Cpu *c) { RET(0); }
static void s_ImmIsIME(Cpu *c) { RET(0); }
static void s_ImmGetOpenStatus(Cpu *c) { RET(0); }
static void s_ImmGetConversionStatus(Cpu *c) { RET(0); }
static void s_ImmSetConversionStatus(Cpu *c) { RET(0); }
static void s_ImmGetDefaultIMEWnd(Cpu *c) { RET(0); }
static void s_ImmAssociateContext(Cpu *c) { RET(0); }
static void s_ImmSetCandidateWindow(Cpu *c) { RET(0); }
static void s_ImmGetVirtualKey(Cpu *c) { RET(0); }
static void s_ImmEscapeW(Cpu *c) { RET(0); }
static void s_ImmEscapeA(Cpu *c) { RET(0); }
static void s_ImmGetProperty(Cpu *c) { RET(0); }
static void s_ImmCreateContext(Cpu *c) { RET(0); }
static void s_ImmGetIMEFileNameW(Cpu *c) { RET(0); }
static void s_ImmGetIMEFileNameA(Cpu *c) { RET(0); }
static void s_ImmInstallIMEW(Cpu *c) { RET(0); }
static void s_ImmSimulateHotKey(Cpu *c) { RET(0); }
static void s_ImmGetCandidateListW(Cpu *c) { RET(0); }
static void s_ImmGetCandidateListA(Cpu *c) { RET(0); }
static void s_ImmGetCandidateListCountW(Cpu *c) { RET(0); }
static void s_ImmGetCandidateListCountA(Cpu *c) { RET(0); }
static void s_ImmSetStatusWindowPos(Cpu *c) { RET(0); }
static void s_ImmGetStatusWindowPos(Cpu *c) { RET(0); }
static void s_ImmConfigureIMEW(Cpu *c) { RET(0); }
static void s_ImmEnumRegisterWordW(Cpu *c) { RET(0); }
static void s_ImmGetRegisterWordStyleW(Cpu *c) { RET(0); }
static void s_ImmRegisterWordW(Cpu *c) { RET(0); }
static void s_ImmUnregisterWordW(Cpu *c) { RET(0); }
static void s_ImmGetGuideLineW(Cpu *c) { RET(0); }
static void s_ImmGetDescriptionW(Cpu *c) { RET(0); }
static void s_ImmGetDescriptionA(Cpu *c) { RET(0); }
static void s_ImmGetCompositionFontW(Cpu *c) { RET(0); }
static void s_ImmGetCompositionFontA(Cpu *c) { RET(0); }
static void s_ImmGetCompositionWindow(Cpu *c) { RET(0); }
static void s_ImmGetCandidateWindow(Cpu *c) { RET(0); }
static void s_ImmGetHotKey(Cpu *c) { RET(0); }
static void s_ImmSetHotKey(Cpu *c) { RET(0); }
static void s_ImmGenerateMessage(Cpu *c) { RET(0); }
static void s_ImmGetImeMenuItemsW(Cpu *c) { RET(0); }
static void s_ImmGetImeMenuItemsA(Cpu *c) { RET(0); }
static void s_ImmSetCompositionStringW(Cpu *c) { RET(0); }
static void s_ImmSetCompositionStringA(Cpu *c) { RET(0); }
static void s_ImmIsUIMessageW(Cpu *c) { RET(0); }
static void s_ImmIsUIMessageA(Cpu *c) { RET(0); }
static void s_ImmGetIMCLockCount(Cpu *c) { RET(0); }
static void s_ImmLockIMC(Cpu *c) { RET(0); }
static void s_ImmUnlockIMC(Cpu *c) { RET(0); }
static void s_ImmGetIMCCSize(Cpu *c) { RET(0); }
static void s_ImmLockIMCC(Cpu *c) { RET(0); }
static void s_ImmUnlockIMCC(Cpu *c) { RET(0); }
static void s_ImmCreateIMCC(Cpu *c) { RET(0); }
static void s_ImmDestroyIMCC(Cpu *c) { RET(0); }
static void s_ImmReSizeIMCC(Cpu *c) { RET(0); }
#define IMM_SHIMS S(ImmAssociateContextEx), S(ImmReleaseContext), S(ImmNotifyIME), S(ImmSetOpenStatus), S(ImmDestroyContext), S(ImmDisableIME), S(ImmDisableTextFrameService), S(ImmDisableLegacyIME), S(ImmGetContext), S(ImmSetCompositionWindow), S(ImmSetCompositionFontW), S(ImmSetCompositionFontA), S(ImmGetCompositionStringW), S(ImmGetCompositionStringA), S(ImmIsIME), S(ImmGetOpenStatus), S(ImmGetConversionStatus), S(ImmSetConversionStatus), S(ImmGetDefaultIMEWnd), S(ImmAssociateContext), S(ImmSetCandidateWindow), S(ImmGetVirtualKey), S(ImmEscapeW), S(ImmEscapeA), S(ImmGetProperty), S(ImmCreateContext), S(ImmGetIMEFileNameW), S(ImmGetIMEFileNameA), S(ImmInstallIMEW), S(ImmSimulateHotKey), S(ImmGetCandidateListW), S(ImmGetCandidateListA), S(ImmGetCandidateListCountW), S(ImmGetCandidateListCountA), S(ImmSetStatusWindowPos), S(ImmGetStatusWindowPos), S(ImmConfigureIMEW), S(ImmEnumRegisterWordW), S(ImmGetRegisterWordStyleW), S(ImmRegisterWordW), S(ImmUnregisterWordW), S(ImmGetGuideLineW), S(ImmGetDescriptionW), S(ImmGetDescriptionA), S(ImmGetCompositionFontW), S(ImmGetCompositionFontA), S(ImmGetCompositionWindow), S(ImmGetCandidateWindow), S(ImmGetHotKey), S(ImmSetHotKey), S(ImmGenerateMessage), S(ImmGetImeMenuItemsW), S(ImmGetImeMenuItemsA), S(ImmSetCompositionStringW), S(ImmSetCompositionStringA), S(ImmIsUIMessageW), S(ImmIsUIMessageA), S(ImmGetIMCLockCount), S(ImmLockIMC), S(ImmUnlockIMC), S(ImmGetIMCCSize), S(ImmLockIMCC), S(ImmUnlockIMCC), S(ImmCreateIMCC), S(ImmDestroyIMCC), S(ImmReSizeIMCC)
