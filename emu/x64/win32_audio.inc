/* win32_audio.inc — sound: the Windows audio APIs on top of the HOST's audio output (Web Audio in the browser → the
 * device's own audio stack, e.g. AudioTrack on Android; a WAV dump / silent clock in the native runner).
 *
 *   winmm   waveOut* (queue of WAVEHDRs, CALLBACK_FUNCTION/EVENT/WINDOW/THREAD), PlaySound/sndPlaySound (RIFF WAVE)
 *   dsound  IDirectSound8 / IDirectSoundBuffer8 (primary + secondary ring buffers, Lock/Unlock/Play/Stop, volume, pan,
 *           frequency, position notifications), IDirectSound3DListener/Buffer accepted (positioning ignored, logged)
 *   mmdevapi/WASAPI  IMMDeviceEnumerator → IMMDevice → IAudioClient(1/2/3) + IAudioRenderClient + IAudioClock +
 *           ISimpleAudioVolume/IAudioStreamVolume/IAudioSessionControl, IPropertyStore (friendly name, device format),
 *           IAudioEndpointVolume — shared or exclusive, polling or event-driven.  This is the layer the REAL
 *           XAudio2_N.dll from the DirectX pack sits on (CoCreateInstance(CLSID_XAudio2) → the DLL's own
 *           DllGetClassObject), so XAudio2 games play through Microsoft's own mixer.
 *
 * Every voice/buffer/stream is an AStream; a 10 ms software mixer (linear resampling, volume, pan) produces interleaved
 * stereo float at the host rate and keeps ~100 ms queued on the host (host_audio_*).  Capture (waveIn, DirectSoundCapture,
 * IAudioCaptureClient, microphone endpoints) is reported honestly as "no device".  COM objects use the d3d11.inc pattern:
 * [0] vtable (one named shim per method — an unimplemented method stops with its exact name), [8] id, [12] refcount. */

enum { CONT_AUDIO_CB = 200, CONT_DSENUM, CONT_COCREATE };
static int mod_load(Win32 *w, const char *name);
static u64 mod_export(Win32 *w, int mi, const char *name, u32 ordinal, int depth);
enum { AS_RING = 1, AS_QUEUE = 2, AS_WASAPI = 3 };
#define AUD_MAX 96
typedef struct { u64 hdr; u64 data; u32 len, pos; } AChunk;
typedef struct {
  int used, kind, primary, closed; Win32 *w; Mem *mem;
  int rate, ch, bits, isfloat; u32 block;             /* source format; block = bytes per frame */
  u64 buf; u32 size;                                  /* ring (guest heap) and its size in bytes */
  u32 play;                                           /* ring/wasapi: read cursor (bytes) */
  u32 filled;                                         /* wasapi: valid bytes not yet consumed (= padding) */
  u64 stage; u32 stage_frames;                        /* wasapi: GetBuffer staging area */
  int playing, looping, event_mode, exclusive, started_once, paused;
  float vol, pan, chvol[8]; int freq;                 /* gain 0..1, pan -1..1, playback rate override (0 = native) */
  int ds_vol, ds_pan;                                 /* DirectSound units (hundredths of dB) for the getters */
  double frac; u64 played_frames; u64 underruns;
  AChunk q[64]; int qn, qh;                           /* waveOut queue (ring of chunks) */
  int ev_kobj;                                        /* event signalled when a period/chunk completes */
  int notif_n; struct { u32 off; int kobj; } notif[32];   /* DirectSound position notifications (0xFFFFFFFF = stop) */
  u64 cb, cb_inst, hwo; u32 cb_kind;                  /* waveOut callback: 0 none, 1 function, 2 event, 3 window, 4 thread */
  struct { u64 hdr; } done[64]; int ndone;            /* waveOut chunks completed by the mixer, callbacks still to deliver */
  int playsound;                                      /* created by PlaySound (freed when finished) */
  u32 mute; u32 guest_tag;                            /* unique id handed to the guest as HWAVEOUT */
} AStream;
static AStream g_astr[AUD_MAX];
static int g_aud_host_rate, g_aud_host_state;         /* host output: 0 not opened, 1 open, -1 unavailable */
static u64 g_aud_written; static u32 g_aud_tag = 0x5100;
static int g_aud_pending_cb;                           /* waveOut callbacks waiting for their owner process */
static int g_aud_log3d;

static void aud_log(const char *fmt, ...) { char m[320]; __builtin_va_list ap; __builtin_va_start(ap, fmt); e_vsnprintf(m, sizeof m, fmt, ap); __builtin_va_end(ap); host_write(3, m, e_strlen(m)); }

/* ---- WAVEFORMATEX ---- */
typedef struct { int tag, ch, rate, bits, block, isfloat, ok; u32 mask; } WFmt;
static WFmt wfmt_read(Cpu *c, u64 p) {
  WFmt f; e_memset(&f, 0, sizeof f); if (!p || !mem_ptr(c->mem, p, 16, 0)) return f;
  f.tag = rd16(c, p); f.ch = rd16(c, p + 2); f.rate = (int)rd32(c, p + 4); f.block = rd16(c, p + 12); f.bits = rd16(c, p + 14);
  if (f.tag == 0xFFFE && rd16(c, p + 16) >= 22) { f.mask = rd32(c, p + 20); u32 sub = rd32(c, p + 24); f.isfloat = sub == 3; if (sub != 1 && sub != 3) f.tag = (int)sub; else f.tag = (int)sub; }
  else if (f.tag == 3) f.isfloat = 1;
  if ((f.tag == 1 || f.tag == 3) && f.ch >= 1 && f.ch <= 8 && f.rate >= 4000 && f.rate <= 384000 && (f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32) && (!f.isfloat || f.bits == 32)) {
    if (!f.block) f.block = f.ch * f.bits / 8; f.ok = f.block == f.ch * f.bits / 8;
  }
  return f;
}
static void wfmt_write(Cpu *c, u64 p, int ch, int rate, int bits, int isfloat, int extensible) {   /* WAVEFORMATEX(TENSIBLE) */
  wr16(c, p, extensible ? 0xFFFE : isfloat ? 3 : 1); wr16(c, p + 2, (u16)ch); wr32(c, p + 4, (u32)rate); wr32(c, p + 8, (u32)(rate * ch * bits / 8));
  wr16(c, p + 12, (u16)(ch * bits / 8)); wr16(c, p + 14, (u16)bits); wr16(c, p + 16, extensible ? 22 : 0);
  if (extensible) { wr16(c, p + 18, (u16)bits); wr32(c, p + 20, ch == 1 ? 4 : 3); static const u8 pcm[16] = { 1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71 }; u8 g[16]; e_memcpy(g, pcm, 16); if (isfloat) g[0] = 3; for (int i = 0; i < 16; i++) wr8(c, p + 24 + i, g[i]); }
}

/* ---- streams ---- */
static AStream *astr_new(Cpu *c, int kind, const WFmt *f) {
  for (int i = 0; i < AUD_MAX; i++) if (!g_astr[i].used) {
    AStream *s = &g_astr[i]; e_memset(s, 0, sizeof *s); s->used = 1; s->kind = kind; s->w = W; s->mem = c->mem;
    s->rate = f->rate; s->ch = f->ch; s->bits = f->bits; s->isfloat = f->isfloat; s->block = (u32)f->block; s->vol = 1; s->ev_kobj = -1; s->guest_tag = g_aud_tag++;
    for (int k = 0; k < 8; k++) s->chvol[k] = 1;
    if (g_aud_host_state == 0) { int r = host_audio_open(48000); if (r > 0) { g_aud_host_rate = r; g_aud_host_state = 1; aud_log("[audio] host output opened: %d Hz stereo float\n", r); } else { g_aud_host_state = -1; g_aud_host_rate = 48000; aud_log("[audio] no host audio output: sound is mixed and discarded (timing is still real)\n"); } }
    return s;
  }
  return 0;
}
static int astr_idx(AStream *s) { return (int)(s - g_astr); }
static void astr_free(AStream *s) { s->used = 0; }
static AStream *astr_by_tag(u32 tag) { for (int i = 0; i < AUD_MAX; i++) if (g_astr[i].used && g_astr[i].guest_tag == tag) return &g_astr[i]; return 0; }
static int kobj_of_event(Cpu *c, u64 h) { int kind, idx; if (!h || !wait_handle(c, h, &kind, &idx) || kind != 2) return -1; KObj *k = kobj_get(idx); return k && k->kind == KO_EVENT ? idx : -1; }
static void kobj_signal(Win32 *w, int idx) { KObj *k = kobj_get(idx); if (!k || k->kind != KO_EVENT) return; if (k->manual || !event_deliver(w, idx, 0)) k->signaled = 1; }

static inline float aud_sample(const u8 *p, int bits, int isfloat) {
  if (isfloat) { float f; e_memcpy(&f, p, 4); return f; }
  switch (bits) {
    case 8: return ((int)p[0] - 128) * (1.0f / 128);
    case 16: return (s16)(p[0] | (p[1] << 8)) * (1.0f / 32768);
    case 24: return (s32)(((u32)p[0] << 8) | ((u32)p[1] << 16) | ((u32)p[2] << 24)) * (1.0f / 2147483648.0f);
    default: { s32 v; e_memcpy(&v, p, 4); return v * (1.0f / 2147483648.0f); }
  }
}
static inline void aud_frame_decode(AStream *s, const u8 *p, float *l, float *r) {
  int bps = s->bits / 8; float a = aud_sample(p, s->bits, s->isfloat);
  if (s->ch == 1) { *l = *r = a; return; }
  float b = aud_sample(p + bps, s->bits, s->isfloat); *l = a; *r = b;
  if (s->ch >= 6) { float cen = aud_sample(p + 2 * bps, s->bits, s->isfloat) * 0.7071f; *l += cen; *r += cen; }
}
/* source frame k (relative to the read cursor) → stereo; returns 0 past the end of the data */
static int aud_src_frame(AStream *s, u32 k, float *l, float *r) {
  *l = *r = 0;
  if (s->kind == AS_RING) {
    u64 off = (u64)s->play + (u64)k * s->block; u32 nfr = s->size / s->block; if (!nfr) return 0;
    if (off >= s->size) { if (!s->looping) return 0; off %= (u64)nfr * s->block; }
    const u8 *p = mem_ptr(s->mem, s->buf + off, s->block, 0); if (!p) return 0; aud_frame_decode(s, p, l, r); return 1;
  }
  if (s->kind == AS_WASAPI) {
    u64 off = (u64)k * s->block; if (off + s->block > s->filled) return 0; off = (s->play + off) % s->size;
    const u8 *p = mem_ptr(s->mem, s->buf + off, s->block, 0); if (!p) return 0; aud_frame_decode(s, p, l, r); return 1;
  }
  /* queue */
  u32 rem = k; for (int n = 0, i = s->qh; n < s->qn; n++, i = (i + 1) % 64) {
    AChunk *q = &s->q[i]; u32 fr = (q->len - q->pos) / s->block;
    if (rem < fr) { const u8 *p = mem_ptr(s->mem, q->data + q->pos + (u64)rem * s->block, s->block, 0); if (!p) return 0; aud_frame_decode(s, p, l, r); return 1; }
    rem -= fr;
  }
  return 0;
}
/* consume n source frames after a mixed block: advance cursors, complete chunks, fire notifications */
static void aud_advance(AStream *s, u32 n) {
  if (s->kind == AS_RING) {
    u32 nfr = s->size / s->block; if (!nfr) return; u32 oldp = s->play; u64 np = (u64)s->play + (u64)n * s->block;
    int wrapped = 0, ended = 0;
    if (np >= s->size) { if (s->looping) { np %= (u64)nfr * s->block; wrapped = 1; } else { np = 0; ended = 1; } }
    for (int i = 0; i < s->notif_n; i++) {   /* offsets passed in (oldp, newp] (circular); 0xFFFFFFFF = stop notification */
      u32 off = s->notif[i].off; int hit;
      if (off == 0xFFFFFFFFu) hit = ended; else if (ended) hit = off >= oldp; else if (wrapped) hit = off >= oldp || off < (u32)np; else hit = off >= oldp && off < (u32)np;
      if (hit) kobj_signal(s->w, s->notif[i].kobj);
    }
    s->play = (u32)np; s->played_frames += n; if (ended) s->playing = 0;
    return;
  }
  if (s->kind == AS_WASAPI) {
    u64 bytes = (u64)n * s->block; if (bytes > s->filled) { s->underruns += (bytes - s->filled) / s->block; bytes = s->filled; }
    s->play = (u32)((s->play + bytes) % s->size); s->filled -= (u32)bytes; s->played_frames += n;
    return;
  }
  u64 bytes = (u64)n * s->block; s->played_frames += n;
  while (bytes && s->qn) {
    AChunk *q = &s->q[s->qh]; u32 left = q->len - q->pos;
    if (bytes < left) { q->pos += (u32)bytes; bytes = 0; break; }
    bytes -= left; q->pos = q->len;
    /* chunk finished: WHDR_DONE, clear WHDR_INQUEUE, remember for the callback */
    Cpu *c = s->w->cpu; u32 fl = rd32(c, q->hdr + 24); wr32(c, q->hdr + 24, (fl | 1) & ~0x10u);
    if (s->ndone < 64) s->done[s->ndone++].hdr = q->hdr;
    s->qh = (s->qh + 1) % 64; s->qn--;
  }
  if (s->ndone) { if (s->cb_kind == 2 && s->ev_kobj >= 0) { kobj_signal(s->w, s->ev_kobj); s->ndone = 0; } else if (s->cb_kind == 0) s->ndone = 0; else g_aud_pending_cb = 1; }
}
static void aud_mix_block(float *out, int n) {
  for (int i = 0; i < 2 * n; i++) out[i] = 0;
  for (int si = 0; si < AUD_MAX; si++) {
    AStream *s = &g_astr[si]; if (!s->used || !s->playing || s->paused || !s->block) continue;
    int src_rate = s->freq ? s->freq : s->rate; double step = (double)src_rate / g_aud_host_rate; double pos = s->frac;
    float gl = s->vol * (s->pan <= 0 ? 1.0f : 1.0f - s->pan) * s->chvol[0], gr = s->vol * (s->pan >= 0 ? 1.0f : 1.0f + s->pan) * s->chvol[s->ch > 1 ? 1 : 0];
    int any = 0;
    for (int i = 0; i < n; i++) {
      u32 k = (u32)pos; float t = (float)(pos - k), l0, r0, l1, r1;
      int ok0 = aud_src_frame(s, k, &l0, &r0); if (!ok0) { pos += step; continue; } any = 1;
      if (!aud_src_frame(s, k + 1, &l1, &r1)) { l1 = l0; r1 = r0; }
      out[2 * i] += (l0 + (l1 - l0) * t) * gl; out[2 * i + 1] += (r0 + (r1 - r0) * t) * gr; pos += step;
    }
    u32 consumed = (u32)pos; s->frac = pos - consumed; aud_advance(s, consumed);
    if (!any && s->kind == AS_QUEUE && !s->qn) { /* idle queue: nothing to do */ }
    (void)any;
  }
  for (int i = 0; i < 2 * n; i++) { float v = out[i]; out[i] = v > 1 ? 1 : v < -1 ? -1 : v; }
}
/* called by the scheduler before every pick: keep the host fed; period events for WASAPI streams */
void w32_audio_pump(void) {
  if (g_aud_host_state == 0) return;
  int rate = g_aud_host_rate, per = rate / 100, target = rate / 10;   /* 10 ms blocks, 100 ms of lead */
  static float blk[2 * 3840]; if (per > 3840) per = 3840;
  int queued = host_audio_queued(); int blocks = 0;
  while (queued < target && blocks < 20) {
    aud_mix_block(blk, per); host_audio_write(blk, per); g_aud_written += (u64)per; queued += per; blocks++;
    for (int i = 0; i < AUD_MAX; i++) { AStream *s = &g_astr[i]; if (s->used && s->kind == AS_WASAPI && s->playing && s->event_mode && s->ev_kobj >= 0) kobj_signal(s->w, s->ev_kobj); }
  }
  for (int i = 0; i < AUD_MAX; i++) { AStream *s = &g_astr[i]; if (s->used && s->playsound && !s->playing) { if (s->buf) w32_heap_free(s->w, s->buf); astr_free(s); } }
}
int w32_audio_pending(void) { if (g_aud_host_state != 1 && g_aud_host_state != -1) return 0; for (int i = 0; i < AUD_MAX; i++) if (g_astr[i].used && g_astr[i].playing) return 1; return g_aud_pending_cb; }
u64 w32_audio_frames(void) { return g_aud_written; }
int w32_audio_cb_pending(void) { return g_aud_pending_cb; }
/* waveOut callbacks are delivered in the owner's process context (helper thread / message queue) */
static void s_nebula_audio_thunk(Cpu *c) {
  u64 job = ARG(0); Win32 *w = W; if (!job || !mem_ptr(c->mem, job, 48, 0)) { RET(0); return; }
  int i = cont_push(c, CONT_AUDIO_CB); if (i < 0) return; w->cont[i].data[0] = job;
  u64 fn = rd64(c, job), hwo = rd64(c, job + 8), msg = rd64(c, job + 16), inst = rd64(c, job + 24), p1 = rd64(c, job + 32);
  u64 sp = (c->r[RSP] - 16) & ~15ull; wr64(c, sp, 0); c->r[RSP] = sp;   /* 5th argument (dwParam2 = 0) lands at [rsp+40] after guest_call's frame */
  guest_call(c, fn, hwo, msg, inst, p1);
}
static int audio_cont_resume(Cpu *c, int kind, u64 rax) {
  Win32 *w = W; int i = w->ncont - 1;
  if (kind == CONT_AUDIO_CB) { w32_heap_free(w, w->cont[i].data[0]); cont_finish(c, 0); return 1; }
  if (kind == CONT_DSENUM) { cont_finish(c, 0); return 1; }
  if (kind == CONT_COCREATE) {
    /* data: 0 step, 1 class-factory slot (guest), 2 riid, 3 ppv, 4 hr */
    u64 slot = w->cont[i].data[1]; int step = (int)(s64)w->cont[i].data[0];
    if (step == -1) { w->cont[i].data[0] = 0; guest_call(c, w->cont[i].data[5], slot + 32, slot + 8, slot, 0); return 1; }   /* DLLs attached → DllGetClassObject(clsid, IID_IClassFactory, &cf) */
    if (step == 0) { if ((u32)rax) { aud_log("[com] DllGetClassObject failed: 0x%08x\n", (u32)rax); cont_finish(c, rax); return 1; }
      u64 cf = rd64(c, slot); if (!cf) { cont_finish(c, 0x80040111u); return 1; } u64 vt = rd64(c, cf);
      w->cont[i].data[0] = 1; guest_call(c, rd64(c, vt + 24), cf, 0, w->cont[i].data[2], w->cont[i].data[3]); return 1; }   /* IClassFactory::CreateInstance(this, NULL, riid, ppv) */
    if (step == 1) { w->cont[i].data[4] = rax; u64 cf = rd64(c, slot), vt = rd64(c, cf); w->cont[i].data[0] = 2; guest_call(c, rd64(c, vt + 16), cf, 0, 0, 0); return 1; }   /* Release the factory */
    u64 hr = w->cont[i].data[4]; w32_heap_free(w, slot); if ((u32)hr) aud_log("[com] IClassFactory::CreateInstance failed: 0x%08x\n", (u32)hr); cont_finish(c, hr); return 1;
  }
  return 0;
}
void w32_audio_deliver(Win32 *w) {
  if (!g_aud_pending_cb) return; int left = 0; Cpu *c = w->cpu;
  for (int i = 0; i < AUD_MAX; i++) {
    AStream *s = &g_astr[i]; if (!s->used || !s->ndone) continue; if (s->w != w) { left = 1; continue; }
    for (int d = 0; d < s->ndone; d++) {
      u64 hdr = s->done[d].hdr;
      if (s->cb_kind == 1) { u64 job = w32_heap_alloc(w, 48, 1); if (!job) break; wr64(c, job, s->cb); wr64(c, job + 8, s->hwo); wr64(c, job + 16, 0x3BD /* WOM_DONE */); wr64(c, job + 24, s->cb_inst); wr64(c, job + 32, hdr);
        u64 th = thread_create(c, w32_shim_slot(w, "dynamic", "nebula_audio_thunk"), job, 0, 0, 0); if (th) { int fd = handle_fd(th); vfs_close(fd); } }
      else if (s->cb_kind == 3 || s->cb_kind == 4) { extern void w32_post_msg(Win32 *, u32, u64, u64); w32_post_msg(w, 0x3BD, s->hwo, hdr); }
      else if (s->cb_kind == 2 && s->ev_kobj >= 0) kobj_signal(w, s->ev_kobj);
    }
    s->ndone = 0;
  }
  g_aud_pending_cb = left;
}

/* ================= winmm: waveOut ================= */
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NOTENABLED 3
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOMEM 7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_INVALPARAM 11
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define WAVERR_UNPREPARED 34
static AStream *wo_of(Cpu *c) { return astr_by_tag((u32)ARG(0)); }
static void s_waveOutGetNumDevs(Cpu *c) { RET(1); }
static void wo_caps(Cpu *c, u64 p, int wide) {   /* WAVEOUTCAPS: wMid, wPid, vDriverVersion, szPname[32], dwFormats, wChannels, wReserved1, dwSupport */
  if (!p) { RET(MMSYSERR_INVALPARAM); return; } const char *nm = "Nebula Audio Output"; u64 q = p;
  wr16(c, q, 1); wr16(c, q + 2, 1); wr32(c, q + 4, 0x0500); q += 8;
  for (int i = 0; i < 32; i++) { char ch = nm[i] ? nm[i] : 0; if (wide) wr16(c, q + 2 * i, (u16)(u8)ch); else wr8(c, q + i, (u8)ch); if (!nm[i]) { /* keep zero padding */ } }
  q += wide ? 64 : 32;
  wr32(c, q, 0x000FFFFFu); wr16(c, q + 4, 2); wr16(c, q + 6, 0); wr32(c, q + 8, 0x1 | 0x2 | 0x4 | 0x8 /* PITCH|PLAYBACKRATE|VOLUME|LRVOLUME */); RET(MMSYSERR_NOERROR);
}
static void s_waveOutGetDevCapsA(Cpu *c) { if (ARG(0) != 0 && ARG(0) != 0xFFFFFFFFu && !astr_by_tag((u32)ARG(0))) { RET(MMSYSERR_BADDEVICEID); return; } wo_caps(c, ARG(1), 0); }
static void s_waveOutGetDevCapsW(Cpu *c) { if (ARG(0) != 0 && ARG(0) != 0xFFFFFFFFu && !astr_by_tag((u32)ARG(0))) { RET(MMSYSERR_BADDEVICEID); return; } wo_caps(c, ARG(1), 1); }
static void s_waveOutOpen(Cpu *c) {
  u64 ph = ARG(0), fmtp = ARG(2), cb = ARG(3), inst = ARG(4); u32 dev = (u32)ARG(1), flags = (u32)ARG(5);
  if (dev != 0 && dev != 0xFFFFFFFFu) { RET(MMSYSERR_BADDEVICEID); return; }
  WFmt f = wfmt_read(c, fmtp); if (!f.ok) { aud_log("[audio] waveOutOpen: unsupported format tag %d ch %d rate %d bits %d\n", f.tag, f.ch, f.rate, f.bits); RET(WAVERR_BADFORMAT); return; }
  if (flags & 1) { RET(MMSYSERR_NOERROR); return; }   /* WAVE_FORMAT_QUERY */
  AStream *s = astr_new(c, AS_QUEUE, &f); if (!s) { RET(MMSYSERR_NOMEM); return; }
  s->playing = 1; s->cb = cb; s->cb_inst = inst; s->hwo = s->guest_tag;
  u32 ck = flags & 0x70000; s->cb_kind = ck == 0x30000 ? 1 : ck == 0x50000 ? 2 : ck == 0x10000 ? 3 : ck == 0x20000 ? 4 : 0;
  if (s->cb_kind == 2) s->ev_kobj = kobj_of_event(c, cb);
  if (ph) wr64(c, ph, s->guest_tag);
  aud_log("[audio] waveOutOpen: %d Hz %d ch %d bit%s, callback %s → hwo %#x\n", f.rate, f.ch, f.bits, f.isfloat ? " float" : "", s->cb_kind == 1 ? "function" : s->cb_kind == 2 ? "event" : s->cb_kind == 3 ? "window" : s->cb_kind == 4 ? "thread" : "none", s->guest_tag);
  if (s->cb_kind == 1) { s->done[0].hdr = 0; /* WOM_OPEN */ u64 job = w32_heap_alloc(W, 48, 1); if (job) { wr64(c, job, cb); wr64(c, job + 8, s->hwo); wr64(c, job + 16, 0x3BB); wr64(c, job + 24, inst); u64 th = thread_create(c, w32_shim_slot(W, "dynamic", "nebula_audio_thunk"), job, 0, 0, 0); if (th) { int fd = handle_fd(th); vfs_close(fd); } } }
  else if (s->cb_kind == 3 || s->cb_kind == 4) { extern void w32_post_msg(Win32 *, u32, u64, u64); w32_post_msg(W, 0x3BB, s->hwo, 0); }
  RET(MMSYSERR_NOERROR);
}
static void s_waveOutClose(Cpu *c) { AStream *s = wo_of(c); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } if (s->qn) { RET(WAVERR_STILLPLAYING); return; }
  if (s->cb_kind == 1) { u64 job = w32_heap_alloc(W, 48, 1); if (job) { wr64(c, job, s->cb); wr64(c, job + 8, s->hwo); wr64(c, job + 16, 0x3BC); wr64(c, job + 24, s->cb_inst); u64 th = thread_create(c, w32_shim_slot(W, "dynamic", "nebula_audio_thunk"), job, 0, 0, 0); if (th) { int fd = handle_fd(th); vfs_close(fd); } } }
  astr_free(s); RET(MMSYSERR_NOERROR); }
static void s_waveOutPrepareHeader(Cpu *c) { AStream *s = wo_of(c); u64 h = ARG(1); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } if (!h || (u32)ARG(2) < 48) { RET(MMSYSERR_INVALPARAM); return; } wr32(c, h + 24, (rd32(c, h + 24) | 2) & ~1u); RET(MMSYSERR_NOERROR); }
static void s_waveOutUnprepareHeader(Cpu *c) { AStream *s = wo_of(c); u64 h = ARG(1); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } if (!h) { RET(MMSYSERR_INVALPARAM); return; } if (rd32(c, h + 24) & 0x10) { RET(WAVERR_STILLPLAYING); return; } wr32(c, h + 24, rd32(c, h + 24) & ~2u); RET(MMSYSERR_NOERROR); }
static void s_waveOutWrite(Cpu *c) {
  AStream *s = wo_of(c); u64 h = ARG(1); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } if (!h || !mem_ptr(c->mem, h, 48, 1)) { RET(MMSYSERR_INVALPARAM); return; }
  u32 fl = rd32(c, h + 24); if (!(fl & 2)) { RET(WAVERR_UNPREPARED); return; } if (fl & 0x10) { RET(WAVERR_STILLPLAYING); return; }
  if (s->qn >= 64) { RET(MMSYSERR_NOMEM); return; }
  AChunk *q = &s->q[(s->qh + s->qn) % 64]; q->hdr = h; q->data = rd64(c, h); q->len = rd32(c, h + 8) / s->block * s->block; q->pos = 0; s->qn++;
  wr32(c, h + 24, (fl | 0x10) & ~1u); RET(MMSYSERR_NOERROR);
}
static void s_waveOutReset(Cpu *c) { AStream *s = wo_of(c); if (!s) { RET(MMSYSERR_INVALHANDLE); return; }
  while (s->qn) { AChunk *q = &s->q[s->qh]; u32 fl = rd32(c, q->hdr + 24); wr32(c, q->hdr + 24, (fl | 1) & ~0x10u); if (s->ndone < 64) s->done[s->ndone++].hdr = q->hdr; s->qh = (s->qh + 1) % 64; s->qn--; }
  s->frac = 0; s->played_frames = 0; s->paused = 0; if (s->ndone) { if (s->cb_kind == 0) s->ndone = 0; else { g_aud_pending_cb = 1; w32_audio_deliver(W); } } RET(MMSYSERR_NOERROR); }
static void s_waveOutPause(Cpu *c) { AStream *s = wo_of(c); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } s->paused = 1; RET(MMSYSERR_NOERROR); }
static void s_waveOutRestart(Cpu *c) { AStream *s = wo_of(c); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } s->paused = 0; RET(MMSYSERR_NOERROR); }
static void s_waveOutBreakLoop(Cpu *c) { RET(wo_of(c) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE); }
static void s_waveOutGetPosition(Cpu *c) {   /* MMTIME: wType@0, u@4 — TIME_MS 1, TIME_SAMPLES 2, TIME_BYTES 4 */
  AStream *s = wo_of(c); u64 mt = ARG(1); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } if (!mt) { RET(MMSYSERR_INVALPARAM); return; }
  u32 ty = rd32(c, mt); u64 fr = s->played_frames;
  if (ty == 1) wr32(c, mt + 4, (u32)(fr * 1000 / (u64)s->rate)); else if (ty == 2) wr32(c, mt + 4, (u32)fr); else { wr32(c, mt, 4); wr32(c, mt + 4, (u32)(fr * s->block)); }
  RET(MMSYSERR_NOERROR); }
static void s_waveOutGetVolume(Cpu *c) { AStream *s = ARG(0) ? wo_of(c) : 0; u32 v = s ? (u32)(s->vol * 65535) : 65535; if (ARG(1)) wr32(c, ARG(1), v | (v << 16)); RET(MMSYSERR_NOERROR); }
static void s_waveOutSetVolume(Cpu *c) { AStream *s = ARG(0) ? wo_of(c) : 0; u32 v = (u32)ARG(1); float l = (v & 0xffff) / 65535.0f, r = (v >> 16) / 65535.0f; if (s) { s->vol = l > r ? l : r; s->pan = (l == r) ? 0 : (r - l); } RET(MMSYSERR_NOERROR); }
static void s_waveOutGetID(Cpu *c) { if (!wo_of(c)) { RET(MMSYSERR_INVALHANDLE); return; } if (ARG(1)) wr32(c, ARG(1), 0); RET(MMSYSERR_NOERROR); }
static void s_waveOutGetErrorTextA(Cpu *c) { static const char *t[] = { "no error", "unspecified error", "device ID out of range", "driver failed enable", "device already allocated", "device handle is invalid", "no device driver present", "memory allocation error", "function isn't supported" }; u32 e = (u32)ARG(0); const char *m = e < 9 ? t[e] : e == 32 ? "unsupported wave format" : e == 33 ? "still something playing" : e == 34 ? "header not prepared" : "unknown error"; u64 p = ARG(1); u32 n = (u32)ARG(2); if (p && n) { u32 i; for (i = 0; m[i] && i + 1 < n; i++) wr8(c, p + i, (u8)m[i]); wr8(c, p + i, 0); } RET(MMSYSERR_NOERROR); }
static void s_waveOutGetErrorTextW(Cpu *c) { u64 p = ARG(1); u32 n = (u32)ARG(2); const char *m = (u32)ARG(0) == 0 ? "no error" : "audio error"; if (p && n) { u32 i; for (i = 0; m[i] && i + 1 < n; i++) wr16(c, p + 2 * i, (u16)m[i]); wr16(c, p + 2 * i, 0); } RET(MMSYSERR_NOERROR); }
static void s_waveOutMessage(Cpu *c) { RET(MMSYSERR_NOTSUPPORTED); }
static void s_waveOutGetPitch(Cpu *c) { RET(MMSYSERR_NOTSUPPORTED); }
static void s_waveOutSetPitch(Cpu *c) { RET(MMSYSERR_NOTSUPPORTED); }
static void s_waveOutGetPlaybackRate(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0x10000); RET(MMSYSERR_NOERROR); }
static void s_waveOutSetPlaybackRate(Cpu *c) { AStream *s = wo_of(c); if (!s) { RET(MMSYSERR_INVALHANDLE); return; } u32 r = (u32)ARG(1); s->freq = r ? (int)((u64)s->rate * r / 65536) : 0; RET(MMSYSERR_NOERROR); }
/* no capture / MIDI / mixer hardware: honest "no such device" (not a crash, not fake data) */
static void s_waveInGetNumDevs(Cpu *c) { RET(0); }
static void s_waveInOpen(Cpu *c) { aud_log("[audio] waveInOpen: there is no capture device (microphone) in this environment\n"); RET(MMSYSERR_BADDEVICEID); }
static void s_waveInGetDevCapsA(Cpu *c) { RET(MMSYSERR_BADDEVICEID); }
static void s_midiOutGetNumDevs(Cpu *c) { RET(0); }
static void s_midiOutOpen(Cpu *c) { aud_log("[audio] midiOutOpen: no MIDI synthesizer in this environment\n"); RET(MMSYSERR_BADDEVICEID); }
static void s_midiInGetNumDevs(Cpu *c) { RET(0); }
static void s_mixerGetNumDevs(Cpu *c) { RET(0); }
static void s_mixerOpen(Cpu *c) { RET(MMSYSERR_BADDEVICEID); }
static void s_auxGetNumDevs(Cpu *c) { RET(0); }
static void s_mciSendStringA(Cpu *c) { aud_log("[audio] mciSendString: MCI (CD audio / media player) is not available\n"); if (ARG(1) && ARG(2)) wr8(c, ARG(1), 0); RET(263 /* MCIERR_DEVICE_NOT_INSTALLED */); }
static void s_mciSendCommandA(Cpu *c) { RET(263); }
static void s_mciGetErrorStringA(Cpu *c) { RET(0); }

/* PlaySound / sndPlaySound: RIFF WAVE from a file or memory, one-shot or looping (SND_LOOP), optionally synchronous */
static AStream *g_playsound;
static int playsound_start(Cpu *c, const u8 *riff, u64 n, int loop) {
  if (n < 44 || e_memcmp(riff, "RIFF", 4) || e_memcmp(riff + 8, "WAVE", 4)) return 0;
  u64 o = 12; WFmt f; e_memset(&f, 0, sizeof f); const u8 *data = 0; u32 dlen = 0;
  while (o + 8 <= n) { u32 len; e_memcpy(&len, riff + o + 4, 4); const u8 *p = riff + o + 8; if (o + 8 + len > n) len = (u32)(n - o - 8);
    if (!e_memcmp(riff + o, "fmt ", 4) && len >= 16) { u16 tag, ch, blk, bits; u32 rate; e_memcpy(&tag, p, 2); e_memcpy(&ch, p + 2, 2); e_memcpy(&rate, p + 4, 4); e_memcpy(&blk, p + 12, 2); e_memcpy(&bits, p + 14, 2);
      if (tag == 0xFFFE && len >= 40) { u32 sub; e_memcpy(&sub, p + 24, 4); tag = (u16)sub; } f.tag = tag; f.ch = ch; f.rate = (int)rate; f.block = blk; f.bits = bits; f.isfloat = tag == 3; f.ok = (tag == 1 || tag == 3) && ch >= 1 && ch <= 8 && (bits == 8 || bits == 16 || bits == 24 || bits == 32) && blk == ch * bits / 8; }
    else if (!e_memcmp(riff + o, "data", 4)) { data = p; dlen = len; }
    o += 8 + len + (len & 1); }
  if (!f.ok || !data || !dlen) { aud_log("[audio] PlaySound: not a PCM RIFF WAVE (tag %d)\n", f.tag); return 0; }
  if (g_playsound && g_playsound->used && g_playsound->playsound) { g_playsound->playing = 0; }
  AStream *s = astr_new(c, AS_RING, &f); if (!s) return 0;
  s->buf = w32_heap_alloc(W, dlen, 0); if (!s->buf) { astr_free(s); return 0; }
  u8 *dst = mem_ptr(c->mem, s->buf, dlen, 1); if (!dst) { astr_free(s); return 0; } e_memcpy(dst, data, dlen);
  s->size = dlen / f.block * f.block; s->looping = loop; s->playing = 1; s->playsound = 1; g_playsound = s;
  aud_log("[audio] PlaySound: %u bytes, %d Hz %d ch %d bit%s\n", dlen, f.rate, f.ch, f.bits, loop ? ", looping" : "");
  return 1;
}
static void playsound_impl(Cpu *c, const char *name, u64 mem_ptr_guest, u32 flags) {
  /* SND_SYNC 0, SND_ASYNC 1, SND_NODEFAULT 2, SND_MEMORY 4, SND_LOOP 8, SND_NOSTOP 0x10, SND_PURGE 0x40, SND_FILENAME 0x20000, SND_RESOURCE 0x40004, SND_ALIAS 0x10000 */
  if (W->sleeping) { W->sleeping = 0; if (g_playsound && g_playsound->used && g_playsound->playing) { W->sleeping = 1; W->sleep_until_ms = host_now_ms() + 20; W->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; } RET(1); return; }
  if (!name && !mem_ptr_guest) { if (g_playsound && g_playsound->used) g_playsound->playing = 0; RET(1); return; }
  int ok = 0;
  if (flags & 4) { const u8 *p = mem_ptr(c->mem, mem_ptr_guest, 44, 0); if (p) { u32 len; e_memcpy(&len, p + 4, 4); const u8 *all = mem_ptr(c->mem, mem_ptr_guest, (u64)len + 8, 0); if (all) ok = playsound_start(c, all, (u64)len + 8, (flags & 8) != 0); } }
  else if ((flags & 0x40004) == 0x40004) { aud_log("[audio] PlaySound(SND_RESOURCE): WAVE resources are not loaded yet — silent\n"); RET(1); return; }
  else if (name) { char full[1024]; vfs_normalize(name, full, sizeof full); int node = vfs_find_node(full); VNodeInfo ni; if (node >= 0 && vfs_node_info(node, &ni) && !ni.is_dir) { const u8 *d = vfs_node_data(node); if (d) ok = playsound_start(c, d, ni.size, (flags & 8) != 0); } else aud_log("[audio] PlaySound: %s not found%s\n", name, (flags & 0x20000) ? "" : " (system sound aliases are silent)"); }
  if (ok && !(flags & 1)) { W->sleeping = 1; W->sleep_until_ms = host_now_ms() + 20; W->block = BLOCK_SLEEP; retry_and_yield(c, g_cur_slot); return; }   /* SND_SYNC: wait for the end (polling) */
  RET(ok || !(flags & 2) ? 1 : 0);
}
static void s_PlaySoundA(Cpu *c) { u32 fl = (u32)ARG(2); playsound_impl(c, (fl & 4) ? 0 : ARG(0) ? rd_str(c, ARG(0), 1024) : 0, ARG(0), fl); }
static void s_PlaySoundW(Cpu *c) { u32 fl = (u32)ARG(2); char tmp[520]; const char *n = 0; if (ARG(0) && !(fl & 4)) { wstr_to_utf8(c, ARG(0), tmp, sizeof tmp, -1); n = tmp; } playsound_impl(c, n, ARG(0), fl); }
static void s_sndPlaySoundA(Cpu *c) { u32 fl = (u32)ARG(1); playsound_impl(c, (fl & 4) ? 0 : ARG(0) ? rd_str(c, ARG(0), 1024) : 0, ARG(0), fl | ((fl & 4) ? 0 : 0x20000)); }
static void s_sndPlaySoundW(Cpu *c) { u32 fl = (u32)ARG(1); char tmp[520]; const char *n = 0; if (ARG(0) && !(fl & 4)) { wstr_to_utf8(c, ARG(0), tmp, sizeof tmp, -1); n = tmp; } playsound_impl(c, n, ARG(0), fl | ((fl & 4) ? 0 : 0x20000)); }

/* ================= COM objects (dsound + mmdevapi) ================= */
enum { AK_DS = 1, AK_DSBUF, AK_DSNOTIFY, AK_DS3DL, AK_DS3DB, AK_ENUM, AK_COLL, AK_DEV, AK_ENDPOINT, AK_PROPS, AK_CLIENT, AK_RENDER, AK_CLOCK, AK_SVOL, AK_STRVOL, AK_SESSION, AK_EPVOL, AK_CAPTURE_DEV };
typedef struct { int used, kind; u64 addr; Win32 *w; int stream; int ds; int inited; u64 fmt_hns; } AObj;
#define AOBJ_MAX 512
static AObj g_aobj[AOBJ_MAX];
static const char *const vt_IDirectSound8[] = { "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize", "VerifyCertification" };
static const char *const vt_IDirectSoundBuffer8[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore", "SetFX", "AcquireResources", "GetObjectInPath" };
static const char *const vt_IDirectSoundNotify[] = { "QueryInterface", "AddRef", "Release", "SetNotificationPositions" };
static const char *const vt_IDirectSound3DListener[] = { "QueryInterface", "AddRef", "Release", "GetAllParameters", "GetDistanceFactor", "GetDopplerFactor", "GetOrientation", "GetPosition", "GetRolloffFactor", "GetVelocity", "SetAllParameters", "SetDistanceFactor", "SetDopplerFactor", "SetOrientation", "SetPosition", "SetRolloffFactor", "SetVelocity", "CommitDeferredSettings" };
static const char *const vt_IDirectSound3DBuffer[] = { "QueryInterface", "AddRef", "Release", "GetAllParameters", "GetConeAngles", "GetConeOrientation", "GetConeOutsideVolume", "GetMaxDistance", "GetMinDistance", "GetMode", "GetPosition", "GetVelocity", "SetAllParameters", "SetConeAngles", "SetConeOrientation", "SetConeOutsideVolume", "SetMaxDistance", "SetMinDistance", "SetMode", "SetPosition", "SetVelocity" };
static const char *const vt_IMMDeviceEnumerator[] = { "QueryInterface", "AddRef", "Release", "EnumAudioEndpoints", "GetDefaultAudioEndpoint", "GetDevice", "RegisterEndpointNotificationCallback", "UnregisterEndpointNotificationCallback" };
static const char *const vt_IMMDeviceCollection[] = { "QueryInterface", "AddRef", "Release", "GetCount", "Item" };
static const char *const vt_IMMDevice[] = { "QueryInterface", "AddRef", "Release", "Activate", "OpenPropertyStore", "GetId", "GetState" };
static const char *const vt_IMMEndpoint[] = { "QueryInterface", "AddRef", "Release", "GetDataFlow" };
static const char *const vt_IPropertyStore[] = { "QueryInterface", "AddRef", "Release", "GetCount", "GetAt", "GetValue", "SetValue", "Commit" };
static const char *const vt_IAudioClient3[] = { "QueryInterface", "AddRef", "Release", "Initialize", "GetBufferSize", "GetStreamLatency", "GetCurrentPadding", "IsFormatSupported", "GetMixFormat", "GetDevicePeriod", "Start", "Stop", "Reset", "SetEventHandle", "GetService", "IsOffloadCapable", "SetClientProperties", "GetBufferSizeLimits", "GetSharedModeEnginePeriod", "GetCurrentSharedModeEnginePeriod", "InitializeSharedAudioStream" };
static const char *const vt_IAudioRenderClient[] = { "QueryInterface", "AddRef", "Release", "GetBuffer", "ReleaseBuffer" };
static const char *const vt_IAudioClock[] = { "QueryInterface", "AddRef", "Release", "GetFrequency", "GetPosition", "GetCharacteristics", "GetDevicePosition" /* IAudioClock2 */ };
static const char *const vt_ISimpleAudioVolume[] = { "QueryInterface", "AddRef", "Release", "SetMasterVolume", "GetMasterVolume", "SetMute", "GetMute" };
static const char *const vt_IAudioStreamVolume[] = { "QueryInterface", "AddRef", "Release", "GetChannelCount", "SetChannelVolume", "GetChannelVolume", "SetAllVolumes", "GetAllVolumes" };
static const char *const vt_IAudioSessionControl2[] = { "QueryInterface", "AddRef", "Release", "GetState", "GetDisplayName", "SetDisplayName", "GetIconPath", "SetIconPath", "GetGroupingParam", "SetGroupingParam", "RegisterAudioSessionNotification", "UnregisterAudioSessionNotification", "GetSessionIdentifier", "GetSessionInstanceIdentifier", "GetProcessId", "IsSystemSoundsSession", "SetDuckingPreference" };
static const char *const vt_IAudioEndpointVolume[] = { "QueryInterface", "AddRef", "Release", "RegisterControlChangeNotify", "UnregisterControlChangeNotify", "GetChannelCount", "SetMasterVolumeLevel", "SetMasterVolumeLevelScalar", "GetMasterVolumeLevel", "GetMasterVolumeLevelScalar", "SetChannelVolumeLevel", "SetChannelVolumeLevelScalar", "GetChannelVolumeLevel", "GetChannelVolumeLevelScalar", "SetMute", "GetMute", "GetVolumeStepInfo", "VolumeStepUp", "VolumeStepDown", "QueryHardwareSupport", "GetVolumeRange" };
typedef struct { const char *name; int n; const char *const *m; } AIface;
static const AIface g_aif[] = {
  { 0, 0, 0 }, { "IDirectSound8", 12, vt_IDirectSound8 }, { "IDirectSoundBuffer8", 24, vt_IDirectSoundBuffer8 }, { "IDirectSoundNotify", 4, vt_IDirectSoundNotify }, { "IDirectSound3DListener", 18, vt_IDirectSound3DListener }, { "IDirectSound3DBuffer", 21, vt_IDirectSound3DBuffer },
  { "IMMDeviceEnumerator", 8, vt_IMMDeviceEnumerator }, { "IMMDeviceCollection", 5, vt_IMMDeviceCollection }, { "IMMDevice", 7, vt_IMMDevice }, { "IMMEndpoint", 4, vt_IMMEndpoint }, { "IPropertyStore", 8, vt_IPropertyStore }, { "IAudioClient3", 21, vt_IAudioClient3 },
  { "IAudioRenderClient", 5, vt_IAudioRenderClient }, { "IAudioClock", 7, vt_IAudioClock }, { "ISimpleAudioVolume", 7, vt_ISimpleAudioVolume }, { "IAudioStreamVolume", 8, vt_IAudioStreamVolume }, { "IAudioSessionControl2", 17, vt_IAudioSessionControl2 }, { "IAudioEndpointVolume", 21, vt_IAudioEndpointVolume }, { "IMMDevice", 7, vt_IMMDevice } };
static u64 g_avt[32]; static Win32 *g_avt_owner;
static u64 aud_vtable(Cpu *c, int kind) {
  Win32 *w = W; if (g_avt_owner != w) { e_memset(g_avt, 0, sizeof g_avt); g_avt_owner = w; }
  if (g_avt[kind]) return g_avt[kind];
  const AIface *f = &g_aif[kind]; u64 vt = w32_heap_alloc(w, (u64)f->n * 8, 1); if (!vt) return 0;
  for (int i = 0; i < f->n; i++) { char nm[96]; e_snprintf(nm, sizeof nm, "%s::%s", f->name, f->m[i]); wr64(c, vt + 8 * i, w32_shim_slot(w, "audio", nm)); }
  g_avt[kind] = vt; return vt;
}
static AObj *aobj_new(Cpu *c, int kind, u64 *addr_out) {
  int id; for (id = 1; id < AOBJ_MAX && g_aobj[id].used; id++) { } if (id == AOBJ_MAX) return 0;
  u64 vt = aud_vtable(c, kind); u64 a = w32_heap_alloc(W, 32, 1); if (!vt || !a) return 0;
  wr64(c, a, vt); wr32(c, a + 8, (u32)id); wr32(c, a + 12, 1);
  AObj *o = &g_aobj[id]; e_memset(o, 0, sizeof *o); o->used = 1; o->kind = kind; o->addr = a; o->w = W; o->stream = -1; *addr_out = a; return o;
}
static AObj *aobj_of(Cpu *c, u64 self, int kind) {
  if (!self || !mem_ptr(c->mem, self, 16, 0)) return 0; u32 id = rd32(c, self + 8); if (id >= AOBJ_MAX || !g_aobj[id].used || g_aobj[id].addr != self) return 0;
  if (kind && g_aobj[id].kind != kind) return 0; return &g_aobj[id];
}
static AStream *aobj_stream(AObj *o) { return o && o->stream >= 0 && g_astr[o->stream].used ? &g_astr[o->stream] : 0; }
static int aud_iid_ok(int kind, u32 d1) {
  if (d1 == 0) return 1;
  switch (kind) {
    case AK_DS: return d1 == 0x279AFA83u || d1 == 0xC50A7E93u;
    case AK_DSBUF: return d1 == 0x279AFA85u || d1 == 0x6825A449u;
    case AK_DSNOTIFY: return d1 == 0xB0210783u;
    case AK_DS3DL: return d1 == 0x279AFA84u;
    case AK_DS3DB: return d1 == 0x279AFA86u;
    case AK_ENUM: return d1 == 0xA95664D2u;
    case AK_COLL: return d1 == 0x0BD7A1BEu;
    case AK_DEV: case AK_CAPTURE_DEV: return d1 == 0xD666063Fu;
    case AK_ENDPOINT: return d1 == 0x1BE09788u;
    case AK_PROPS: return d1 == 0x886d8eebu;
    case AK_CLIENT: return d1 == 0x1CB9AD4Cu || d1 == 0x726778CDu || d1 == 0x7ED4EE07u;
    case AK_RENDER: return d1 == 0xF294ACFCu;
    case AK_CLOCK: return d1 == 0xCD63314Fu || d1 == 0x6f49ff73u;
    case AK_SVOL: return d1 == 0x87CE5498u;
    case AK_STRVOL: return d1 == 0x93014887u;
    case AK_SESSION: return d1 == 0xF4B1A599u || d1 == 0xbfb7ff88u;
    case AK_EPVOL: return d1 == 0x5CDF2C82u;
  }
  return 0;
}
static void aud_QueryInterface(Cpu *c) {
  AObj *o = aobj_of(c, ARG(0), 0); u64 riid = ARG(1), out = ARG(2); if (!o || !out) { RET(E_INVALIDARG_); return; }
  u32 d1 = rd32(c, riid);
  if (aud_iid_ok(o->kind, d1)) { wr32(c, o->addr + 12, rd32(c, o->addr + 12) + 1); wrp(c, out, o->addr); RET(0); return; }
  /* secondary interfaces of a DirectSound buffer: notify / 3-D buffer; of the device: 3-D listener via the primary buffer */
  if (o->kind == AK_DSBUF && (d1 == 0xB0210783u || d1 == 0x279AFA86u || d1 == 0x279AFA84u)) {
    u64 a; AObj *n = aobj_new(c, d1 == 0xB0210783u ? AK_DSNOTIFY : d1 == 0x279AFA86u ? AK_DS3DB : AK_DS3DL, &a); if (!n) { RET(E_OUTOFMEMORY_); return; } n->stream = o->stream;
    if (d1 != 0xB0210783u && !g_aud_log3d++) aud_log("[audio] DirectSound 3-D buffers/listener: accepted, but positioning is not applied (sounds play at their set volume/pan)\n");
    wrp(c, out, a); RET(0); return;
  }
  if (o->kind == AK_DEV && d1 == 0x1BE09788u) { u64 a; AObj *n = aobj_new(c, AK_ENDPOINT, &a); if (!n) { RET(E_OUTOFMEMORY_); return; } n->ds = o->kind == AK_CAPTURE_DEV; wrp(c, out, a); RET(0); return; }
  wrp(c, out, 0); aud_log("[audio] QueryInterface on %s for {%08x-…}: E_NOINTERFACE\n", g_aif[o->kind].name, d1); RET(E_NOINTERFACE_);
}
static void aud_AddRef(Cpu *c) { AObj *o = aobj_of(c, ARG(0), 0); if (!o) { RET(0); return; } u32 r = rd32(c, o->addr + 12) + 1; wr32(c, o->addr + 12, r); RET(r); }
static void aud_Release(Cpu *c) {
  AObj *o = aobj_of(c, ARG(0), 0); if (!o) { RET(0); return; } u32 r = rd32(c, o->addr + 12); if (r) r--; wr32(c, o->addr + 12, r); if (r) { RET(r); return; }
  if (o->kind == AK_DSBUF || o->kind == AK_CLIENT) { AStream *s = aobj_stream(o); if (s) { s->playing = 0; if (s->buf) w32_heap_free(W, s->buf); if (s->stage) w32_heap_free(W, s->stage); astr_free(s); } }
  o->used = 0; RET(0);
}
static void aud_ok(Cpu *c) { RET(0); }   /* accepted, nothing to do (3-D parameters, notifications we do not emit, …) */

/* ---- DirectSound ---- */
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_BUFFERLOST 0x88780096u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
static void ds_create_common(Cpu *c, u64 pp, int v8) {
  if (!pp) { RET(DSERR_INVALIDPARAM); return; } u64 a; AObj *o = aobj_new(c, AK_DS, &a); if (!o) { RET(DSERR_OUTOFMEMORY); return; } o->inited = 1; o->ds = v8; wr64(c, pp, a);
  aud_log("[audio] DirectSoundCreate%s → host audio output\n", v8 ? "8" : ""); RET(0);
}
static void s_DirectSoundCreate(Cpu *c) { ds_create_common(c, ARG(1), 0); }
static void s_DirectSoundCreate8(Cpu *c) { ds_create_common(c, ARG(1), 1); }
static void s_DirectSoundCaptureCreate(Cpu *c) { aud_log("[audio] DirectSoundCaptureCreate: no capture device\n"); if (ARG(1)) wr64(c, ARG(1), 0); RET(DSERR_NODRIVER); }
static void s_DirectSoundCaptureCreate8(Cpu *c) { s_DirectSoundCaptureCreate(c); }
static void s_DirectSoundCaptureEnumerateA(Cpu *c) { RET(0); }
static void s_DirectSoundCaptureEnumerateW(Cpu *c) { RET(0); }
static void s_GetDeviceID(Cpu *c) { if (ARG(1)) { for (int i = 0; i < 16; i++) wr8(c, ARG(1) + i, 0); } RET(0); }
static void ds_enum(Cpu *c, int wide) {   /* callback(LPGUID, LPCSTR desc, LPCSTR module, LPVOID ctx) once for the primary driver (GUID NULL), then once for the device */
  u64 cb = ARG(0), ctx = ARG(1); if (!cb) { RET(DSERR_INVALIDPARAM); return; }
  u64 strs = w32_heap_alloc(W, 128, 1); if (!strs) { RET(DSERR_OUTOFMEMORY); return; }
  const char *d = "Primary Sound Driver", *m = "nebula.drv";
  if (wide) { for (int i = 0; d[i]; i++) wr16(c, strs + 2 * i, (u16)d[i]); for (int i = 0; m[i]; i++) wr16(c, strs + 64 + 2 * i, (u16)m[i]); } else { for (int i = 0; d[i]; i++) wr8(c, strs + i, (u8)d[i]); for (int i = 0; m[i]; i++) wr8(c, strs + 64 + i, (u8)m[i]); }
  cont_push(c, CONT_DSENUM); guest_call(c, cb, 0, strs, strs + 64, ctx);
}
static void s_DirectSoundEnumerateA(Cpu *c) { ds_enum(c, 0); }
static void s_DirectSoundEnumerateW(Cpu *c) { ds_enum(c, 1); }
static void ds_CreateSoundBuffer(Cpu *c) {
  AObj *d = aobj_of(c, ARG(0), AK_DS); u64 desc = ARG(1), pp = ARG(2); if (!d || !desc || !pp) { RET(DSERR_INVALIDPARAM); return; }
  u32 dsize = rd32(c, desc), flags = rd32(c, desc + 4), bytes = rd32(c, desc + 8); u64 fmtp = rd64(c, desc + 16); (void)dsize;
  int primary = (flags & 1) != 0; WFmt f;
  if (primary) { e_memset(&f, 0, sizeof f); f.tag = 1; f.ch = 2; f.rate = 44100; f.bits = 16; f.block = 4; f.ok = 1; bytes = 44100 * 4 / 10; }
  else { f = wfmt_read(c, fmtp); if (!f.ok) { aud_log("[audio] CreateSoundBuffer: unsupported format (tag %d, %d ch, %d Hz, %d bit)\n", f.tag, f.ch, f.rate, f.bits); RET(DSERR_INVALIDPARAM); return; } if (bytes < (u32)f.block * 4 || bytes > 0x0FFFFFFFu) { RET(DSERR_INVALIDPARAM); return; } }
  AStream *s = astr_new(c, AS_RING, &f); if (!s) { RET(DSERR_OUTOFMEMORY); return; }
  bytes = bytes / f.block * f.block; s->buf = w32_heap_alloc(W, bytes, 1); if (!s->buf) { astr_free(s); RET(DSERR_OUTOFMEMORY); return; } s->size = bytes; s->primary = primary;
  u64 a; AObj *o = aobj_new(c, AK_DSBUF, &a); if (!o) { w32_heap_free(W, s->buf); astr_free(s); RET(DSERR_OUTOFMEMORY); return; } o->stream = astr_idx(s); o->ds = (int)flags;
  wr64(c, pp, a); aud_log("[audio] CreateSoundBuffer: %s %u bytes, %d Hz %d ch %d bit, flags %#x\n", primary ? "PRIMARY" : "secondary", bytes, f.rate, f.ch, f.bits, flags); RET(0);
}
static void ds_GetCaps(Cpu *c) { u64 p = ARG(1); if (!p) { RET(DSERR_INVALIDPARAM); return; } u32 sz = rd32(c, p); for (u32 i = 4; i + 4 <= sz && i < 96; i += 4) wr32(c, p + i, 0);
  wr32(c, p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 /* PRIMARYMONO/STEREO/8/16, SECONDARY*, CONTINUOUSRATE */); wr32(c, p + 8, 100); wr32(c, p + 12, 200000); wr32(c, p + 16, 1); wr32(c, p + 20, 0); wr32(c, p + 24, AUD_MAX); wr32(c, p + 28, AUD_MAX); RET(0); }
static void ds_SetCooperativeLevel(Cpu *c) { RET(aobj_of(c, ARG(0), AK_DS) ? 0 : DSERR_INVALIDPARAM); }
static void ds_GetSpeakerConfig(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 4 /* DSSPEAKER_STEREO */); RET(0); }
static void ds_Initialize(Cpu *c) { AObj *d = aobj_of(c, ARG(0), AK_DS); if (!d) { RET(DSERR_INVALIDPARAM); return; } if (d->inited) { RET(0x88780082u /* DSERR_ALREADYINITIALIZED */); return; } d->inited = 1; RET(0); }
static void ds_DuplicateSoundBuffer(Cpu *c) {
  AObj *src = aobj_of(c, ARG(1), AK_DSBUF); AStream *s0 = aobj_stream(src); u64 pp = ARG(2); if (!s0 || !pp) { RET(DSERR_INVALIDPARAM); return; }
  WFmt f; f.tag = s0->isfloat ? 3 : 1; f.ch = s0->ch; f.rate = s0->rate; f.bits = s0->bits; f.block = (int)s0->block; f.isfloat = s0->isfloat; f.ok = 1;
  AStream *s = astr_new(c, AS_RING, &f); if (!s) { RET(DSERR_OUTOFMEMORY); return; } s->buf = s0->buf; s->size = s0->size;   /* shares the sample memory, like Windows */
  u64 a; AObj *o = aobj_new(c, AK_DSBUF, &a); if (!o) { astr_free(s); RET(DSERR_OUTOFMEMORY); return; } o->stream = astr_idx(s); o->ds = src->ds | 0x40000000; wr64(c, pp, a); RET(0);
}
static void dsb_GetCaps(Cpu *c) { AObj *o = aobj_of(c, ARG(0), AK_DSBUF); AStream *s = aobj_stream(o); u64 p = ARG(1); if (!s || !p) { RET(DSERR_INVALIDPARAM); return; } wr32(c, p + 4, (u32)o->ds | 8 /* LOCSOFTWARE */); wr32(c, p + 8, s->size); wr32(c, p + 12, 0); wr32(c, p + 16, 0); RET(0); }
static void dsb_GetCurrentPosition(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s) { RET(DSERR_INVALIDPARAM); return; }
  u32 play = s->play, lead = (u32)((u64)s->rate * s->block * 15 / 1000) / s->block * s->block, wr = s->size ? (play + lead) % s->size : 0; if (!s->playing) wr = play;
  if (ARG(1)) wr32(c, ARG(1), play); if (ARG(2)) wr32(c, ARG(2), wr); RET(0); }
static void dsb_GetFormat(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); u64 p = ARG(1); u32 n = (u32)ARG(2); if (!s) { RET(DSERR_INVALIDPARAM); return; }
  if (ARG(3)) wr32(c, ARG(3), 18); if (p && n >= 16) { u64 tmp = w32_heap_alloc(W, 64, 1); wfmt_write(c, tmp, s->ch, s->rate, s->bits, s->isfloat, 0); for (u32 i = 0; i < (n < 18 ? n : 18); i++) wr8(c, p + i, rd8(c, tmp + i)); w32_heap_free(W, tmp); } RET(0); }
static void dsb_GetVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s || !ARG(1)) { RET(DSERR_INVALIDPARAM); return; } wr32(c, ARG(1), (u32)s->ds_vol); RET(0); }
static void dsb_GetPan(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s || !ARG(1)) { RET(DSERR_INVALIDPARAM); return; } wr32(c, ARG(1), (u32)s->ds_pan); RET(0); }
static void dsb_GetFrequency(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s || !ARG(1)) { RET(DSERR_INVALIDPARAM); return; } wr32(c, ARG(1), (u32)(s->freq ? s->freq : s->rate)); RET(0); }
static void dsb_GetStatus(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s || !ARG(1)) { RET(DSERR_INVALIDPARAM); return; } wr32(c, ARG(1), (s->playing ? 1 : 0) | (s->playing && s->looping ? 4 : 0)); RET(0); }
static void dsb_Initialize(Cpu *c) { RET(0x88780082u); }
static void dsb_Lock(Cpu *c) {
  AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); u32 off = (u32)ARG(1), n = (u32)ARG(2); u64 p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6); u32 fl = (u32)ARG(7);
  if (!s || !p1 || !n1) { RET(DSERR_INVALIDPARAM); return; }
  if (fl & 1) { u32 lead = (u32)((u64)s->rate * s->block * 15 / 1000) / s->block * s->block; off = (s->play + lead) % s->size; }   /* DSBLOCK_FROMWRITECURSOR */
  if (fl & 2) { off = 0; n = s->size; }                                                                                             /* DSBLOCK_ENTIREBUFFER */
  if (off >= s->size || n > s->size) { RET(DSERR_INVALIDPARAM); return; }
  u32 first = s->size - off < n ? s->size - off : n;
  wr64(c, p1, s->buf + off); wr32(c, n1, first);
  if (p2) wr64(c, p2, n > first ? s->buf : 0); if (n2) wr32(c, n2, n > first ? n - first : 0);
  RET(0);
}
static void dsb_Unlock(Cpu *c) { RET(aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)) ? 0 : DSERR_INVALIDPARAM); }
static void dsb_Play(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s) { RET(DSERR_INVALIDPARAM); return; } s->looping = ((u32)ARG(3) & 1) != 0 || s->primary; if (!s->playing) { s->frac = 0; } s->playing = 1; RET(0); }
static void dsb_Stop(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); if (!s) { RET(DSERR_INVALIDPARAM); return; } if (s->playing) { s->playing = 0; for (int i = 0; i < s->notif_n; i++) if (s->notif[i].off == 0xFFFFFFFFu) kobj_signal(s->w, s->notif[i].kobj); } RET(0); }
static void dsb_SetCurrentPosition(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); u32 p = (u32)ARG(1); if (!s || p >= s->size) { RET(DSERR_INVALIDPARAM); return; } s->play = p / s->block * s->block; s->frac = 0; RET(0); }
static void dsb_SetFormat(Cpu *c) { AObj *o = aobj_of(c, ARG(0), AK_DSBUF); AStream *s = aobj_stream(o); if (!s) { RET(DSERR_INVALIDPARAM); return; } if (!s->primary) { RET(DSERR_INVALIDCALL); return; }
  WFmt f = wfmt_read(c, ARG(1)); if (!f.ok) { RET(DSERR_INVALIDPARAM); return; } s->rate = f.rate; s->ch = f.ch; s->bits = f.bits; s->isfloat = f.isfloat; s->block = (u32)f.block; s->play = 0; RET(0); }
static float ds_gain(int hundredths_db) { if (hundredths_db >= 0) return 1; if (hundredths_db <= -10000) return 0; return (float)host_math(5, 10.0, hundredths_db / 2000.0); }
static void dsb_SetVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); int v = (int)(s32)(u32)ARG(1); if (!s || v > 0 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } s->ds_vol = v; s->vol = ds_gain(v); RET(0); }
static void dsb_SetPan(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); int v = (int)(s32)(u32)ARG(1); if (!s || v > 10000 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } s->ds_pan = v;
  /* DirectSound pan attenuates the far channel: pan -10000 = right silent; map to our linear pan */
  float att = ds_gain(-(v < 0 ? -v : v)); s->pan = v == 0 ? 0 : v < 0 ? -(1 - att) : (1 - att); RET(0); }
static void dsb_SetFrequency(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSBUF)); u32 f = (u32)ARG(1); if (!s || (f && (f < 100 || f > 200000))) { RET(DSERR_INVALIDPARAM); return; } s->freq = (int)f; RET(0); }
static void dsb_Restore(Cpu *c) { RET(0); }
static void dsb_SetFX(Cpu *c) { aud_log("[audio] IDirectSoundBuffer8::SetFX: effects are not available (DSERR_CONTROLUNAVAIL)\n"); RET(DSERR_CONTROLUNAVAIL); }
static void dsb_AcquireResources(Cpu *c) { RET(0); }
static void dsb_GetObjectInPath(Cpu *c) { RET(0x88780032u); }
static void dsn_SetNotificationPositions(Cpu *c) {
  AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_DSNOTIFY)); u32 n = (u32)ARG(1); u64 arr = ARG(2); if (!s || (n && !arr)) { RET(DSERR_INVALIDPARAM); return; }
  if (n > 32) { aud_log("[audio] SetNotificationPositions: %u positions, keeping the first 32\n", n); n = 32; }
  s->notif_n = 0; for (u32 i = 0; i < n; i++) { u32 off = rd32(c, arr + 16 * i); int k = kobj_of_event(c, rd64(c, arr + 16 * i + 8)); if (k < 0) { s->notif_n = 0; RET(DSERR_INVALIDPARAM); return; } s->notif[s->notif_n].off = off; s->notif[s->notif_n].kobj = k; s->notif_n++; }
  RET(0);
}

/* ---- mmdevapi / WASAPI ---- */
#define AUDCLNT_E_NOT_INITIALIZED 0x88890001u
#define AUDCLNT_E_ALREADY_INITIALIZED 0x88890002u
#define AUDCLNT_E_WRONG_ENDPOINT_TYPE 0x88890003u
#define AUDCLNT_E_NOT_STOPPED 0x88890005u
#define AUDCLNT_E_BUFFER_TOO_LARGE 0x88890006u
#define AUDCLNT_E_OUT_OF_ORDER 0x88890007u
#define AUDCLNT_E_UNSUPPORTED_FORMAT 0x88890008u
#define AUDCLNT_E_INVALID_SIZE 0x88890009u
#define AUDCLNT_E_EVENTHANDLE_NOT_SET 0x88890013u
#define AUDCLNT_E_BUFFER_SIZE_ERROR 0x88890016u
#define AUDCLNT_E_INVALID_DEVICE_PERIOD 0x88890020u
#define E_NOTFOUND_ 0x80070490u
#define E_NOTIMPL_ 0x80004001u
static const char *AUD_RENDER_ID = "{0.0.0.00000000}.{6f1a3c5e-4b2d-4e8a-9c1f-2e7d5a6b8c90}";
static u64 aud_wstr_cotask(Cpu *c, const char *s) { size_t n = e_strlen(s); u64 p = w32_heap_alloc(W, (n + 1) * 2, 1); if (p) for (size_t i = 0; i <= n; i++) wr16(c, p + 2 * i, (u16)(u8)s[i]); return p; }
static void mme_create_enum(Cpu *c, u64 ppv) { u64 a; AObj *o = aobj_new(c, AK_ENUM, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wrp(c, ppv, a); RET(0); }
static void mme_EnumAudioEndpoints(Cpu *c) {   /* (dataFlow, stateMask, ppDevices): eRender 0, eCapture 1, eAll 2 */
  u32 flow = (u32)ARG(1), mask = (u32)ARG(2); u64 pp = ARG(3); if (!pp) { RET(E_INVALIDARG_); return; }
  u64 a; AObj *o = aobj_new(c, AK_COLL, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } o->ds = (flow == 1 || !(mask & 1)) ? 0 : 1;   /* number of devices in the collection: one active render endpoint */
  wr64(c, pp, a); RET(0);
}
static void mme_GetDefaultAudioEndpoint(Cpu *c) {
  u32 flow = (u32)ARG(1); u64 pp = ARG(3); if (!pp) { RET(E_INVALIDARG_); return; }
  if (flow == 1) { aud_log("[audio] GetDefaultAudioEndpoint(eCapture): no microphone in this environment (E_NOTFOUND)\n"); wr64(c, pp, 0); RET(E_NOTFOUND_); return; }
  u64 a; AObj *o = aobj_new(c, AK_DEV, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wr64(c, pp, a); RET(0);
}
static void mme_GetDevice(Cpu *c) { char id[128]; wstr_to_utf8(c, ARG(1), id, sizeof id, -1); u64 pp = ARG(2); if (!pp) { RET(E_INVALIDARG_); return; }
  if (e_strcmp(id, AUD_RENDER_ID)) { aud_log("[audio] IMMDeviceEnumerator::GetDevice(%s): E_NOTFOUND\n", id); wr64(c, pp, 0); RET(E_NOTFOUND_); return; }
  u64 a; AObj *o = aobj_new(c, AK_DEV, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wr64(c, pp, a); RET(0); }
static void coll_GetCount(Cpu *c) { AObj *o = aobj_of(c, ARG(0), AK_COLL); if (!o || !ARG(1)) { RET(E_INVALIDARG_); return; } wr32(c, ARG(1), (u32)o->ds); RET(0); }
static void coll_Item(Cpu *c) { AObj *o = aobj_of(c, ARG(0), AK_COLL); u32 i = (u32)ARG(1); u64 pp = ARG(2); if (!o || !pp) { RET(E_INVALIDARG_); return; } if (i >= (u32)o->ds) { RET(E_INVALIDARG_); return; } u64 a; AObj *d = aobj_new(c, AK_DEV, &a); if (!d) { RET(E_OUTOFMEMORY_); return; } wr64(c, pp, a); RET(0); }
static void dev_Activate(Cpu *c) {
  AObj *d = aobj_of(c, ARG(0), AK_DEV); u64 riid = ARG(1), pp = ARG(4); if (!d || !riid || !pp) { RET(E_INVALIDARG_); return; } u32 d1 = rd32(c, riid);
  int kind = (d1 == 0x1CB9AD4Cu || d1 == 0x726778CDu || d1 == 0x7ED4EE07u) ? AK_CLIENT : d1 == 0x5CDF2C82u ? AK_EPVOL : 0;
  if (!kind) { aud_log("[audio] IMMDevice::Activate({%08x-…}): E_NOINTERFACE (IAudioClient/2/3 and IAudioEndpointVolume are available)\n", d1); wr64(c, pp, 0); RET(E_NOINTERFACE_); return; }
  u64 a; AObj *o = aobj_new(c, kind, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wr64(c, pp, a); RET(0);
}
static void dev_OpenPropertyStore(Cpu *c) { u64 pp = ARG(2); if (!pp) { RET(E_INVALIDARG_); return; } u64 a; AObj *o = aobj_new(c, AK_PROPS, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } wr64(c, pp, a); RET(0); }
static void dev_GetId(Cpu *c) { u64 pp = ARG(1); if (!pp) { RET(E_INVALIDARG_); return; } wr64(c, pp, aud_wstr_cotask(c, AUD_RENDER_ID)); RET(0); }
static void dev_GetState(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 1 /* DEVICE_STATE_ACTIVE */); RET(0); }
static void ep_GetDataFlow(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0 /* eRender */); RET(0); }
static void props_GetCount(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 4); RET(0); }
static void props_GetValue(Cpu *c) {   /* (PROPERTYKEY *key, PROPVARIANT *pv) */
  u64 key = ARG(1), pv = ARG(2); if (!key || !pv) { RET(E_INVALIDARG_); return; } u32 g1 = rd32(c, key), pid = rd32(c, key + 16);
  for (int i = 0; i < 24; i++) wr8(c, pv + i, 0);
  if (g1 == 0xa45c254eu && pid == 14) { wr16(c, pv, 31); wr64(c, pv + 8, aud_wstr_cotask(c, "Nebula Audio Output (host)")); RET(0); return; }             /* PKEY_Device_FriendlyName */
  if (g1 == 0xa45c254eu && pid == 2) { wr16(c, pv, 31); wr64(c, pv + 8, aud_wstr_cotask(c, "Nebula Audio Output")); RET(0); return; }                     /* PKEY_Device_DeviceDesc */
  if (g1 == 0xb3f8fa53u && pid == 6) { wr16(c, pv, 31); wr64(c, pv + 8, aud_wstr_cotask(c, "Nebula x64")); RET(0); return; }                              /* PKEY_DeviceInterface_FriendlyName */
  if (g1 == 0x1da5d803u && pid == 4) { wr16(c, pv, 31); wr64(c, pv + 8, aud_wstr_cotask(c, "{6f1a3c5e-4b2d-4e8a-9c1f-2e7d5a6b8c90}")); RET(0); return; }  /* PKEY_AudioEndpoint_GUID */
  if (g1 == 0x1da5d803u && pid == 3) { wr16(c, pv, 19); wr32(c, pv + 8, 3 /* front L+R */); RET(0); return; }                                            /* PKEY_AudioEndpoint_PhysicalSpeakers */
  if (g1 == 0x1da5d803u && pid == 0) { wr16(c, pv, 19); wr32(c, pv + 8, 1 /* Speakers */); RET(0); return; }                                             /* PKEY_AudioEndpoint_FormFactor */
  if (g1 == 0xf19f064du && (pid == 0 || pid == 1)) { u64 b = w32_heap_alloc(W, 40, 1); wfmt_write(c, b, 2, g_aud_host_rate ? g_aud_host_rate : 48000, 32, 1, 1); wr16(c, pv, 65 /* VT_BLOB */); wr32(c, pv + 8, 40); wr64(c, pv + 16, b); RET(0); return; }   /* PKEY_AudioEngine_DeviceFormat / OEMFormat */
  RET(0);   /* VT_EMPTY for everything else (that is what Windows does for unknown keys) */
}
static void props_GetAt(Cpu *c) { RET(E_INVALIDARG_); }
static void props_SetValue(Cpu *c) { RET(0x80070005u /* E_ACCESSDENIED */); }
static void aclient_init_impl(Cpu *c, AObj *o, u32 mode, u32 flags, s64 hns, u64 fmtp) {
  if (!o) { RET(E_INVALIDARG_); return; } if (o->stream >= 0) { RET(AUDCLNT_E_ALREADY_INITIALIZED); return; }
  WFmt f = wfmt_read(c, fmtp); if (!f.ok) { aud_log("[audio] IAudioClient::Initialize: unsupported format (tag %d, %d ch, %d Hz, %d bit)\n", f.tag, f.ch, f.rate, f.bits); RET(AUDCLNT_E_UNSUPPORTED_FORMAT); return; }
  if ((flags & 0x40000) && mode == 1 && hns == 0) { RET(AUDCLNT_E_INVALID_DEVICE_PERIOD); return; }
  if (hns <= 0) hns = 200000;                                                   /* 20 ms default */
  if (hns < 30000) hns = 30000; if (hns > 50000000) hns = 50000000;
  u32 frames = (u32)((u64)hns * (u64)f.rate / 10000000ull); if (frames < 64) frames = 64;
  AStream *s = astr_new(c, AS_WASAPI, &f); if (!s) { RET(E_OUTOFMEMORY_); return; }
  u32 ring_frames = mode == 1 ? frames * 2 : frames; s->size = ring_frames * s->block; s->buf = w32_heap_alloc(W, s->size, 1);
  s->stage_frames = frames; s->stage = w32_heap_alloc(W, (u64)frames * s->block, 1);
  if (!s->buf || !s->stage) { astr_free(s); RET(E_OUTOFMEMORY_); return; }
  s->event_mode = (flags & 0x40000) != 0; s->exclusive = mode == 1; o->stream = astr_idx(s); o->fmt_hns = (u64)hns;
  aud_log("[audio] IAudioClient::Initialize: %s, %s, %d Hz %d ch %d bit%s, buffer %u frames (%lld hns)\n", mode == 1 ? "exclusive" : "shared", s->event_mode ? "event-driven" : "polling", f.rate, f.ch, f.bits, f.isfloat ? " float" : "", frames, (long long)hns);
  RET(0);
}
static void aclient_Initialize(Cpu *c) { aclient_init_impl(c, aobj_of(c, ARG(0), AK_CLIENT), (u32)ARG(1), (u32)ARG(2), (s64)ARG(3), ARG(5)); }
static void aclient_GetBufferSize(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (ARG(1)) wr32(c, ARG(1), s->stage_frames); RET(0); }
static void aclient_GetStreamLatency(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (ARG(1)) wr64(c, ARG(1), 100000); RET(0); }
static void aclient_GetCurrentPadding(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (ARG(1)) wr32(c, ARG(1), s->filled / s->block); RET(0); }
static void aclient_IsFormatSupported(Cpu *c) {   /* (mode, pFormat, ppClosestMatch) */
  u64 fmtp = ARG(2), pp = ARG(3); WFmt f = wfmt_read(c, fmtp); if (pp) wr64(c, pp, 0);
  if (f.ok) { RET(0); return; }
  if (pp && (u32)ARG(1) == 0) { u64 b = w32_heap_alloc(W, 40, 1); wfmt_write(c, b, 2, g_aud_host_rate ? g_aud_host_rate : 48000, 32, 1, 1); wr64(c, pp, b); RET(1 /* S_FALSE */); return; }
  RET(AUDCLNT_E_UNSUPPORTED_FORMAT);
}
static void aclient_GetMixFormat(Cpu *c) { u64 pp = ARG(1); if (!pp) { RET(E_INVALIDARG_); return; } u64 b = w32_heap_alloc(W, 40, 1); if (!b) { RET(E_OUTOFMEMORY_); return; } wfmt_write(c, b, 2, g_aud_host_rate ? g_aud_host_rate : 48000, 32, 1, 1); wr64(c, pp, b); RET(0); }
static void aclient_GetDevicePeriod(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), 100000); if (ARG(2)) wr64(c, ARG(2), 30000); RET(0); }
static void aclient_Start(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (s->playing) { RET(AUDCLNT_E_NOT_STOPPED); return; } if (s->event_mode && s->ev_kobj < 0) { RET(AUDCLNT_E_EVENTHANDLE_NOT_SET); return; } s->playing = 1; s->started_once = 1; if (s->event_mode) kobj_signal(s->w, s->ev_kobj); RET(0); }
static void aclient_Stop(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } int was = s->playing; s->playing = 0; RET(was ? 0 : 1); }
static void aclient_Reset(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (s->playing) { RET(AUDCLNT_E_NOT_STOPPED); return; } s->play = 0; s->filled = 0; s->played_frames = 0; s->frac = 0; RET(0); }
static void aclient_SetEventHandle(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLIENT)); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } int k = kobj_of_event(c, ARG(1)); if (k < 0) { RET(E_INVALIDARG_); return; } s->ev_kobj = k; RET(0); }
static void aclient_GetService(Cpu *c) {
  AObj *o = aobj_of(c, ARG(0), AK_CLIENT); u64 riid = ARG(1), pp = ARG(2); if (!o || !riid || !pp) { RET(E_INVALIDARG_); return; } if (o->stream < 0) { RET(AUDCLNT_E_NOT_INITIALIZED); return; }
  u32 d1 = rd32(c, riid); int kind = d1 == 0xF294ACFCu ? AK_RENDER : (d1 == 0xCD63314Fu || d1 == 0x6f49ff73u) ? AK_CLOCK : d1 == 0x87CE5498u ? AK_SVOL : d1 == 0x93014887u ? AK_STRVOL : (d1 == 0xF4B1A599u || d1 == 0xbfb7ff88u) ? AK_SESSION : 0;
  if (d1 == 0xC8ADBD64u) { aud_log("[audio] IAudioClient::GetService(IAudioCaptureClient) on a render endpoint: AUDCLNT_E_WRONG_ENDPOINT_TYPE\n"); wr64(c, pp, 0); RET(AUDCLNT_E_WRONG_ENDPOINT_TYPE); return; }
  if (!kind) { aud_log("[audio] IAudioClient::GetService({%08x-…}): E_NOINTERFACE\n", d1); wr64(c, pp, 0); RET(E_NOINTERFACE_); return; }
  u64 a; AObj *n = aobj_new(c, kind, &a); if (!n) { RET(E_OUTOFMEMORY_); return; } n->stream = o->stream; wr64(c, pp, a); RET(0);
}
static void aclient_IsOffloadCapable(Cpu *c) { if (ARG(2)) wr32(c, ARG(2), 0); RET(0); }
static void aclient_SetClientProperties(Cpu *c) { RET(0); }
static void aclient_GetBufferSizeLimits(Cpu *c) { if (ARG(3)) wr64(c, ARG(3), 30000); if (ARG(4)) wr64(c, ARG(4), 50000000); RET(0); }
static void aclient_GetSharedModeEnginePeriod(Cpu *c) { /* (pFormat, pDefault, pFundamental, pMin, pMax) in frames */ WFmt f = wfmt_read(c, ARG(1)); u32 r = f.ok ? (u32)f.rate : 48000; if (ARG(2)) wr32(c, ARG(2), r / 100); if (ARG(3)) wr32(c, ARG(3), r / 1000); if (ARG(4)) wr32(c, ARG(4), r / 1000 * 3); if (ARG(5)) wr32(c, ARG(5), r / 2); RET(0); }
static void aclient_GetCurrentSharedModeEnginePeriod(Cpu *c) { u64 pp = ARG(1); if (pp) { u64 b = w32_heap_alloc(W, 40, 1); wfmt_write(c, b, 2, g_aud_host_rate ? g_aud_host_rate : 48000, 32, 1, 1); wr64(c, pp, b); } if (ARG(2)) wr32(c, ARG(2), (u32)(g_aud_host_rate ? g_aud_host_rate : 48000) / 100); RET(0); }
static void aclient_InitializeSharedAudioStream(Cpu *c) {   /* (flags, periodFrames, pFormat, guid) → shared Initialize with 3 periods of buffer */
  WFmt f = wfmt_read(c, ARG(3)); u32 per = (u32)ARG(2); if (!f.ok) { RET(AUDCLNT_E_UNSUPPORTED_FORMAT); return; }
  aclient_init_impl(c, aobj_of(c, ARG(0), AK_CLIENT), 0, (u32)ARG(1), (s64)((u64)per * 3 * 10000000ull / (u64)f.rate), ARG(3));
}
static void render_GetBuffer(Cpu *c) {
  AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_RENDER)); u32 n = (u32)ARG(1); u64 pp = ARG(2); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } if (!pp) { RET(E_INVALIDARG_); return; }
  if (s->closed) { RET(AUDCLNT_E_OUT_OF_ORDER); return; }   /* GetBuffer twice without ReleaseBuffer */
  if (n == 0) { wr64(c, pp, 0); RET(0); return; }
  u32 free_frames = (s->size - s->filled) / s->block; if (n > s->stage_frames || n > free_frames) { RET(AUDCLNT_E_BUFFER_TOO_LARGE); return; }
  s->closed = 1; wr64(c, pp, s->stage); RET(0);
}
static void render_ReleaseBuffer(Cpu *c) {
  AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_RENDER)); u32 n = (u32)ARG(1), fl = (u32)ARG(2); if (!s) { RET(AUDCLNT_E_NOT_INITIALIZED); return; }
  if (!s->closed) { if (n == 0) { RET(0); return; } RET(AUDCLNT_E_OUT_OF_ORDER); return; } s->closed = 0;
  if (n > s->stage_frames) { RET(AUDCLNT_E_INVALID_SIZE); return; }
  u64 bytes = (u64)n * s->block; u32 wpos = (s->play + s->filled) % s->size;
  u8 *src = mem_ptr(c->mem, s->stage, bytes, 0); if (!src && bytes) { RET(E_INVALIDARG_); return; }
  for (u64 done = 0; done < bytes;) { u32 chunk = (u32)(bytes - done); if (chunk > s->size - wpos) chunk = s->size - wpos; u8 *dst = mem_ptr(c->mem, s->buf + wpos, chunk, 1); if (!dst) break; if (fl & 2) e_memset(dst, s->bits == 8 && !s->isfloat ? 0x80 : 0, chunk); else e_memcpy(dst, src + done, chunk); done += chunk; wpos = (wpos + chunk) % s->size; }
  s->filled += (u32)bytes; RET(0);
}
static void clock_GetFrequency(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLOCK)); if (!s || !ARG(1)) { RET(E_INVALIDARG_); return; } wr64(c, ARG(1), (u64)s->rate); RET(0); }
static void clock_GetPosition(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_CLOCK)); if (!s || !ARG(1)) { RET(E_INVALIDARG_); return; } wr64(c, ARG(1), s->played_frames > s->underruns ? s->played_frames - s->underruns : 0); if (ARG(2)) wr64(c, ARG(2), (u64)(host_now_ms() * 10000.0)); RET(0); }
static void clock_GetCharacteristics(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(0); }
static void clock_GetDevicePosition(Cpu *c) { clock_GetPosition(c); }
static void svol_SetMasterVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_SVOL)); if (!s) { RET(E_INVALIDARG_); return; } float v = c->x[1].f32[0]; if (v < 0 || v > 1) { RET(E_INVALIDARG_); return; } s->vol = v; RET(0); }
static void svol_GetMasterVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_SVOL)); if (!s || !ARG(1)) { RET(E_INVALIDARG_); return; } float v = s->vol; u32 b; e_memcpy(&b, &v, 4); wr32(c, ARG(1), b); RET(0); }
static void svol_SetMute(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_SVOL)); if (!s) { RET(E_INVALIDARG_); return; } s->mute = (u32)ARG(1) != 0; s->paused = s->mute ? 2 : 0; RET(0); }
static void svol_GetMute(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_SVOL)); if (!s || !ARG(1)) { RET(E_INVALIDARG_); return; } wr32(c, ARG(1), s->mute); RET(0); }
static void strvol_GetChannelCount(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_STRVOL)); if (!s || !ARG(1)) { RET(E_INVALIDARG_); return; } wr32(c, ARG(1), (u32)s->ch); RET(0); }
static void strvol_SetChannelVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_STRVOL)); u32 i = (u32)ARG(1); if (!s || i >= (u32)s->ch) { RET(E_INVALIDARG_); return; } s->chvol[i < 8 ? i : 7] = c->x[2].f32[0]; RET(0); }
static void strvol_GetChannelVolume(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_STRVOL)); u32 i = (u32)ARG(1); if (!s || i >= (u32)s->ch || !ARG(2)) { RET(E_INVALIDARG_); return; } float v = s->chvol[i < 8 ? i : 7]; u32 b; e_memcpy(&b, &v, 4); wr32(c, ARG(2), b); RET(0); }
static void strvol_SetAllVolumes(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_STRVOL)); u32 n = (u32)ARG(1); u64 arr = ARG(2); if (!s || n != (u32)s->ch || !arr) { RET(E_INVALIDARG_); return; } for (u32 i = 0; i < n && i < 8; i++) { u32 b = rd32(c, arr + 4 * i); float v; e_memcpy(&v, &b, 4); s->chvol[i] = v; } RET(0); }
static void strvol_GetAllVolumes(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_STRVOL)); u32 n = (u32)ARG(1); u64 arr = ARG(2); if (!s || n != (u32)s->ch || !arr) { RET(E_INVALIDARG_); return; } for (u32 i = 0; i < n && i < 8; i++) { u32 b; e_memcpy(&b, &s->chvol[i], 4); wr32(c, arr + 4 * i, b); } RET(0); }
static void session_GetState(Cpu *c) { AStream *s = aobj_stream(aobj_of(c, ARG(0), AK_SESSION)); if (ARG(1)) wr32(c, ARG(1), s && s->playing ? 1 : 0); RET(0); }
static void session_GetDisplayName(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), aud_wstr_cotask(c, "")); RET(0); }
static void session_GetIconPath(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), aud_wstr_cotask(c, "")); RET(0); }
static void session_GetGroupingParam(Cpu *c) { if (ARG(1)) for (int i = 0; i < 16; i++) wr8(c, ARG(1) + i, 0); RET(0); }
static void session_GetSessionIdentifier(Cpu *c) { if (ARG(1)) wr64(c, ARG(1), aud_wstr_cotask(c, "{nebula-session}")); RET(0); }
static void session_GetProcessId(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), (u32)W->pid); RET(0); }
static void session_IsSystemSoundsSession(Cpu *c) { RET(1 /* S_FALSE */); }
static float g_ep_master = 1.0f; static int g_ep_mute;
static void epvol_GetChannelCount(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 2); RET(0); }
static void epvol_SetMasterVolumeLevelScalar(Cpu *c) { float v = c->x[1].f32[0]; if (v < 0 || v > 1) { RET(E_INVALIDARG_); return; } g_ep_master = v; RET(0); }
static void epvol_GetMasterVolumeLevelScalar(Cpu *c) { if (!ARG(1)) { RET(E_INVALIDARG_); return; } u32 b; e_memcpy(&b, &g_ep_master, 4); wr32(c, ARG(1), b); RET(0); }
static void epvol_SetMasterVolumeLevel(Cpu *c) { float db = c->x[1].f32[0]; g_ep_master = db <= -96 ? 0 : (float)host_math(5, 10.0, db / 20.0); if (g_ep_master > 1) g_ep_master = 1; RET(0); }
static void epvol_GetMasterVolumeLevel(Cpu *c) { if (!ARG(1)) { RET(E_INVALIDARG_); return; } float db = g_ep_master <= 0 ? -96.0f : (float)(20.0 * host_math(13, g_ep_master, 0)); u32 b; e_memcpy(&b, &db, 4); wr32(c, ARG(1), b); RET(0); }
static void epvol_GetChannelVolumeLevelScalar(Cpu *c) { if (!ARG(2)) { RET(E_INVALIDARG_); return; } u32 b; e_memcpy(&b, &g_ep_master, 4); wr32(c, ARG(2), b); RET(0); }
static void epvol_SetMute(Cpu *c) { g_ep_mute = (u32)ARG(1) != 0; RET(0); }
static void epvol_GetMute(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), (u32)g_ep_mute); RET(0); }
static void epvol_GetVolumeRange(Cpu *c) { float a = -96, b = 0, st = 1; u32 x; if (ARG(1)) { e_memcpy(&x, &a, 4); wr32(c, ARG(1), x); } if (ARG(2)) { e_memcpy(&x, &b, 4); wr32(c, ARG(2), x); } if (ARG(3)) { e_memcpy(&x, &st, 4); wr32(c, ARG(3), x); } RET(0); }
static void epvol_QueryHardwareSupport(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 0); RET(0); }
static void epvol_GetVolumeStepInfo(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), (u32)(g_ep_master * 100)); if (ARG(2)) wr32(c, ARG(2), 101); RET(0); }

/* ---- ole32: CoCreateInstance for the classes we know ---- */
typedef struct { u32 d1; const char *dll; const char *what; } ComClass;
static const ComClass g_com_classes[] = {
  { 0xBCDE0395u, 0, "MMDeviceEnumerator" }, { 0x47D4D946u, 0, "DirectSound" }, { 0x3901CC3Fu, 0, "DirectSound8" }, { 0xB0210780u, 0, "DirectSoundCapture" }, { 0xE4BCAC13u, 0, "DirectSoundCapture8" },
  { 0x5a508685u, "XAudio2_7.dll", "XAudio2 2.7" }, { 0xdb05ea35u, "XAudio2_7.dll", "XAudio2 2.7 (debug)" }, { 0x3eda9b49u, "XAudio2_6.dll", "XAudio2 2.6" }, { 0x47199894u, "XAudio2_6.dll", "XAudio2 2.6 (debug)" },
  { 0x4c5e637au, "XAudio2_5.dll", "XAudio2 2.5" }, { 0x715bdd1au, "XAudio2_5.dll", "XAudio2 2.5 (debug)" }, { 0x03219e78u, "XAudio2_4.dll", "XAudio2 2.4" }, { 0x4256535cu, "XAudio2_4.dll", "XAudio2 2.4 (debug)" },
  { 0x4c9b6ddeu, "XAudio2_3.dll", "XAudio2 2.3" }, { 0xef0aa05du, "XAudio2_3.dll", "XAudio2 2.3 (debug)" }, { 0xb802058au, "XAudio2_2.dll", "XAudio2 2.2" }, { 0x97dfb7e7u, "XAudio2_2.dll", "XAudio2 2.2 (debug)" },
  { 0xe21a7345u, "XAudio2_1.dll", "XAudio2 2.1" }, { 0xf7a76c21u, "XAudio2_1.dll", "XAudio2 2.1 (debug)" }, { 0xfac23f48u, "XAudio2_0.dll", "XAudio2 2.0" }, { 0xfac23f49u, "XAudio2_0.dll", "XAudio2 2.0 (debug)" },
  { 0xbcc782bcu, "xactengine3_7.dll", "XACT3 3.7" }, { 0x248d8a3bu, "xactengine3_6.dll", "XACT3 3.6" }, { 0x074b110fu, "xactengine3_5.dll", "XACT3 3.5" }, { 0xbc3e0fc6u, "xactengine3_4.dll", "XACT3 3.4" }, { 0x94c1affau, "xactengine3_3.dll", "XACT3 3.3" }, { 0xd3332f02u, "xactengine3_2.dll", "XACT3 3.2" }, { 0x962f5027u, "xactengine3_1.dll", "XACT3 3.1" }, { 0x3b80ee2au, "xactengine3_0.dll", "XACT3 3.0" },
  { 0, 0, 0 } };
static int shell_cocreate(Cpu *c, u32 clsid_d1, u64 riid, u64 ppv);
static void s_CoCreateInstance(Cpu *c) {
  u64 clsid = ARG(0), riid = ARG(3), ppv = ARG(4); if (!clsid || !riid || !ppv) { RET(E_INVALIDARG_); return; }   /* (rclsid, pUnkOuter, dwClsContext, riid, ppv) */
  u32 d1 = rd32(c, clsid), i1 = rd32(c, riid); wrp(c, ppv, 0);
  if (shell_cocreate(c, d1, riid, ppv)) return;
  if (gdi_wic_cocreate(c, d1, riid, ppv)) return;
  const ComClass *k = 0; for (int i = 0; g_com_classes[i].what; i++) if (g_com_classes[i].d1 == d1) { k = &g_com_classes[i]; break; }
  if (!k) { aud_log("[com] CoCreateInstance({%08x-…}): class not registered — the emulator provides MMDeviceEnumerator, DirectSound(8), WICImagingFactory, ShellLink and routes XAudio2/XACT to the DirectX-pack DLLs\n", d1); RET(0x80040154u); return; }
  if (!k->dll) {
    if (d1 == 0xBCDE0395u) { if (i1 != 0xA95664D2u && i1 != 0) { RET(E_NOINTERFACE_); return; } mme_create_enum(c, ppv); return; }
    if (d1 == 0x47D4D946u || d1 == 0x3901CC3Fu) { u64 a; AObj *o = aobj_new(c, AK_DS, &a); if (!o) { RET(E_OUTOFMEMORY_); return; } o->ds = d1 == 0x3901CC3Fu; wrp(c, ppv, a); RET(0); return; }   /* needs Initialize() like Windows */
    aud_log("[audio] CoCreateInstance(%s): no capture device (REGDB_E_CLASSNOTREG)\n", k->what); RET(0x80040154u); return;
  }
  /* a COM class served by a real DLL from the DirectX pack: load it (DllMain via the CONT_DLLMAIN continuation) → DllGetClassObject → IClassFactory::CreateInstance → Release (guest code, one continuation step each) */
  int m = mod_load(W, k->dll);
  if (m < 0) { aud_log("[com] CoCreateInstance(%s): %s is not on the virtual disk — install the DirectX pack\n", k->what, k->dll); RET(0x80040154u); return; }
  u64 fn = mod_export(W, m, "DllGetClassObject", 0, 0); if (!fn) { aud_log("[com] %s has no DllGetClassObject\n", k->dll); RET(0x80040154u); return; }
  u64 slot = w32_heap_alloc(W, 64, 1); if (!slot) { RET(E_OUTOFMEMORY_); return; }
  static const u8 iid_cf[16] = { 1,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46 }; for (int i = 0; i < 16; i++) { wr8(c, slot + 8 + i, iid_cf[i]); wr8(c, slot + 32 + i, rd8(c, clsid + i)); }
  int ci = cont_push(c, CONT_COCREATE); if (ci < 0) return; W->cont[ci].data[0] = 0; W->cont[ci].data[1] = slot; W->cont[ci].data[2] = riid; W->cont[ci].data[3] = ppv; W->cont[ci].data[5] = fn;
  aud_log("[com] CoCreateInstance(%s) → %s!DllGetClassObject\n", k->what, k->dll);
  if (!W->mods[m].attached || W->nmods > m + 1) {   /* DllMain(s) first; when they are done the DLLMAIN continuation returns into ours (ret_addr = the continuation slot) */
    W->cont[ci].data[0] = (u64)-1; c->rip = SLOT_ADDR(SLOT_CONT); int j = cont_push(c, CONT_DLLMAIN); if (j < 0) return; W->cont[j].data[0] = W->mods[m].base; w32_cont_resume(c); return; }
  guest_call(c, fn, slot + 32, slot + 8, slot, 0);
}
static void s_CoCreateInstanceEx(Cpu *c) { aud_log("[com] CoCreateInstanceEx: not supported (use CoCreateInstance)\n"); RET(0x80040154u); }
static void s_PropVariantClear(Cpu *c) { u64 pv = ARG(0); if (!pv) { RET(0); return; } u16 vt = rd16(c, pv); if (vt == 31 || vt == 30) { u64 p = rdp(c, pv + 8); if (p) w32_heap_free(W, p); } else if (vt == 65) { u64 p = rdp(c, pv + 8 + PSZ_OF(c)); if (p) w32_heap_free(W, p); } for (u32 i = 0; i < (c->mode32 ? 16u : 24u); i++) wr8(c, pv + i, 0); RET(0); }
static void s_CLSIDFromString(Cpu *c) {   /* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (wide) → GUID */
  char s[64]; wstr_to_utf8(c, ARG(0), s, sizeof s, -1); u64 out = ARG(1); if (!out) { RET(E_INVALIDARG_); return; }
  const char *p = s; if (*p == '{') p++; u32 v[11]; int vi = 0; static const int lens[] = { 8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2 };
  for (vi = 0; vi < 11; vi++) { u32 x = 0; for (int i = 0; i < lens[vi]; i++, p++) { int h = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1; if (h < 0) { RET(0x800401F3u /* CO_E_CLASSSTRING */); return; } x = x * 16 + (u32)h; } v[vi] = x; if (vi < 4 && *p == '-') p++; }
  wr32(c, out, v[0]); wr16(c, out + 4, (u16)v[1]); wr16(c, out + 6, (u16)v[2]); for (int i = 0; i < 8; i++) wr8(c, out + 8 + i, (u8)v[3 + i]); RET(0);
}
static void s_IIDFromString(Cpu *c) { s_CLSIDFromString(c); }
static void s_lstrcmpW(Cpu *c) { char a[1024], b[1024]; wstr_to_utf8(c, ARG(0), a, sizeof a, -1); wstr_to_utf8(c, ARG(1), b, sizeof b, -1); int r = e_strcmp(a, b); RET((u64)(s64)(r < 0 ? -1 : r > 0 ? 1 : 0)); }
static void s_AvSetMmThreadCharacteristicsA(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 1); RET(0x1A51); }
static void s_AvSetMmThreadCharacteristicsW(Cpu *c) { if (ARG(1)) wr32(c, ARG(1), 1); RET(0x1A51); }
static void s_AvRevertMmThreadCharacteristics(Cpu *c) { RET(1); }
static void s_AvSetMmThreadPriority(Cpu *c) { RET(1); }

#define AUD_SHIMS \
  S(waveOutGetNumDevs), S(waveOutGetDevCapsA), S(waveOutGetDevCapsW), S(waveOutOpen), S(waveOutClose), S(waveOutPrepareHeader), S(waveOutUnprepareHeader), S(waveOutWrite), S(waveOutReset), S(waveOutPause), S(waveOutRestart), \
  S(waveOutBreakLoop), S(waveOutGetPosition), S(waveOutGetVolume), S(waveOutSetVolume), S(waveOutGetID), S(waveOutGetErrorTextA), S(waveOutGetErrorTextW), S(waveOutMessage), S(waveOutGetPitch), S(waveOutSetPitch), S(waveOutGetPlaybackRate), S(waveOutSetPlaybackRate), \
  S(waveInGetNumDevs), S(waveInOpen), S(waveInGetDevCapsA), SA("waveInGetDevCapsW", waveInGetDevCapsA), S(midiOutGetNumDevs), S(midiOutOpen), S(midiInGetNumDevs), S(mixerGetNumDevs), S(mixerOpen), S(auxGetNumDevs), \
  S(mciSendStringA), SA("mciSendStringW", mciSendStringA), S(mciSendCommandA), SA("mciSendCommandW", mciSendCommandA), S(mciGetErrorStringA), SA("mciGetErrorStringW", mciGetErrorStringA), \
  S(PlaySoundA), S(PlaySoundW), S(sndPlaySoundA), S(sndPlaySoundW), S(nebula_audio_thunk), \
  S(DirectSoundCreate), S(DirectSoundCreate8), S(DirectSoundCaptureCreate), S(DirectSoundCaptureCreate8), S(DirectSoundCaptureEnumerateA), S(DirectSoundCaptureEnumerateW), S(DirectSoundEnumerateA), S(DirectSoundEnumerateW), S(GetDeviceID), \
  S(CoCreateInstance), S(CoCreateInstanceEx), S(PropVariantClear), S(CLSIDFromString), S(IIDFromString), S(lstrcmpW), S(AvSetMmThreadCharacteristicsA), S(AvSetMmThreadCharacteristicsW), S(AvRevertMmThreadCharacteristics), S(AvSetMmThreadPriority), \
  DS("IDirectSound8::QueryInterface", aud_QueryInterface), DS("IDirectSound8::AddRef", aud_AddRef), DS("IDirectSound8::Release", aud_Release), DS("IDirectSound8::CreateSoundBuffer", ds_CreateSoundBuffer), DS("IDirectSound8::GetCaps", ds_GetCaps), \
  DS("IDirectSound8::DuplicateSoundBuffer", ds_DuplicateSoundBuffer), DS("IDirectSound8::SetCooperativeLevel", ds_SetCooperativeLevel), DS("IDirectSound8::Compact", aud_ok), DS("IDirectSound8::GetSpeakerConfig", ds_GetSpeakerConfig), DS("IDirectSound8::SetSpeakerConfig", aud_ok), DS("IDirectSound8::Initialize", ds_Initialize), DS("IDirectSound8::VerifyCertification", aud_ok), \
  DS("IDirectSoundBuffer8::QueryInterface", aud_QueryInterface), DS("IDirectSoundBuffer8::AddRef", aud_AddRef), DS("IDirectSoundBuffer8::Release", aud_Release), DS("IDirectSoundBuffer8::GetCaps", dsb_GetCaps), DS("IDirectSoundBuffer8::GetCurrentPosition", dsb_GetCurrentPosition), \
  DS("IDirectSoundBuffer8::GetFormat", dsb_GetFormat), DS("IDirectSoundBuffer8::GetVolume", dsb_GetVolume), DS("IDirectSoundBuffer8::GetPan", dsb_GetPan), DS("IDirectSoundBuffer8::GetFrequency", dsb_GetFrequency), DS("IDirectSoundBuffer8::GetStatus", dsb_GetStatus), DS("IDirectSoundBuffer8::Initialize", dsb_Initialize), \
  DS("IDirectSoundBuffer8::Lock", dsb_Lock), DS("IDirectSoundBuffer8::Play", dsb_Play), DS("IDirectSoundBuffer8::SetCurrentPosition", dsb_SetCurrentPosition), DS("IDirectSoundBuffer8::SetFormat", dsb_SetFormat), DS("IDirectSoundBuffer8::SetVolume", dsb_SetVolume), DS("IDirectSoundBuffer8::SetPan", dsb_SetPan), \
  DS("IDirectSoundBuffer8::SetFrequency", dsb_SetFrequency), DS("IDirectSoundBuffer8::Stop", dsb_Stop), DS("IDirectSoundBuffer8::Unlock", dsb_Unlock), DS("IDirectSoundBuffer8::Restore", dsb_Restore), DS("IDirectSoundBuffer8::SetFX", dsb_SetFX), DS("IDirectSoundBuffer8::AcquireResources", dsb_AcquireResources), DS("IDirectSoundBuffer8::GetObjectInPath", dsb_GetObjectInPath), \
  DS("IDirectSoundNotify::QueryInterface", aud_QueryInterface), DS("IDirectSoundNotify::AddRef", aud_AddRef), DS("IDirectSoundNotify::Release", aud_Release), DS("IDirectSoundNotify::SetNotificationPositions", dsn_SetNotificationPositions), \
  DS("IDirectSound3DListener::QueryInterface", aud_QueryInterface), DS("IDirectSound3DListener::AddRef", aud_AddRef), DS("IDirectSound3DListener::Release", aud_Release), \
  DS("IDirectSound3DListener::GetAllParameters", aud_ok), DS("IDirectSound3DListener::GetDistanceFactor", aud_ok), DS("IDirectSound3DListener::GetDopplerFactor", aud_ok), DS("IDirectSound3DListener::GetOrientation", aud_ok), DS("IDirectSound3DListener::GetPosition", aud_ok), DS("IDirectSound3DListener::GetRolloffFactor", aud_ok), DS("IDirectSound3DListener::GetVelocity", aud_ok), \
  DS("IDirectSound3DListener::SetAllParameters", aud_ok), DS("IDirectSound3DListener::SetDistanceFactor", aud_ok), DS("IDirectSound3DListener::SetDopplerFactor", aud_ok), DS("IDirectSound3DListener::SetOrientation", aud_ok), DS("IDirectSound3DListener::SetPosition", aud_ok), DS("IDirectSound3DListener::SetRolloffFactor", aud_ok), DS("IDirectSound3DListener::SetVelocity", aud_ok), DS("IDirectSound3DListener::CommitDeferredSettings", aud_ok), \
  DS("IDirectSound3DBuffer::QueryInterface", aud_QueryInterface), DS("IDirectSound3DBuffer::AddRef", aud_AddRef), DS("IDirectSound3DBuffer::Release", aud_Release), \
  DS("IDirectSound3DBuffer::GetAllParameters", aud_ok), DS("IDirectSound3DBuffer::GetConeAngles", aud_ok), DS("IDirectSound3DBuffer::GetConeOrientation", aud_ok), DS("IDirectSound3DBuffer::GetConeOutsideVolume", aud_ok), DS("IDirectSound3DBuffer::GetMaxDistance", aud_ok), DS("IDirectSound3DBuffer::GetMinDistance", aud_ok), DS("IDirectSound3DBuffer::GetMode", aud_ok), DS("IDirectSound3DBuffer::GetPosition", aud_ok), DS("IDirectSound3DBuffer::GetVelocity", aud_ok), \
  DS("IDirectSound3DBuffer::SetAllParameters", aud_ok), DS("IDirectSound3DBuffer::SetConeAngles", aud_ok), DS("IDirectSound3DBuffer::SetConeOrientation", aud_ok), DS("IDirectSound3DBuffer::SetConeOutsideVolume", aud_ok), DS("IDirectSound3DBuffer::SetMaxDistance", aud_ok), DS("IDirectSound3DBuffer::SetMinDistance", aud_ok), DS("IDirectSound3DBuffer::SetMode", aud_ok), DS("IDirectSound3DBuffer::SetPosition", aud_ok), DS("IDirectSound3DBuffer::SetVelocity", aud_ok), \
  DS("IMMDeviceEnumerator::QueryInterface", aud_QueryInterface), DS("IMMDeviceEnumerator::AddRef", aud_AddRef), DS("IMMDeviceEnumerator::Release", aud_Release), DS("IMMDeviceEnumerator::EnumAudioEndpoints", mme_EnumAudioEndpoints), DS("IMMDeviceEnumerator::GetDefaultAudioEndpoint", mme_GetDefaultAudioEndpoint), \
  DS("IMMDeviceEnumerator::GetDevice", mme_GetDevice), DS("IMMDeviceEnumerator::RegisterEndpointNotificationCallback", aud_ok), DS("IMMDeviceEnumerator::UnregisterEndpointNotificationCallback", aud_ok), \
  DS("IMMDeviceCollection::QueryInterface", aud_QueryInterface), DS("IMMDeviceCollection::AddRef", aud_AddRef), DS("IMMDeviceCollection::Release", aud_Release), DS("IMMDeviceCollection::GetCount", coll_GetCount), DS("IMMDeviceCollection::Item", coll_Item), \
  DS("IMMDevice::QueryInterface", aud_QueryInterface), DS("IMMDevice::AddRef", aud_AddRef), DS("IMMDevice::Release", aud_Release), DS("IMMDevice::Activate", dev_Activate), DS("IMMDevice::OpenPropertyStore", dev_OpenPropertyStore), DS("IMMDevice::GetId", dev_GetId), DS("IMMDevice::GetState", dev_GetState), \
  DS("IMMEndpoint::QueryInterface", aud_QueryInterface), DS("IMMEndpoint::AddRef", aud_AddRef), DS("IMMEndpoint::Release", aud_Release), DS("IMMEndpoint::GetDataFlow", ep_GetDataFlow), \
  DS("IPropertyStore::QueryInterface", aud_QueryInterface), DS("IPropertyStore::AddRef", aud_AddRef), DS("IPropertyStore::Release", aud_Release), DS("IPropertyStore::GetCount", props_GetCount), DS("IPropertyStore::GetAt", props_GetAt), DS("IPropertyStore::GetValue", props_GetValue), DS("IPropertyStore::SetValue", props_SetValue), DS("IPropertyStore::Commit", aud_ok), \
  DS("IAudioClient3::QueryInterface", aud_QueryInterface), DS("IAudioClient3::AddRef", aud_AddRef), DS("IAudioClient3::Release", aud_Release), DS("IAudioClient3::Initialize", aclient_Initialize), DS("IAudioClient3::GetBufferSize", aclient_GetBufferSize), DS("IAudioClient3::GetStreamLatency", aclient_GetStreamLatency), \
  DS("IAudioClient3::GetCurrentPadding", aclient_GetCurrentPadding), DS("IAudioClient3::IsFormatSupported", aclient_IsFormatSupported), DS("IAudioClient3::GetMixFormat", aclient_GetMixFormat), DS("IAudioClient3::GetDevicePeriod", aclient_GetDevicePeriod), DS("IAudioClient3::Start", aclient_Start), DS("IAudioClient3::Stop", aclient_Stop), \
  DS("IAudioClient3::Reset", aclient_Reset), DS("IAudioClient3::SetEventHandle", aclient_SetEventHandle), DS("IAudioClient3::GetService", aclient_GetService), DS("IAudioClient3::IsOffloadCapable", aclient_IsOffloadCapable), DS("IAudioClient3::SetClientProperties", aclient_SetClientProperties), DS("IAudioClient3::GetBufferSizeLimits", aclient_GetBufferSizeLimits), \
  DS("IAudioClient3::GetSharedModeEnginePeriod", aclient_GetSharedModeEnginePeriod), DS("IAudioClient3::GetCurrentSharedModeEnginePeriod", aclient_GetCurrentSharedModeEnginePeriod), DS("IAudioClient3::InitializeSharedAudioStream", aclient_InitializeSharedAudioStream), \
  DS("IAudioRenderClient::QueryInterface", aud_QueryInterface), DS("IAudioRenderClient::AddRef", aud_AddRef), DS("IAudioRenderClient::Release", aud_Release), DS("IAudioRenderClient::GetBuffer", render_GetBuffer), DS("IAudioRenderClient::ReleaseBuffer", render_ReleaseBuffer), \
  DS("IAudioClock::QueryInterface", aud_QueryInterface), DS("IAudioClock::AddRef", aud_AddRef), DS("IAudioClock::Release", aud_Release), DS("IAudioClock::GetFrequency", clock_GetFrequency), DS("IAudioClock::GetPosition", clock_GetPosition), DS("IAudioClock::GetCharacteristics", clock_GetCharacteristics), DS("IAudioClock::GetDevicePosition", clock_GetDevicePosition), \
  DS("ISimpleAudioVolume::QueryInterface", aud_QueryInterface), DS("ISimpleAudioVolume::AddRef", aud_AddRef), DS("ISimpleAudioVolume::Release", aud_Release), DS("ISimpleAudioVolume::SetMasterVolume", svol_SetMasterVolume), DS("ISimpleAudioVolume::GetMasterVolume", svol_GetMasterVolume), DS("ISimpleAudioVolume::SetMute", svol_SetMute), DS("ISimpleAudioVolume::GetMute", svol_GetMute), \
  DS("IAudioStreamVolume::QueryInterface", aud_QueryInterface), DS("IAudioStreamVolume::AddRef", aud_AddRef), DS("IAudioStreamVolume::Release", aud_Release), DS("IAudioStreamVolume::GetChannelCount", strvol_GetChannelCount), DS("IAudioStreamVolume::SetChannelVolume", strvol_SetChannelVolume), DS("IAudioStreamVolume::GetChannelVolume", strvol_GetChannelVolume), DS("IAudioStreamVolume::SetAllVolumes", strvol_SetAllVolumes), DS("IAudioStreamVolume::GetAllVolumes", strvol_GetAllVolumes), \
  DS("IAudioSessionControl2::QueryInterface", aud_QueryInterface), DS("IAudioSessionControl2::AddRef", aud_AddRef), DS("IAudioSessionControl2::Release", aud_Release), DS("IAudioSessionControl2::GetState", session_GetState), DS("IAudioSessionControl2::GetDisplayName", session_GetDisplayName), DS("IAudioSessionControl2::SetDisplayName", aud_ok), \
  DS("IAudioSessionControl2::GetIconPath", session_GetIconPath), DS("IAudioSessionControl2::SetIconPath", aud_ok), DS("IAudioSessionControl2::GetGroupingParam", session_GetGroupingParam), DS("IAudioSessionControl2::SetGroupingParam", aud_ok), DS("IAudioSessionControl2::RegisterAudioSessionNotification", aud_ok), DS("IAudioSessionControl2::UnregisterAudioSessionNotification", aud_ok), \
  DS("IAudioSessionControl2::GetSessionIdentifier", session_GetSessionIdentifier), DS("IAudioSessionControl2::GetSessionInstanceIdentifier", session_GetSessionIdentifier), DS("IAudioSessionControl2::GetProcessId", session_GetProcessId), DS("IAudioSessionControl2::IsSystemSoundsSession", session_IsSystemSoundsSession), DS("IAudioSessionControl2::SetDuckingPreference", aud_ok), \
  DS("IAudioEndpointVolume::QueryInterface", aud_QueryInterface), DS("IAudioEndpointVolume::AddRef", aud_AddRef), DS("IAudioEndpointVolume::Release", aud_Release), DS("IAudioEndpointVolume::RegisterControlChangeNotify", aud_ok), DS("IAudioEndpointVolume::UnregisterControlChangeNotify", aud_ok), DS("IAudioEndpointVolume::GetChannelCount", epvol_GetChannelCount), \
  DS("IAudioEndpointVolume::SetMasterVolumeLevel", epvol_SetMasterVolumeLevel), DS("IAudioEndpointVolume::SetMasterVolumeLevelScalar", epvol_SetMasterVolumeLevelScalar), DS("IAudioEndpointVolume::GetMasterVolumeLevel", epvol_GetMasterVolumeLevel), DS("IAudioEndpointVolume::GetMasterVolumeLevelScalar", epvol_GetMasterVolumeLevelScalar), \
  DS("IAudioEndpointVolume::SetChannelVolumeLevel", aud_ok), DS("IAudioEndpointVolume::SetChannelVolumeLevelScalar", aud_ok), DS("IAudioEndpointVolume::GetChannelVolumeLevel", epvol_GetMasterVolumeLevel), DS("IAudioEndpointVolume::GetChannelVolumeLevelScalar", epvol_GetChannelVolumeLevelScalar), DS("IAudioEndpointVolume::SetMute", epvol_SetMute), DS("IAudioEndpointVolume::GetMute", epvol_GetMute), \
  DS("IAudioEndpointVolume::GetVolumeStepInfo", epvol_GetVolumeStepInfo), DS("IAudioEndpointVolume::VolumeStepUp", aud_ok), DS("IAudioEndpointVolume::VolumeStepDown", aud_ok), DS("IAudioEndpointVolume::QueryHardwareSupport", epvol_QueryHardwareSupport), DS("IAudioEndpointVolume::GetVolumeRange", epvol_GetVolumeRange),
