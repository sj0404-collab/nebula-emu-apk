/* WebGL2 back end for the emulator's Direct3D 11 layer.
 *
 * The guest's D3D11 calls are turned into GLSL ES 3.00 shaders + one `GfxCmd` per draw inside the wasm module
 * (emu/x64/d3d11.inc, emu/x64/dxbc.c).  This file owns the GL objects and executes those commands on an offscreen
 * WebGL2 context; rendered targets are read back into the guest's back-buffer memory on Present, so the existing
 * window presenter keeps working unchanged.  Word layout of a command: emu/x64/gfx_cmd.h. */

const GC = {
  VS: 0, PS: 1, TOPOLOGY: 2, INDEXED: 3, COUNT: 4, START: 5, BASE_VERTEX: 6, INSTANCES: 7,
  IB: 8, IB_FMT: 9, IB_OFFSET: 10, NVB: 11, VB: 12, NLAYOUT: 24, LAYOUT: 25,
  VS_CB: 105, PS_CB: 109, PS_TEX: 113, PS_SAMP: 121, RTV: 129, DSV: 130, VIEWPORT: 131,
  SCISSOR_ON: 137, SCISSOR: 138, BLEND: 142, DEPTH: 150, CULL: 153, FRONT_CCW: 154, FILL: 155, TARGET_W: 156, TARGET_H: 157, WORDS: 160,
} as const;

interface Tex { tex: WebGLTexture | null; depth: WebGLRenderbuffer | null; fbo: WebGLFramebuffer | null; w: number; h: number; fmt: number; isDepth: boolean }

/** DXGI_FORMAT → vertex attribute description */
function attribFormat(fmt: number): { size: number; type: number; norm: boolean; int: boolean } | null {
  const F = WebGL2RenderingContext;
  switch (fmt) {
    case 2: return { size: 4, type: F.FLOAT, norm: false, int: false };            // R32G32B32A32_FLOAT
    case 6: return { size: 3, type: F.FLOAT, norm: false, int: false };            // R32G32B32_FLOAT
    case 16: return { size: 2, type: F.FLOAT, norm: false, int: false };           // R32G32_FLOAT
    case 41: return { size: 1, type: F.FLOAT, norm: false, int: false };           // R32_FLOAT
    case 10: return { size: 4, type: F.HALF_FLOAT, norm: false, int: false };      // R16G16B16A16_FLOAT
    case 34: return { size: 2, type: F.HALF_FLOAT, norm: false, int: false };      // R16G16_FLOAT
    case 28: case 29: return { size: 4, type: F.UNSIGNED_BYTE, norm: true, int: false };   // R8G8B8A8_UNORM(_SRGB)
    case 31: return { size: 4, type: F.BYTE, norm: true, int: false };             // R8G8B8A8_SNORM
    case 30: return { size: 4, type: F.UNSIGNED_BYTE, norm: false, int: true };    // R8G8B8A8_UINT
    case 49: return { size: 2, type: F.UNSIGNED_BYTE, norm: true, int: false };    // R8G8_UNORM
    case 61: return { size: 1, type: F.UNSIGNED_BYTE, norm: true, int: false };    // R8_UNORM
    case 11: return { size: 4, type: F.UNSIGNED_SHORT, norm: true, int: false };   // R16G16B16A16_UNORM
    case 13: return { size: 4, type: F.SHORT, norm: true, int: false };            // R16G16B16A16_SNORM
    case 35: return { size: 2, type: F.UNSIGNED_SHORT, norm: true, int: false };   // R16G16_UNORM
    case 37: return { size: 2, type: F.SHORT, norm: true, int: false };            // R16G16_SNORM
    case 42: return { size: 1, type: F.UNSIGNED_INT, norm: false, int: true };     // R32_UINT
    case 43: return { size: 1, type: F.INT, norm: false, int: true };              // R32_SINT
    case 87: case 91: return { size: 4, type: F.UNSIGNED_BYTE, norm: true, int: false };   // B8G8R8A8_UNORM (channel order fixed in the shader? no — rare as a vertex format)
    default: return null;
  }
}
const BLEND = [0, 0x0, 0x1, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307, 0x308, 0x309, 0x8001, 0x8002, 0, 0x8003, 0x8004, 0x300, 0x301, 0x300, 0x301];
// D3D11_BLEND: 1 ZERO 2 ONE 3 SRC_COLOR 4 INV_SRC_COLOR 5 SRC_ALPHA 6 INV_SRC_ALPHA 7 DEST_ALPHA 8 INV_DEST_ALPHA 9 DEST_COLOR 10 INV_DEST_COLOR 11 SRC_ALPHA_SAT 14 BLEND_FACTOR 15 INV_BLEND_FACTOR 16 SRC1_COLOR …
const BLEND_OP = [0, 0x8006, 0x800a, 0x800b, 0x8007, 0x8008];   // ADD, SUBTRACT, REV_SUBTRACT, MIN, MAX
const CMP = [0, 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207];   // NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS

export class WebGLGfx {
  private gl: WebGL2RenderingContext | null = null;
  private shaders = new Map<number, WebGLShader>();
  private programs = new Map<string, WebGLProgram>();
  private buffers = new Map<number, WebGLBuffer>();
  private textures = new Map<number, Tex>();
  private nextShader = 1;
  private vao: WebGLVertexArrayObject | null = null;
  private attribEnabled: boolean[] = [];
  log: ((text: string) => void) | null = null;
  draws = 0;

  /** 1 when WebGL2 is available (what the guest sees as "GPU present") */
  init(): number {
    if (this.gl) return 1;
    try {
      const canvas = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(16, 16) : document.createElement('canvas');
      const gl = canvas.getContext('webgl2', { antialias: false, depth: false, preserveDrawingBuffer: false, premultipliedAlpha: false }) as WebGL2RenderingContext | null;
      if (!gl) { this.log?.('[gfx] WebGL2 is not available in this browser: D3D11 draw calls will stop the program\n'); return 0; }
      this.gl = gl;
      this.vao = gl.createVertexArray(); gl.bindVertexArray(this.vao);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1); gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      this.log?.(`[gfx] WebGL2 ready: ${gl.getParameter(gl.RENDERER)}\n`);
      return 1;
    } catch (e) { this.log?.(`[gfx] WebGL2 init failed: ${String(e)}\n`); return 0; }
  }

  shader(kind: number, glsl: string): number {
    const gl = this.gl!; const sh = gl.createShader(kind ? gl.VERTEX_SHADER : gl.FRAGMENT_SHADER)!;
    gl.shaderSource(sh, glsl); gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      this.log?.(`[gfx] ${kind ? 'vertex' : 'fragment'} shader failed to compile: ${gl.getShaderInfoLog(sh)}\n${glsl}\n`);
      gl.deleteShader(sh); return -1;
    }
    const id = this.nextShader++; this.shaders.set(id, sh); return id;
  }

  private program(vs: number, ps: number): WebGLProgram | null {
    const key = `${vs}:${ps}`; let p = this.programs.get(key); if (p) return p;
    const gl = this.gl!; const v = this.shaders.get(vs), f = this.shaders.get(ps); if (!v || !f) return null;
    p = gl.createProgram()!; gl.attachShader(p, v); gl.attachShader(p, f); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) { this.log?.(`[gfx] program link failed: ${gl.getProgramInfoLog(p)}\n`); gl.deleteProgram(p); return null; }
    gl.useProgram(p);
    for (let i = 0; i < 4; i++) { const bv = gl.getUniformBlockIndex(p, `vs_cb${i}`); if (bv !== gl.INVALID_INDEX) gl.uniformBlockBinding(p, bv, i); const bp = gl.getUniformBlockIndex(p, `ps_cb${i}`); if (bp !== gl.INVALID_INDEX) gl.uniformBlockBinding(p, bp, 4 + i); }
    for (let i = 0; i < 16; i++) { const loc = gl.getUniformLocation(p, `t${i}`); if (loc) gl.uniform1i(loc, i); }
    this.programs.set(key, p); return p;
  }

  buffer(id: number, data: Uint8Array | null, bind: number): void {
    const gl = this.gl!; let b = this.buffers.get(id); if (!b) { b = gl.createBuffer()!; this.buffers.set(id, b); }
    // WebGL forbids re-targeting: a buffer first bound as ELEMENT_ARRAY_BUFFER stays an index buffer, so pick the target from the D3D bind flags
    const target = bind & 0x2 ? gl.ELEMENT_ARRAY_BUFFER : bind & 0x4 ? gl.UNIFORM_BUFFER : gl.ARRAY_BUFFER;
    gl.bindBuffer(target, b);
    // uniform blocks need a multiple of 16 bytes; pad the upload
    const n = data ? data.length : 16; const padded = (n + 15) & ~15;
    if (data && padded === n) gl.bufferData(target, data, gl.DYNAMIC_DRAW);
    else { const tmp = new Uint8Array(padded); if (data) tmp.set(data); gl.bufferData(target, tmp, gl.DYNAMIC_DRAW); }
  }

  texture(id: number, w: number, h: number, fmt: number, px: Uint8Array | null, pitch: number): void {
    const gl = this.gl!; const isDepth = fmt === 45 || fmt === 40 || fmt === 20 || fmt === 44 || fmt === 39 || fmt === 55 || fmt === 54;
    let t = this.textures.get(id);
    if (t && (t.w !== w || t.h !== h || t.isDepth !== isDepth)) { this.free(id); t = undefined; }
    if (!t) { t = { tex: null, depth: null, fbo: null, w, h, fmt, isDepth }; this.textures.set(id, t); }
    if (isDepth) { if (!t.depth) { t.depth = gl.createRenderbuffer(); gl.bindRenderbuffer(gl.RENDERBUFFER, t.depth); gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, w, h); } return; }
    if (!t.tex) { t.tex = gl.createTexture(); gl.activeTexture(gl.TEXTURE15); gl.bindTexture(gl.TEXTURE_2D, t.tex); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE); }
    gl.activeTexture(gl.TEXTURE15); gl.bindTexture(gl.TEXTURE_2D, t.tex);
    // upload: convert to tightly packed RGBA8 / R8 / RGBA32F
    if (fmt === 2) { const src = px ? new Float32Array(px.buffer, px.byteOffset, w * h * 4) : null; gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, w, h, 0, gl.RGBA, gl.FLOAT, src); return; }
    if (fmt === 61 || fmt === 65) { const out = px ? this.repack(px, w, h, pitch || w, 1) : null; gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, w, h, 0, gl.RED, gl.UNSIGNED_BYTE, out); return; }
    if (fmt === 49) { const out = px ? this.repack(px, w, h, pitch || w * 2, 2) : null; gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG8, w, h, 0, gl.RG, gl.UNSIGNED_BYTE, out); return; }
    let out = px ? this.repack(px, w, h, pitch || w * 4, 4) : null;
    if (out && (fmt === 87 || fmt === 88 || fmt === 91 || fmt === 90)) { const o = out === px ? new Uint8Array(out) : out; for (let i = 0; i < o.length; i += 4) { const b = o[i]; o[i] = o[i + 2]; o[i + 2] = b; } out = o; }   // BGRA → RGBA
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, out);
  }
  private repack(px: Uint8Array, w: number, h: number, pitch: number, bpp: number): Uint8Array {
    const row = w * bpp; if (pitch === row) return px.subarray(0, row * h);
    const out = new Uint8Array(row * h); for (let y = 0; y < h; y++) out.set(px.subarray(y * pitch, y * pitch + row), y * row); return out;
  }

  private bindTarget(rtv: number, dsv: number): boolean {
    const gl = this.gl!; const t = this.textures.get(rtv); if (!t || !t.tex) return false;
    if (!t.fbo) { t.fbo = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, t.fbo); gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t.tex, 0); }
    else gl.bindFramebuffer(gl.FRAMEBUFFER, t.fbo);
    const d = dsv ? this.textures.get(dsv) : undefined;
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, d?.depth ?? null);
    return gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
  }

  clear(id: number, rgba: Float32Array): void {
    const gl = this.gl!; if (!this.bindTarget(id, 0)) return;
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); gl.clearColor(rgba[0], rgba[1], rgba[2], rgba[3]); gl.clear(gl.COLOR_BUFFER_BIT);
  }
  clearDepth(id: number, depth: number): void {
    const gl = this.gl!; const d = this.textures.get(id); if (!d?.depth) return;
    // clear through any framebuffer that has this depth buffer attached: use a scratch FBO
    const fbo = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fbo); gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, d.depth);
    gl.disable(gl.SCISSOR_TEST); gl.depthMask(true); gl.clearDepth(depth); gl.clear(gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT); gl.deleteFramebuffer(fbo);
  }

  draw(cmd: Uint32Array): void {
    const gl = this.gl!; const f = new Float32Array(cmd.buffer, cmd.byteOffset, GC.WORDS);
    const p = this.program(cmd[GC.VS], cmd[GC.PS]); if (!p) return;
    if (!this.bindTarget(cmd[GC.RTV], cmd[GC.DSV])) { this.log?.('[gfx] draw: incomplete framebuffer\n'); return; }
    gl.useProgram(p);
    const th = cmd[GC.TARGET_H];
    // viewport: D3D is top-left origin, GL bottom-left; the vertex shader flips y so rows already match → flip the viewport rectangle too
    const vx = f[GC.VIEWPORT], vy = f[GC.VIEWPORT + 1], vw = f[GC.VIEWPORT + 2], vh = f[GC.VIEWPORT + 3];
    gl.viewport(Math.round(vx), Math.round(th - vy - vh), Math.max(0, Math.round(vw)), Math.max(0, Math.round(vh)));
    gl.depthRange(f[GC.VIEWPORT + 4], f[GC.VIEWPORT + 5]);
    if (cmd[GC.SCISSOR_ON]) { const l = cmd[GC.SCISSOR] | 0, t = cmd[GC.SCISSOR + 1] | 0, r = cmd[GC.SCISSOR + 2] | 0, b = cmd[GC.SCISSOR + 3] | 0; gl.enable(gl.SCISSOR_TEST); gl.scissor(l, th - b, Math.max(0, r - l), Math.max(0, b - t)); } else gl.disable(gl.SCISSOR_TEST);
    // blend
    if (cmd[GC.BLEND]) { gl.enable(gl.BLEND); gl.blendFuncSeparate(BLEND[cmd[GC.BLEND + 1]] ?? gl.ONE, BLEND[cmd[GC.BLEND + 2]] ?? gl.ZERO, BLEND[cmd[GC.BLEND + 4]] ?? gl.ONE, BLEND[cmd[GC.BLEND + 5]] ?? gl.ZERO); gl.blendEquationSeparate(BLEND_OP[cmd[GC.BLEND + 3]] ?? gl.FUNC_ADD, BLEND_OP[cmd[GC.BLEND + 6]] ?? gl.FUNC_ADD); } else gl.disable(gl.BLEND);
    const wm = cmd[GC.BLEND + 7]; gl.colorMask(!!(wm & 1), !!(wm & 2), !!(wm & 4), !!(wm & 8));
    // depth
    if (cmd[GC.DEPTH]) { gl.enable(gl.DEPTH_TEST); gl.depthMask(!!cmd[GC.DEPTH + 1]); gl.depthFunc(CMP[cmd[GC.DEPTH + 2]] ?? gl.LESS); } else gl.disable(gl.DEPTH_TEST);
    // rasterizer: D3D CullMode 1 NONE 2 FRONT 3 BACK; the y flip in the vertex shader turns D3D clockwise into GL counter-clockwise
    const cull = cmd[GC.CULL]; if (cull === 2 || cull === 3) { gl.enable(gl.CULL_FACE); gl.cullFace(cull === 3 ? gl.BACK : gl.FRONT); } else gl.disable(gl.CULL_FACE);
    gl.frontFace(cmd[GC.FRONT_CCW] ? gl.CW : gl.CCW);
    // constant buffers
    for (let i = 0; i < 4; i++) { gl.bindBufferBase(gl.UNIFORM_BUFFER, i, this.buffers.get(cmd[GC.VS_CB + i]) ?? null); gl.bindBufferBase(gl.UNIFORM_BUFFER, 4 + i, this.buffers.get(cmd[GC.PS_CB + i]) ?? null); }
    // textures + samplers
    for (let i = 0; i < 8; i++) { const t = this.textures.get(cmd[GC.PS_TEX + i]); gl.activeTexture(gl.TEXTURE0 + i); gl.bindTexture(gl.TEXTURE_2D, t?.tex ?? null);
      if (t?.tex) { const s = cmd[GC.PS_SAMP + i]; const filter = s & 0xff, au = (s >> 8) & 0xff, av = (s >> 16) & 0xff; const point = filter === 0 || filter === 0x80;
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, point ? gl.NEAREST : gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, point ? gl.NEAREST : gl.LINEAR);
        const wrap = (m: number) => m === 1 ? gl.REPEAT : m === 2 ? gl.MIRRORED_REPEAT : gl.CLAMP_TO_EDGE; gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap(au)); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap(av)); } }
    // vertex attributes
    const used: boolean[] = [];
    for (let i = 0; i < cmd[GC.NLAYOUT]; i++) {
      const L = GC.LAYOUT + 5 * i; const loc = cmd[L], fmt = cmd[L + 1], slot = cmd[L + 2], off = cmd[L + 3], perInst = cmd[L + 4];
      const af = attribFormat(fmt); const vb = this.buffers.get(cmd[GC.VB + 3 * slot]); if (!af || !vb) { if (!af) this.log?.(`[gfx] draw: unsupported vertex format DXGI ${fmt}\n`); continue; }
      gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.enableVertexAttribArray(loc); used[loc] = true;
      const stride = cmd[GC.VB + 3 * slot + 1], base = cmd[GC.VB + 3 * slot + 2] + off;
      if (af.int) gl.vertexAttribIPointer(loc, af.size, af.type, stride, base); else gl.vertexAttribPointer(loc, af.size, af.type, af.norm, stride, base);
      gl.vertexAttribDivisor(loc, perInst ? 1 : 0);
    }
    for (let loc = 0; loc < 16; loc++) { if (!used[loc] && this.attribEnabled[loc]) gl.disableVertexAttribArray(loc); if (!used[loc]) gl.vertexAttrib4f(loc, 0, 0, 0, 1); }
    this.attribEnabled = used;
    // topology: D3D 1 POINTLIST 2 LINELIST 3 LINESTRIP 4 TRIANGLELIST 5 TRIANGLESTRIP
    const topo = cmd[GC.TOPOLOGY]; const mode = topo === 1 ? gl.POINTS : topo === 2 ? gl.LINES : topo === 3 ? gl.LINE_STRIP : topo === 5 ? gl.TRIANGLE_STRIP : gl.TRIANGLES;
    const count = cmd[GC.COUNT];
    if (cmd[GC.INDEXED]) {
      const ib = this.buffers.get(cmd[GC.IB]); if (!ib) return; gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
      const u32 = cmd[GC.IB_FMT] === 42; const bytes = u32 ? 4 : 2; const offset = cmd[GC.IB_OFFSET] + cmd[GC.START] * bytes; const base = cmd[GC.BASE_VERTEX] | 0;
      if (base) this.log?.(`[gfx] DrawIndexed with BaseVertexLocation ${base}: WebGL2 has no base vertex, drawing without it\n`);
      gl.drawElements(mode, count, u32 ? gl.UNSIGNED_INT : gl.UNSIGNED_SHORT, offset);
    } else gl.drawArrays(mode, cmd[GC.START], count);
    this.draws++;
    const err = gl.getError(); if (err) this.log?.(`[gfx] GL error 0x${err.toString(16)} in draw ${this.draws}\n`);
  }

  /** GPU target → BGRA rows, top row first, into guest memory */
  readback(id: number, out: Uint8Array): void {
    const gl = this.gl!; const t = this.textures.get(id); if (!t?.tex || !this.bindTarget(id, 0)) return;
    gl.readPixels(0, 0, t.w, t.h, gl.RGBA, gl.UNSIGNED_BYTE, out);   // the vertex-shader y flip makes GL row 0 the D3D top row
    for (let i = 0; i < out.length; i += 4) { const r = out[i]; out[i] = out[i + 2]; out[i + 2] = r; }
  }

  size(id: number): { w: number; h: number } | null { const t = this.textures.get(id); return t ? { w: t.w, h: t.h } : null; }

  free(id: number): void {
    const gl = this.gl; if (!gl) return;
    const b = this.buffers.get(id); if (b) { gl.deleteBuffer(b); this.buffers.delete(id); }
    const t = this.textures.get(id); if (t) { if (t.tex) gl.deleteTexture(t.tex); if (t.depth) gl.deleteRenderbuffer(t.depth); if (t.fbo) gl.deleteFramebuffer(t.fbo); this.textures.delete(id); }
  }
}
