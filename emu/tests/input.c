/* input.c — Raw Input + DirectInput 8 smoke test for the emulator (x86 and x64).
   Driven by x64run's injected events: --key 0x27@1000:1500 (Right held 1.5 s) --key 0x28@3000:400 (Down) --move 300,300@4200
   A timer polls the devices; prints ok/FAIL lines and "ALL OK n/n".
   Build: zig cc -target x86_64-windows-gnu -O2 -o input.exe input.c -luser32 -ldinput8 -ldxguid -lole32 (same with x86-windows-gnu) */
#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>
static int g_ok, g_fail; static BYTE g_dummy[256]; static void *od_dummy(void) { return g_dummy; }
static void chk(int cond, const char *what) { printf("%s %s\n", cond ? "ok  " : "FAIL", what); if (cond) g_ok++; else g_fail++; fflush(stdout); }
static int g_raw_key_down_right, g_raw_key_up_right, g_raw_mouse_moves, g_raw_mouse_dx, g_raw_other, g_wm_keydown_right;
static IDirectInput8W *g_di; static IDirectInputDevice8W *g_kbd, *g_mouse; static int g_enum_all, g_enum_pad, g_phase;
static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEW inst, LPVOID ref) { int *n = ref; (*n)++; if (inst->dwSize != sizeof(DIDEVICEINSTANCEW)) printf("  (instance size %lu)\n", (unsigned long)inst->dwSize); printf("  device: %ls type %lx\n", inst->tszInstanceName, (unsigned long)inst->dwDevType); return DIENUM_CONTINUE; }
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_KEYDOWN: if (wp == VK_RIGHT) g_wm_keydown_right++; return 0;
    case WM_INPUT: { UINT sz = 0; GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)); BYTE buf[128]; if (sz > sizeof buf) { g_raw_other++; return 0; }
      UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)); RAWINPUT *ri = (RAWINPUT *)buf; if (got != sz) { g_raw_other++; return 0; }
      if (ri->header.dwType == RIM_TYPEKEYBOARD) { if (ri->data.keyboard.VKey == VK_RIGHT) { if (ri->data.keyboard.Flags & RI_KEY_BREAK) g_raw_key_up_right++; else g_raw_key_down_right++; } }
      else if (ri->header.dwType == RIM_TYPEMOUSE) { g_raw_mouse_moves++; g_raw_mouse_dx += ri->data.mouse.lLastX; } else g_raw_other++; return 0; }
    case WM_TIMER: {
      g_phase++;
      if (g_phase == 1) { /* t ≈ 1.6 s: Right is held */ BYTE ks[256]; HRESULT hr = IDirectInputDevice8_GetDeviceState(g_kbd, sizeof ks, ks); chk(hr == DI_OK && ks[DIK_RIGHT] == 0x80 && ks[DIK_DOWN] == 0, "DirectInput keyboard: GetDeviceState shows DIK_RIGHT pressed while Right is held");
        chk(g_raw_key_down_right >= 1 && g_raw_key_up_right == 0, "Raw Input: WM_INPUT RAWKEYBOARD VKey=VK_RIGHT make received, no break yet"); chk(g_wm_keydown_right >= 1, "legacy WM_KEYDOWN still delivered (no RIDEV_NOLEGACY)");
        chk(GetAsyncKeyState(VK_RIGHT) & 0x8000, "GetAsyncKeyState(VK_RIGHT) high bit set"); }
      else if (g_phase == 2) { /* t ≈ 3.2 s: Right released, Down held */ BYTE ks[256]; IDirectInputDevice8_GetDeviceState(g_kbd, sizeof ks, ks); chk(ks[DIK_RIGHT] == 0 && ks[DIK_DOWN] == 0x80, "DirectInput keyboard: Right released, DIK_DOWN pressed"); chk(g_raw_key_up_right >= 1, "Raw Input: RI_KEY_BREAK for Right arrived");
        DIDEVICEOBJECTDATA od[16]; DWORD n = 16; HRESULT hr = IDirectInputDevice8_GetDeviceData(g_kbd, sizeof(DIDEVICEOBJECTDATA), od, &n, 0); int saw_right_down = 0, saw_right_up = 0, saw_down = 0; for (DWORD k = 0; k < n; k++) { if (od[k].dwOfs == DIK_RIGHT && (od[k].dwData & 0x80)) saw_right_down = 1; if (od[k].dwOfs == DIK_RIGHT && !(od[k].dwData & 0x80)) saw_right_up = 1; if (od[k].dwOfs == DIK_DOWN && (od[k].dwData & 0x80)) saw_down = 1; }
        chk(hr == DI_OK && n >= 3 && saw_right_down && saw_right_up && saw_down, "DirectInput keyboard: buffered GetDeviceData holds Right make/break and Down make in order"); n = 16; IDirectInputDevice8_GetDeviceData(g_kbd, sizeof(DIDEVICEOBJECTDATA), od, &n, 0); chk(n == 0, "buffered data consumed"); }
      else if (g_phase == 3) { /* t ≈ 4.8 s: the pointer moved to 300,300 */ DIMOUSESTATE2 ms; HRESULT hr = IDirectInputDevice8_GetDeviceState(g_mouse, sizeof ms, &ms); chk(hr == DI_OK && ms.lX != 0 && ms.lY != 0, "DirectInput mouse: relative lX/lY accumulated from the pointer move"); chk(g_raw_mouse_moves >= 1 && g_raw_mouse_dx != 0, "Raw Input: RAWMOUSE relative move received");
        IDirectInputDevice8_GetDeviceState(g_mouse, sizeof ms, &ms); chk(ms.lX == 0 && ms.lY == 0, "DirectInput mouse: deltas reset after reading");
        chk(IDirectInputDevice8_Unacquire(g_kbd) == DI_OK && IDirectInputDevice8_GetDeviceState(g_kbd, 256, od_dummy()) == DIERR_NOTACQUIRED, "DirectInput: GetDeviceState after Unacquire -> DIERR_NOTACQUIRED");
        KillTimer(h, 1); DestroyWindow(h); }
      return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

int main(void) {
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSW wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = wnd_proc; wc.hInstance = inst; wc.lpszClassName = L"NebulaInputTest"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc);
  HWND h = CreateWindowExW(0, L"NebulaInputTest", L"Input test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL, inst, NULL); SetFocus(h);
  /* raw input */
  UINT nd = 0; UINT r = GetRawInputDeviceList(NULL, &nd, sizeof(RAWINPUTDEVICELIST)); RAWINPUTDEVICELIST dl[8]; UINT nd2 = 8; UINT got = GetRawInputDeviceList(dl, &nd2, sizeof(RAWINPUTDEVICELIST));
  chk(r == 0 && nd == 2 && got == 2 && ((dl[0].dwType == RIM_TYPEMOUSE && dl[1].dwType == RIM_TYPEKEYBOARD) || (dl[1].dwType == RIM_TYPEMOUSE && dl[0].dwType == RIM_TYPEKEYBOARD)), "GetRawInputDeviceList: a mouse and a keyboard");
  RID_DEVICE_INFO info; info.cbSize = sizeof info; UINT isz = sizeof info; UINT ir = GetRawInputDeviceInfoW(dl[0].hDevice, RIDI_DEVICEINFO, &info, &isz); WCHAR name[128]; UINT nsz = 128; UINT nr = GetRawInputDeviceInfoW(dl[0].hDevice, RIDI_DEVICENAME, name, &nsz);
  chk(ir == sizeof info && info.dwType == dl[0].dwType && nr > 10 && wcsstr(name, L"\\\\?\\HID#") == name, "GetRawInputDeviceInfoW: RIDI_DEVICEINFO + RIDI_DEVICENAME");
  RAWINPUTDEVICE rid[2]; rid[0].usUsagePage = 1; rid[0].usUsage = 6; rid[0].dwFlags = 0; rid[0].hwndTarget = h; rid[1].usUsagePage = 1; rid[1].usUsage = 2; rid[1].dwFlags = 0; rid[1].hwndTarget = h;
  chk(RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)), "RegisterRawInputDevices(keyboard + mouse)");
  RAWINPUTDEVICE reg[4]; UINT nreg = 4; chk(GetRegisteredRawInputDevices(reg, &nreg, sizeof(RAWINPUTDEVICE)) == 2 && ((reg[0].usUsage == 6 && reg[1].usUsage == 2) || (reg[0].usUsage == 2 && reg[1].usUsage == 6)), "GetRegisteredRawInputDevices returns both");
  /* DirectInput 8 */
  HRESULT hr = DirectInput8Create(inst, DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&g_di, NULL); chk(hr == DI_OK && g_di != NULL, "DirectInput8Create(IID_IDirectInput8W)");
  hr = IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_ALL, enum_cb, &g_enum_all, DIEDFL_ATTACHEDONLY); chk(hr == DI_OK && g_enum_all == 2, "EnumDevices(DI8DEVCLASS_ALL): keyboard + mouse through the callback");
  hr = IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, enum_cb, &g_enum_pad, DIEDFL_ATTACHEDONLY); chk(hr == DI_OK && g_enum_pad == 0, "EnumDevices(DI8DEVCLASS_GAMECTRL): no game controllers, DI_OK");
  hr = IDirectInput8_CreateDevice(g_di, &GUID_SysKeyboard, &g_kbd, NULL); chk(hr == DI_OK && g_kbd != NULL, "CreateDevice(GUID_SysKeyboard)");
  hr = IDirectInput8_CreateDevice(g_di, &GUID_SysMouse, &g_mouse, NULL); chk(hr == DI_OK && g_mouse != NULL, "CreateDevice(GUID_SysMouse)");
  IDirectInputDevice8W *pad = NULL; GUID bogus = { 0x12345678, 0, 0, { 0 } }; hr = IDirectInput8_CreateDevice(g_di, &bogus, &pad, NULL); chk(hr == DIERR_DEVICENOTREG && pad == NULL, "CreateDevice(unknown GUID) -> DIERR_DEVICENOTREG");
  chk(IDirectInputDevice8_SetDataFormat(g_kbd, &c_dfDIKeyboard) == DI_OK && IDirectInputDevice8_SetCooperativeLevel(g_kbd, h, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK, "keyboard: SetDataFormat(c_dfDIKeyboard) + SetCooperativeLevel");
  DIPROPDWORD pd; memset(&pd, 0, sizeof pd); pd.diph.dwSize = sizeof pd; pd.diph.dwHeaderSize = sizeof pd.diph; pd.diph.dwHow = DIPH_DEVICE; pd.dwData = 32; chk(IDirectInputDevice8_SetProperty(g_kbd, DIPROP_BUFFERSIZE, &pd.diph) == DI_OK, "keyboard: SetProperty(DIPROP_BUFFERSIZE = 32)");
  DIDEVCAPS caps; caps.dwSize = sizeof caps; chk(IDirectInputDevice8_GetCapabilities(g_kbd, &caps) == DI_OK && (caps.dwFlags & DIDC_ATTACHED) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD, "keyboard: GetCapabilities (attached, DI8DEVTYPE_KEYBOARD)");
  DIDEVICEINSTANCEW di; di.dwSize = sizeof di; chk(IDirectInputDevice8_GetDeviceInfo(g_mouse, &di) == DI_OK && IsEqualGUID(&di.guidInstance, &GUID_SysMouse) && GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_MOUSE && wcslen(di.tszProductName) > 0, "mouse: GetDeviceInfo (GUID_SysMouse, DI8DEVTYPE_MOUSE, product name)");
  BYTE ks[256]; chk(IDirectInputDevice8_GetDeviceState(g_kbd, sizeof ks, ks) == DIERR_NOTACQUIRED, "GetDeviceState before Acquire -> DIERR_NOTACQUIRED");
  chk(IDirectInputDevice8_Acquire(g_kbd) == DI_OK && IDirectInputDevice8_Acquire(g_kbd) == S_FALSE, "Acquire -> DI_OK, second Acquire -> S_FALSE");
  chk(IDirectInputDevice8_SetDataFormat(g_mouse, &c_dfDIMouse2) == DI_OK && IDirectInputDevice8_Acquire(g_mouse) == DI_OK && IDirectInputDevice8_Poll(g_mouse) == DI_NOEFFECT, "mouse: SetDataFormat(c_dfDIMouse2), Acquire, Poll");
  SetTimer(h, 1, 1600, NULL);
  MSG msg; DWORD t0 = GetTickCount();
  while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); if (GetTickCount() - t0 > 12000) { printf("FAIL timeout\n"); g_fail++; break; } }
  IDirectInputDevice8_Release(g_mouse); IDirectInputDevice8_Release(g_kbd); chk(IDirectInput8_Release(g_di) == 0, "Release chain frees the DirectInput object");
  printf("%s %d/%d\n", g_fail ? "FAILED" : "ALL OK", g_ok, g_ok + g_fail); return g_fail ? 1 : 0;
}
