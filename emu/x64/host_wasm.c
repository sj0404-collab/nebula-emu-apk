/* wasm64 host glue: imports provided by the JS side (src/lib/x64/runtime.ts) */
#include "emu.h"
__attribute__((import_module("env"), import_name("host_write"))) void js_write(int fd, const char *p, size_t n);
__attribute__((import_module("env"), import_name("host_now_ms"))) double js_now_ms(void);
__attribute__((import_module("env"), import_name("host_present"))) void js_present(const u32 *bgra, int w, int h);
__attribute__((import_module("env"), import_name("host_math"))) double js_math(int op, double a, double b);
void host_write(int fd, const char *p, size_t n) { js_write(fd, p, n); }
double host_now_ms(void) { return js_now_ms(); }
__attribute__((import_module("env"), import_name("host_wall_ms"))) double js_wall_ms(void);
double host_wall_ms(void) { return js_wall_ms(); }
void host_present(const u32 *bgra, int w, int h) { js_present(bgra, w, h); }
int host_key(int vk) { (void)vk; return 0; }
void host_exit(int code) { (void)code; }
void host_mem_discard(void *p, size_t n) { e_memset(p, 0, n); }   /* linear memory cannot shrink; keep the zero-on-recommit semantics */
double host_math(int op, double a, double b) { return js_math(op, a, b); }
int host_fs_op(int op, const char *a, const char *b, u8 **data, u64 *size) { (void)op; (void)a; (void)b; (void)data; (void)size; return 0; }   /* browser: no write-back mounts (yet) */
/* networking: no raw TCP from a browser page — every call fails cleanly (WSAENETDOWN); a WebSocket relay can plug in here later */
int host_net_resolve(const char *name, u8 *ip4s, int max) { (void)name; (void)ip4s; (void)max; return -1; }
int host_net_connect(const u8 *ip4, int port, int *wsa_err) { (void)ip4; (void)port; if (wsa_err) *wsa_err = 10050; return -1; }
int host_net_listen(const u8 *ip4, int port, int backlog, int *wsa_err) { (void)ip4; (void)port; (void)backlog; if (wsa_err) *wsa_err = 10050; return -1; }
int host_net_accept(int s, u8 *peer_ip4, int *peer_port) { (void)s; (void)peer_ip4; (void)peer_port; return -3; }
int host_net_status(int s) { (void)s; return -10050; }
s64 host_net_read(int s, u8 *buf, u64 n) { (void)s; (void)buf; (void)n; return -3; }
s64 host_net_write(int s, const u8 *buf, u64 n) { (void)s; (void)buf; (void)n; return -3; }
int host_net_readable(int s) { (void)s; return 1; }
int host_net_avail(int s) { (void)s; return 0; }
void host_net_shutdown(int s, int how) { (void)s; (void)how; }
void host_net_close(int s) { (void)s; }
int host_net_name(int s, int peer, u8 *ip4, int *port) { (void)s; (void)peer; (void)ip4; (void)port; return 0; }
/* GPU back end (WebGL2 in src/lib/x64/gfx.ts) */
__attribute__((import_module("env"), import_name("host_gfx_init"))) int js_gfx_init(void);
__attribute__((import_module("env"), import_name("host_gfx_shader"))) int js_gfx_shader(int kind, const char *glsl);
__attribute__((import_module("env"), import_name("host_gfx_buffer"))) void js_gfx_buffer(int id, const u8 *data, u32 len, u32 bind);
__attribute__((import_module("env"), import_name("host_gfx_texture"))) void js_gfx_texture(int id, int w, int h, int fmt, const u8 *px, u32 pitch);
__attribute__((import_module("env"), import_name("host_gfx_clear"))) void js_gfx_clear(int tex_id, const float *rgba);
__attribute__((import_module("env"), import_name("host_gfx_clear_depth"))) void js_gfx_clear_depth(int tex_id, float depth);
__attribute__((import_module("env"), import_name("host_gfx_draw"))) void js_gfx_draw(const u32 *cmd);
__attribute__((import_module("env"), import_name("host_gfx_readback"))) void js_gfx_readback(int tex_id, u32 *bgra);
__attribute__((import_module("env"), import_name("host_gfx_free"))) void js_gfx_free(int id);
int host_gfx_init(void) { return js_gfx_init(); }
int host_gfx_shader(int kind, const char *glsl) { return js_gfx_shader(kind, glsl); }
void host_gfx_buffer(int id, const u8 *data, u32 len, u32 bind) { js_gfx_buffer(id, data, len, bind); }
void host_gfx_texture(int id, int w, int h, int fmt, const u8 *px, u32 pitch) { js_gfx_texture(id, w, h, fmt, px, pitch); }
void host_gfx_clear(int tex_id, const float *rgba) { js_gfx_clear(tex_id, rgba); }
void host_gfx_clear_depth(int tex_id, float depth) { js_gfx_clear_depth(tex_id, depth); }
void host_gfx_draw(const u32 *cmd) { js_gfx_draw(cmd); }
void host_gfx_readback(int tex_id, u32 *bgra) { js_gfx_readback(tex_id, bgra); }
void host_gfx_free(int id) { js_gfx_free(id); }

__attribute__((import_module("env"), import_name("host_audio_open"))) int js_audio_open(int rate);
__attribute__((import_module("env"), import_name("host_audio_write"))) void js_audio_write(const float *lr, int frames);
__attribute__((import_module("env"), import_name("host_audio_queued"))) int js_audio_queued(void);
int host_audio_open(int rate) { return js_audio_open(rate); }
void host_audio_write(const float *lr, int frames) { js_audio_write(lr, frames); }
int host_audio_queued(void) { return js_audio_queued(); }
