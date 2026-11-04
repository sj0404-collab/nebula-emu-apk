/* gfx_cmd.h — the draw command passed from the D3D11 layer to the host GPU back end (u32 words; floats as bit patterns).
 * Mirrored by src/lib/x64/gfx.ts. */
#ifndef NEBULA_GFX_CMD_H
#define NEBULA_GFX_CMD_H
enum {
  GC_VS = 0, GC_PS, GC_TOPOLOGY, GC_INDEXED, GC_COUNT, GC_START, GC_BASE_VERTEX, GC_INSTANCES,   /* 0..7 */
  GC_IB = 8, GC_IB_FMT, GC_IB_OFFSET,                                                          /* 8..10 */
  GC_NVB = 11, GC_VB = 12,            /* 4 × {id, stride, offset} → 12..23 */
  GC_NLAYOUT = 24, GC_LAYOUT = 25,    /* 16 × {location, format, slot, offset, per_instance} → 25..104 */
  GC_VS_CB = 105,                     /* 4 ids (0 = none) */
  GC_PS_CB = 109,                     /* 4 ids */
  GC_PS_TEX = 113,                    /* 8 texture ids */
  GC_PS_SAMP = 121,                   /* 8 × (filter | addrU << 8 | addrV << 16) */
  GC_RTV = 129, GC_DSV = 130,
  GC_VIEWPORT = 131,                  /* 6 floats */
  GC_SCISSOR_ON = 137, GC_SCISSOR = 138,   /* 4 ints */
  GC_BLEND = 142,                     /* enable, src, dst, op, srcA, dstA, opA, writemask */
  GC_DEPTH = 150,                     /* enable, write, func */
  GC_CULL = 153, GC_FRONT_CCW = 154, GC_FILL = 155,
  GC_TARGET_W = 156, GC_TARGET_H = 157,
  GFX_CMD_WORDS = 160
};
#endif
