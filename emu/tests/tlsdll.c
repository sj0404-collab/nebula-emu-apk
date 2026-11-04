#include <windows.h>
_Thread_local int tl_counter = 7;
__declspec(dllexport) int bump(int by) { tl_counter += by; return tl_counter; }
