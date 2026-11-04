/* audio.exe — the Windows sound APIs end to end on the emulator's host audio output:
 *   [1] winmm waveOut (CALLBACK_EVENT + CALLBACK_FUNCTION), [2] DirectSound 8 (secondary buffer, notifications, volume, duplicate),
 *   [3] WASAPI (IMMDeviceEnumerator → IAudioClient event-driven shared mode → IAudioRenderClient/IAudioClock),
 *   [4] PlaySound(SND_MEMORY | SND_SYNC), [5] the REAL XAudio2_7.dll from the DirectX pack (CoCreateInstance → mastering + source voice)
 * Each section plays a different tone for a short time; exit code = number of failed sections.  Build:
 *   zig cc -target x86_64-windows-gnu -O2 -o audio.exe audio.c -lwinmm -lole32 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); return 1; } } while (0)
static void fill_s16(short *dst, int frames, int ch, int rate, double hz, double *phase) { for (int i = 0; i < frames; i++) { short v = (short)(sin(*phase) * 12000); *phase += 2 * 3.14159265358979 * hz / rate; for (int k = 0; k < ch; k++) dst[i * ch + k] = v; } }

/* ---- [1] waveOut ---- */
static volatile int g_wom_done, g_wom_open, g_wom_close;
static void CALLBACK wo_cb(HWAVEOUT hwo, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) { (void)hwo; (void)p1; (void)p2; if (inst != 0x1234) return; if (msg == WOM_DONE) g_wom_done++; else if (msg == WOM_OPEN) g_wom_open++; else if (msg == WOM_CLOSE) g_wom_close++; }
static int test_waveout(void) {
  printf("[1] waveOut: %u output device(s)\n", waveOutGetNumDevs()); CHECK(waveOutGetNumDevs() >= 1, "no waveOut device");
  WAVEOUTCAPSA caps; CHECK(waveOutGetDevCapsA(0, &caps, sizeof caps) == MMSYSERR_NOERROR, "waveOutGetDevCaps"); printf("  device 0: %s, %u channels\n", caps.szPname, caps.wChannels);
  WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 }; HANDLE ev = CreateEventA(0, FALSE, FALSE, 0); HWAVEOUT h;
  MMRESULT r = waveOutOpen(&h, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT); CHECK(r == MMSYSERR_NOERROR, "waveOutOpen (event) = %u", r);
  enum { N = 4, FR = 4410 }; static short pcm[N][FR * 2]; WAVEHDR hd[N]; double ph = 0;
  for (int i = 0; i < N; i++) { fill_s16(pcm[i], FR, 2, 44100, 440, &ph); memset(&hd[i], 0, sizeof hd[i]); hd[i].lpData = (LPSTR)pcm[i]; hd[i].dwBufferLength = sizeof pcm[i]; CHECK(waveOutPrepareHeader(h, &hd[i], sizeof hd[i]) == 0, "prepare %d", i); CHECK(waveOutWrite(h, &hd[i], sizeof hd[i]) == 0, "write %d", i); }
  DWORD t0 = GetTickCount(); int done = 0;
  while (done < N && GetTickCount() - t0 < 3000) { WaitForSingleObject(ev, 200); done = 0; for (int i = 0; i < N; i++) if (hd[i].dwFlags & WHDR_DONE) done++; }
  MMTIME mt; mt.wType = TIME_SAMPLES; waveOutGetPosition(h, &mt, sizeof mt);
  printf("  event callback: %d/%d buffers done in %lu ms, position %lu samples\n", done, N, (unsigned long)(GetTickCount() - t0), (unsigned long)mt.u.sample);
  CHECK(done == N, "buffers not completed"); CHECK(mt.u.sample >= FR * N, "position did not advance"); CHECK(GetTickCount() - t0 >= 250, "4 x 100 ms finished too fast (%lu ms): the clock is not real (buffers complete when the mixer consumes them, ~100 ms ahead of the speaker)", (unsigned long)(GetTickCount() - t0));
  for (int i = 0; i < N; i++) waveOutUnprepareHeader(h, &hd[i], sizeof hd[i]);
  CHECK(waveOutClose(h) == 0, "close");
  /* function callback: WOM_OPEN, N x WOM_DONE, WOM_CLOSE on a helper thread */
  r = waveOutOpen(&h, 0, &f, (DWORD_PTR)wo_cb, 0x1234, CALLBACK_FUNCTION); CHECK(r == MMSYSERR_NOERROR, "waveOutOpen (function) = %u", r);
  for (int i = 0; i < 2; i++) { hd[i].dwFlags = 0; waveOutPrepareHeader(h, &hd[i], sizeof hd[i]); waveOutWrite(h, &hd[i], sizeof hd[i]); }
  t0 = GetTickCount(); while (g_wom_done < 2 && GetTickCount() - t0 < 2000) Sleep(10);
  waveOutReset(h); waveOutClose(h); Sleep(50);
  printf("  function callback: WOM_OPEN %d, WOM_DONE %d, WOM_CLOSE %d\n", g_wom_open, g_wom_done, g_wom_close);
  CHECK(g_wom_open == 1 && g_wom_done == 2 && g_wom_close == 1, "callback counts wrong");
  CHECK(waveInGetNumDevs() == 0, "waveIn should report no capture device");
  printf("  waveOut OK\n"); return 0;
}

/* ---- [2] DirectSound ---- */
typedef HRESULT (WINAPI *PFN_DSC8)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
static int test_dsound(void) {
  printf("[2] DirectSound 8\n");
  HMODULE dll = LoadLibraryA("dsound.dll"); CHECK(dll, "dsound.dll"); PFN_DSC8 create = (PFN_DSC8)GetProcAddress(dll, "DirectSoundCreate8"); CHECK(create, "DirectSoundCreate8 export");
  LPDIRECTSOUND8 ds = 0; HRESULT hr = create(0, &ds, 0); CHECK(hr == DS_OK && ds, "DirectSoundCreate8 = 0x%08lx", (long)hr);
  CHECK(IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY) == DS_OK, "SetCooperativeLevel");
  DSCAPS dc = { sizeof dc }; CHECK(IDirectSound8_GetCaps(ds, &dc) == DS_OK, "GetCaps"); printf("  caps: flags %#lx, rates %lu..%lu\n", (unsigned long)dc.dwFlags, (unsigned long)dc.dwMinSecondarySampleRate, (unsigned long)dc.dwMaxSecondarySampleRate);
  DSBUFFERDESC pd; memset(&pd, 0, sizeof pd); pd.dwSize = sizeof pd; pd.dwFlags = DSBCAPS_PRIMARYBUFFER; LPDIRECTSOUNDBUFFER prim = 0;
  CHECK(IDirectSound8_CreateSoundBuffer(ds, &pd, &prim, 0) == DS_OK, "primary buffer"); WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 }; CHECK(IDirectSoundBuffer_SetFormat(prim, &pf) == DS_OK, "primary SetFormat");
  WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, 22050, 22050, 1, 8, 0 }; DSBUFFERDESC bd; memset(&bd, 0, sizeof bd); bd.dwSize = sizeof bd; bd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GLOBALFOCUS; bd.dwBufferBytes = 22050 / 2; bd.lpwfxFormat = &f;
  LPDIRECTSOUNDBUFFER b = 0; hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &b, 0); CHECK(hr == DS_OK && b, "secondary buffer = 0x%08lx", (long)hr);
  void *p1, *p2; DWORD n1, n2; CHECK(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER) == DS_OK, "Lock"); CHECK(n1 == bd.dwBufferBytes && p1, "Lock returned %lu bytes", (unsigned long)n1);
  { unsigned char *d = p1; double ph = 0; for (DWORD i = 0; i < n1; i++) { d[i] = (unsigned char)(128 + sin(ph) * 100); ph += 2 * 3.14159265358979 * 880 / 22050; } }
  CHECK(IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2) == DS_OK, "Unlock");
  LPDIRECTSOUNDNOTIFY nf = 0; CHECK(IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&nf) == DS_OK && nf, "QI IDirectSoundNotify");
  HANDLE evh = CreateEventA(0, FALSE, FALSE, 0), eve = CreateEventA(0, FALSE, FALSE, 0); DSBPOSITIONNOTIFY pn[2] = { { bd.dwBufferBytes / 2, evh }, { DSBPN_OFFSETSTOP, eve } };
  CHECK(IDirectSoundNotify_SetNotificationPositions(nf, 2, pn) == DS_OK, "SetNotificationPositions");
  CHECK(IDirectSoundBuffer_SetVolume(b, -600) == DS_OK, "SetVolume"); LONG vol = 1; IDirectSoundBuffer_GetVolume(b, &vol); CHECK(vol == -600, "GetVolume %ld", (long)vol);
  CHECK(IDirectSoundBuffer_SetPan(b, 2000) == DS_OK, "SetPan"); CHECK(IDirectSoundBuffer_SetFrequency(b, 22050) == DS_OK, "SetFrequency");
  DWORD t0 = GetTickCount(); CHECK(IDirectSoundBuffer_Play(b, 0, 0, 0) == DS_OK, "Play");
  DWORD st = 0; IDirectSoundBuffer_GetStatus(b, &st); CHECK(st & DSBSTATUS_PLAYING, "status after Play = %#lx", (unsigned long)st);
  CHECK(WaitForSingleObject(evh, 2000) == WAIT_OBJECT_0, "half-way notification never fired");
  DWORD play = 0, wr = 0; IDirectSoundBuffer_GetCurrentPosition(b, &play, &wr); printf("  half-way notification after %lu ms, play cursor %lu, write cursor %lu\n", (unsigned long)(GetTickCount() - t0), (unsigned long)play, (unsigned long)wr);
  CHECK(WaitForSingleObject(eve, 2000) == WAIT_OBJECT_0, "stop notification never fired"); IDirectSoundBuffer_GetStatus(b, &st);
  printf("  finished after %lu ms (status %#lx)\n", (unsigned long)(GetTickCount() - t0), (unsigned long)st); CHECK(!(st & DSBSTATUS_PLAYING), "still playing after the end"); CHECK(GetTickCount() - t0 >= 350, "a 500 ms buffer ended after %lu ms", (unsigned long)(GetTickCount() - t0));
  LPDIRECTSOUNDBUFFER dup = 0; CHECK(IDirectSound8_DuplicateSoundBuffer(ds, b, &dup) == DS_OK && dup, "DuplicateSoundBuffer"); CHECK(IDirectSoundBuffer_Play(dup, 0, 0, DSBPLAY_LOOPING) == DS_OK, "Play looping"); Sleep(150); IDirectSoundBuffer_GetStatus(dup, &st); CHECK((st & (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING)) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "looping status %#lx", (unsigned long)st); IDirectSoundBuffer_Stop(dup);
  IDirectSoundBuffer_Release(dup); IDirectSoundNotify_Release(nf); IDirectSoundBuffer_Release(b); IDirectSoundBuffer_Release(prim); IDirectSound8_Release(ds);
  printf("  DirectSound OK\n"); return 0;
}

/* ---- [3] WASAPI ---- */
DEFINE_GUID(CLSID_MMDeviceEnumerator_, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient_, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_IAudioClock_, 0xCD63314F, 0x3FBA, 0x4a1b, 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7);
DEFINE_GUID(PKEY_Device_FriendlyName_fmtid, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0);
static int test_wasapi(void) {
  printf("[3] WASAPI (mmdevapi + audioclient)\n");
  CoInitializeEx(0, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *en = 0; HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, 0, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en); CHECK(hr == S_OK && en, "CoCreateInstance(MMDeviceEnumerator) = 0x%08lx", (long)hr);
  IMMDevice *dev = 0; hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev); CHECK(hr == S_OK && dev, "GetDefaultAudioEndpoint = 0x%08lx", (long)hr);
  IMMDevice *mic = 0; hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &mic); CHECK(hr != S_OK && !mic, "a capture endpoint was reported although there is no microphone");
  LPWSTR id = 0; IMMDevice_GetId(dev, &id); IPropertyStore *ps = 0; CHECK(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps) == S_OK && ps, "OpenPropertyStore");
  PROPERTYKEY key; key.fmtid = PKEY_Device_FriendlyName_fmtid; key.pid = 14; PROPVARIANT pv; PropVariantInit(&pv); CHECK(IPropertyStore_GetValue(ps, &key, &pv) == S_OK && pv.vt == VT_LPWSTR, "friendly name");
  printf("  endpoint %ls: %ls\n", id, pv.pwszVal); PropVariantClear(&pv); IPropertyStore_Release(ps); CoTaskMemFree(id);
  IAudioClient *ac = 0; hr = IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, 0, (void **)&ac); CHECK(hr == S_OK && ac, "Activate(IAudioClient) = 0x%08lx", (long)hr);
  WAVEFORMATEX *mix = 0; CHECK(IAudioClient_GetMixFormat(ac, &mix) == S_OK && mix, "GetMixFormat"); printf("  mix format: %u Hz, %u ch, %u bit, tag %#x\n", (unsigned)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);
  WAVEFORMATEX f = { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0 }; WAVEFORMATEX *closest = 0; hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &f, &closest); CHECK(hr == S_OK, "IsFormatSupported(float48k) = 0x%08lx", (long)hr);
  HANDLE ev = CreateEventA(0, FALSE, FALSE, 0);
  hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 300000 /* 30 ms */, 0, &f, 0); CHECK(hr == S_OK, "Initialize = 0x%08lx", (long)hr);
  UINT32 bufFrames = 0; CHECK(IAudioClient_GetBufferSize(ac, &bufFrames) == S_OK && bufFrames >= 1000, "GetBufferSize %u", bufFrames);
  CHECK(IAudioClient_SetEventHandle(ac, ev) == S_OK, "SetEventHandle");
  IAudioRenderClient *rc = 0; CHECK(IAudioClient_GetService(ac, &IID_IAudioRenderClient_, (void **)&rc) == S_OK && rc, "GetService(IAudioRenderClient)");
  IAudioClock *clk = 0; CHECK(IAudioClient_GetService(ac, &IID_IAudioClock_, (void **)&clk) == S_OK && clk, "GetService(IAudioClock)");
  CHECK(IAudioClient_Start(ac) == S_OK, "Start");
  double ph = 0; UINT32 total = 0, wakeups = 0; DWORD t0 = GetTickCount();
  while (total < 48000 / 4 && GetTickCount() - t0 < 3000) {   /* 250 ms of 660 Hz */
    if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) continue; wakeups++;
    UINT32 pad = 0; CHECK(IAudioClient_GetCurrentPadding(ac, &pad) == S_OK, "GetCurrentPadding"); UINT32 room = bufFrames - pad; if (!room) continue;
    BYTE *data = 0; hr = IAudioRenderClient_GetBuffer(rc, room, &data); CHECK(hr == S_OK && data, "GetBuffer(%u) = 0x%08lx", room, (long)hr);
    float *fp = (float *)data; for (UINT32 i = 0; i < room; i++) { float v = (float)sin(ph) * 0.4f; ph += 2 * 3.14159265358979 * 660 / 48000; fp[2 * i] = v; fp[2 * i + 1] = v; }
    CHECK(IAudioRenderClient_ReleaseBuffer(rc, room, 0) == S_OK, "ReleaseBuffer"); total += room;
  }
  UINT64 pos = 0, freq = 0; IAudioClock_GetFrequency(clk, &freq); IAudioClock_GetPosition(clk, &pos, 0);
  printf("  %u frames submitted in %u wake-ups over %lu ms; clock %llu / %llu Hz\n", total, wakeups, (unsigned long)(GetTickCount() - t0), (unsigned long long)pos, (unsigned long long)freq);
  CHECK(total >= 48000 / 4, "could not submit 250 ms"); CHECK(wakeups >= 5, "too few period events"); CHECK(pos > 0 && freq == 48000, "clock did not advance");
  Sleep(100); CHECK(IAudioClient_Stop(ac) == S_OK, "Stop");
  IAudioClock_Release(clk); IAudioRenderClient_Release(rc); IAudioClient_Release(ac); CoTaskMemFree(mix); IMMDevice_Release(dev); IMMDeviceEnumerator_Release(en);
  printf("  WASAPI OK\n"); return 0;
}

/* ---- [4] PlaySound ---- */
static int test_playsound(void) {
  printf("[4] PlaySound(SND_MEMORY | SND_SYNC)\n");
  enum { RATE = 11025, FR = RATE * 2 / 5 }; static unsigned char wav[44 + FR];
  memcpy(wav, "RIFF", 4); unsigned v = 36 + FR; memcpy(wav + 4, &v, 4); memcpy(wav + 8, "WAVEfmt ", 8); v = 16; memcpy(wav + 16, &v, 4);
  unsigned short s = 1; memcpy(wav + 20, &s, 2); memcpy(wav + 22, &s, 2); v = RATE; memcpy(wav + 24, &v, 4); memcpy(wav + 28, &v, 4); s = 1; memcpy(wav + 32, &s, 2); s = 8; memcpy(wav + 34, &s, 2); memcpy(wav + 36, "data", 4); v = FR; memcpy(wav + 40, &v, 4);
  double ph = 0; for (int i = 0; i < FR; i++) { wav[44 + i] = (unsigned char)(128 + sin(ph) * 90); ph += 2 * 3.14159265358979 * 330 / RATE; }
  DWORD t0 = GetTickCount(); BOOL ok = PlaySoundA((LPCSTR)wav, 0, SND_MEMORY | SND_SYNC); DWORD dt = GetTickCount() - t0;
  printf("  400 ms clip: returned %d after %lu ms\n", ok, (unsigned long)dt); CHECK(ok, "PlaySound failed"); CHECK(dt >= 250, "SND_SYNC returned before the clip ended (the mixer runs ~100 ms ahead of the speaker)");
  printf("  PlaySound OK\n"); return 0;
}

/* ---- [5] XAudio2 2.7 (the real DLL from the DirectX pack), vtables declared by hand (2.7 layout) ---- */
DEFINE_GUID(CLSID_XAudio2_27, 0x5a508685, 0xa254, 0x4fba, 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf);
DEFINE_GUID(IID_IXAudio2_27, 0x8bcf1f58, 0x9fe7, 0x4583, 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb);
typedef struct IXA2 IXA2; typedef struct IXA2Voice IXA2Voice;
typedef struct { UINT32 Flags, AudioBytes; const BYTE *pAudioData; UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount; void *pContext; } XA2_BUFFER;
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XA2_STATE;
typedef struct { HRESULT (WINAPI *QueryInterface)(IXA2 *, REFIID, void **); ULONG (WINAPI *AddRef)(IXA2 *); ULONG (WINAPI *Release)(IXA2 *);
  HRESULT (WINAPI *GetDeviceCount)(IXA2 *, UINT32 *); HRESULT (WINAPI *GetDeviceDetails)(IXA2 *, UINT32, void *); HRESULT (WINAPI *Initialize)(IXA2 *, UINT32, UINT32);
  HRESULT (WINAPI *RegisterForCallbacks)(IXA2 *, void *); void (WINAPI *UnregisterForCallbacks)(IXA2 *, void *);
  HRESULT (WINAPI *CreateSourceVoice)(IXA2 *, IXA2Voice **, const WAVEFORMATEX *, UINT32, float, void *, const void *, const void *);
  HRESULT (WINAPI *CreateSubmixVoice)(IXA2 *, IXA2Voice **, UINT32, UINT32, UINT32, UINT32, const void *, const void *);
  HRESULT (WINAPI *CreateMasteringVoice)(IXA2 *, IXA2Voice **, UINT32, UINT32, UINT32, UINT32, const void *);
  HRESULT (WINAPI *StartEngine)(IXA2 *); void (WINAPI *StopEngine)(IXA2 *); HRESULT (WINAPI *CommitChanges)(IXA2 *, UINT32); void (WINAPI *GetPerformanceData)(IXA2 *, void *); void (WINAPI *SetDebugConfiguration)(IXA2 *, const void *, void *); } IXA2Vtbl;
struct IXA2 { const IXA2Vtbl *lpVtbl; };
typedef struct { void (WINAPI *GetVoiceDetails)(IXA2Voice *, void *); HRESULT (WINAPI *SetOutputVoices)(IXA2Voice *, const void *); HRESULT (WINAPI *SetEffectChain)(IXA2Voice *, const void *);
  HRESULT (WINAPI *EnableEffect)(IXA2Voice *, UINT32, UINT32); HRESULT (WINAPI *DisableEffect)(IXA2Voice *, UINT32, UINT32); void (WINAPI *GetEffectState)(IXA2Voice *, UINT32, BOOL *);
  HRESULT (WINAPI *SetEffectParameters)(IXA2Voice *, UINT32, const void *, UINT32, UINT32); HRESULT (WINAPI *GetEffectParameters)(IXA2Voice *, UINT32, void *, UINT32);
  HRESULT (WINAPI *SetFilterParameters)(IXA2Voice *, const void *, UINT32); void (WINAPI *GetFilterParameters)(IXA2Voice *, void *);
  HRESULT (WINAPI *SetOutputFilterParameters)(IXA2Voice *, IXA2Voice *, const void *, UINT32); void (WINAPI *GetOutputFilterParameters)(IXA2Voice *, IXA2Voice *, void *);
  HRESULT (WINAPI *SetVolume)(IXA2Voice *, float, UINT32); void (WINAPI *GetVolume)(IXA2Voice *, float *); HRESULT (WINAPI *SetChannelVolumes)(IXA2Voice *, UINT32, const float *, UINT32); void (WINAPI *GetChannelVolumes)(IXA2Voice *, UINT32, float *);
  HRESULT (WINAPI *SetOutputMatrix)(IXA2Voice *, IXA2Voice *, UINT32, UINT32, const float *, UINT32); void (WINAPI *GetOutputMatrix)(IXA2Voice *, IXA2Voice *, UINT32, UINT32, float *); void (WINAPI *DestroyVoice)(IXA2Voice *);
  /* source voice */ HRESULT (WINAPI *Start)(IXA2Voice *, UINT32, UINT32); HRESULT (WINAPI *Stop)(IXA2Voice *, UINT32, UINT32); HRESULT (WINAPI *SubmitSourceBuffer)(IXA2Voice *, const XA2_BUFFER *, const void *);
  HRESULT (WINAPI *FlushSourceBuffers)(IXA2Voice *); HRESULT (WINAPI *Discontinuity)(IXA2Voice *); HRESULT (WINAPI *ExitLoop)(IXA2Voice *, UINT32); void (WINAPI *GetState)(IXA2Voice *, XA2_STATE *);
  HRESULT (WINAPI *SetFrequencyRatio)(IXA2Voice *, float, UINT32); void (WINAPI *GetFrequencyRatio)(IXA2Voice *, float *); HRESULT (WINAPI *SetSourceSampleRate)(IXA2Voice *, UINT32); } IXA2VoiceVtbl;
struct IXA2Voice { const IXA2VoiceVtbl *lpVtbl; };
static int test_xaudio2(void) {
  printf("[5] XAudio2 2.7 (REAL XAudio2_7.dll from the DirectX pack, on top of the emulator's WASAPI)\n");
  if (!SearchPathA(0, "XAudio2_7.dll", 0, 0, 0, 0) && GetFileAttributesA("C:\\Windows\\System32\\XAudio2_7.dll") == INVALID_FILE_ATTRIBUTES) { printf("  skip: XAudio2_7.dll is not installed (DirectX pack)\n"); return 0; }
  IXA2 *xa = 0; HRESULT hr = CoCreateInstance(&CLSID_XAudio2_27, 0, CLSCTX_INPROC_SERVER, &IID_IXAudio2_27, (void **)&xa); CHECK(hr == S_OK && xa, "CoCreateInstance(XAudio2 2.7) = 0x%08lx", (long)hr);
  hr = xa->lpVtbl->Initialize(xa, 0, 0xffffffff /* XAUDIO2_DEFAULT_PROCESSOR */); CHECK(hr == S_OK, "IXAudio2::Initialize = 0x%08lx", (long)hr);
  UINT32 ndev = 0; CHECK(xa->lpVtbl->GetDeviceCount(xa, &ndev) == S_OK, "GetDeviceCount"); printf("  XAudio2 sees %u device(s)\n", ndev); CHECK(ndev >= 1, "no devices");
  IXA2Voice *master = 0; hr = xa->lpVtbl->CreateMasteringVoice(xa, &master, 2, 48000, 0, 0, 0); CHECK(hr == S_OK && master, "CreateMasteringVoice = 0x%08lx", (long)hr);
  WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, 22050, 22050 * 2, 2, 16, 0 }; IXA2Voice *src = 0; hr = xa->lpVtbl->CreateSourceVoice(xa, &src, &f, 0, 2.0f, 0, 0, 0); CHECK(hr == S_OK && src, "CreateSourceVoice = 0x%08lx", (long)hr);
  enum { FR = 22050 * 3 / 10 }; static short pcm[FR]; double ph = 0; fill_s16(pcm, FR, 1, 22050, 523.25, &ph);
  XA2_BUFFER b; memset(&b, 0, sizeof b); b.Flags = 0x0040 /* XAUDIO2_END_OF_STREAM */; b.AudioBytes = sizeof pcm; b.pAudioData = (const BYTE *)pcm;
  CHECK(src->lpVtbl->SubmitSourceBuffer(src, &b, 0) == S_OK, "SubmitSourceBuffer"); DWORD t0 = GetTickCount(); CHECK(src->lpVtbl->Start(src, 0, 0) == S_OK, "Start");
  XA2_STATE st; st.BuffersQueued = 1; while (GetTickCount() - t0 < 3000) { src->lpVtbl->GetState(src, &st); if (!st.BuffersQueued) break; Sleep(10); }
  printf("  300 ms buffer: queued %u, samples played %llu after %lu ms\n", st.BuffersQueued, (unsigned long long)st.SamplesPlayed, (unsigned long)(GetTickCount() - t0));
  CHECK(st.BuffersQueued == 0, "buffer never finished"); CHECK(GetTickCount() - t0 >= 200, "300 ms finished after %lu ms", (unsigned long)(GetTickCount() - t0));
  src->lpVtbl->DestroyVoice(src); master->lpVtbl->DestroyVoice(master); xa->lpVtbl->Release(xa);
  printf("  XAudio2 OK\n"); return 0;
}

int main(int argc, char **argv) {
  int only = argc > 1 ? atoi(argv[1]) : 0;
  if (!only || only == 1) fails += test_waveout();
  if (!only || only == 2) fails += test_dsound();
  if (!only || only == 3) fails += test_wasapi();
  if (!only || only == 4) fails += test_playsound();
  if (!only || only == 5) fails += test_xaudio2();
  printf("audio: %d failure(s)\n", fails); return fails;
}
