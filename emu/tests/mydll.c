#include <windows.h>
#include <stdio.h>
static int g_attached;
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { if (reason == DLL_PROCESS_ATTACH) g_attached = 42; return TRUE; }
__declspec(dllexport) int add3(int a, int b, int c) { return a + b + c + (g_attached == 42 ? 0 : 1000); }
__declspec(dllexport) const char *dll_name(void) { return "mydll says hi"; }
__declspec(dllexport) double dll_hyp(double a, double b) { return a * a + b * b; }
