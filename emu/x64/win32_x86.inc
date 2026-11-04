/* 32-bit (IA-32, PE32) process support for the shim layer:
 *   - stdcall cleanup table (bytes popped by the callee), generated from Wine .spec files → stdcall32.inc
 *   - argument signatures for shims whose x64 implementation reads doubles from xmm registers or 64-bit integers from one
 *     slot: the 32-bit frame is unpacked into xmm0..3 / an argument override array, double results go to x87 ST(0)
 *   - the x86-only CRT helpers MSVC emits calls to (_alldiv, _chkstk, _ftol2, _CIsin …) with their register conventions
 * Everything else is shared with the x64 path: ARG(n) reads 4-byte stack slots, RET() sets eax (edx = high half). */

#include "stdcall32.inc"
static inline void fpush(Cpu *c, double v) { c->ftop = (c->ftop - 1) & 7; c->fp[c->ftop] = v; c->ftag |= 1u << c->ftop; }
static inline double fpop(Cpu *c) { double v = c->fp[c->ftop]; c->ftag &= ~(1u << c->ftop); c->ftop = (c->ftop + 1) & 7; return v; }

/* signature letters: i 32-bit int/pointer, q 64-bit int (two slots), d double, f float; "ret:args" */
static const struct { const char *n; const char *sig; } sig32_tab[] = {
  /* built-in window procedures and COM methods implemented as shims: stdcall from 32-bit callers */
  { "NebulaButtonProc", "i:iiii" }, { "NebulaStaticProc", "i:iiii" }, { "NebulaLinkProc", "i:iiii" }, { "NebulaEditProc", "i:iiii" }, { "NebulaListBoxProc", "i:iiii" }, { "NebulaComboListProc", "i:iiii" }, { "NebulaMenuProc", "i:iiii" }, { "NebulaStatusProc", "i:iiii" }, { "NebulaToolbarProc", "i:iiii" }, { "NebulaTabProc", "i:iiii" }, { "NebulaSubclassProc", "i:iiii" }, { "NebulaComboProc", "i:iiii" }, { "NebulaProgressProc", "i:iiii" }, { "NebulaScrollBarProc", "i:iiii" }, { "NebulaUnsupportedProc", "i:iiii" }, { "NebulaMsgBoxProc", "i:iiii" }, { "NebulaBrowseProc", "i:iiii" },
  { "IShellLink::QueryInterface", "i:iii" }, { "IShellLink::AddRef", "i:i" }, { "IShellLink::Release", "i:i" }, { "IShellLink::GetPath", "i:iiiii" }, { "IShellLink::GetIDList", "i:ii" }, { "IShellLink::SetIDList", "i:ii" }, { "IShellLink::GetDescription", "i:iii" }, { "IShellLink::SetDescription", "i:ii" }, { "IShellLink::GetWorkingDirectory", "i:iii" }, { "IShellLink::SetWorkingDirectory", "i:ii" }, { "IShellLink::GetArguments", "i:iii" }, { "IShellLink::SetArguments", "i:ii" }, { "IShellLink::GetHotkey", "i:ii" }, { "IShellLink::SetHotkey", "i:ii" }, { "IShellLink::GetShowCmd", "i:ii" }, { "IShellLink::SetShowCmd", "i:ii" }, { "IShellLink::GetIconLocation", "i:iiii" }, { "IShellLink::SetIconLocation", "i:iii" }, { "IShellLink::SetRelativePath", "i:iii" }, { "IShellLink::Resolve", "i:iii" }, { "IShellLink::SetPath", "i:ii" },
  { "IShellLinkProps::QueryInterface", "i:iii" }, { "IShellLinkProps::AddRef", "i:i" }, { "IShellLinkProps::Release", "i:i" }, { "IShellLinkProps::GetCount", "i:ii" }, { "IShellLinkProps::GetAt", "i:iii" }, { "IShellLinkProps::GetValue", "i:iii" }, { "IShellLinkProps::SetValue", "i:iii" }, { "IShellLinkProps::Commit", "i:i" },
  { "IPersistFile::QueryInterface", "i:iii" }, { "IPersistFile::AddRef", "i:i" }, { "IPersistFile::Release", "i:i" }, { "IPersistFile::GetClassID", "i:ii" }, { "IPersistFile::IsDirty", "i:i" }, { "IPersistFile::Load", "i:iii" }, { "IPersistFile::Save", "i:iii" }, { "IPersistFile::SaveCompleted", "i:ii" }, { "IPersistFile::GetCurFile", "i:ii" },
  { "IMalloc::QueryInterface", "i:iii" }, { "IMalloc::AddRef", "i:i" }, { "IMalloc::Release", "i:i" }, { "IMalloc::Alloc", "i:ii" }, { "IMalloc::Realloc", "i:iii" }, { "IMalloc::Free", "i:ii" }, { "IMalloc::GetSize", "i:ii" }, { "IMalloc::DidAlloc", "i:ii" }, { "IMalloc::HeapMinimize", "i:i" },
  /* DirectInput8 COM methods (input.inc) */
  { "IDirectInput8::QueryInterface", "i:iii" }, { "IDirectInput8::AddRef", "i:i" }, { "IDirectInput8::Release", "i:i" }, { "IDirectInput8::CreateDevice", "i:iiii" }, { "IDirectInput8::EnumDevices", "i:iiiii" }, { "IDirectInput8::GetDeviceStatus", "i:ii" }, { "IDirectInput8::RunControlPanel", "i:iii" }, { "IDirectInput8::Initialize", "i:iii" }, { "IDirectInput8::FindDevice", "i:iiii" }, { "IDirectInput8::EnumDevicesBySemantics", "i:iiiiii" }, { "IDirectInput8::ConfigureDevices", "i:iiiii" }, { "IDirectInputDevice8::QueryInterface", "i:iii" }, { "IDirectInputDevice8::AddRef", "i:i" }, { "IDirectInputDevice8::Release", "i:i" }, { "IDirectInputDevice8::GetCapabilities", "i:ii" }, { "IDirectInputDevice8::EnumObjects", "i:iiii" }, { "IDirectInputDevice8::GetProperty", "i:iii" }, { "IDirectInputDevice8::SetProperty", "i:iii" }, { "IDirectInputDevice8::Acquire", "i:i" }, { "IDirectInputDevice8::Unacquire", "i:i" }, { "IDirectInputDevice8::GetDeviceState", "i:iii" }, { "IDirectInputDevice8::GetDeviceData", "i:iiiii" }, { "IDirectInputDevice8::SetDataFormat", "i:ii" }, { "IDirectInputDevice8::SetEventNotification", "i:ii" }, { "IDirectInputDevice8::SetCooperativeLevel", "i:iii" }, { "IDirectInputDevice8::GetObjectInfo", "i:iiii" }, { "IDirectInputDevice8::GetDeviceInfo", "i:ii" }, { "IDirectInputDevice8::RunControlPanel", "i:iii" }, { "IDirectInputDevice8::Initialize", "i:iiii" }, { "IDirectInputDevice8::CreateEffect", "i:iiiii" }, { "IDirectInputDevice8::EnumEffects", "i:iiii" }, { "IDirectInputDevice8::GetEffectInfo", "i:iii" }, { "IDirectInputDevice8::GetForceFeedbackState", "i:ii" }, { "IDirectInputDevice8::SendForceFeedbackCommand", "i:ii" }, { "IDirectInputDevice8::EnumCreatedEffectObjects", "i:iiii" }, { "IDirectInputDevice8::Escape", "i:ii" }, { "IDirectInputDevice8::Poll", "i:i" }, { "IDirectInputDevice8::SendDeviceData", "i:iiiii" }, { "IDirectInputDevice8::EnumEffectsInFile", "i:iiiii" }, { "IDirectInputDevice8::WriteEffectToFile", "i:iiiii" }, { "IDirectInputDevice8::BuildActionMap", "i:iiii" }, { "IDirectInputDevice8::SetActionMap", "i:iiii" }, { "IDirectInputDevice8::GetImageInfo", "i:ii" },
  /* WIC COM methods (win32_wic.inc) */
  { "IWICImagingFactory::QueryInterface", "i:iii" }, { "IWICImagingFactory::AddRef", "i:i" }, { "IWICImagingFactory::Release", "i:i" }, { "IWICImagingFactory::CreateDecoderFromFilename", "i:iiiiii" }, { "IWICImagingFactory::CreateDecoderFromStream", "i:iiiii" }, { "IWICImagingFactory::CreateDecoderFromFileHandle", "i:iiiii" }, { "IWICImagingFactory::CreateComponentInfo", "i:iii" }, { "IWICImagingFactory::CreateDecoder", "i:iiii" }, { "IWICImagingFactory::CreateEncoder", "i:iiii" }, { "IWICImagingFactory::CreatePalette", "i:ii" }, { "IWICImagingFactory::CreateFormatConverter", "i:ii" }, { "IWICImagingFactory::CreateBitmapScaler", "i:ii" }, { "IWICImagingFactory::CreateBitmapClipper", "i:ii" }, { "IWICImagingFactory::CreateBitmapFlipRotator", "i:ii" }, { "IWICImagingFactory::CreateStream", "i:ii" }, { "IWICImagingFactory::CreateColorContext", "i:ii" }, { "IWICImagingFactory::CreateColorTransformer", "i:ii" }, { "IWICImagingFactory::CreateBitmap", "i:iiiiii" }, { "IWICImagingFactory::CreateBitmapFromSource", "i:iiii" }, { "IWICImagingFactory::CreateBitmapFromSourceRect", "i:iiiiiii" }, { "IWICImagingFactory::CreateBitmapFromMemory", "i:iiiiiiii" }, { "IWICImagingFactory::CreateBitmapFromHBITMAP", "i:iiiii" }, { "IWICImagingFactory::CreateBitmapFromHICON", "i:iii" }, { "IWICImagingFactory::CreateComponentEnumerator", "i:iiii" }, { "IWICImagingFactory::CreateFastMetadataEncoderFromDecoder", "i:iii" }, { "IWICImagingFactory::CreateFastMetadataEncoderFromFrameDecode", "i:iii" }, { "IWICImagingFactory::CreateQueryWriter", "i:iiii" }, { "IWICImagingFactory::CreateQueryWriterFromReader", "i:iiii" }, { "IWICImagingFactory::CreateImageEncoder", "i:iii" },
  { "IWICStream::QueryInterface", "i:iii" }, { "IWICStream::AddRef", "i:i" }, { "IWICStream::Release", "i:i" }, { "IWICStream::Read", "i:iiii" }, { "IWICStream::Write", "i:iiii" }, { "IWICStream::Seek", "i:iqii" }, { "IWICStream::SetSize", "i:iq" }, { "IWICStream::CopyTo", "i:iiqii" }, { "IWICStream::Commit", "i:ii" }, { "IWICStream::Revert", "i:i" }, { "IWICStream::LockRegion", "i:iqqi" }, { "IWICStream::UnlockRegion", "i:iqqi" }, { "IWICStream::Stat", "i:iii" }, { "IWICStream::Clone", "i:ii" }, { "IWICStream::InitializeFromIStream", "i:ii" }, { "IWICStream::InitializeFromFilename", "i:iii" }, { "IWICStream::InitializeFromMemory", "i:iii" }, { "IWICStream::InitializeFromIStreamRegion", "i:iiqq" },
  { "IWICBitmapDecoder::QueryInterface", "i:iii" }, { "IWICBitmapDecoder::AddRef", "i:i" }, { "IWICBitmapDecoder::Release", "i:i" }, { "IWICBitmapDecoder::QueryCapability", "i:iii" }, { "IWICBitmapDecoder::Initialize", "i:iii" }, { "IWICBitmapDecoder::GetContainerFormat", "i:ii" }, { "IWICBitmapDecoder::GetDecoderInfo", "i:ii" }, { "IWICBitmapDecoder::CopyPalette", "i:ii" }, { "IWICBitmapDecoder::GetMetadataQueryReader", "i:ii" }, { "IWICBitmapDecoder::GetPreview", "i:ii" }, { "IWICBitmapDecoder::GetColorContexts", "i:iiii" }, { "IWICBitmapDecoder::GetThumbnail", "i:ii" }, { "IWICBitmapDecoder::GetFrameCount", "i:ii" }, { "IWICBitmapDecoder::GetFrame", "i:iii" },
  { "IWICBitmapFrameDecode::QueryInterface", "i:iii" }, { "IWICBitmapFrameDecode::AddRef", "i:i" }, { "IWICBitmapFrameDecode::Release", "i:i" }, { "IWICBitmapFrameDecode::GetSize", "i:iii" }, { "IWICBitmapFrameDecode::GetPixelFormat", "i:ii" }, { "IWICBitmapFrameDecode::GetResolution", "i:iii" }, { "IWICBitmapFrameDecode::CopyPalette", "i:ii" }, { "IWICBitmapFrameDecode::CopyPixels", "i:iiiii" }, { "IWICBitmapFrameDecode::GetMetadataQueryReader", "i:ii" }, { "IWICBitmapFrameDecode::GetColorContexts", "i:iiii" }, { "IWICBitmapFrameDecode::GetThumbnail", "i:ii" },
  { "IWICBitmap::QueryInterface", "i:iii" }, { "IWICBitmap::AddRef", "i:i" }, { "IWICBitmap::Release", "i:i" }, { "IWICBitmap::GetSize", "i:iii" }, { "IWICBitmap::GetPixelFormat", "i:ii" }, { "IWICBitmap::GetResolution", "i:iii" }, { "IWICBitmap::CopyPalette", "i:ii" }, { "IWICBitmap::CopyPixels", "i:iiiii" }, { "IWICBitmap::Lock", "i:iiii" }, { "IWICBitmap::SetPalette", "i:ii" }, { "IWICBitmap::SetResolution", "i:idd" },
  { "IWICFormatConverter::QueryInterface", "i:iii" }, { "IWICFormatConverter::AddRef", "i:i" }, { "IWICFormatConverter::Release", "i:i" }, { "IWICFormatConverter::GetSize", "i:iii" }, { "IWICFormatConverter::GetPixelFormat", "i:ii" }, { "IWICFormatConverter::GetResolution", "i:iii" }, { "IWICFormatConverter::CopyPalette", "i:ii" }, { "IWICFormatConverter::CopyPixels", "i:iiiii" }, { "IWICFormatConverter::Initialize", "i:iiiiidi" }, { "IWICFormatConverter::CanConvert", "i:iiii" },
  { "IWICBitmapScaler::QueryInterface", "i:iii" }, { "IWICBitmapScaler::AddRef", "i:i" }, { "IWICBitmapScaler::Release", "i:i" }, { "IWICBitmapScaler::GetSize", "i:iii" }, { "IWICBitmapScaler::GetPixelFormat", "i:ii" }, { "IWICBitmapScaler::GetResolution", "i:iii" }, { "IWICBitmapScaler::CopyPalette", "i:ii" }, { "IWICBitmapScaler::CopyPixels", "i:iiiii" }, { "IWICBitmapScaler::Initialize", "i:iiiii" },
  { "IWICBitmapLock::QueryInterface", "i:iii" }, { "IWICBitmapLock::AddRef", "i:i" }, { "IWICBitmapLock::Release", "i:i" }, { "IWICBitmapLock::GetSize", "i:iii" }, { "IWICBitmapLock::GetStride", "i:ii" }, { "IWICBitmapLock::GetDataPointer", "i:iii" }, { "IWICBitmapLock::GetPixelFormat", "i:ii" },
  { "WICCreateImagingFactory_Proxy", "i:ii" }, { "WICConvertBitmapSource", "i:iii" },
#define D1(n) { #n, "d:d" }, { #n "f", "f:f" }, { #n "l", "d:d" },
#define D2(n) { #n, "d:dd" }, { #n "f", "f:ff" }, { #n "l", "d:dd" },
  D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh) D1(asinh) D1(acosh) D1(atanh) D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p)
  D1(sqrt) D1(cbrt) D1(fabs) D1(floor) D1(ceil) D1(trunc) D1(round) D1(rint) D1(nearbyint) D1(erf) D1(erfc) D1(tgamma) D1(lgamma) D1(logb)
  D2(pow) D2(atan2) D2(fmod) D2(hypot) D2(fmin) D2(fmax) D2(fdim) D2(copysign) D2(nextafter) D2(remainder)
#undef D1
#undef D2
  { "fma", "d:ddd" }, { "fmaf", "f:fff" }, { "fmal", "d:ddd" }, { "ldexp", "d:di" }, { "ldexpf", "f:fi" }, { "ldexpl", "d:di" }, { "_ldexpf", "f:fi" }, { "scalbn", "d:di" }, { "scalbnf", "f:fi" }, { "scalbnl", "d:di" },
  { "scalbln", "d:di" }, { "scalblnf", "f:fi" }, { "scalblnl", "d:di" }, { "_scalb", "d:di" }, { "_scalbf", "f:fi" }, { "frexp", "d:di" }, { "frexpf", "f:fi" }, { "frexpl", "d:di" }, { "modf", "d:di" }, { "modff", "f:fi" }, { "modfl", "d:di" },
  { "ilogb", "i:d" }, { "ilogbf", "i:f" }, { "ilogbl", "i:d" }, { "lround", "i:d" }, { "lroundf", "i:f" }, { "lroundl", "i:d" }, { "llround", "q:d" }, { "llroundf", "q:f" }, { "llroundl", "q:d" }, { "lrint", "i:d" }, { "lrintf", "i:f" }, { "llrint", "q:d" }, { "llrintf", "q:f" },
  { "nexttoward", "d:dd" }, { "nexttowardf", "f:fd" }, { "nexttowardl", "d:dd" }, { "_copysign", "d:dd" }, { "_copysignf", "f:ff" }, { "_chgsign", "d:d" }, { "_chgsignf", "f:f" }, { "_hypot", "d:dd" }, { "_hypotf", "f:ff" }, { "_logb", "d:d" }, { "_logbf", "f:f" },
  { "_nextafter", "d:dd" }, { "_nextafterf", "f:ff" }, { "_nextafterl", "d:dd" }, { "_j0", "d:d" }, { "_j1", "d:d" }, { "_y0", "d:d" }, { "_y1", "d:d" }, { "_jn", "d:id" }, { "_yn", "d:id" }, { "j0", "d:d" }, { "j1", "d:d" }, { "y0", "d:d" }, { "y1", "d:d" }, { "jn", "d:id" }, { "yn", "d:id" },
  { "_finite", "i:d" }, { "_finitef", "i:f" }, { "_isnan", "i:d" }, { "_isnanf", "i:f" }, { "_fpclass", "i:d" }, { "_fpclassf", "i:f" }, { "_dclass", "i:d" }, { "_fdclass", "i:f" }, { "_ldclass", "i:d" }, { "_dsign", "i:d" }, { "_fdsign", "i:f" }, { "_ldsign", "i:d" },
  { "_dpcomp", "i:dd" }, { "_fdpcomp", "i:ff" }, { "_ldpcomp", "i:dd" }, { "_dtest", "i:i" }, { "_fdtest", "i:i" }, { "_ldtest", "i:i" }, { "_cabs", "d:dd" }, { "cabs", "d:dd" }, { "cabsf", "f:ff" }, { "cabsl", "d:dd" }, { "_isinf", "i:d" }, { "_isinff", "i:f" },
  { "atof", "d:i" }, { "_atof_l", "d:ii" }, { "strtod", "d:ii" }, { "_strtod_l", "d:iii" }, { "wcstod", "d:ii" }, { "_wcstod_l", "d:iii" }, { "strtof", "f:ii" }, { "wcstof", "f:ii" }, { "difftime", "d:qq" }, { "_difftime64", "d:qq" }, { "_difftime32", "d:ii" },
  { "_lseeki64", "q:iqi" }, { "_telli64", "q:i" }, { "_filelengthi64", "q:i" }, { "_fseeki64", "i:iqi" }, { "_ftelli64", "q:i" }, { "_chsize_s", "i:iq" }, { "_abs64", "q:q" }, { "llabs", "q:q" }, { "_atoi64", "q:i" }, { "_atoi64_l", "q:ii" }, { "_wtoi64", "q:i" }, { "_wtoi64_l", "q:ii" },
  { "_i64toa", "i:qii" }, { "_ui64toa", "i:qii" }, { "_i64tow", "i:qii" }, { "_ui64tow", "i:qii" }, { "_i64toa_s", "i:qiii" }, { "_ui64toa_s", "i:qiii" }, { "_i64tow_s", "i:qiii" }, { "_ui64tow_s", "i:qiii" }, { "_rotl64", "q:qi" }, { "_rotr64", "q:qi" }, { "_byteswap_uint64", "q:q" },
  { "_localtime64", "i:i" }, { "_gmtime64", "i:i" }, { "_ctime64", "i:i" }, { "_mktime64", "q:i" }, { "_time64", "q:i" }, { "_mkgmtime64", "q:i" }, { "_mkgmtime", "q:i" }, { "_utime64", "i:ii" }, { "_futime64", "i:ii" },
  { "SetFilePointerEx", "i:iqii" }, { "VerSetConditionMask", "q:qii" }, { "VerifyVersionInfoA", "i:iiq" }, { "VerifyVersionInfoW", "i:iiq" }, { "RtlVerifyVersionInfo", "i:iiq" }, { "GetTickCount64", "q:" }, { "SetWaitableTimer", "i:iiiiii" },
  { "InterlockedCompareExchange64", "q:iqq" }, { "InterlockedExchange64", "q:iq" }, { "InterlockedExchangeAdd64", "q:iq" }, { "InterlockedIncrement64", "q:i" }, { "InterlockedDecrement64", "q:i" }, { "InterlockedAnd64", "q:iq" }, { "InterlockedOr64", "q:iq" },
  { "RtlLargeIntegerDivide", "q:qqi" }, { "RtlExtendedLargeIntegerDivide", "q:qii" }, { "RtlEnlargedIntegerMultiply", "q:ii" }, { "RtlEnlargedUnsignedMultiply", "q:ii" }, { "RtlEnlargedUnsignedDivide", "q:qii" }, { "RtlExtendedMagicDivide", "q:qqi" }, { "RtlExtendedIntegerMultiply", "q:qi" },
  { "RtlLargeIntegerAdd", "q:qq" }, { "RtlLargeIntegerSubtract", "q:qq" }, { "RtlLargeIntegerNegate", "q:q" }, { "RtlLargeIntegerShiftLeft", "q:qi" }, { "RtlLargeIntegerShiftRight", "q:qi" }, { "RtlLargeIntegerArithmeticShift", "q:qi" }, { "RtlConvertLongToLargeInteger", "q:i" }, { "RtlConvertUlongToLargeInteger", "q:i" },
  { "Int64ShllMod32", "q:qi" }, { "Int64ShraMod32", "q:qi" }, { "Int64ShrlMod32", "q:qi" }, { "_allmul", "q:qq" }, { "_alldiv", "q:qq" }, { "_allrem", "q:qq" }, { "_aulldiv", "q:qq" }, { "_aullrem", "q:qq" },
  { "StrFormatByteSizeW", "i:qii" }, { "StrFormatByteSize64A", "i:qii" }, { "StrFormatKBSizeW", "i:qii" }, { "StrFormatKBSizeA", "i:qii" }, { "PathBuildRootW", "i:ii" },
  { "timeBeginPeriod", "i:i" }, { "waveOutGetPosition", "i:iii" },
  /* UCRT stdio entry points: the first argument (_CRT_INTERNAL_*_OPTIONS) is a 64-bit integer = two slots */
  { "__stdio_common_vfprintf", "i:qiiii" }, { "__stdio_common_vfprintf_s", "i:qiiii" }, { "__stdio_common_vfprintf_p", "i:qiiii" }, { "__stdio_common_vfwprintf", "i:qiiii" }, { "__stdio_common_vfwprintf_s", "i:qiiii" }, { "__stdio_common_vfwprintf_p", "i:qiiii" }, { "__stdio_common_vfscanf", "i:qiiii" }, { "__stdio_common_vfwscanf", "i:qiiii" },
  { "__stdio_common_vsprintf", "i:qiiiii" }, { "__stdio_common_vsprintf_s", "i:qiiiii" }, { "__stdio_common_vsprintf_p", "i:qiiiii" }, { "__stdio_common_vswprintf", "i:qiiiii" }, { "__stdio_common_vswprintf_s", "i:qiiiii" }, { "__stdio_common_vswprintf_p", "i:qiiiii" }, { "__stdio_common_vsscanf", "i:qiiiii" }, { "__stdio_common_vswscanf", "i:qiiiii" },
  { "__stdio_common_vsnprintf_s", "i:qiiiiii" }, { "__stdio_common_vsnwprintf_s", "i:qiiiiii" }, { "__conio_common_vcprintf", "i:qiii" }, { "__conio_common_vcwprintf", "i:qiii" }, { "__conio_common_vcprintf_s", "i:qiii" }, { "__conio_common_vcwprintf_s", "i:qiii" }, { "__conio_common_vcprintf_p", "i:qiii" }, { "__conio_common_vcwprintf_p", "i:qiii" }, { "_localtime64_s", "i:ii" }, { "_gmtime64_s", "i:ii" }, { "_ctime64_s", "i:iii" }, { "_wctime64_s", "i:iii" }, { "_wctime64", "i:i" }, { "_findfirst64", "i:ii" }, { "_findnext64", "i:ii" }, { "_wfindfirst64", "i:ii" }, { "_wfindnext64", "i:ii" }, { "_findclose", "i:i" }, { "_wfseeki64", "i:iqi" }, { "_fseeki64_nolock", "i:iqi" }, { "_ftelli64_nolock", "q:i" }, { "_lseeki64_nolock", "q:iqi" }, { "_set_abort_behavior", "i:ii" }, { "_wtime64", "q:i" }, { "_rotl64", "q:qi" },
};
static const char *sig32_lookup(const char *name) { for (size_t i = 0; i < sizeof sig32_tab / sizeof sig32_tab[0]; i++) if (!e_strcmp(sig32_tab[i].n, name)) return sig32_tab[i].sig; return 0; }
static int clean32_lookup(const char *name) {   /* sorted table → binary search; cdecl/unknown → 0 (the caller cleans) */
  int lo = 0, hi = (int)(sizeof stdcall32_tab / sizeof stdcall32_tab[0]) - 1;
  while (lo <= hi) { int mid = (lo + hi) / 2; int r = e_strcmp(stdcall32_tab[mid].n, name); if (r == 0) return stdcall32_tab[mid].bytes; if (r < 0) lo = mid + 1; else hi = mid - 1; }
  return 0;
}
/* names that end in "A"/"W" variants, "Ex" … are all in the table; a few families that Wine implements as cdecl/varargs but Windows
   exports as stdcall in 32-bit kernel32 are fixed up here */
static int clean32_of(const char *name, const char *sig) {
  int b = clean32_lookup(name);
  if (!b && sig) {   /* stdcall APIs with a signature but no Wine entry: compute from the signature when the name looks like a Win32 API (capitalised) */
    if (name[0] >= 'A' && name[0] <= 'Z') { for (const char *p = sig + 2; *p; p++) b += (*p == 'q' || *p == 'd') ? 8 : 4; }
  }
  return b;
}

/* argument override: a shim called from another shim (or a 32-bit frame unpacked by a signature) reads its arguments from here */
static void w32_marshal32_in(Cpu *c, ShimSlot *s) {
  const char *p = s->sig32; if (!p || p[0] == 0 || p[1] != ':') return; p += 2;
  u64 sp = c->r[RSP]; int n = 0;
  for (; *p && n < 16; p++, n++) {
    switch (*p) {
      case 'd': { u64 b = rd64(c, sp); e_memcpy(&c->x[n].f64[0], &b, 8); g_a32[n] = b; sp += 8; break; }
      case 'f': { u32 b = rd32(c, sp); e_memcpy(&c->x[n].f32[0], &b, 4); g_a32[n] = b; sp += 4; break; }
      case 'q': { g_a32[n] = rd64(c, sp); sp += 8; break; }
      default:  { g_a32[n] = rd32(c, sp); sp += 4; break; }
    }
    if (n == 0) c->r[RCX] = g_a32[0]; else if (n == 1) c->r[RDX] = g_a32[1]; else if (n == 2) c->r[R8] = g_a32[2]; else if (n == 3) c->r[R9] = g_a32[3];
  }
  g_a32_n = n;
}
static void w32_marshal32_out(Cpu *c, ShimSlot *s) {
  g_a32_n = -1; const char *p = s->sig32; if (!p) return;
  if (p[0] == 'd') fpush(c, c->x[0].f64[0]); else if (p[0] == 'f') fpush(c, (double)c->x[0].f32[0]);
}
/* call shim `fn` with explicit arguments from inside another shim (register ABI in x64, override array in 32-bit mode) */
void call_shim(Cpu *c, ShimFn fn, u64 a0, u64 a1, u64 a2, u64 a3) {
  u64 sv[4] = { c->r[RCX], c->r[RDX], c->r[R8], c->r[R9] }; u64 so[4] = { 0, 0, 0, 0 }; int sn = g_a32_n;
  if (c->mode32) { for (int k = 0; k < 4; k++) so[k] = g_a32[k]; g_a32[0] = a0; g_a32[1] = a1; g_a32[2] = a2; g_a32[3] = a3; g_a32_n = 4; }
  c->r[RCX] = a0; c->r[RDX] = a1; c->r[R8] = a2; c->r[R9] = a3;
  fn(c);
  c->r[RCX] = sv[0]; c->r[RDX] = sv[1]; c->r[R8] = sv[2]; c->r[R9] = sv[3];
  if (c->mode32) { for (int k = 0; k < 4; k++) g_a32[k] = so[k]; g_a32_n = sn; }
}

void call_shim_tail(Cpu *c, ShimFn fn, u64 a0, u64 a1, u64 a2, u64 a3) {
  u64 so[4] = { 0, 0, 0, 0 }; int sn = g_a32_n;
  if (c->mode32) { for (int k = 0; k < 4; k++) so[k] = g_a32[k]; g_a32[0] = a0; g_a32[1] = a1; g_a32[2] = a2; g_a32[3] = a3; g_a32_n = 4; }
  else { c->r[RCX] = a0; c->r[RDX] = a1; c->r[R8] = a2; c->r[R9] = a3; }
  fn(c);
  if (c->mode32) { for (int k = 0; k < 4; k++) g_a32[k] = so[k]; g_a32_n = sn; }
}

/* ---- x86 CRT helpers with register conventions (MSVC 32-bit code generation) ---- */
/* _chkstk / _alloca_probe: eax = bytes; on return esp has been lowered by eax (the return address was already popped), eax is scratch */
static void s_chkstk32(Cpu *c) { u32 n = (u32)c->r[RAX]; c->r[RSP] = (u32)(c->r[RSP] - n); RET(c->rip); }
static void s_chkstk_ms32(Cpu *c) { (void)c; }   /* ___chkstk_ms (mingw): probe only, no esp change */
/* _ftol / _ftol2 / _ftol2_sse: ST(0) → truncated int64 in edx:eax, pops ST(0) */
static void s_ftol32(Cpu *c) { double v = fpop(c); s64 r = (v != v) ? (s64)0x8000000000000000ll : v >= 9223372036854775807.0 ? (s64)0x8000000000000000ll : v <= -9223372036854775808.0 ? (s64)0x8000000000000000ll : (s64)v; RET((u64)r); }
/* _CIsin … : ST(0) = f(ST(0)); two-argument forms take x in ST(1), y in ST(0) and pop both */
#define CI1(name, op) static void s_CI##name(Cpu *c) { c->fp[c->ftop] = host_math(op, c->fp[c->ftop], 0); }
CI1(sin, 0) CI1(cos, 1) CI1(tan, 2) CI1(exp, 3) CI1(log, 4) CI1(asin, 7) CI1(acos, 8) CI1(atan, 9) CI1(sinh, 10) CI1(cosh, 11) CI1(tanh, 12) CI1(log10, 13)
#undef CI1
static void s_CIsqrt(Cpu *c) { c->fp[c->ftop] = e_sqrt(c->fp[c->ftop]); }
static void s_CIpow(Cpu *c) { double y = fpop(c), x = fpop(c); fpush(c, host_math(5, x, y)); }
static void s_CIatan2(Cpu *c) { double x = fpop(c), y = fpop(c); fpush(c, host_math(6, y, x)); }
static void s_CIfmod(Cpu *c) { double y = fpop(c), x = fpop(c); fpush(c, e_fmod(x, y)); }
/* 64-bit integer helpers: operands on the stack (a lo, a hi, b lo, b hi), result edx:eax; the cleanup table pops 16 bytes */
static void s_allmul(Cpu *c) { u64 a = ARG(0) | ARG(1) << 32, b = ARG(2) | ARG(3) << 32; RET(a * b); }
static void s_alldiv(Cpu *c) { s64 a = (s64)(ARG(0) | ARG(1) << 32), b = (s64)(ARG(2) | ARG(3) << 32); RET(b ? (u64)(a / b) : 0); }
static void s_allrem(Cpu *c) { s64 a = (s64)(ARG(0) | ARG(1) << 32), b = (s64)(ARG(2) | ARG(3) << 32); RET(b ? (u64)(a % b) : 0); }
static void s_aulldiv(Cpu *c) { u64 a = ARG(0) | ARG(1) << 32, b = ARG(2) | ARG(3) << 32; RET(b ? a / b : 0); }
static void s_aullrem(Cpu *c) { u64 a = ARG(0) | ARG(1) << 32, b = ARG(2) | ARG(3) << 32; RET(b ? a % b : 0); }
static void s_alldvrm(Cpu *c) { s64 a = (s64)(ARG(0) | ARG(1) << 32), b = (s64)(ARG(2) | ARG(3) << 32); s64 q = b ? a / b : 0, r = b ? a % b : 0; RET((u64)q); c->r[RCX] = (u32)r; c->r[RBX] = (u32)((u64)r >> 32); }   /* quotient edx:eax, remainder ebx:ecx */
static void s_aulldvrm(Cpu *c) { u64 a = ARG(0) | ARG(1) << 32, b = ARG(2) | ARG(3) << 32; u64 q = b ? a / b : 0, r = b ? a % b : 0; RET(q); c->r[RCX] = (u32)r; c->r[RBX] = (u32)(r >> 32); }
/* shifts: value edx:eax, count cl, no stack arguments */
static void s_allshl(Cpu *c) { u64 v = (u32)c->r[RAX] | (u64)(u32)c->r[RDX] << 32; u32 n = (u32)c->r[RCX] & 0xff; RET(n >= 64 ? 0 : v << n); }
static void s_allshr(Cpu *c) { s64 v = (s64)((u32)c->r[RAX] | (u64)(u32)c->r[RDX] << 32); u32 n = (u32)c->r[RCX] & 0xff; RET((u64)(n >= 64 ? (v < 0 ? -1 : 0) : v >> n)); }
static void s_aullshr(Cpu *c) { u64 v = (u32)c->r[RAX] | (u64)(u32)c->r[RDX] << 32; u32 n = (u32)c->r[RCX] & 0xff; RET(n >= 64 ? 0 : v >> n); }
/* x86 stack-cookie / SEH prologue helpers that are plain functions in the x64 CRT */
static void s_chkesp32(Cpu *c) { (void)c; }

#define X86_SHIMS \
  SA("_chkstk", chkstk32), SA("_alloca_probe", chkstk32), SA("_alloca_probe_8", chkstk32), SA("_alloca_probe_16", chkstk32), SA("___chkstk_ms", chkstk_ms32), SA("__chkstk_ms", chkstk_ms32), SA("_chkesp", chkesp32), \
  SA("_ftol", ftol32), SA("_ftol2", ftol32), SA("_ftol2_sse", ftol32), SA("_ftol2_sse_excpt", ftol32), SA("_ftoul2", ftol32), SA("_ftoul2_legacy", ftol32), \
  SA("_CIsin", CIsin), SA("_CIcos", CIcos), SA("_CItan", CItan), SA("_CIasin", CIasin), SA("_CIacos", CIacos), SA("_CIatan", CIatan), SA("_CIsinh", CIsinh), SA("_CIcosh", CIcosh), SA("_CItanh", CItanh), \
  SA("_CIexp", CIexp), SA("_CIlog", CIlog), SA("_CIlog10", CIlog10), SA("_CIsqrt", CIsqrt), SA("_CIpow", CIpow), SA("_CIatan2", CIatan2), SA("_CIfmod", CIfmod), \
  SA("_allmul", allmul), SA("_alldiv", alldiv), SA("_allrem", allrem), SA("_aulldiv", aulldiv), SA("_aullrem", aullrem), SA("_alldvrm", alldvrm), SA("_aulldvrm", aulldvrm), \
  SA("_allshl", allshl), SA("_allshr", allshr), SA("_aullshr", aullshr),
