/*
 * pwsh.c — a small, freestanding powershell.exe for the Nebula x64 emulator.
 *
 * Real PowerShell is a .NET application (pwsh 7 = ~100 MB of CoreCLR + assemblies) — far too heavy to run as a helper
 * inside a wasm64 x86-64 interpreter just so that installers can unzip a file.  This is a PE32+ console program built
 * with `zig cc -target x86_64-windows-gnu -nostdlib` (kernel32 only, ~60 KB) that the emulator places at
 * C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe (+ powershell.exe / pwsh.exe in System32) and that
 * implements the subset of the PowerShell language and cmdlets that command-line tools actually spawn:
 *
 *   powershell [-NoProfile] [-NonInteractive] [-NoLogo] [-ExecutionPolicy x] [-WindowStyle x] [-InputFormat x]
 *              [-OutputFormat x] [-Version] [-Help] [-File script.ps1 [args]] [-EncodedCommand b64] [-Command | -c] script…
 *   statements: `;` / newline separated, pipelines `a | b`, `&&` `||`, `> file` `>> file` `2>&1` `2>$null`
 *   $var = <value | (expr) | pipeline>, $env:NAME = …, $global:/$script: prefixes, "double" (expands $var, $env:X, `n `t ``)
 *   'single', @"…"@ / @'…'@ here-strings (one-line), $(…) subexpressions inside double quotes
 *   if (…) {…} elseif (…) {…} else {…}   try {…} catch {…} finally {…}   foreach ($x in …) {…}   while (…) {…}
 *   comparison -eq -ne -like -notlike -match -notmatch -gt -lt -ge -le (-ieq… too), -and -or -not !, -contains
 *   -join, -split, -replace, -f (format {0}), string methods .Trim() .ToLower() .ToUpper() .Replace(a,b) .Length
 *   .StartsWith() .EndsWith() .Contains() .Split() .Substring(), arrays @(a,b) / a,b (as newline-separated lists)
 *   [System.IO.Compression.ZipFile]::ExtractToDirectory(zip, dir)   [IO.File]::Exists(p)  [Environment]::GetEnvironmentVariable(n)
 *   [Environment]::SetEnvironmentVariable(n, v, 'Process')   [Console]::WriteLine(s)   [IO.Path]::Combine(a,b)
 *   [IO.Directory]::CreateDirectory(p)  [IO.File]::ReadAllText(p)  [IO.File]::WriteAllText(p, s)  [IO.Directory]::Exists(p)
 *   cmdlets: Expand-Archive (deflate + stored, directories, -Force)  Compress-Archive (stored)  Get-ChildItem/gci/ls/dir
 *     Test-Path  New-Item  Remove-Item/rm/del/rmdir  Copy-Item/cp/copy  Move-Item/mv/move  Rename-Item/ren
 *     Get-Content/cat/type  Set-Content  Add-Content  Out-File  Out-Null  Out-String  Out-Host  Write-Output/echo/Write-Host
 *     Write-Error  Write-Warning  Write-Verbose  Write-Debug  Write-Information  Write-Progress  Get-Location/pwd
 *     Set-Location/cd  Push-Location  Pop-Location  Get-Item  Join-Path  Split-Path  Resolve-Path  Get-Command/gcm
 *     Select-String/sls  Select-Object/select (-First -Last -Skip -Unique -ExpandProperty)  Where-Object/where/?
 *     ForEach-Object/foreach/%  Sort-Object/sort  Measure-Object  Get-Unique  Start-Sleep  Get-Date  Start-Process
 *     Get-Process  Stop-Process  Get-Host  Get-FileHash (SHA256)  ConvertTo-Json (strings/arrays)  Invoke-Expression/iex
 *     Set-Variable  Get-Variable  Set-Alias  Get-ExecutionPolicy  Set-ExecutionPolicy  Set-StrictMode  Import-Module
 *     $PSVersionTable  $PSScriptRoot  $PWD  $HOME  $null  $true  $false  $LASTEXITCODE  $?  $args  $_  $IsWindows
 *     Invoke-WebRequest / iwr / curl / wget → honest error (no HTTP stack inside this helper), same for Start-BitsTransfer
 *   external programs: `& 'path' args`, `.\prog.exe`, anything with an extension or found on PATH; exit code → $LASTEXITCODE
 *
 * Not implemented: functions/param blocks, classes, .NET objects beyond the static calls above, remoting, the ISE, formatting
 * tables (Get-ChildItem prints a plain listing).  Unknown cmdlets fail with the standard "is not recognized" message.
 * Public domain / CC0 — part of the nebula_gaming repository.
 */
typedef unsigned long DWORD; typedef unsigned short WORD; typedef unsigned char BYTE; typedef int BOOL; typedef void *HANDLE;
typedef unsigned long long u64; typedef long long s64; typedef unsigned int u32; typedef unsigned short u16;
typedef struct { DWORD lo, hi; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { DWORD cb; char *lpReserved, *lpDesktop, *lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; BYTE *lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOA;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; char cFileName[260]; char cAlternateFileName[14]; } WIN32_FIND_DATAA;
#define DI __declspec(dllimport)
DI HANDLE GetStdHandle(DWORD); DI BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *); DI BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
DI __attribute__((noreturn)) void ExitProcess(u32); DI char *GetCommandLineA(void); DI DWORD GetLastError(void); DI BOOL CloseHandle(HANDLE);
DI HANDLE CreateFileA(const char *, DWORD, DWORD, SECURITY_ATTRIBUTES *, DWORD, DWORD, HANDLE);
DI HANDLE FindFirstFileA(const char *, WIN32_FIND_DATAA *); DI BOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA *); DI BOOL FindClose(HANDLE);
DI DWORD GetFileAttributesA(const char *); DI BOOL DeleteFileA(const char *); DI BOOL CreateDirectoryA(const char *, void *); DI BOOL RemoveDirectoryA(const char *);
DI BOOL MoveFileExA(const char *, const char *, DWORD); DI BOOL CopyFileA(const char *, const char *, BOOL);
DI DWORD GetCurrentDirectoryA(DWORD, char *); DI BOOL SetCurrentDirectoryA(const char *); DI DWORD GetFullPathNameA(const char *, DWORD, char *, char **);
DI DWORD GetEnvironmentVariableA(const char *, char *, DWORD); DI BOOL SetEnvironmentVariableA(const char *, const char *); DI char *GetEnvironmentStringsA(void);
DI BOOL CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, STARTUPINFOA *, PROCESS_INFORMATION *);
DI DWORD WaitForSingleObject(HANDLE, DWORD); DI BOOL GetExitCodeProcess(HANDLE, DWORD *); DI BOOL CreatePipe(HANDLE *, HANDLE *, SECURITY_ATTRIBUTES *, DWORD);
DI BOOL SetHandleInformation(HANDLE, DWORD, DWORD); DI DWORD GetTickCount(void); DI void GetLocalTime(SYSTEMTIME *); DI void GetSystemTime(SYSTEMTIME *); DI void Sleep(DWORD);
DI HANDLE GetProcessHeap(void); DI void *HeapAlloc(HANDLE, DWORD, u64); DI BOOL HeapFree(HANDLE, DWORD, void *); DI BOOL GetFileSizeEx(HANDLE, s64 *);
DI DWORD GetFileType(HANDLE); DI BOOL GetConsoleMode(HANDLE, DWORD *); DI DWORD GetCurrentProcessId(void); DI BOOL TerminateProcess(HANDLE, u32); DI HANDLE OpenProcess(DWORD, BOOL, DWORD);
DI BOOL SetFilePointerEx(HANDLE, s64, s64 *, DWORD); DI DWORD GetModuleFileNameA(void *, char *, DWORD);
DI BOOL FileTimeToLocalFileTime(const FILETIME *, FILETIME *); DI BOOL FileTimeToSystemTime(const FILETIME *, SYSTEMTIME *);
#define INVALID ((HANDLE)(s64)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_RW 3
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define ATTR_DIR 0x10

/* ------------------------------------------------------------------ tiny libc ------------------------------------------------------------------ */
void *memcpy(void *d, const void *s, u64 n) { BYTE *a = d; const BYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, u64 n) { BYTE *a = d; const BYTE *b = s; if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } return d; }
void *memset(void *d, int v, u64 n) { BYTE *a = d; while (n--) *a++ = (BYTE)v; return d; }
int memcmp(const void *x, const void *y, u64 n) { const BYTE *a = x, *b = y; for (u64 i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i]; return 0; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static char uc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static int sieq(const char *a, const char *b) { while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; } return *a == *b; }
static int seq(const char *a, const char *b) { while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b; }
static int sicmp(const char *a, const char *b) { while (*a && *b) { if (lc(*a) != lc(*b)) return (BYTE)lc(*a) - (BYTE)lc(*b); a++; b++; } return (BYTE)*a - (BYTE)*b; }
static int snieq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) { if (lc(a[i]) != lc(b[i])) return 0; if (!a[i]) return 1; } return 1; }
static void scpy(char *d, int cap, const char *s) { int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static void scat(char *d, int cap, const char *s) { int n = slen(d); scpy(d + n, cap - n, s); }
static const char *sistr(const char *h, const char *n) { int ln = slen(n); if (!ln) return h; for (; *h; h++) if (snieq(h, n, ln)) return h; return 0; }
static const char *sstr(const char *h, const char *n) { int ln = slen(n); if (!ln) return h; for (; *h; h++) if (!memcmp(h, n, (u64)ln)) return h; return 0; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_digit(c) || (lc(c) >= 'a' && lc(c) <= 'z') || c == '_'; }
static s64 satoi(const char *s, const char **end) { int neg = 0; while (is_space(*s)) s++; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; s64 v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; for (;;) { char c = lc(*s); if (c >= '0' && c <= '9') v = v * 16 + (c - '0'); else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10); else break; s++; } }
  else while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); if (end) *end = s; return neg ? -v : v; }
static int is_number(const char *s) { const char *e; if (!*s) return 0; satoi(s, &e); while (is_space(*e)) e++; return *e == 0; }
static int fmt_v(char *out, int cap, const char *f, __builtin_va_list ap) {
  int o = 0; char tmp[40];
#define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++; int width = 0, left = 0, zero = 0; if (*f == '-') { left = 1; f++; } if (*f == '0') { zero = 1; f++; } while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    const char *s = tmp; int n;
    if (*f == 's') { s = __builtin_va_arg(ap, const char *); if (!s) s = ""; }
    else if (*f == 'c') { tmp[0] = (char)__builtin_va_arg(ap, int); tmp[1] = 0; }
    else if (*f == 'd' || *f == 'L') { s64 v = *f == 'd' ? (s64)__builtin_va_arg(ap, int) : __builtin_va_arg(ap, s64); char *p = tmp + 39; *p = 0; int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v; do { *--p = (char)('0' + u % 10); u /= 10; } while (u); if (neg) *--p = '-'; s = p; }
    else if (*f == 'u') { u32 v = __builtin_va_arg(ap, u32); char *p = tmp + 39; *p = 0; do { *--p = (char)('0' + v % 10); v /= 10; } while (v); s = p; }
    else if (*f == 'x' || *f == 'X') { u64 v = __builtin_va_arg(ap, u64); char *p = tmp + 39; *p = 0; do { int d = (int)(v & 15); *--p = (char)(d < 10 ? '0' + d : (*f == 'x' ? 'a' : 'A') + d - 10); v >>= 4; } while (v); s = p; }
    else if (*f == '%') { tmp[0] = '%'; tmp[1] = 0; }
    else { tmp[0] = '%'; tmp[1] = *f; tmp[2] = 0; }
    n = slen(s); if (!left) for (int i = n; i < width; i++) PUT(zero ? '0' : ' '); for (int i = 0; i < n; i++) PUT(s[i]); if (left) for (int i = n; i < width; i++) PUT(' ');
  }
  if (cap) out[o < cap ? o : cap - 1] = 0; return o;
#undef PUT
}
static int sfmt(char *out, int cap, const char *f, ...) { __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(out, cap, f, ap); __builtin_va_end(ap); return n; }

/* ------------------------------------------------------------------ memory / strings ------------------------------------------------------------------ */
static HANDLE g_heap;
static void *xalloc(u64 n) { void *p = HeapAlloc(g_heap, 8, n ? n : 1); if (!p) { DWORD w; WriteFile(GetStdHandle((DWORD)-12), "Out of memory\r\n", 15, &w, 0); ExitProcess(8); } return p; }
static void xfree(void *p) { if (p) HeapFree(g_heap, 0, p); }
static char *sdup(const char *s) { int n = slen(s); char *d = xalloc((u64)n + 1); memcpy(d, s, (u64)n + 1); return d; }
static char *sdupn(const char *s, int n) { char *d = xalloc((u64)n + 1); memcpy(d, s, (u64)n); d[n] = 0; return d; }
/* growable string buffer */
typedef struct { char *p; int n, cap; } Buf;
static void bput(Buf *b, const char *s, int n) { if (n < 0) n = slen(s); if (b->n + n + 1 > b->cap) { int nc = b->cap ? b->cap : 256; while (nc < b->n + n + 1) nc *= 2; char *np = xalloc((u64)nc); if (b->p) { memcpy(np, b->p, (u64)b->n); xfree(b->p); } b->p = np; b->cap = nc; } memcpy(b->p + b->n, s, (u64)n); b->n += n; b->p[b->n] = 0; }
static void bputc(Buf *b, char c) { bput(b, &c, 1); }
static const char *bstr(Buf *b) { if (!b->p) bput(b, "", 0); return b->p; }
static void bfree(Buf *b) { xfree(b->p); b->p = 0; b->n = b->cap = 0; }
/* iterate newline-separated input */
#define FOR_LINES(input, p, e, n) for (const char *p = (input); p && *p;) { const char *e = p; while (*e && *e != '\n') e++; int n = (int)(e - p); if (n && p[n - 1] == '\r') n--;
#define END_LINES(p, e) p = *e ? e + 1 : e; }

/* ------------------------------------------------------------------ I/O ------------------------------------------------------------------ */
static HANDLE g_in, g_out, g_err;
static Buf *g_capture;                      /* when set, pipeline output goes here instead of g_out */
static int g_last_exit, g_last_ok = 1, g_noninteractive, g_quit, g_exit_code;
static char g_self[520], g_script_root[520];
static void hwrite(HANDLE h, const char *s, int n) { if (!h || h == INVALID) return; DWORD w; while (n > 0) { if (!WriteFile(h, s, (DWORD)n, &w, 0) || !w) return; s += w; n -= (int)w; } }
static void outn(const char *s, int n) { if (n < 0) n = slen(s); if (g_capture) bput(g_capture, s, n); else hwrite(g_out, s, n); }
static void outs(const char *s) { outn(s, -1); }
static void outline(const char *s) { outn(s, -1); outn("\r\n", 2); }
static void errs(const char *s) { hwrite(g_err, s, slen(s)); }
static void errf(const char *f, ...) { char b[4096]; __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(b, sizeof b, f, ap); __builtin_va_end(ap); hwrite(g_err, b, n < (int)sizeof b ? n : (int)sizeof b - 1); }
static void outf(const char *f, ...) { char b[4096]; __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(b, sizeof b, f, ap); __builtin_va_end(ap); outn(b, n < (int)sizeof b ? n : (int)sizeof b - 1); }
static char *read_all(HANDLE h, int *len) { int cap = 65536, n = 0; char *b = xalloc((u64)cap); DWORD got; for (;;) { if (n + 4096 > cap) { cap *= 2; char *nb = xalloc((u64)cap); memcpy(nb, b, (u64)n); xfree(b); b = nb; } if (!ReadFile(h, b + n, (DWORD)(cap - n - 1), &got, 0) || !got) break; n += (int)got; } b[n] = 0; if (len) *len = n; return b; }
static char *read_file(const char *path, int *len) { HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_RW, 0, OPEN_EXISTING, 0x80, 0); if (h == INVALID) return 0; char *b = read_all(h, len); CloseHandle(h); return b; }
static int write_file(const char *path, const char *data, int n, int append) { HANDLE h = CreateFileA(path, append ? 4 /*FILE_APPEND_DATA*/ : GENERIC_WRITE, FILE_SHARE_RW, 0, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0x80, 0); if (h == INVALID) return 0; if (append) SetFilePointerEx(h, 0, 0, 2); hwrite(h, data, n); CloseHandle(h); return 1; }
static int read_line(HANDLE h, char *buf, int cap) { int n = 0; DWORD got; char c; for (;;) { if (!ReadFile(h, &c, 1, &got, 0) || got == 0) { if (!n) return 0; break; } if (c == '\n') break; if (c == 0x1a && n == 0) return 0; if (n + 1 < cap) buf[n++] = c; } while (n && buf[n - 1] == '\r') n--; buf[n] = 0; return 1; }

/* ------------------------------------------------------------------ paths ------------------------------------------------------------------ */
static void getenv_s(const char *name, char *out, int cap) { DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap); if (!n || n >= (DWORD)cap) out[0] = 0; }
static void cwd(char *out, int cap) { if (!GetCurrentDirectoryA((DWORD)cap, out)) scpy(out, cap, "C:\\"); }
static void norm_slashes(char *p) { for (; *p; p++) if (*p == '/') *p = '\\'; }
static void strip_trailing(char *t) { int n = slen(t); while (n > 1 && (t[n - 1] == '\\' || t[n - 1] == '/') && t[n - 2] != ':') t[--n] = 0; }
static void full_path(const char *in, char *out, int cap) {
  char t[1024]; scpy(t, sizeof t, in); norm_slashes(t);
  if (t[0] == '~' && (t[1] == 0 || t[1] == '\\')) { char h[520]; getenv_s("USERPROFILE", h, sizeof h); char u[1024]; sfmt(u, sizeof u, "%s%s", h, t + 1); scpy(t, sizeof t, u); }
  if (!GetFullPathNameA(t, (DWORD)cap, out, 0)) scpy(out, cap, t);
}
static DWORD attrs(const char *p) { char t[1024]; full_path(p, t, sizeof t); strip_trailing(t); return GetFileAttributesA(t); }
static int exists(const char *p) { return attrs(p) != 0xFFFFFFFFu; }
static int is_dir(const char *p) { DWORD a = attrs(p); return a != 0xFFFFFFFFu && (a & ATTR_DIR); }
static int has_wild(const char *p) { for (; *p; p++) if (*p == '*' || *p == '?') return 1; return 0; }
static const char *basename_of(const char *p) { const char *b = p; for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1; return b; }
static void dirname_of(const char *p, char *out, int cap) { const char *b = basename_of(p); int n = (int)(b - p); if (n >= cap) n = cap - 1; memcpy(out, p, (u64)n); out[n] = 0; strip_trailing(out); if (!out[0]) scpy(out, cap, "."); }
static void join_path(char *out, int cap, const char *dir, const char *name) { if (!dir[0]) { scpy(out, cap, name); return; } scpy(out, cap, dir); int n = slen(out); if (n && out[n - 1] != '\\' && out[n - 1] != '/') scat(out, cap, "\\"); scat(out, cap, name); }
static int mkdir_p(const char *path) {
  char t[1024]; full_path(path, t, sizeof t); strip_trailing(t);
  if (is_dir(t)) return 1;
  for (char *p = t + 3; *p; p++) if (*p == '\\') { *p = 0; if (!is_dir(t)) CreateDirectoryA(t, 0); *p = '\\'; }
  if (!CreateDirectoryA(t, 0) && GetLastError() != 183) return 0; return 1;
}
static int wild_match(const char *pat, const char *s) {
  while (*pat) { if (*pat == '*') { pat++; if (!*pat) return 1; for (; *s; s++) if (wild_match(pat, s)) return 1; return wild_match(pat, s); } if (*pat == '?') { if (!*s) return 0; } else if (lc(*pat) != lc(*s)) return 0; pat++; s++; }
  return !*s;
}
typedef void (*walk_fn)(const char *path, const WIN32_FIND_DATAA *fd, void *ctx);
static void walk(const char *dir, const char *filter, int recurse, walk_fn fn, void *ctx) {
  char pat[1024]; join_path(pat, sizeof pat, dir, "*"); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID) return;
  do {
    if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue;
    char full[1024]; join_path(full, sizeof full, dir, fd.cFileName);
    if (!filter || wild_match(filter, fd.cFileName)) fn(full, &fd, ctx);
    if (recurse && (fd.dwFileAttributes & ATTR_DIR)) walk(full, filter, recurse, fn, ctx);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}
static void rm_cb(const char *path, const WIN32_FIND_DATAA *fd, void *ctx) { (void)ctx; if (fd->dwFileAttributes & ATTR_DIR) { walk(path, 0, 0, rm_cb, 0); RemoveDirectoryA(path); } else DeleteFileA(path); }
static int remove_tree(const char *path) { if (is_dir(path)) { walk(path, 0, 0, rm_cb, 0); return RemoveDirectoryA(path) != 0; } return DeleteFileA(path) != 0; }
static int copy_tree(const char *src, const char *dst);
typedef struct { const char *dst; int ok; } CopyCtx;
static void cp_cb(const char *path, const WIN32_FIND_DATAA *fd, void *ctx) { CopyCtx *c = ctx; char d[1024]; join_path(d, sizeof d, c->dst, fd->cFileName); if (!copy_tree(path, d)) c->ok = 0; }
static int copy_tree(const char *src, const char *dst) { if (is_dir(src)) { if (!mkdir_p(dst)) return 0; CopyCtx c = { dst, 1 }; walk(src, 0, 0, cp_cb, &c); return c.ok; } return CopyFileA(src, dst, 0) != 0; }

/* ------------------------------------------------------------------ inflate (RFC 1951) + CRC-32 ------------------------------------------------------------------ */
typedef struct { const BYTE *src; u64 srclen, pos; u32 bitbuf; int bitcnt; BYTE *dst; u64 dstcap, dstlen; int err; } Inf;
typedef struct { u16 count[16]; u16 symbol[320]; } Huff;
static u32 inf_bits(Inf *s, int n) { u32 v = 0; for (int i = 0; i < n; i++) { if (!s->bitcnt) { if (s->pos >= s->srclen) { s->err = 1; return 0; } s->bitbuf = s->src[s->pos++]; s->bitcnt = 8; } v |= (s->bitbuf & 1) << i; s->bitbuf >>= 1; s->bitcnt--; } return v; }
static void huff_build(Huff *h, const u16 *len, int n) { for (int i = 0; i < 16; i++) h->count[i] = 0; for (int i = 0; i < n; i++) h->count[len[i]]++; h->count[0] = 0; u16 offs[16]; offs[1] = 0; for (int i = 1; i < 15; i++) offs[i + 1] = (u16)(offs[i] + h->count[i]); for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (u16)i; }
static int huff_decode(Inf *s, const Huff *h) { int code = 0, first = 0, index = 0; for (int len = 1; len < 16; len++) { code |= (int)inf_bits(s, 1); if (s->err) return -1; int count = h->count[len]; if (code - count < first) return h->symbol[index + (code - first)]; index += count; first += count; first <<= 1; code <<= 1; } return -1; }
static const u16 LBASE[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u16 LEXT[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 DBASE[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u16 DEXT[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void inf_put(Inf *s, BYTE b) { if (s->dstlen < s->dstcap) s->dst[s->dstlen++] = b; else s->err = 2; }
static int inf_codes(Inf *s, const Huff *lit, const Huff *dist) {
  for (;;) {
    int sym = huff_decode(s, lit); if (sym < 0 || s->err) return 0;
    if (sym < 256) { inf_put(s, (BYTE)sym); continue; }
    if (sym == 256) return 1;
    sym -= 257; if (sym >= 29) return 0;
    int len = LBASE[sym] + (int)inf_bits(s, LEXT[sym]);
    int ds = huff_decode(s, dist); if (ds < 0 || ds >= 30) return 0;
    u64 d = DBASE[ds] + inf_bits(s, DEXT[ds]); if (d > s->dstlen) return 0;
    for (int i = 0; i < len; i++) inf_put(s, s->dst[s->dstlen - d]);
    if (s->err) return 0;
  }
}
static int inflate_buf(const BYTE *src, u64 srclen, BYTE *dst, u64 dstcap, u64 *outlen) {
  Inf s = { src, srclen, 0, 0, 0, dst, dstcap, 0, 0 }; static Huff lit, dist; u16 lengths[320];
  int last;
  do {
    last = (int)inf_bits(&s, 1); int type = (int)inf_bits(&s, 2); if (s.err) return 0;
    if (type == 0) { s.bitcnt = 0; if (s.pos + 4 > srclen) return 0; u32 len = src[s.pos] | (src[s.pos + 1] << 8); s.pos += 4; if (s.pos + len > srclen) return 0; for (u32 i = 0; i < len; i++) inf_put(&s, src[s.pos + i]); s.pos += len; }
    else if (type == 1) { int i = 0; for (; i < 144; i++) lengths[i] = 8; for (; i < 256; i++) lengths[i] = 9; for (; i < 280; i++) lengths[i] = 7; for (; i < 288; i++) lengths[i] = 8; huff_build(&lit, lengths, 288); for (i = 0; i < 30; i++) lengths[i] = 5; huff_build(&dist, lengths, 30); if (!inf_codes(&s, &lit, &dist)) return 0; }
    else if (type == 2) {
      int nlen = (int)inf_bits(&s, 5) + 257, ndist = (int)inf_bits(&s, 5) + 1, ncode = (int)inf_bits(&s, 4) + 4; if (nlen > 286 || ndist > 30) return 0;
      static const BYTE order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
      int i = 0; for (; i < ncode; i++) lengths[order[i]] = (u16)inf_bits(&s, 3); for (; i < 19; i++) lengths[order[i]] = 0;
      huff_build(&lit, lengths, 19);
      i = 0; while (i < nlen + ndist) { int sym = huff_decode(&s, &lit); if (sym < 0) return 0; if (sym < 16) lengths[i++] = (u16)sym; else { u16 len = 0; int rep; if (sym == 16) { if (!i) return 0; len = lengths[i - 1]; rep = 3 + (int)inf_bits(&s, 2); } else if (sym == 17) rep = 3 + (int)inf_bits(&s, 3); else rep = 11 + (int)inf_bits(&s, 7); if (i + rep > nlen + ndist) return 0; while (rep--) lengths[i++] = len; } }
      huff_build(&lit, lengths, nlen); huff_build(&dist, lengths + nlen, ndist);
      if (!inf_codes(&s, &lit, &dist)) return 0;
    } else return 0;
  } while (!last && !s.err);
  if (s.err) return 0; *outlen = s.dstlen; return 1;
}
static u32 g_crc_tab[256];
static u32 crc32_buf(const BYTE *p, u64 n) { if (!g_crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_tab[i] = c; } u32 c = 0xFFFFFFFFu; for (u64 i = 0; i < n; i++) c = g_crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
static u32 rd32le(const BYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u32 rd16le(const BYTE *p) { return p[0] | (p[1] << 8); }

/* ------------------------------------------------------------------ ZIP ------------------------------------------------------------------ */
static int zip_extract(const char *zip, const char *dest, int force, int verbose) {
  int zlen; char *zp = read_file(zip, &zlen); if (!zp) { errf("Expand-Archive : The path '%s' either does not exist or is not a valid file system path.\r\n", zip); return 0; }
  const BYTE *z = (const BYTE *)zp;
  s64 eocd = -1; for (s64 i = zlen - 22; i >= 0 && i >= zlen - 22 - 65536; i--) if (rd32le(z + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { errf("Expand-Archive : '%s' is not a valid zip archive (no end-of-central-directory record).\r\n", zip); xfree(zp); return 0; }
  u32 entries = rd16le(z + eocd + 10), cd_off = rd32le(z + eocd + 16);
  if (entries == 0xFFFF || cd_off == 0xFFFFFFFFu) { errf("Expand-Archive : '%s' uses ZIP64, which this helper does not support.\r\n", zip); xfree(zp); return 0; }
  char droot[1024]; full_path(dest, droot, sizeof droot); if (!mkdir_p(droot)) { errf("Expand-Archive : cannot create destination '%s'.\r\n", droot); xfree(zp); return 0; }
  u64 p = cd_off;
  for (u32 e = 0; e < entries; e++) {
    if (p + 46 > (u64)zlen || rd32le(z + p) != 0x02014b50) { errf("Expand-Archive : central directory of '%s' is corrupt.\r\n", zip); xfree(zp); return 0; }
    u32 method = rd16le(z + p + 10), crc = rd32le(z + p + 16), csize = rd32le(z + p + 20), usize = rd32le(z + p + 24), nlen = rd16le(z + p + 28), xlen = rd16le(z + p + 30), clen = rd16le(z + p + 32), loff = rd32le(z + p + 42);
    char name[1024]; int nn = nlen < 1023 ? (int)nlen : 1023; memcpy(name, z + p + 46, (u64)nn); name[nn] = 0; norm_slashes(name);
    p += 46 + nlen + xlen + clen;
    if (sstr(name, "..\\") || name[0] == '\\' || (name[1] == ':')) { errf("Expand-Archive : skipping unsafe entry '%s'.\r\n", name); continue; }
    char out[1024]; join_path(out, sizeof out, droot, name);
    int is_directory = nn && name[nn - 1] == '\\';
    if (is_directory) { mkdir_p(out); continue; }
    char parent[1024]; dirname_of(out, parent, sizeof parent); mkdir_p(parent);
    if (!force && exists(out)) { errf("Expand-Archive : '%s' already exists. Use -Force to overwrite.\r\n", out); xfree(zp); return 0; }
    if ((u64)loff + 30 > (u64)zlen || rd32le(z + loff) != 0x04034b50) { errf("Expand-Archive : local header of '%s' is corrupt.\r\n", name); xfree(zp); return 0; }
    u64 data = (u64)loff + 30 + rd16le(z + loff + 26) + rd16le(z + loff + 28);
    if (data + csize > (u64)zlen) { errf("Expand-Archive : '%s' is truncated.\r\n", name); xfree(zp); return 0; }
    BYTE *buf = xalloc(usize ? usize : 1); u64 got = 0; int ok = 0;
    if (method == 0) { if (csize == usize) { memcpy(buf, z + data, usize); got = usize; ok = 1; } }
    else if (method == 8) ok = inflate_buf(z + data, csize, buf, usize, &got) && got == usize;
    else errf("Expand-Archive : '%s' uses compression method %u (only deflate and stored are supported).\r\n", name, method);
    if (ok && crc32_buf(buf, got) != crc) { errf("Expand-Archive : CRC mismatch in '%s'.\r\n", name); ok = 0; }
    if (!ok) { if (method == 8 || method == 0) errf("Expand-Archive : failed to extract '%s'.\r\n", name); xfree(buf); xfree(zp); return 0; }
    if (!write_file(out, (const char *)buf, (int)got, 0)) { errf("Expand-Archive : cannot write '%s'.\r\n", out); xfree(buf); xfree(zp); return 0; }
    if (verbose) outf("VERBOSE: Created '%s'.\r\n", out);
    xfree(buf);
  }
  xfree(zp); return 1;
}
/* Compress-Archive: stored (method 0) entries — enough for scripts that bundle a few files */
typedef struct { Buf data, cd; u32 count; const char *root; int rootlen; } ZipW;
static void zipw_add_file(ZipW *w, const char *path, const char *name) {
  int n; char *d = read_file(path, &n); if (!d) return; u32 crc = crc32_buf((const BYTE *)d, (u64)n); u32 off = (u32)w->data.n; int nl = slen(name);
  BYTE h[30] = { 0x50, 0x4b, 3, 4, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; u32 v[3] = { crc, (u32)n, (u32)n };
  for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) h[14 + i * 4 + k] = (BYTE)(v[i] >> (8 * k)); h[26] = (BYTE)nl; h[27] = (BYTE)(nl >> 8);
  bput(&w->data, (const char *)h, 30); bput(&w->data, name, nl); bput(&w->data, d, n); xfree(d);
  BYTE c[46] = { 0x50, 0x4b, 1, 2, 20, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) c[16 + i * 4 + k] = (BYTE)(v[i] >> (8 * k)); c[28] = (BYTE)nl; c[29] = (BYTE)(nl >> 8); for (int k = 0; k < 4; k++) c[42 + k] = (BYTE)(off >> (8 * k));
  bput(&w->cd, (const char *)c, 46); bput(&w->cd, name, nl); w->count++;
}
static void zipw_cb(const char *path, const WIN32_FIND_DATAA *fd, void *ctx) { ZipW *w = ctx; char name[1024]; scpy(name, sizeof name, path + w->rootlen); for (char *p = name; *p; p++) if (*p == '\\') *p = '/'; if (fd->dwFileAttributes & ATTR_DIR) { walk(path, 0, 0, zipw_cb, w); } else zipw_add_file(w, path, name); }
static int zip_create(const char *src, const char *dest) {
  ZipW w = { { 0, 0, 0 }, { 0, 0, 0 }, 0, 0, 0 }; char full[1024]; full_path(src, full, sizeof full); strip_trailing(full);
  if (is_dir(full)) { char root[1024]; dirname_of(full, root, sizeof root); w.root = root; w.rootlen = slen(root) + 1; char tmp[1024]; scpy(tmp, sizeof tmp, full); WIN32_FIND_DATAA fd; fd.dwFileAttributes = ATTR_DIR; zipw_cb(tmp, &fd, &w); }
  else if (has_wild(full)) { char dir[1024]; dirname_of(full, dir, sizeof dir); w.root = dir; w.rootlen = slen(dir) + 1; walk(dir, basename_of(full), 0, zipw_cb, &w); }
  else { w.rootlen = (int)(basename_of(full) - full); zipw_add_file(&w, full, basename_of(full)); }
  u32 cdoff = (u32)w.data.n, cdsize = (u32)w.cd.n; bput(&w.data, bstr(&w.cd), w.cd.n);
  BYTE e[22] = { 0x50, 0x4b, 5, 6, 0, 0, 0, 0 }; e[8] = e[10] = (BYTE)w.count; e[9] = e[11] = (BYTE)(w.count >> 8); for (int k = 0; k < 4; k++) { e[12 + k] = (BYTE)(cdsize >> (8 * k)); e[16 + k] = (BYTE)(cdoff >> (8 * k)); }
  bput(&w.data, (const char *)e, 22);
  char out[1024]; full_path(dest, out, sizeof out); if (!sistr(basename_of(out), ".zip")) scat(out, sizeof out, ".zip");
  int ok = write_file(out, bstr(&w.data), w.data.n, 0); bfree(&w.data); bfree(&w.cd); return ok;
}

/* ------------------------------------------------------------------ SHA-256 (Get-FileHash) ------------------------------------------------------------------ */
static const u32 K256[64] = { 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(u32 h[8], const BYTE *block) {
  u32 w[64]; for (int k = 0; k < 16; k++) w[k] = ((u32)block[k * 4] << 24) | ((u32)block[k * 4 + 1] << 16) | ((u32)block[k * 4 + 2] << 8) | block[k * 4 + 3];
  for (int k = 16; k < 64; k++) { u32 s0 = ROR(w[k - 15], 7) ^ ROR(w[k - 15], 18) ^ (w[k - 15] >> 3), s1 = ROR(w[k - 2], 17) ^ ROR(w[k - 2], 19) ^ (w[k - 2] >> 10); w[k] = w[k - 16] + s0 + w[k - 7] + s1; }
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int k = 0; k < 64; k++) { u32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K256[k] + w[k], S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + maj; hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha256(const BYTE *msg, u64 len, BYTE out[32]) {
  u32 h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
  u64 i = 0; for (; i + 64 <= len; i += 64) sha256_block(h, msg + i);
  BYTE tail[128]; int n = (int)(len - i); memcpy(tail, msg + i, (u64)n); tail[n++] = 0x80; int total = n <= 56 ? 64 : 128; while (n < total - 8) tail[n++] = 0;
  u64 bits = len * 8; for (int k = 0; k < 8; k++) tail[total - 8 + k] = (BYTE)(bits >> (56 - 8 * k));
  sha256_block(h, tail); if (total == 128) sha256_block(h, tail + 64);
  for (int k = 0; k < 8; k++) { out[k * 4] = (BYTE)(h[k] >> 24); out[k * 4 + 1] = (BYTE)(h[k] >> 16); out[k * 4 + 2] = (BYTE)(h[k] >> 8); out[k * 4 + 3] = (BYTE)h[k]; }
}

/* ------------------------------------------------------------------ variables ------------------------------------------------------------------ */
typedef struct { char name[64]; char *val; } Var;
static Var g_vars[256]; static int g_nvars;
static char *g_pipeline_item;   /* $_ */
static const char *var_get(const char *name) {
  if (snieq(name, "env:", 4)) { static char ev[8192]; getenv_s(name + 4, ev, sizeof ev); return ev; }
  if (snieq(name, "global:", 7)) name += 7; else if (snieq(name, "script:", 7)) name += 7; else if (snieq(name, "local:", 6)) name += 6; else if (snieq(name, "private:", 8)) name += 8;
  if (seq(name, "_") || sieq(name, "PSItem")) return g_pipeline_item ? g_pipeline_item : "";
  if (sieq(name, "null")) return ""; if (sieq(name, "true")) return "True"; if (sieq(name, "false")) return "False";
  if (sieq(name, "LASTEXITCODE")) { static char b[16]; sfmt(b, sizeof b, "%d", g_last_exit); return b; }
  if (seq(name, "?")) return g_last_ok ? "True" : "False";
  if (sieq(name, "PWD")) { static char d[1024]; cwd(d, sizeof d); return d; }
  if (sieq(name, "HOME")) { static char h[1024]; getenv_s("USERPROFILE", h, sizeof h); return h; }
  if (sieq(name, "PSScriptRoot")) return g_script_root;
  if (sieq(name, "PSHOME")) return "C:\\Windows\\System32\\WindowsPowerShell\\v1.0";
  if (sieq(name, "PID")) { static char b[16]; sfmt(b, sizeof b, "%u", (u32)GetCurrentProcessId()); return b; }
  if (sieq(name, "IsWindows")) return "True"; if (sieq(name, "IsLinux") || sieq(name, "IsMacOS") || sieq(name, "IsCoreCLR")) return "False";
  if (sieq(name, "PSVersionTable")) return "Name                           Value\r\n----                           -----\r\nPSVersion                      5.1.19041.1 (nebula)\r\nPSEdition                      Desktop\r\nBuildVersion                   10.0.19041.1\r\nCLRVersion                     4.0.30319.42000\r\nPlatform                       Win32NT";
  if (sieq(name, "ErrorActionPreference")) { for (int i = 0; i < g_nvars; i++) if (sieq(g_vars[i].name, name)) return g_vars[i].val; return "Continue"; }
  if (sieq(name, "ProgressPreference")) { for (int i = 0; i < g_nvars; i++) if (sieq(g_vars[i].name, name)) return g_vars[i].val; return "Continue"; }
  for (int i = 0; i < g_nvars; i++) if (sieq(g_vars[i].name, name)) return g_vars[i].val;
  return "";
}
static void var_set(const char *name, const char *val) {
  if (snieq(name, "env:", 4)) { SetEnvironmentVariableA(name + 4, val[0] ? val : 0); return; }
  if (snieq(name, "global:", 7)) name += 7; else if (snieq(name, "script:", 7)) name += 7; else if (snieq(name, "local:", 6)) name += 6; else if (snieq(name, "private:", 8)) name += 8;
  for (int i = 0; i < g_nvars; i++) if (sieq(g_vars[i].name, name)) { xfree(g_vars[i].val); g_vars[i].val = sdup(val); return; }
  if (g_nvars < 256) { scpy(g_vars[g_nvars].name, 64, name); g_vars[g_nvars].val = sdup(val); g_nvars++; }
}

/* ------------------------------------------------------------------ tokenizer ------------------------------------------------------------------ */
enum { T_WORD = 1, T_STR1, T_STR2, T_VAR, T_SEMI, T_PIPE, T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_ASSIGN, T_ANDAND, T_OROR, T_REDIR, T_COMMA, T_EOF, T_BLOCK, T_SUBEXPR, T_ARRAY, T_DOT, T_STATIC };
typedef struct { int type; char *text; int line; } Tok;
typedef struct { Tok *t; int n, cap; } Toks;
static void tok_push(Toks *ts, int type, const char *s, int n, int line) { if (ts->n + 1 > ts->cap) { int nc = ts->cap ? ts->cap * 2 : 64; Tok *nt = xalloc((u64)nc * sizeof(Tok)); if (ts->t) { memcpy(nt, ts->t, (u64)ts->n * sizeof(Tok)); xfree(ts->t); } ts->t = nt; ts->cap = nc; } ts->t[ts->n].type = type; ts->t[ts->n].text = sdupn(s, n); ts->t[ts->n].line = line; ts->n++; }
static int is_word_char(char c) { return !is_space(c) && c != ';' && c != '|' && c != '{' && c != '}' && c != '(' && c != ')' && c != '"' && c != '\'' && c != ',' && c != '\n' && c != '#'; }
static void tokenize(const char *s, Toks *ts) {
  int line = 1, depth_paren = 0;
  while (*s) {
    if (*s == '\n') { line++; if (depth_paren == 0) tok_push(ts, T_SEMI, ";", 1, line); s++; continue; }
    if (is_space(*s)) { s++; continue; }
    if (*s == '`' && s[1] == '\r') { s += 2; continue; } if (*s == '`' && s[1] == '\n') { s += 2; line++; continue; }   /* line continuation */
    if (*s == '#') { while (*s && *s != '\n') s++; continue; }
    if (*s == '<' && s[1] == '#') { const char *e = sstr(s + 2, "#>"); s = e ? e + 2 : s + slen(s); continue; }
    if (*s == ';') { tok_push(ts, T_SEMI, ";", 1, line); s++; continue; }
    if (*s == ',') { tok_push(ts, T_COMMA, ",", 1, line); s++; continue; }
    if (*s == '|' && s[1] == '|') { tok_push(ts, T_OROR, "||", 2, line); s += 2; continue; }
    if (*s == '&' && s[1] == '&') { tok_push(ts, T_ANDAND, "&&", 2, line); s += 2; continue; }
    if (*s == '|') { tok_push(ts, T_PIPE, "|", 1, line); s++; continue; }
    if (*s == '{') { tok_push(ts, T_LBRACE, "{", 1, line); s++; continue; }
    if (*s == '}') { tok_push(ts, T_RBRACE, "}", 1, line); s++; continue; }
    if (*s == '@' && s[1] == '(') { tok_push(ts, T_ARRAY, "@(", 2, line); depth_paren++; s += 2; continue; }
    if (*s == '@' && s[1] == '{') { tok_push(ts, T_WORD, "@{", 2, line); s += 2; continue; }   /* hashtables: treated as plain words (unsupported) */
    if (*s == '$' && s[1] == '(') { tok_push(ts, T_SUBEXPR, "$(", 2, line); depth_paren++; s += 2; continue; }
    if (*s == '(') { tok_push(ts, T_LPAREN, "(", 1, line); depth_paren++; s++; continue; }
    if (*s == ')') { tok_push(ts, T_RPAREN, ")", 1, line); if (depth_paren) depth_paren--; s++;
      while (*s == '.' && s[1] >= 'A' && is_alnum(s[1])) { const char *m = s + 1; s++; while (is_alnum(*s)) s++; tok_push(ts, T_DOT, m, (int)(s - m), line); if (*s == '(') { /* method args: tokenized normally */ break; } }
      continue; }
    if (*s == '=' && s[1] != '=') { tok_push(ts, T_ASSIGN, "=", 1, line); s++; continue; }
    if ((*s == '+' || *s == '-' || *s == '*' || *s == '/') && s[1] == '=' ) { tok_push(ts, T_ASSIGN, s, 2, line); s += 2; continue; }
    if (*s == '>' || (*s == '2' && s[1] == '>') || (*s == '1' && s[1] == '>') || (*s == '*' && s[1] == '>')) {
      const char *b = s; if (*s != '>') s++; s++; if (*s == '>') s++; if (*s == '&' && (s[1] == '1' || s[1] == '2')) s += 2; tok_push(ts, T_REDIR, b, (int)(s - b), line); continue; }
    if (*s == '\'') { s++; Buf b = { 0, 0, 0 }; while (*s) { if (*s == '\'' && s[1] == '\'') { bputc(&b, '\''); s += 2; continue; } if (*s == '\'') break; if (*s == '\n') line++; bputc(&b, *s++); } if (*s) s++; tok_push(ts, T_STR1, bstr(&b), b.n, line); bfree(&b); continue; }
    if (*s == '"') { s++; const char *b = s; int n = 0; while (s[n]) { if (s[n] == '`' && s[n + 1]) { n += 2; continue; } if (s[n] == '"' && s[n + 1] == '"') { n += 2; continue; } if (s[n] == '"') break; if (s[n] == '\n') line++; n++; } tok_push(ts, T_STR2, b, n, line); s += n; if (*s) s++; continue; }
    if (*s == '@' && (s[1] == '"' || s[1] == '\'')) { char q = s[1]; s += 2; while (*s == '\r' || *s == '\n') s++; const char *b = s; int n = 0; while (s[n] && !((s[n] == '\n') && s[n + 1] == q && s[n + 2] == '@') && !(s[n] == '\n' && s[n + 1] == '\r' && s[n + 2] == q)) n++; int m = n; while (m && (b[m - 1] == '\r' || b[m - 1] == '\n')) m--; tok_push(ts, q == '"' ? T_STR2 : T_STR1, b, m, line); s += n; while (*s && *s != '@') s++; if (*s) s++; continue; }
    if (*s == '[' ) { /* [Type]::Member or [type] cast */ const char *b = s; while (*s && *s != ']') s++; if (*s) s++; if (s[0] == ':' && s[1] == ':') { s += 2; while (is_alnum(*s)) s++; tok_push(ts, T_STATIC, b, (int)(s - b), line); } else { /* cast like [string] / [int] / [switch] — ignore */ } continue; }
    if (*s == '$') { const char *b = s; s++; if (*s == '{') { s++; while (*s && *s != '}') s++; if (*s) s++; char nm[256]; int n = (int)(s - b - 3); if (n > 255) n = 255; memcpy(nm, b + 2, (u64)n); nm[n] = 0; tok_push(ts, T_VAR, nm, n, line); }
      else { while (is_alnum(*s) || *s == ':' || *s == '?' ) { if (*s == ':' && !is_alnum(s[1])) break; s++; } tok_push(ts, T_VAR, b + 1, (int)(s - b - 1), line); }
      if (*s == '.' && is_alnum(s[1]) && s[1] >= 'A') { /* member access: .Name / .Method(args) */ const char *m = s; s++; while (is_alnum(*s)) s++; tok_push(ts, T_DOT, m + 1, (int)(s - m - 1), line); }
      continue; }
    { const char *b = s; while (*s && is_word_char(*s)) { if (*s == '$' && s > b) break; if (*s == '`' && s[1]) s++; s++; } if (s == b) s++; tok_push(ts, T_WORD, b, (int)(s - b), line); }
  }
  tok_push(ts, T_EOF, "", 0, line);
}

/* ------------------------------------------------------------------ evaluation ------------------------------------------------------------------ */
static int exec_range(Toks *ts, int from, int to);
static char *eval_pipeline_capture(Toks *ts, int from, int to);
static char *expand_dq(const char *s);
static char *apply_members(Toks *ts, int *ip, int to, char *v);
/* value of a single token (as a freshly allocated string) */
static char *tok_value(Toks *ts, int *ip, int to) {
  Tok *t = &ts->t[*ip]; char *v;
  if (t->type == T_STR1) v = sdup(t->text);
  else if (t->type == T_STR2) v = expand_dq(t->text);
  else if (t->type == T_VAR) { v = sdup(var_get(t->text)); v = apply_members(ts, ip, to, v); }
  else if (t->type == T_SUBEXPR || t->type == T_LPAREN || t->type == T_ARRAY) {
    int depth = 0, j = *ip; for (; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RPAREN && --depth == 0) break; }
    v = eval_pipeline_capture(ts, *ip + 1, j); *ip = j; int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
    v = apply_members(ts, ip, to, v);
  }
  else if (t->type == T_WORD) {
    const char *w = t->text; Buf b = { 0, 0, 0 };
    for (const char *p = w; *p; p++) { if (*p == '`' && p[1]) { p++; bputc(&b, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p); } else bputc(&b, *p); }
    v = sdup(bstr(&b)); bfree(&b);
  }
  else v = sdup(t->text);
  /* string concatenation with + : "a" + $b */
  while (*ip + 2 < to && ts->t[*ip + 1].type == T_WORD && seq(ts->t[*ip + 1].text, "+")) {
    int j = *ip + 2; char *r = tok_value(ts, &j, to); *ip = j;
    if (is_number(v) && is_number(r)) { s64 x = satoi(v, 0) + satoi(r, 0); xfree(v); v = xalloc(32); sfmt(v, 32, "%L", x); xfree(r); }
    else { Buf b = { 0, 0, 0 }; bput(&b, v, -1); bput(&b, r, -1); xfree(v); xfree(r); v = sdup(bstr(&b)); bfree(&b); }
  }
  return v;
}
/* .Member / .Method(args) chains applied to a string value */
static char *apply_members(Toks *ts, int *ip, int to, char *v) {
    while (*ip + 1 < to && ts->t[*ip + 1].type == T_DOT) {   /* .Member or .Method(args) */
      (*ip)++; const char *m = ts->t[*ip].text; char *args[4] = { 0, 0, 0, 0 }; int na = 0;
      if (*ip + 1 < to && ts->t[*ip + 1].type == T_LPAREN) { int j = *ip + 2; while (j < to && ts->t[j].type != T_RPAREN) { if (ts->t[j].type == T_COMMA) { j++; continue; } if (na < 4) args[na++] = tok_value(ts, &j, to); j++; } *ip = j; }
      char *nv = 0; int n = slen(v);
      if ((sieq(m, "Count") || sieq(m, "Lines") || sieq(m, "Sum") || sieq(m, "Characters") || sieq(m, "Words")) && is_number(v)) nv = sdup(v);   /* result of Measure-Object */
      else if (sieq(m, "Count") || (sieq(m, "Length") && sstr(v, "\n"))) { int lines = 0; FOR_LINES(v, p, e, k) (void)k; lines++; END_LINES(p, e) nv = xalloc(16); sfmt(nv, 16, "%d", lines); }   /* arrays are newline lists */
      else if (sieq(m, "Length")) { nv = xalloc(16); sfmt(nv, 16, "%d", n); }
      else if (sieq(m, "Trim")) { int a = 0, b = n; while (a < b && is_space(v[a])) a++; while (b > a && is_space(v[b - 1])) b--; nv = sdupn(v + a, b - a); }
      else if (sieq(m, "TrimEnd")) { int b = n; while (b > 0 && (is_space(v[b - 1]) || (args[0] && args[0][0] && v[b - 1] == args[0][0]))) b--; nv = sdupn(v, b); }
      else if (sieq(m, "TrimStart")) { int a = 0; while (a < n && (is_space(v[a]) || (args[0] && args[0][0] && v[a] == args[0][0]))) a++; nv = sdup(v + a); }
      else if (sieq(m, "ToLower") || sieq(m, "ToLowerInvariant")) { nv = sdup(v); for (char *p = nv; *p; p++) *p = lc(*p); }
      else if (sieq(m, "ToUpper") || sieq(m, "ToUpperInvariant")) { nv = sdup(v); for (char *p = nv; *p; p++) *p = uc(*p); }
      else if (sieq(m, "ToString")) nv = sdup(v);
      else if (sieq(m, "Replace") && args[0]) { Buf b = { 0, 0, 0 }; int ln = slen(args[0]); for (const char *p = v; *p;) { if (ln && !memcmp(p, args[0], (u64)ln)) { bput(&b, args[1] ? args[1] : "", -1); p += ln; } else bputc(&b, *p++); } nv = sdup(bstr(&b)); bfree(&b); }
      else if (sieq(m, "StartsWith") && args[0]) nv = sdup(snieq(v, args[0], slen(args[0])) && slen(v) >= slen(args[0]) ? "True" : "False");
      else if (sieq(m, "EndsWith") && args[0]) { int la = slen(args[0]); nv = sdup(n >= la && sieq(v + n - la, args[0]) ? "True" : "False"); }
      else if (sieq(m, "Contains") && args[0]) nv = sdup(sstr(v, args[0]) ? "True" : "False");
      else if (sieq(m, "IndexOf") && args[0]) { const char *f = sstr(v, args[0]); nv = xalloc(16); sfmt(nv, 16, "%d", f ? (int)(f - v) : -1); }
      else if (sieq(m, "Substring") && args[0]) { int a = (int)satoi(args[0], 0); if (a > n) a = n; int l = args[1] ? (int)satoi(args[1], 0) : n - a; if (a + l > n) l = n - a; nv = sdupn(v + a, l); }
      else if (sieq(m, "Split")) { char sep = args[0] && args[0][0] ? args[0][0] : ' '; nv = sdup(v); for (char *p = nv; *p; p++) if (*p == sep) *p = '\n'; }
      else if (sieq(m, "PadLeft") || sieq(m, "PadRight")) { int w = args[0] ? (int)satoi(args[0], 0) : 0; char pc = args[1] && args[1][0] ? args[1][0] : ' '; Buf b = { 0, 0, 0 }; if (sieq(m, "PadLeft")) for (int k = n; k < w; k++) bputc(&b, pc); bput(&b, v, n); if (sieq(m, "PadRight")) for (int k = n; k < w; k++) bputc(&b, pc); nv = sdup(bstr(&b)); bfree(&b); }
      else if (sieq(m, "FullName") || sieq(m, "Path") || sieq(m, "Name") || sieq(m, "BaseName") || sieq(m, "Directory") || sieq(m, "DirectoryName") || sieq(m, "Extension")) {
        char full[1024]; full_path(v, full, sizeof full);
        if (sieq(m, "Name")) nv = sdup(basename_of(full)); else if (sieq(m, "Extension")) { const char *b = basename_of(full); const char *d = 0; for (const char *p = b; *p; p++) if (*p == '.') d = p; nv = sdup(d ? d : ""); }
        else if (sieq(m, "BaseName")) { const char *b = basename_of(full); const char *d = 0; for (const char *p = b; *p; p++) if (*p == '.') d = p; nv = sdupn(b, d ? (int)(d - b) : slen(b)); }
        else if (sieq(m, "Directory") || sieq(m, "DirectoryName")) { char d[1024]; dirname_of(full, d, sizeof d); nv = sdup(d); } else nv = sdup(full); }
      else if (sieq(m, "Exists")) nv = sdup(exists(v) ? "True" : "False");
      else if (sieq(m, "Value") || sieq(m, "Line") || sieq(m, "Matches") || sieq(m, "Hash") || sieq(m, "Content") || sieq(m, "Message")) nv = sdup(v);
      else { errf("Method invocation failed: [string] does not contain a method named '%s'.\r\n", m); nv = sdup(v); }
      for (int k = 0; k < na; k++) xfree(args[k]); xfree(v); v = nv;
    }
  return v;
}
/* "double-quoted" expansion: $var, ${var}, $env:X, $(subexpr), `escapes */
static char *expand_dq(const char *s) {
  Buf b = { 0, 0, 0 };
  for (const char *p = s; *p;) {
    if (*p == '`' && p[1]) { p++; char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == '0' ? 0 : *p; if (c) bputc(&b, c); p++; continue; }
    if (*p == '"' && p[1] == '"') { bputc(&b, '"'); p += 2; continue; }
    if (*p == '$' && p[1] == '(') { int depth = 0; const char *q = p + 1; for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')' && --depth == 0) break; } char *inner = sdupn(p + 2, (int)(q - p - 2)); Toks ts = { 0, 0, 0 }; tokenize(inner, &ts); char *v = eval_pipeline_capture(&ts, 0, ts.n - 1); int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0; bput(&b, v, -1); xfree(v); xfree(inner); p = *q ? q + 1 : q; continue; }
    if (*p == '$' && p[1] == '{') { const char *q = p + 2; while (*q && *q != '}') q++; char nm[256]; int n = (int)(q - p - 2); if (n > 255) n = 255; memcpy(nm, p + 2, (u64)n); nm[n] = 0; bput(&b, var_get(nm), -1); p = *q ? q + 1 : q; continue; }
    if (*p == '$' && (is_alnum(p[1]) || p[1] == '_' || p[1] == '?')) { const char *q = p + 1; while (is_alnum(*q) || (*q == ':' && is_alnum(q[1])) || *q == '?') q++; char nm[256]; int n = (int)(q - p - 1); if (n > 255) n = 255; memcpy(nm, p + 1, (u64)n); nm[n] = 0; const char *v = var_get(nm);
      if (*q == '.' && q[1] >= 'A' && is_alnum(q[1])) { /* $x.Name inside a string */ const char *m = q + 1; while (is_alnum(*m)) m++; char mem[64]; int ml = (int)(m - q - 1); if (ml > 63) ml = 63; memcpy(mem, q + 1, (u64)ml); mem[ml] = 0; if (sieq(mem, "Length") || sieq(mem, "Count")) { char t[16]; sfmt(t, 16, "%d", slen(v)); bput(&b, t, -1); q = m; } else if (sieq(mem, "Name") || sieq(mem, "FullName")) { char f[1024]; full_path(v, f, sizeof f); bput(&b, sieq(mem, "Name") ? basename_of(f) : f, -1); q = m; } else bput(&b, v, -1); }
      else bput(&b, v, -1); p = q; continue; }
    bputc(&b, *p++);
  }
  char *r = sdup(bstr(&b)); bfree(&b); return r;
}
static int truthy(const char *v) { if (!v || !*v) return 0; if (sieq(v, "False") || seq(v, "0")) return 0; return 1; }
static int regex_lite(const char *pat, const char *s);
/* comparison / logical expression over a token range → "True"/"False" or a value */
static char *eval_expr(Toks *ts, int from, int to) {
  if (from >= to) return sdup("");
  /* a pipeline (top-level |) is evaluated as a whole and its output captured */
  { int depth = 0; for (int i = from; i < to; i++) { int ty = ts->t[i].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY || ty == T_LBRACE) depth++; else if (ty == T_RPAREN || ty == T_RBRACE) depth--; else if (!depth && ty == T_PIPE) { char *v = eval_pipeline_capture(ts, from, to); int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0; return v; } } }
  /* -and / -or at top level (lowest precedence) */
  int depth = 0;
  for (int i = from; i < to; i++) { int ty = ts->t[i].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY || ty == T_LBRACE) depth++; else if (ty == T_RPAREN || ty == T_RBRACE) depth--; else if (!depth && ty == T_WORD && (sieq(ts->t[i].text, "-or") || sieq(ts->t[i].text, "-and"))) { char *l = eval_expr(ts, from, i); int lv = truthy(l); xfree(l); int is_or = sieq(ts->t[i].text, "-or"); if (is_or ? lv : !lv) return sdup(lv ? "True" : "False"); char *r = eval_expr(ts, i + 1, to); int rv = truthy(r); xfree(r); return sdup(rv ? "True" : "False"); } }
  if (ts->t[from].type == T_WORD && (seq(ts->t[from].text, "!") || sieq(ts->t[from].text, "-not"))) { char *r = eval_expr(ts, from + 1, to); int v = truthy(r); xfree(r); return sdup(v ? "False" : "True"); }
  if (ts->t[from].type == T_WORD && ts->t[from].text[0] == '!' && ts->t[from].text[1]) { /* !$x tokenized together only for ( … ); handle "!(" */ }
  /* binary comparison */
  depth = 0;
  for (int i = from; i < to; i++) { int ty = ts->t[i].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY || ty == T_LBRACE) depth++; else if (ty == T_RPAREN || ty == T_RBRACE) depth--; else if (!depth && ty == T_WORD && ts->t[i].text[0] == '-' && i > from) {
      const char *op = ts->t[i].text + 1; if (*op == 'i' || *op == 'c') { if (sieq(op + 1, "eq") || sieq(op + 1, "ne") || sieq(op + 1, "like") || sieq(op + 1, "notlike") || sieq(op + 1, "match") || sieq(op + 1, "notmatch") || sieq(op + 1, "contains") || sieq(op + 1, "notcontains") || sieq(op + 1, "gt") || sieq(op + 1, "lt") || sieq(op + 1, "ge") || sieq(op + 1, "le")) op++; }
      int known = sieq(op, "eq") || sieq(op, "ne") || sieq(op, "like") || sieq(op, "notlike") || sieq(op, "match") || sieq(op, "notmatch") || sieq(op, "gt") || sieq(op, "lt") || sieq(op, "ge") || sieq(op, "le") || sieq(op, "contains") || sieq(op, "notcontains") || sieq(op, "in") || sieq(op, "notin") || sieq(op, "is") || sieq(op, "isnot");
      if (!known) continue;
      char *l = eval_expr(ts, from, i), *r = eval_expr(ts, i + 1, to); int res = 0;
      if (sieq(op, "eq") || sieq(op, "ne")) { res = is_number(l) && is_number(r) ? satoi(l, 0) == satoi(r, 0) : sieq(l, r); if (sieq(op, "ne")) res = !res; }
      else if (sieq(op, "like") || sieq(op, "notlike")) { res = wild_match(r, l); if (sieq(op, "notlike")) res = !res; }
      else if (sieq(op, "match") || sieq(op, "notmatch")) { res = regex_lite(r, l); if (sieq(op, "notmatch")) res = !res; }
      else if (sieq(op, "contains") || sieq(op, "notcontains") || sieq(op, "in") || sieq(op, "notin")) { const char *list = sieq(op, "in") || sieq(op, "notin") ? r : l, *item = list == r ? l : r; res = 0; for (const char *p = list; *p;) { const char *e = p; while (*e && *e != '\n') e++; int n = (int)(e - p); if (n && p[n - 1] == '\r') n--; if (n == slen(item) && snieq(p, item, n)) { res = 1; break; } p = *e ? e + 1 : e; } if (sieq(op, "notcontains") || sieq(op, "notin")) res = !res; }
      else if (sieq(op, "is") || sieq(op, "isnot")) res = sieq(op, "is");
      else { s64 a = satoi(l, 0), b = satoi(r, 0); int c = is_number(l) && is_number(r) ? (a < b ? -1 : a > b) : sicmp(l, r); res = sieq(op, "gt") ? c > 0 : sieq(op, "lt") ? c < 0 : sieq(op, "ge") ? c >= 0 : c <= 0; }
      xfree(l); xfree(r); return sdup(res ? "True" : "False"); } }
  /* -join / -split / -replace / -f */
  depth = 0;
  for (int i = from; i < to; i++) { int ty = ts->t[i].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY || ty == T_LBRACE) depth++; else if (ty == T_RPAREN || ty == T_RBRACE) depth--; else if (!depth && ty == T_WORD && (sieq(ts->t[i].text, "-join") || sieq(ts->t[i].text, "-split") || sieq(ts->t[i].text, "-replace") || sieq(ts->t[i].text, "-f"))) {
      char *l = eval_expr(ts, from, i), *r = eval_expr(ts, i + 1, to); Buf b = { 0, 0, 0 };
      if (sieq(ts->t[i].text, "-join")) { for (const char *p = l; *p;) { const char *e = p; while (*e && *e != '\n') e++; int n = (int)(e - p); if (n && p[n - 1] == '\r') n--; if (p != l) bput(&b, r, -1); bput(&b, p, n); p = *e ? e + 1 : e; } }
      else if (sieq(ts->t[i].text, "-split")) { int ln = slen(r); for (const char *p = l; *p;) { if (ln && !memcmp(p, r, (u64)ln)) { bputc(&b, '\n'); p += ln; } else bputc(&b, *p++); } }
      else if (sieq(ts->t[i].text, "-replace")) { char *pat = r, *rep = ""; for (char *p = r; *p; p++) if (*p == '\n') { *p = 0; rep = p + 1; break; } int ln = slen(pat); for (const char *p = l; *p;) { if (ln && snieq(p, pat, ln)) { bput(&b, rep, -1); p += ln; } else bputc(&b, *p++); } }
      else { /* -f */ char *args[16]; int na = 0; for (char *p = r; *p && na < 16;) { args[na++] = p; while (*p && *p != '\n') p++; if (*p) *p++ = 0; } for (const char *p = l; *p;) { if (*p == '{' && is_digit(p[1])) { int k = (int)satoi(p + 1, &p); while (*p && *p != '}') p++; if (*p) p++; if (k < na) bput(&b, args[k], -1); } else bputc(&b, *p++); } }
      xfree(l); xfree(r); char *res = sdup(bstr(&b)); bfree(&b); return res; } }
  /* comma lists → newline-separated arrays */
  depth = 0; int has_comma = 0;
  for (int i = from; i < to; i++) { int ty = ts->t[i].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RPAREN) depth--; else if (!depth && ty == T_COMMA) has_comma = 1; }
  if (has_comma) { Buf b = { 0, 0, 0 }; int start = from; depth = 0; for (int i = from; i <= to; i++) { int ty = i < to ? ts->t[i].type : T_COMMA; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RPAREN) depth--; else if (!depth && ty == T_COMMA) { char *v = eval_expr(ts, start, i); if (b.n) bputc(&b, '\n'); bput(&b, v, -1); xfree(v); start = i + 1; } } char *res = sdup(bstr(&b)); bfree(&b); return res; }
  /* single value or arithmetic */
  int j = from; char *v = tok_value(ts, &j, to);
  if (j + 1 < to && ts->t[j + 1].type == T_WORD && (seq(ts->t[j + 1].text, "-") || seq(ts->t[j + 1].text, "*") || seq(ts->t[j + 1].text, "/") || seq(ts->t[j + 1].text, "%")) ) { int k = j + 2; char *r = tok_value(ts, &k, to); s64 a = satoi(v, 0), b = satoi(r, 0), x = seq(ts->t[j + 1].text, "-") ? a - b : seq(ts->t[j + 1].text, "*") ? a * b : b ? (seq(ts->t[j + 1].text, "/") ? a / b : a % b) : 0; xfree(v); xfree(r); v = xalloc(32); sfmt(v, 32, "%L", x); j = k; }
  if (j + 1 < to && (ts->t[from].type == T_WORD || ts->t[from].type == T_STATIC)) { xfree(v); v = eval_pipeline_capture(ts, from, to); int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0; return v; }   /* a command: capture its output */
  if (j + 1 < to) { /* bare list of values: join with newlines */ Buf b = { 0, 0, 0 }; bput(&b, v, -1); for (int k = j + 1; k < to; k++) { char *w = tok_value(ts, &k, to); bputc(&b, '\n'); bput(&b, w, -1); xfree(w); } xfree(v); v = sdup(bstr(&b)); bfree(&b); }
  return v;
}
/* minimal regex: ^ $ . * + ? [] \d \w \s \. and literals; good enough for the -match uses in install scripts */
static int re_match_here(const char *re, const char *s);
static int re_class(const char **rep, char c, int *matched) { const char *re = *rep; if (*re == '\\') { char k = re[1]; *rep = re + 2; if (k == 'd') *matched = is_digit(c); else if (k == 'w') *matched = is_alnum(c); else if (k == 's') *matched = is_space(c); else if (k == 'D') *matched = !is_digit(c); else if (k == 'W') *matched = !is_alnum(c); else if (k == 'S') *matched = !is_space(c); else *matched = lc(c) == lc(k); return 1; }
  if (*re == '[') { re++; int neg = 0; if (*re == '^') { neg = 1; re++; } int m = 0; while (*re && *re != ']') { if (re[1] == '-' && re[2] && re[2] != ']') { if (lc(c) >= lc(re[0]) && lc(c) <= lc(re[2])) m = 1; re += 3; } else { if (*re == '\\' && re[1]) re++; if (lc(*re) == lc(c)) m = 1; re++; } } if (*re) re++; *rep = re; *matched = neg ? !m : m; return 1; }
  if (*re == '.') { *rep = re + 1; *matched = c != 0; return 1; }
  if (*re == '(' || *re == ')') { *rep = re + 1; *matched = -1; return 1; }   /* groups: ignored (transparent) */
  *rep = re + 1; *matched = lc(*re) == lc(c); return 1; }
static int re_match_here(const char *re, const char *s) {
  if (!*re) return 1; if (*re == '$' && !re[1]) return *s == 0;
  if (*re == '(' || *re == ')') return re_match_here(re + 1, s);
  if (*re == '|') return 1;
  const char *next = re; int m; re_class(&next, *s, &m);
  if (*next == '*' || *next == '+' || *next == '?') { char q = *next; const char *after = next + 1; if (*after == '?') after++; int count = 0; const char *p = s; if (q == '+') { int mm; const char *tmp = re; re_class(&tmp, *p, &mm); if (!*p || !mm) return 0; p++; count = 1; }
    for (;;) { if (re_match_here(after, p)) return 1; if (q == '?' && count >= 1) return 0; if (!*p) return 0; int mm; const char *tmp = re; re_class(&tmp, *p, &mm); if (!mm) return 0; p++; count++; } }
  if (m == -1) return re_match_here(next, s);
  if (*s && m) return re_match_here(next, s + 1);
  return 0;
}
static int regex_lite(const char *pat, const char *s) {
  /* alternation at top level */
  for (const char *p = pat; *p; p++) if (*p == '|') { char *l = sdupn(pat, (int)(p - pat)); int r = regex_lite(l, s) || regex_lite(p + 1, s); xfree(l); return r; }
  if (*pat == '^') return re_match_here(pat + 1, s);
  for (;; s++) { if (re_match_here(pat, s)) return 1; if (!*s) return 0; }
}

/* ------------------------------------------------------------------ commands ------------------------------------------------------------------ */
typedef struct { char *name; char **argv; int argc; int *is_param; char *input; int in_len; } Cmd;   /* argv: expanded strings; is_param[i]: 1 if "-Name" */
static const char *cmd_opt(Cmd *c, const char *name) { for (int i = 0; i + 1 < c->argc; i++) if (c->is_param[i] && sieq(c->argv[i] + 1, name)) return c->is_param[i + 1] ? "" : c->argv[i + 1]; return 0; }
static int cmd_flag(Cmd *c, const char *name) { for (int i = 0; i < c->argc; i++) if (c->is_param[i] && (sieq(c->argv[i] + 1, name) || (snieq(c->argv[i] + 1, name, slen(c->argv[i]) - 1) && slen(c->argv[i]) > 3))) return 1; return 0; }
static const char *cmd_pos(Cmd *c, int k) { int n = 0; for (int i = 0; i < c->argc; i++) { if (c->is_param[i]) { if (i + 1 < c->argc && !c->is_param[i + 1] && !sieq(c->argv[i], "-Recurse") && !sieq(c->argv[i], "-Force") && !sieq(c->argv[i], "-ErrorAction") && !sieq(c->argv[i], "-r") && !sieq(c->argv[i], "-rf") && !sieq(c->argv[i], "-Confirm") && !sieq(c->argv[i], "-WhatIf") && !sieq(c->argv[i], "-Wait") && !sieq(c->argv[i], "-NoNewWindow") && !sieq(c->argv[i], "-PassThru") && !sieq(c->argv[i], "-Raw") && !sieq(c->argv[i], "-Quiet") && !sieq(c->argv[i], "-Directory") && !sieq(c->argv[i], "-File") && !sieq(c->argv[i], "-Name") && !sieq(c->argv[i], "-Unique") && !sieq(c->argv[i], "-Descending") && !sieq(c->argv[i], "-NoTypeInformation") && !sieq(c->argv[i], "-Append") && !sieq(c->argv[i], "-NoNewline") && !sieq(c->argv[i], "-NoEnumerate") && !sieq(c->argv[i], "-Verbose") && !sieq(c->argv[i], "-Compress") && !sieq(c->argv[i], "-Leaf") && !sieq(c->argv[i], "-Parent") && !sieq(c->argv[i], "-Qualifier") && !sieq(c->argv[i], "-Extension") && !sieq(c->argv[i], "-LeafBase") && !sieq(c->argv[i], "-SimpleMatch") && !sieq(c->argv[i], "-CaseSensitive") && !sieq(c->argv[i], "-Hidden") && !sieq(c->argv[i], "-Stream") && !sieq(c->argv[i], "-UseBasicParsing") && !sieq(c->argv[i], "-NoProfile") && !sieq(c->argv[i], "-AsHashtable") && !sieq(c->argv[i], "-Confirm:$false") && !sieq(c->argv[i], "-Recurse:$true") && !sieq(c->argv[i], "-Force:$true") && !sieq(c->argv[i], "-Container")) i++; continue; } if (n++ == k) return c->argv[i]; } return 0; }
static void emit_lines(const char *s) { for (const char *p = s; *p;) { const char *e = p; while (*e && *e != '\n') e++; int n = (int)(e - p); if (n && p[n - 1] == '\r') n--; outn(p, n); outn("\r\n", 2); p = *e ? e + 1 : e; } }
typedef struct { Buf *b; int names_only, dirs_only, files_only; const char *root; } LsCtx;
static void ls_cb(const char *path, const WIN32_FIND_DATAA *fd, void *ctx) { LsCtx *c = ctx; int d = (fd->dwFileAttributes & ATTR_DIR) != 0; if ((c->dirs_only && !d) || (c->files_only && d)) return; if (c->names_only) { const char *rel = fd->cFileName; if (c->root) { u64 rl = slen(c->root); if (snieq(path, c->root, (int)rl) && (path[rl] == '\\' || path[rl] == '/')) rel = path + rl + 1; } bput(c->b, rel, -1); bputc(c->b, '\n'); return; } char line[1200]; u64 size = ((u64)fd->nFileSizeHigh << 32) | fd->nFileSizeLow; SYSTEMTIME st; FILETIME lt; FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st); char sz[24]; if (d) scpy(sz, sizeof sz, ""); else sfmt(sz, sizeof sz, "%L", (s64)size); sfmt(line, sizeof line, "%s%s   %02d.%02d.%04d  %02d:%02d %14s %s\n", d ? "d" : "-", "a---", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, sz, path); bput(c->b, line, -1); }
static int find_on_path(const char *name, char *out, int cap) {
  if (name[0] == '.' || name[1] == ':' || name[0] == '\\' || name[0] == '/' || sstr(name, "\\") || sstr(name, "/")) { full_path(name, out, cap); if (exists(out)) return 1; char t[1024]; sfmt(t, sizeof t, "%s.exe", out); if (exists(t)) { scpy(out, cap, t); return 1; } return 0; }
  static const char *exts[] = { "", ".exe", ".com", ".bat", ".cmd", ".ps1", 0 }; char path[8192]; char d[1024]; cwd(d, sizeof d); getenv_s("PATH", path, sizeof path);
  char dirs[8192 + 1024]; sfmt(dirs, sizeof dirs, "%s;%s", d, path);
  for (char *p = dirs; *p;) { char *e = p; while (*e && *e != ';') e++; char dir[1024]; int n = (int)(e - p); if (n > 1023) n = 1023; memcpy(dir, p, (u64)n); dir[n] = 0; if (n) for (int k = 0; exts[k]; k++) { char cand[1200]; join_path(cand, sizeof cand, dir, name); scat(cand, sizeof cand, exts[k]); if (exists(cand) && !is_dir(cand)) { scpy(out, cap, cand); return 1; } } p = *e ? e + 1 : e; }
  return 0;
}
static void quote_arg(Buf *b, const char *a) { int need = !*a; for (const char *p = a; *p; p++) if (is_space(*p) || *p == '"') need = 1; if (!need) { bput(b, a, -1); return; } bputc(b, '"'); for (const char *p = a; *p; p++) { if (*p == '"') bput(b, "\\\"", 2); else bputc(b, *p); } bputc(b, '"'); }
/* run an external program; stdout is captured when g_capture is set; returns exit code */
static int run_external(const char *exe, char **argv, int argc, const char *input, int in_len) {
  Buf cl = { 0, 0, 0 }; quote_arg(&cl, exe); for (int i = 0; i < argc; i++) { bputc(&cl, ' '); quote_arg(&cl, argv[i]); }
  if (sistr(basename_of(exe), ".ps1")) { /* nested script */ Buf cl2 = { 0, 0, 0 }; quote_arg(&cl2, g_self); bput(&cl2, " -NoProfile -File ", -1); bput(&cl2, bstr(&cl), -1); bfree(&cl); cl = cl2; exe = g_self; }
  else if (sistr(basename_of(exe), ".bat") || sistr(basename_of(exe), ".cmd")) { Buf cl2 = { 0, 0, 0 }; bput(&cl2, "C:\\Windows\\System32\\cmd.exe /c ", -1); bput(&cl2, bstr(&cl), -1); bfree(&cl); cl = cl2; exe = "C:\\Windows\\System32\\cmd.exe"; }
  SECURITY_ATTRIBUTES sa = { sizeof sa, 0, 1 }; HANDLE or_ = 0, ow = 0, ir = 0, iw = 0;
  if (g_capture) { CreatePipe(&or_, &ow, &sa, 0); SetHandleInformation(or_, 1, 0); }
  if (input) { CreatePipe(&ir, &iw, &sa, 0); SetHandleInformation(iw, 1, 0); }
  STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = 0x100; si.hStdInput = input ? ir : g_in; si.hStdOutput = g_capture ? ow : g_out; si.hStdError = g_err;
  PROCESS_INFORMATION pi; char *cmdline = sdup(bstr(&cl));
  if (!CreateProcessA(exe, cmdline, 0, 0, 1, 0, 0, 0, &si, &pi)) { DWORD e = GetLastError(); errf("Program '%s' failed to run: Win32 error %u.\r\n", exe, (u32)e); xfree(cmdline); bfree(&cl); if (ow) { CloseHandle(ow); CloseHandle(or_); } if (ir) { CloseHandle(ir); CloseHandle(iw); } return 1; }
  if (ow) CloseHandle(ow); if (ir) CloseHandle(ir);
  if (input) { hwrite(iw, input, in_len); CloseHandle(iw); }
  if (g_capture) { int n; char *o = read_all(or_, &n); bput(g_capture, o, n); xfree(o); CloseHandle(or_); }
  WaitForSingleObject(pi.hProcess, 0xFFFFFFFF); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  xfree(cmdline); bfree(&cl); return (int)code;
}
static int exec_block_with_item(Toks *ts, int from, int to, const char *item) { char *save = g_pipeline_item; g_pipeline_item = sdup(item); int r = exec_range(ts, from, to); xfree(g_pipeline_item); g_pipeline_item = save; return r; }
static int not_recognized(const char *name) { errf("%s : The term '%s' is not recognized as the name of a cmdlet, function, script file, or operable program. Check the spelling of the name, or if a path was included, verify that the path is correct and try again.\r\n", name, name); return 1; }

/* execute one simple command (cmdlet / external); block_from/to: a { } script block argument if present. returns exit status (0 ok) */
static int run_cmd(Cmd *c, Toks *ts, int blk_from, int blk_to) {
  const char *n = c->name;
  if (sieq(n, "Write-Output") || sieq(n, "echo") || sieq(n, "Write-Host") || sieq(n, "Out-Host") || sieq(n, "Write-Information") || sieq(n, "Out-Default")) {
    if (c->input) { outn(c->input, c->in_len); if (c->in_len && c->input[c->in_len - 1] != '\n') outn("\r\n", 2); }
    Buf b = { 0, 0, 0 }; int first = 1; for (int i = 0; i < c->argc; i++) { if (c->is_param[i]) { if (sieq(c->argv[i], "-ForegroundColor") || sieq(c->argv[i], "-BackgroundColor") || sieq(c->argv[i], "-Separator")) i++; continue; } if (!first) bputc(&b, ' '); bput(&b, c->argv[i], -1); first = 0; }
    if (c->argc || !c->input) { if (!cmd_flag(c, "NoNewline") && !(sieq(n, "Write-Output") && b.n && b.p[b.n - 1] == '\n')) { if (sieq(n, "Write-Output") || sieq(n, "echo")) { emit_lines(bstr(&b)); } else { outn(bstr(&b), b.n); outn("\r\n", 2); } } else outn(bstr(&b), b.n); }
    bfree(&b); return 0;
  }
  if (sieq(n, "Write-Error")) { const char *m = cmd_pos(c, 0); if (!m) m = cmd_opt(c, "Message"); errf("%s\r\n", m ? m : ""); g_last_ok = 0; return 0; }
  if (sieq(n, "Write-Warning")) { const char *m = cmd_pos(c, 0); errf("WARNING: %s\r\n", m ? m : ""); return 0; }
  if (sieq(n, "Write-Verbose") || sieq(n, "Write-Debug") || sieq(n, "Write-Progress") || sieq(n, "Set-StrictMode") || sieq(n, "Import-Module") || sieq(n, "Add-Type") || sieq(n, "Set-ExecutionPolicy") || sieq(n, "Update-Help") || sieq(n, "Register-ArgumentCompleter") || sieq(n, "Set-PSDebug") || sieq(n, "Clear-Host") || sieq(n, "cls") || sieq(n, "clear") || sieq(n, "Set-PSReadLineOption") || sieq(n, "Set-PSReadlineKeyHandler") || sieq(n, "Remove-Module") || sieq(n, "Unblock-File") || sieq(n, "Set-ItemProperty") || sieq(n, "Set-Item")) return 0;
  if (sieq(n, "Get-ExecutionPolicy")) { outline("Unrestricted"); return 0; }
  if (sieq(n, "Out-Null")) return 0;
  if (sieq(n, "Out-String")) { if (c->input) { outn(c->input, c->in_len); if (c->in_len && c->input[c->in_len - 1] != '\n') outn("\r\n", 2); } return 0; }
  if (sieq(n, "Out-File") || sieq(n, "Set-Content") || sieq(n, "sc") || sieq(n, "Add-Content") || sieq(n, "ac") || sieq(n, "Tee-Object") || sieq(n, "tee")) {
    const char *path = cmd_opt(c, "FilePath"); if (!path) path = cmd_opt(c, "Path"); if (!path) path = cmd_opt(c, "LiteralPath"); if (!path) path = cmd_pos(c, 0);
    const char *val = cmd_opt(c, "Value"); if (!val && !c->input) val = cmd_pos(c, 1);
    if (!path) { errf("%s : Missing an argument for parameter 'Path'.\r\n", n); return 1; }
    Buf b = { 0, 0, 0 }; if (c->input) bput(&b, c->input, c->in_len); else if (val) { for (const char *p = val; *p; p++) { if (*p == '\n') bput(&b, "\r\n", 2); else bputc(&b, *p); } if (!cmd_flag(c, "NoNewline")) bput(&b, "\r\n", 2); }
    int append = sieq(n, "Add-Content") || sieq(n, "ac") || cmd_flag(c, "Append"); char full[1024]; full_path(path, full, sizeof full);
    int ok = write_file(full, bstr(&b), b.n, append); if (sieq(n, "Tee-Object") || sieq(n, "tee")) outn(bstr(&b), b.n); bfree(&b);
    if (!ok) { errf("%s : Could not find a part of the path '%s'.\r\n", n, full); return 1; } return 0;
  }
  if (sieq(n, "Get-Content") || sieq(n, "gc") || sieq(n, "cat") || sieq(n, "type")) {
    const char *path = cmd_opt(c, "Path"); if (!path) path = cmd_opt(c, "LiteralPath"); if (!path) path = cmd_pos(c, 0); if (!path && c->input) path = c->input;
    if (!path) { errs("Get-Content : Missing path.\r\n"); return 1; }
    char full[1024]; full_path(path, full, sizeof full); int len; char *d = read_file(full, &len); if (!d) { errf("Get-Content : Cannot find path '%s' because it does not exist.\r\n", full); return 1; }
    const char *tail = cmd_opt(c, "Tail"); const char *head = cmd_opt(c, "TotalCount"); if (!head) head = cmd_opt(c, "First"); if (!head) head = cmd_opt(c, "Head");
    if (cmd_flag(c, "Raw") && !tail && !head) outn(d, len);
    else { int total = 0; FOR_LINES(d, p, e, k) (void)k; total++; END_LINES(p, e) int skip = tail ? total - (int)satoi(tail, 0) : 0, lim = head ? (int)satoi(head, 0) : total, i = 0; FOR_LINES(d, p, e, k) if (i >= skip && i - skip < lim) { outn(p, k); outn("\r\n", 2); } i++; END_LINES(p, e) }
    xfree(d); return 0;
  }
  if (sieq(n, "Test-Path")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); if (!p && c->input) p = c->input; if (!p) { outline("False"); return 0; } const char *pt = cmd_opt(c, "PathType"); int r; if (has_wild(p)) { char dir[1024]; dirname_of(p, dir, sizeof dir); WIN32_FIND_DATAA fd; char full[1024]; full_path(p, full, sizeof full); HANDLE h = FindFirstFileA(full, &fd); r = h != INVALID; if (r) FindClose(h); } else r = pt && sieq(pt, "Container") ? is_dir(p) : pt && sieq(pt, "Leaf") ? exists(p) && !is_dir(p) : exists(p); outline(r ? "True" : "False"); return 0; }
  if (sieq(n, "Get-Location") || sieq(n, "pwd") || sieq(n, "gl")) { char d[1024]; cwd(d, sizeof d); outline(d); return 0; }
  if (sieq(n, "Set-Location") || sieq(n, "cd") || sieq(n, "chdir") || sieq(n, "sl") || sieq(n, "Push-Location") || sieq(n, "pushd")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); char full[1024]; if (!p) getenv_s("USERPROFILE", full, sizeof full); else full_path(p, full, sizeof full); static char stack[16][1024]; static int sp; if (sieq(n, "Push-Location") || sieq(n, "pushd")) { if (sp < 16) cwd(stack[sp++], 1024); } if (!SetCurrentDirectoryA(full)) { errf("%s : Cannot find path '%s' because it does not exist.\r\n", n, full); return 1; } return 0; }
  if (sieq(n, "Pop-Location") || sieq(n, "popd")) return 0;
  if (sieq(n, "Get-ChildItem") || sieq(n, "gci") || sieq(n, "ls") || sieq(n, "dir") || sieq(n, "Get-Item") || sieq(n, "gi")) {
    const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); if (!p && c->input) p = c->input; const char *filter = cmd_opt(c, "Filter"); if (!filter) filter = cmd_opt(c, "Include");
    char full[1024]; if (!p || !*p) cwd(full, sizeof full); else full_path(p, full, sizeof full); strip_trailing(full);
    Buf b = { 0, 0, 0 }; LsCtx ctx = { &b, cmd_flag(c, "Name"), cmd_flag(c, "Directory"), cmd_flag(c, "File"), 0 }; int recurse = cmd_flag(c, "Recurse") || cmd_flag(c, "r");
    if (sieq(n, "Get-Item") || sieq(n, "gi")) { if (!exists(full)) { errf("Get-Item : Cannot find path '%s' because it does not exist.\r\n", full); return 1; } outline(full); return 0; }
    if (has_wild(basename_of(full))) { char dir[1024]; dirname_of(full, dir, sizeof dir); const char *bn = basename_of(full); walk(dir, filter ? filter : bn, recurse, ls_cb, &ctx); }
    else if (is_dir(full)) { ctx.root = full; walk(full, filter, recurse, ls_cb, &ctx); }
    else if (exists(full)) { WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(full, &fd); if (h != INVALID) { ls_cb(full, &fd, &ctx); FindClose(h); } }
    else { errf("Get-ChildItem : Cannot find path '%s' because it does not exist.\r\n", full); bfree(&b); return 1; }
    if (ctx.names_only) emit_lines(bstr(&b)); else { /* FullName list when captured, table when printed */ if (g_capture) { FOR_LINES(bstr(&b), q, e, k) const char *sp = q + k; while (sp > q && sp[-1] != ' ') sp--; outn(sp, (int)(q + k - sp)); outn("\r\n", 2); END_LINES(q, e) } else emit_lines(bstr(&b)); }
    bfree(&b); return 0;
  }
  if (sieq(n, "New-Item") || sieq(n, "ni") || sieq(n, "mkdir") || sieq(n, "md")) {
    const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_opt(c, "Name"); if (!p) p = cmd_pos(c, 0); const char *type = cmd_opt(c, "ItemType"); const char *val = cmd_opt(c, "Value");
    if (!p) { errs("New-Item : Missing path.\r\n"); return 1; } char full[1024]; full_path(p, full, sizeof full); const char *nm = cmd_opt(c, "Name"); if (nm && cmd_opt(c, "Path")) join_path(full, sizeof full, full, nm);
    int dir = sieq(n, "mkdir") || sieq(n, "md") || (type && (sieq(type, "Directory") || sieq(type, "Container") || sieq(type, "d")));
    if (dir) { if (!mkdir_p(full)) { errf("New-Item : Cannot create directory '%s'.\r\n", full); return 1; } }
    else { if (exists(full) && !cmd_flag(c, "Force")) { errf("New-Item : The file '%s' already exists.\r\n", full); return 1; } char parent[1024]; dirname_of(full, parent, sizeof parent); mkdir_p(parent); if (!write_file(full, val ? val : "", val ? slen(val) : 0, 0)) { errf("New-Item : Cannot create '%s'.\r\n", full); return 1; } }
    outline(full); return 0;
  }
  if (sieq(n, "Remove-Item") || sieq(n, "ri") || sieq(n, "rm") || sieq(n, "del") || sieq(n, "erase") || sieq(n, "rmdir") || sieq(n, "rd")) {
    int any = 0; for (int i = 0; i < c->argc; i++) { if (c->is_param[i]) { if (sieq(c->argv[i], "-ErrorAction") || sieq(c->argv[i], "-Exclude") || sieq(c->argv[i], "-Include")) i++; continue; } any = 1; char full[1024]; full_path(c->argv[i], full, sizeof full);
      if (has_wild(basename_of(full))) { char dir[1024]; dirname_of(full, dir, sizeof dir); char pat[1024]; scpy(pat, sizeof pat, basename_of(full)); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(full, &fd); if (h != INVALID) { do { if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue; char fp[1024]; join_path(fp, sizeof fp, dir, fd.cFileName); remove_tree(fp); } while (FindNextFileA(h, &fd)); FindClose(h); } continue; }
      if (!exists(full)) { if (!cmd_opt(c, "ErrorAction") || !sieq(cmd_opt(c, "ErrorAction"), "SilentlyContinue")) errf("Remove-Item : Cannot find path '%s' because it does not exist.\r\n", full); g_last_ok = 0; continue; }
      if (!remove_tree(full)) { errf("Remove-Item : Cannot remove item '%s': %u.\r\n", full, (u32)GetLastError()); return 1; } }
    if (!any && c->input) { FOR_LINES(c->input, p, e, k) char t[1024]; if (k > 1023) k = 1023; memcpy(t, p, (u64)k); t[k] = 0; remove_tree(t); END_LINES(p, e) }
    return 0;
  }
  if (sieq(n, "Copy-Item") || sieq(n, "cp") || sieq(n, "copy") || sieq(n, "cpi") || sieq(n, "Move-Item") || sieq(n, "mv") || sieq(n, "move") || sieq(n, "mi") || sieq(n, "Rename-Item") || sieq(n, "ren") || sieq(n, "rni")) {
    const char *s = cmd_opt(c, "Path"); if (!s) s = cmd_opt(c, "LiteralPath"); const char *d = cmd_opt(c, "Destination"); if (!d) d = cmd_opt(c, "NewName"); int k = 0; if (!s) s = cmd_pos(c, k++); if (!d) d = cmd_pos(c, k);
    if (!s || !d) { errf("%s : Missing source or destination.\r\n", n); return 1; }
    char fs[1024], fdst[1024]; full_path(s, fs, sizeof fs); if (sieq(n, "Rename-Item") || sieq(n, "ren") || sieq(n, "rni")) { char dir[1024]; dirname_of(fs, dir, sizeof dir); if (!sstr(d, "\\") && !sstr(d, "/")) join_path(fdst, sizeof fdst, dir, d); else full_path(d, fdst, sizeof fdst); } else { full_path(d, fdst, sizeof fdst); if (is_dir(fdst)) join_path(fdst, sizeof fdst, fdst, basename_of(fs)); }
    int move = !(sieq(n, "Copy-Item") || sieq(n, "cp") || sieq(n, "copy") || sieq(n, "cpi")); int ok;
    if (has_wild(basename_of(fs))) { char dir[1024]; dirname_of(fs, dir, sizeof dir); char dd[1024]; full_path(d, dd, sizeof dd); mkdir_p(dd); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(fs, &fd); ok = 1; if (h != INVALID) { do { if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue; char a[1024], b2[1024]; join_path(a, sizeof a, dir, fd.cFileName); join_path(b2, sizeof b2, dd, fd.cFileName); if (move) { if (cmd_flag(c, "Force")) remove_tree(b2); ok &= MoveFileExA(a, b2, 3) != 0; } else ok &= copy_tree(a, b2); } while (FindNextFileA(h, &fd)); FindClose(h); } }
    else if (move) { if (cmd_flag(c, "Force") && exists(fdst)) remove_tree(fdst); ok = MoveFileExA(fs, fdst, 3) != 0; }
    else { if (is_dir(fs) && !cmd_flag(c, "Recurse") && !sieq(n, "cp")) { if (!exists(fs)) { errf("Copy-Item : Cannot find path '%s'.\r\n", fs); return 1; } } char parent[1024]; dirname_of(fdst, parent, sizeof parent); mkdir_p(parent); ok = copy_tree(fs, fdst); }
    if (!ok) { errf("%s : Cannot %s '%s' to '%s' (error %u).\r\n", n, move ? "move" : "copy", fs, fdst, (u32)GetLastError()); return 1; } return 0;
  }
  if (sieq(n, "Expand-Archive") || sieq(n, "Extract-Archive")) {
    const char *z = cmd_opt(c, "Path"); if (!z) z = cmd_opt(c, "LiteralPath"); const char *d = cmd_opt(c, "DestinationPath"); int k = 0; if (!z) z = cmd_pos(c, k++); if (!d) d = cmd_pos(c, k); if (!z && c->input) z = c->input;
    if (!z) { errs("Expand-Archive : Missing an argument for parameter 'Path'.\r\n"); return 1; }
    char zf[1024]; full_path(z, zf, sizeof zf); char dd[1024]; if (d) full_path(d, dd, sizeof dd); else { char b[1024]; scpy(b, sizeof b, basename_of(zf)); char *dot = 0; for (char *p = b; *p; p++) if (*p == '.') dot = p; if (dot) *dot = 0; char dir[1024]; cwd(dir, sizeof dir); join_path(dd, sizeof dd, dir, b); }
    if (!zip_extract(zf, dd, cmd_flag(c, "Force"), cmd_flag(c, "Verbose"))) { g_last_ok = 0; return 1; } if (cmd_flag(c, "PassThru")) outline(dd); return 0;
  }
  if (sieq(n, "Compress-Archive")) { const char *s = cmd_opt(c, "Path"); if (!s) s = cmd_opt(c, "LiteralPath"); const char *d = cmd_opt(c, "DestinationPath"); int k = 0; if (!s) s = cmd_pos(c, k++); if (!d) d = cmd_pos(c, k); if (!s || !d) { errs("Compress-Archive : Missing -Path or -DestinationPath.\r\n"); return 1; } if (!zip_create(s, d)) { errs("Compress-Archive : failed.\r\n"); return 1; } return 0; }
  if (sieq(n, "Join-Path")) { const char *a = cmd_opt(c, "Path"); const char *b = cmd_opt(c, "ChildPath"); int k = 0; if (!a) a = cmd_pos(c, k++); if (!b) b = cmd_pos(c, k++); if (!a || !b) { errs("Join-Path : Missing arguments.\r\n"); return 1; } char out[1024]; join_path(out, sizeof out, a, b); for (const char *more; (more = cmd_pos(c, k)); k++) join_path(out, sizeof out, out, more); norm_slashes(out); outline(out); return 0; }
  if (sieq(n, "Split-Path")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); if (!p && c->input) p = c->input; if (!p) { errs("Split-Path : Missing path.\r\n"); return 1; } char t[1024]; scpy(t, sizeof t, p); norm_slashes(t); int len = slen(t); while (len && (t[len - 1] == '\r' || t[len - 1] == '\n')) t[--len] = 0; if (cmd_flag(c, "Leaf")) outline(basename_of(t)); else if (cmd_flag(c, "Extension")) { const char *b = basename_of(t); const char *d = 0; for (const char *q = b; *q; q++) if (*q == '.') d = q; outline(d ? d : ""); } else if (cmd_flag(c, "LeafBase")) { const char *b = basename_of(t); const char *d = 0; for (const char *q = b; *q; q++) if (*q == '.') d = q; char x[1024]; scpy(x, sizeof x, b); if (d) x[d - b] = 0; outline(x); } else if (cmd_flag(c, "Qualifier")) { char x[3] = { t[0], t[1], 0 }; outline(x); } else { char d[1024]; dirname_of(t, d, sizeof d); outline(d); } return 0; }
  if (sieq(n, "Resolve-Path") || sieq(n, "rvpa") || sieq(n, "Convert-Path")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); if (!p && c->input) p = c->input; if (!p) { errs("Resolve-Path : Missing path.\r\n"); return 1; } char full[1024]; full_path(p, full, sizeof full); if (!exists(full) && !has_wild(full)) { errf("Resolve-Path : Cannot find path '%s' because it does not exist.\r\n", full); return 1; } outline(full); return 0; }
  if (sieq(n, "Get-Command") || sieq(n, "gcm")) { const char *p = cmd_pos(c, 0); if (!p) p = cmd_opt(c, "Name"); if (!p) { outline("powershell"); return 0; } char full[1024]; if (find_on_path(p, full, sizeof full)) { outline(full); return 0; } if (sieq(p, "powershell") || sieq(p, "pwsh")) { outline(g_self); return 0; } if (!cmd_opt(c, "ErrorAction")) errf("Get-Command : The term '%s' is not recognized as a name of a cmdlet, function, script file, or executable program.\r\n", p); g_last_ok = 0; return 1; }
  if (sieq(n, "Select-String") || sieq(n, "sls") || sieq(n, "findstr")) {
    const char *pat = cmd_opt(c, "Pattern"); int k = 0; if (!pat) pat = cmd_pos(c, k++); const char *path = cmd_opt(c, "Path"); if (!path) path = cmd_pos(c, k); int simple = cmd_flag(c, "SimpleMatch"), quiet = cmd_flag(c, "Quiet"), notm = cmd_flag(c, "NotMatch"); if (!pat) { errs("Select-String : Missing pattern.\r\n"); return 1; }
    int found = 0;
    if (path) { WIN32_FIND_DATAA fd; char full[1024]; full_path(path, full, sizeof full); char dir[1024]; dirname_of(full, dir, sizeof dir); HANDLE h = FindFirstFileA(full, &fd); if (h == INVALID) { errf("Select-String : Cannot find path '%s'.\r\n", full); return 1; } do { char fp[1024]; join_path(fp, sizeof fp, dir, fd.cFileName); int len; char *d = read_file(fp, &len); if (!d) continue; int ln = 0; FOR_LINES(d, p, e, kk) ln++; char line[8192]; if (kk > 8191) kk = 8191; memcpy(line, p, (u64)kk); line[kk] = 0; int m = simple ? sistr(line, pat) != 0 : regex_lite(pat, line); if (notm) m = !m; if (m) { found = 1; if (!quiet) outf("%s:%d:%s\r\n", fp, ln, line); } END_LINES(p, e) xfree(d); } while (FindNextFileA(h, &fd)); FindClose(h); }
    else if (c->input) { FOR_LINES(c->input, p, e, kk) char line[8192]; if (kk > 8191) kk = 8191; memcpy(line, p, (u64)kk); line[kk] = 0; int m = simple ? sistr(line, pat) != 0 : regex_lite(pat, line); if (notm) m = !m; if (m) { found = 1; if (!quiet) outline(line); } END_LINES(p, e) }
    if (quiet) outline(found ? "True" : "False"); return 0;
  }
  if (sieq(n, "Select-Object") || sieq(n, "select")) {
    const char *first = cmd_opt(c, "First"), *last = cmd_opt(c, "Last"), *skip = cmd_opt(c, "Skip"); int uniq = cmd_flag(c, "Unique"); int total = 0; FOR_LINES(c->input, p, e, k) (void)k; total++; END_LINES(p, e)
    int sk = skip ? (int)satoi(skip, 0) : 0, lim = first ? (int)satoi(first, 0) : total; if (last) { sk = total - (int)satoi(last, 0); if (sk < 0) sk = 0; lim = total; } int i = 0; Buf seen = { 0, 0, 0 };
    FOR_LINES(c->input, p, e, k) if (i >= sk && i - sk < lim) { char line[4096]; if (k > 4095) k = 4095; memcpy(line, p, (u64)k); line[k] = 0; char key[4100]; sfmt(key, sizeof key, "\n%s\n", line); if (!uniq || !sistr(bstr(&seen), key)) { outline(line); if (uniq) { if (!seen.n) bputc(&seen, '\n'); bput(&seen, line, -1); bputc(&seen, '\n'); } } } i++; END_LINES(p, e) bfree(&seen); return 0;
  }
  if (sieq(n, "Sort-Object") || sieq(n, "sort") || sieq(n, "Get-Unique") || sieq(n, "gu")) {
    char **lines = xalloc(sizeof(char *) * 65536); int cnt = 0; FOR_LINES(c->input, p, e, k) if (cnt < 65536) lines[cnt++] = sdupn(p, k); END_LINES(p, e)
    int desc = cmd_flag(c, "Descending"), uniq = cmd_flag(c, "Unique") || sieq(n, "Get-Unique") || sieq(n, "gu"); if (!sieq(n, "Get-Unique") && !sieq(n, "gu")) for (int i = 1; i < cnt; i++) { char *x = lines[i]; int j = i - 1; while (j >= 0 && (desc ? sicmp(lines[j], x) < 0 : sicmp(lines[j], x) > 0)) { lines[j + 1] = lines[j]; j--; } lines[j + 1] = x; }
    for (int i = 0; i < cnt; i++) { if (uniq && i && sieq(lines[i], lines[i - 1])) continue; outline(lines[i]); } for (int i = 0; i < cnt; i++) xfree(lines[i]); xfree(lines); return 0;
  }
  if (sieq(n, "Measure-Object") || sieq(n, "measure")) { int cnt = 0; s64 sum = 0; FOR_LINES(c->input, p, e, k) if (k) { cnt++; sum += satoi(p, 0); } END_LINES(p, e) if (cmd_flag(c, "Sum")) outf("%L\r\n", sum); else if (cmd_flag(c, "Line")) outf("%d\r\n", cnt); else outf("%d\r\n", cnt); return 0; }
  if (sieq(n, "ForEach-Object") || sieq(n, "foreach") || seq(n, "%")) { if (blk_from < 0) { const char *m = cmd_opt(c, "MemberName"); if (!m) m = cmd_pos(c, 0); if (m) { FOR_LINES(c->input, p, e, k) char line[4096]; if (k > 4095) k = 4095; memcpy(line, p, (u64)k); line[k] = 0; char expr[4200]; sfmt(expr, sizeof expr, "$_.%s", m); char *save = g_pipeline_item; g_pipeline_item = line; Toks t2 = { 0, 0, 0 }; tokenize(expr, &t2); char *v = eval_expr(&t2, 0, t2.n - 1); outline(v); xfree(v); g_pipeline_item = save; END_LINES(p, e) } return 0; } FOR_LINES(c->input, p, e, k) char line[8192]; if (k > 8191) k = 8191; memcpy(line, p, (u64)k); line[k] = 0; exec_block_with_item(ts, blk_from, blk_to, line); if (g_quit) break; END_LINES(p, e) return 0; }
  if (sieq(n, "Where-Object") || sieq(n, "where") || seq(n, "?")) {
    if (blk_from < 0) { /* Where-Object Name -like '*x*' */ const char *prop = cmd_pos(c, 0); if (!prop) return 0; int op_i = -1; for (int i = 0; i < c->argc; i++) if (c->is_param[i] && i > 0) { op_i = i; break; } if (op_i < 0) return 0; const char *op = c->argv[op_i], *val = op_i + 1 < c->argc ? c->argv[op_i + 1] : ""; FOR_LINES(c->input, p, e, k) char line[4096]; if (k > 4095) k = 4095; memcpy(line, p, (u64)k); line[k] = 0; const char *subject = line; if (sieq(prop, "Name")) subject = basename_of(line); int m = sieq(op, "-like") ? wild_match(val, subject) : sieq(op, "-notlike") ? !wild_match(val, subject) : sieq(op, "-eq") ? sieq(subject, val) : sieq(op, "-ne") ? !sieq(subject, val) : sieq(op, "-match") ? regex_lite(val, subject) : 0; if (m) outline(line); END_LINES(p, e) return 0; }
    FOR_LINES(c->input, p, e, k) char line[8192]; if (k > 8191) k = 8191; memcpy(line, p, (u64)k); line[k] = 0; char *save = g_pipeline_item; g_pipeline_item = line; char *v = eval_expr(ts, blk_from, blk_to); g_pipeline_item = save; if (truthy(v)) outline(line); xfree(v); END_LINES(p, e) return 0;
  }
  if (sieq(n, "Start-Sleep") || sieq(n, "sleep")) { const char *ms = cmd_opt(c, "Milliseconds"); const char *s = cmd_opt(c, "Seconds"); if (!s && !ms) s = cmd_pos(c, 0); Sleep(ms ? (DWORD)satoi(ms, 0) : (DWORD)(satoi(s ? s : "0", 0) * 1000)); return 0; }
  if (sieq(n, "Get-Date")) { SYSTEMTIME st; GetLocalTime(&st); const char *f = cmd_opt(c, "Format"); if (!f) f = cmd_opt(c, "UFormat"); if (cmd_flag(c, "UFormat") && f && seq(f, "%s")) { outf("%u\r\n", (u32)(GetTickCount() / 1000)); return 0; } if (f) { Buf b = { 0, 0, 0 }; for (const char *p = f; *p;) { char t[16]; if (snieq(p, "yyyy", 4)) { sfmt(t, 16, "%04d", st.wYear); p += 4; } else if (snieq(p, "MM", 2) && p[0] == 'M') { sfmt(t, 16, "%02d", st.wMonth); p += 2; } else if (snieq(p, "dd", 2) && p[0] == 'd') { sfmt(t, 16, "%02d", st.wDay); p += 2; } else if (snieq(p, "HH", 2) && p[0] == 'H') { sfmt(t, 16, "%02d", st.wHour); p += 2; } else if (snieq(p, "mm", 2) && p[0] == 'm') { sfmt(t, 16, "%02d", st.wMinute); p += 2; } else if (snieq(p, "ss", 2) && p[0] == 's') { sfmt(t, 16, "%02d", st.wSecond); p += 2; } else if (snieq(p, "fff", 3)) { sfmt(t, 16, "%03d", st.wMilliseconds); p += 3; } else { t[0] = *p++; t[1] = 0; } bput(&b, t, -1); } outline(bstr(&b)); bfree(&b); return 0; } outf("%02d.%02d.%04d %02d:%02d:%02d\r\n", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond); return 0; }
  if (sieq(n, "Get-FileHash")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_opt(c, "LiteralPath"); if (!p) p = cmd_pos(c, 0); if (!p && c->input) p = c->input; if (!p) { errs("Get-FileHash : Missing path.\r\n"); return 1; } char full[1024]; full_path(p, full, sizeof full); int len; char *d = read_file(full, &len); if (!d) { errf("Get-FileHash : Cannot find path '%s'.\r\n", full); return 1; } BYTE h[32]; sha256((const BYTE *)d, (u64)len, h); char hex[65]; for (int i = 0; i < 32; i++) sfmt(hex + i * 2, 3, "%02X", (u64)h[i]); if (g_capture) outline(hex); else outf("Algorithm       Hash                                                                   Path\r\n---------       ----                                                                   ----\r\nSHA256          %s %s\r\n", hex, full); xfree(d); return 0; }
  if (sieq(n, "Get-Host")) { outline("Name             : ConsoleHost\r\nVersion          : 5.1.19041.1\r\nCurrentCulture   : en-US"); return 0; }
  if (sieq(n, "Get-Process") || sieq(n, "ps") || sieq(n, "gps")) { outf("    Id ProcessName\r\n    -- -----------\r\n %5u powershell\r\n", (u32)GetCurrentProcessId()); return 0; }
  if (sieq(n, "Stop-Process") || sieq(n, "kill") || sieq(n, "spps")) { const char *id = cmd_opt(c, "Id"); if (!id) id = cmd_pos(c, 0); if (id && is_number(id)) { HANDLE h = OpenProcess(1, 0, (DWORD)satoi(id, 0)); if (h) { TerminateProcess(h, 1); CloseHandle(h); } } return 0; }
  if (sieq(n, "Start-Process") || sieq(n, "saps") || sieq(n, "start")) { const char *fp = cmd_opt(c, "FilePath"); if (!fp) fp = cmd_pos(c, 0); const char *al = cmd_opt(c, "ArgumentList"); if (!fp) { errs("Start-Process : Missing -FilePath.\r\n"); return 1; } char full[1024]; if (!find_on_path(fp, full, sizeof full)) { errf("Start-Process : This command cannot be run due to the error: The system cannot find the file specified ('%s').\r\n", fp); return 1; } char *args[64]; int na = 0; char *alc = al ? sdup(al) : 0; if (alc) { for (char *p = alc; *p && na < 64;) { while (is_space(*p) || *p == '\n') p++; if (!*p) break; if (*p == '"') { p++; args[na++] = p; while (*p && *p != '"') p++; } else { args[na++] = p; while (*p && !is_space(*p) && *p != '\n') p++; } if (*p) *p++ = 0; } } const char *wd = cmd_opt(c, "WorkingDirectory"); char save[1024]; cwd(save, sizeof save); if (wd) SetCurrentDirectoryA(wd); int code = run_external(full, args, na, 0, 0); if (wd) SetCurrentDirectoryA(save); xfree(alc); if (cmd_flag(c, "Wait") || cmd_flag(c, "PassThru")) g_last_exit = code; return 0; }
  if (sieq(n, "ConvertTo-Json")) { Buf b = { 0, 0, 0 }; const char *in = c->input ? c->input : (cmd_pos(c, 0) ? cmd_pos(c, 0) : ""); int cnt = 0; FOR_LINES(in, p, e, k) (void)k; cnt++; END_LINES(p, e) if (cnt > 1) bput(&b, "[\r\n", -1); FOR_LINES(in, p, e, k) if (cnt > 1) bput(&b, "    ", 4); if (k && is_number(p) && (k == slen(p) || !is_digit(p[k]))) { char t[32]; sfmt(t, 32, "%L", satoi(p, 0)); bput(&b, t, -1); } else { bputc(&b, '"'); for (int i = 0; i < k; i++) { if (p[i] == '"' || p[i] == '\\') bputc(&b, '\\'); bputc(&b, p[i]); } bputc(&b, '"'); } if (cnt > 1 && *e) bput(&b, ",\r\n", -1); END_LINES(p, e) if (cnt > 1) bput(&b, "\r\n]", -1); outline(bstr(&b)); bfree(&b); return 0; }
  if (sieq(n, "Invoke-Expression") || sieq(n, "iex")) { const char *s = cmd_opt(c, "Command"); if (!s) s = cmd_pos(c, 0); if (!s && c->input) s = c->input; if (!s) return 0; Toks t2 = { 0, 0, 0 }; tokenize(s, &t2); return exec_range(&t2, 0, t2.n - 1); }
  if (sieq(n, "Set-Variable") || sieq(n, "sv")) { const char *nm = cmd_opt(c, "Name"); const char *v = cmd_opt(c, "Value"); int k = 0; if (!nm) nm = cmd_pos(c, k++); if (!v) v = cmd_pos(c, k); if (nm) var_set(nm, v ? v : ""); return 0; }
  if (sieq(n, "Get-Variable") || sieq(n, "gv")) { const char *nm = cmd_opt(c, "Name"); if (!nm) nm = cmd_pos(c, 0); if (nm) outline(var_get(nm)); return 0; }
  if (sieq(n, "Set-Alias") || sieq(n, "sal") || sieq(n, "New-Alias")) return 0;
  if (sieq(n, "Get-Member") || sieq(n, "gm")) { outline("   TypeName: System.String"); return 0; }
  if (sieq(n, "Invoke-WebRequest") || sieq(n, "iwr") || sieq(n, "curl") || sieq(n, "wget") || sieq(n, "Invoke-RestMethod") || sieq(n, "irm") || sieq(n, "Start-BitsTransfer")) {
    char full[1024]; if ((sieq(n, "curl") || sieq(n, "wget")) && find_on_path(sieq(n, "curl") ? "curl.exe" : "wget.exe", full, sizeof full)) return run_external(full, c->argv, c->argc, c->input, c->in_len);
    errf("%s : this helper has no HTTP client (Nebula's powershell.exe implements the file-system subset only). Download the file with the hosting application and call Expand-Archive on it instead.\r\n", n); g_last_ok = 0; return 1; }
  if (sieq(n, "exit")) { g_quit = 1; const char *v = cmd_pos(c, 0); g_exit_code = v ? (int)satoi(v, 0) : 0; return g_exit_code; }
  if (sieq(n, "return") || sieq(n, "break") || sieq(n, "continue")) { g_quit = seq(n, "return") ? 2 : 3; return 0; }
  if (sieq(n, "throw")) { const char *m = cmd_pos(c, 0); errf("%s\r\n", m ? m : "ScriptHalted"); g_last_ok = 0; g_quit = 4; return 1; }
  if (sieq(n, "Read-Host")) { const char *pr = cmd_opt(c, "Prompt"); if (!pr) pr = cmd_pos(c, 0); if (pr) { hwrite(g_out, pr, slen(pr)); hwrite(g_out, ": ", 2); } char line[4096]; if (!read_line(g_in, line, sizeof line)) line[0] = 0; outline(line); return 0; }
  if (sieq(n, "Get-Help") || sieq(n, "help") || sieq(n, "man")) { outline("Nebula powershell.exe — a small PowerShell-compatible helper. Supported: Expand-Archive, Compress-Archive, Get-ChildItem, Test-Path, New-Item, Remove-Item, Copy-Item, Move-Item, Get-Content, Set-Content, Out-File, Select-String, Join-Path, Split-Path, Get-FileHash, Start-Process, if/try/foreach/while, pipelines and external programs."); return 0; }
  if (sieq(n, "Get-PSDrive")) { outline("Name   Root\r\n----   ----\r\nC      C:\\\r\nEnv"); return 0; }
  if (sieq(n, "Get-Alias")) { outline("ls -> Get-ChildItem\r\ncat -> Get-Content\r\nrm -> Remove-Item\r\ncp -> Copy-Item\r\nmv -> Move-Item"); return 0; }
  if (sieq(n, "Get-ItemProperty") || sieq(n, "Get-ItemPropertyValue")) { const char *p = cmd_opt(c, "Path"); if (!p) p = cmd_pos(c, 0); if (p && snieq(p, "HK", 2)) { errf("%s : Cannot find path '%s' because it does not exist. (no registry in Nebula)\r\n", n, p); g_last_ok = 0; return 1; } if (p) outline(p); return 0; }
  if (sieq(n, "Get-WmiObject") || sieq(n, "Get-CimInstance") || sieq(n, "gwmi")) { const char *cls = cmd_opt(c, "Class"); if (!cls) cls = cmd_opt(c, "ClassName"); if (!cls) cls = cmd_pos(c, 0); if (cls && sieq(cls, "Win32_OperatingSystem")) { outline("Caption   : Microsoft Windows 10 Pro\r\nVersion   : 10.0.19045\r\nOSArchitecture : 64-bit"); return 0; } if (cls && sieq(cls, "Win32_Processor")) { outline("Name : Nebula x86-64 emulator\r\nNumberOfCores : 1"); return 0; } outline(""); return 0; }
  if (sieq(n, "Get-ComputerInfo")) { outline("OsName : Microsoft Windows 10 Pro\r\nOsVersion : 10.0.19045\r\nOsArchitecture : 64-bit"); return 0; }
  if (sieq(n, "Get-NetAdapter") || sieq(n, "Get-NetIPAddress") || sieq(n, "Test-Connection") || sieq(n, "Test-NetConnection")) { errf("%s : networking cmdlets are not available in this helper.\r\n", n); g_last_ok = 0; return 1; }
  /* external program / script */
  char full[1024];
  if (find_on_path(n, full, sizeof full)) { int code = run_external(full, c->argv, c->argc, c->input, c->in_len); g_last_exit = code; return code; }
  return not_recognized(n);
}

/* [Type]::Member(args) static calls */
static char *eval_static(Toks *ts, int *ip, int to) {
  const char *s = ts->t[*ip].text; char type[128], mem[64]; const char *rb = s; while (*rb && *rb != ']') rb++; int tl = (int)(rb - s - 1); if (tl > 127) tl = 127; memcpy(type, s + 1, (u64)tl); type[tl] = 0; scpy(mem, sizeof mem, rb + 3);
  char *args[6] = { 0, 0, 0, 0, 0, 0 }; int na = 0;
  if (*ip + 1 < to && ts->t[*ip + 1].type == T_LPAREN) { int j = *ip + 2, depth = 0, start = j; for (; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RPAREN) { if (!depth) break; depth--; } else if (ty == T_COMMA && !depth) { if (na < 6) args[na++] = eval_expr(ts, start, j); start = j + 1; } } if (j > start && na < 6) args[na++] = eval_expr(ts, start, j); *ip = j; }
  const char *t = type; if (snieq(t, "System.", 7)) t += 7; if (snieq(t, "IO.", 3)) t += 3;
  const char *a0 = args[0] ? args[0] : "", *a1 = args[1] ? args[1] : "";
  char *r = 0; char full[1024];
  if (sieq(t, "Compression.ZipFile") && sieq(mem, "ExtractToDirectory")) { full_path(a0, full, sizeof full); r = sdup(zip_extract(full, a1, args[2] && truthy(args[2]), 0) ? "" : "ERROR"); if (seq(r, "ERROR")) { g_last_ok = 0; g_quit = 4; } }
  else if (sieq(t, "Compression.ZipFile") && sieq(mem, "CreateFromDirectory")) { r = sdup(zip_create(a0, a1) ? "" : "ERROR"); }
  else if (sieq(t, "File") && sieq(mem, "Exists")) r = sdup(exists(a0) && !is_dir(a0) ? "True" : "False");
  else if (sieq(t, "Directory") && sieq(mem, "Exists")) r = sdup(is_dir(a0) ? "True" : "False");
  else if (sieq(t, "Directory") && sieq(mem, "CreateDirectory")) { mkdir_p(a0); full_path(a0, full, sizeof full); r = sdup(full); }
  else if (sieq(t, "Directory") && sieq(mem, "GetCurrentDirectory")) { cwd(full, sizeof full); r = sdup(full); }
  else if (sieq(t, "Directory") && sieq(mem, "Delete")) { remove_tree(a0); r = sdup(""); }
  else if (sieq(t, "Directory") && (sieq(mem, "GetFiles") || sieq(mem, "GetDirectories") || sieq(mem, "EnumerateFiles"))) { Buf b = { 0, 0, 0 }; LsCtx ctx = { &b, 0, sieq(mem, "GetDirectories"), !sieq(mem, "GetDirectories") }; full_path(a0, full, sizeof full); walk(full, args[1] ? a1 : 0, args[2] && sistr(args[2], "All") != 0, ls_cb, &ctx); Buf o = { 0, 0, 0 }; FOR_LINES(bstr(&b), q, e, k) const char *sp = q + k; while (sp > q && sp[-1] != ' ') sp--; bput(&o, sp, (int)(q + k - sp)); bputc(&o, '\n'); END_LINES(q, e) r = sdup(bstr(&o)); bfree(&o); bfree(&b); }
  else if (sieq(t, "File") && sieq(mem, "ReadAllText")) { full_path(a0, full, sizeof full); int len; char *d = read_file(full, &len); r = d ? d : sdup(""); if (!d) { errf("Exception calling \"ReadAllText\": Could not find file '%s'.\r\n", full); g_last_ok = 0; g_quit = 4; } }
  else if (sieq(t, "File") && sieq(mem, "ReadAllLines")) { full_path(a0, full, sizeof full); int len; char *d = read_file(full, &len); r = d ? d : sdup(""); }
  else if (sieq(t, "File") && (sieq(mem, "WriteAllText") || sieq(mem, "WriteAllLines") || sieq(mem, "AppendAllText"))) { full_path(a0, full, sizeof full); write_file(full, a1, slen(a1), sieq(mem, "AppendAllText")); r = sdup(""); }
  else if (sieq(t, "File") && sieq(mem, "Delete")) { full_path(a0, full, sizeof full); DeleteFileA(full); r = sdup(""); }
  else if (sieq(t, "File") && sieq(mem, "Copy")) { char f2[1024]; full_path(a0, full, sizeof full); full_path(a1, f2, sizeof f2); CopyFileA(full, f2, 0); r = sdup(""); }
  else if (sieq(t, "File") && sieq(mem, "Move")) { char f2[1024]; full_path(a0, full, sizeof full); full_path(a1, f2, sizeof f2); MoveFileExA(full, f2, 3); r = sdup(""); }
  else if (sieq(t, "Path") && sieq(mem, "Combine")) { join_path(full, sizeof full, a0, a1); for (int k = 2; k < na; k++) join_path(full, sizeof full, full, args[k]); r = sdup(full); }
  else if (sieq(t, "Path") && sieq(mem, "GetFileName")) r = sdup(basename_of(a0));
  else if (sieq(t, "Path") && sieq(mem, "GetDirectoryName")) { dirname_of(a0, full, sizeof full); r = sdup(full); }
  else if (sieq(t, "Path") && sieq(mem, "GetExtension")) { const char *b = basename_of(a0); const char *d = 0; for (const char *p = b; *p; p++) if (*p == '.') d = p; r = sdup(d ? d : ""); }
  else if (sieq(t, "Path") && sieq(mem, "GetFileNameWithoutExtension")) { const char *b = basename_of(a0); const char *d = 0; for (const char *p = b; *p; p++) if (*p == '.') d = p; r = sdupn(b, d ? (int)(d - b) : slen(b)); }
  else if (sieq(t, "Path") && sieq(mem, "GetFullPath")) { full_path(a0, full, sizeof full); r = sdup(full); }
  else if (sieq(t, "Path") && (sieq(mem, "GetTempPath"))) { getenv_s("TEMP", full, sizeof full); scat(full, sizeof full, "\\"); r = sdup(full); }
  else if (sieq(t, "Path") && (sieq(mem, "GetTempFileName") || sieq(mem, "GetRandomFileName"))) { getenv_s("TEMP", full, sizeof full); char nm[64]; sfmt(nm, sizeof nm, "tmp%x.tmp", (u64)GetTickCount()); if (sieq(mem, "GetRandomFileName")) r = sdup(nm); else { join_path(full, sizeof full, full, nm); write_file(full, "", 0, 0); r = sdup(full); } }
  else if (sieq(t, "Environment") && sieq(mem, "GetEnvironmentVariable")) r = sdup(var_get(((void)sfmt(full, sizeof full, "env:%s", a0), full)));
  else if (sieq(t, "Environment") && sieq(mem, "SetEnvironmentVariable")) { SetEnvironmentVariableA(a0, args[1] && a1[0] ? a1 : 0); r = sdup(""); }
  else if (sieq(t, "Environment") && sieq(mem, "GetFolderPath")) { if (sistr(a0, "UserProfile") || sistr(a0, "Personal") || sistr(a0, "MyDocuments")) getenv_s("USERPROFILE", full, sizeof full); else if (sistr(a0, "LocalApplicationData")) { getenv_s("USERPROFILE", full, sizeof full); scat(full, sizeof full, "\\AppData\\Local"); } else if (sistr(a0, "ApplicationData")) { getenv_s("USERPROFILE", full, sizeof full); scat(full, sizeof full, "\\AppData\\Roaming"); } else if (sistr(a0, "ProgramFiles")) scpy(full, sizeof full, "C:\\Program Files"); else if (sistr(a0, "System")) scpy(full, sizeof full, "C:\\Windows\\System32"); else if (sistr(a0, "Windows")) scpy(full, sizeof full, "C:\\Windows"); else scpy(full, sizeof full, "C:\\"); r = sdup(full); }
  else if (sieq(t, "Environment") && sieq(mem, "Exit")) { g_quit = 1; g_exit_code = (int)satoi(a0, 0); r = sdup(""); }
  else if (sieq(t, "Environment") && (sieq(mem, "Is64BitOperatingSystem") || sieq(mem, "Is64BitProcess"))) r = sdup("True");
  else if (sieq(t, "Environment") && sieq(mem, "OSVersion")) r = sdup("Microsoft Windows NT 10.0.19045.0");
  else if (sieq(t, "Environment") && sieq(mem, "MachineName")) r = sdup("NEBULA");
  else if (sieq(t, "Environment") && sieq(mem, "UserName")) r = sdup("user");
  else if (sieq(t, "Environment") && sieq(mem, "ProcessorCount")) r = sdup("1");
  else if (sieq(t, "Environment") && sieq(mem, "NewLine")) r = sdup("\r\n");
  else if (sieq(t, "Environment") && sieq(mem, "CurrentDirectory")) { cwd(full, sizeof full); r = sdup(full); }
  else if (sieq(t, "Environment") && sieq(mem, "GetCommandLineArgs")) r = sdup(GetCommandLineA());
  else if (sieq(t, "Console") && (sieq(mem, "WriteLine") || sieq(mem, "Write"))) { outs(a0); if (sieq(mem, "WriteLine")) outs("\r\n"); r = sdup(""); }
  else if (sieq(t, "Console") && sieq(mem, "Error")) { r = sdup(""); }
  else if (sieq(t, "Console") && sieq(mem, "ReadLine")) { char line[4096]; if (!read_line(g_in, line, sizeof line)) line[0] = 0; r = sdup(line); }
  else if (sieq(t, "Console") && (sieq(mem, "OutputEncoding") || sieq(mem, "InputEncoding"))) r = sdup("utf-8");
  else if (sieq(t, "Text.Encoding") || sieq(t, "Text.UTF8Encoding")) r = sdup("utf-8");
  else if (sieq(t, "String") && sieq(mem, "IsNullOrEmpty")) r = sdup(a0[0] ? "False" : "True");
  else if (sieq(t, "String") && sieq(mem, "IsNullOrWhiteSpace")) { int ws = 1; for (const char *p = a0; *p; p++) if (!is_space(*p)) ws = 0; r = sdup(ws ? "True" : "False"); }
  else if (sieq(t, "String") && sieq(mem, "Join")) { Buf b = { 0, 0, 0 }; for (int k = 1; k < na; k++) { FOR_LINES(args[k], p, e, kk) if (b.n) bput(&b, a0, -1); bput(&b, p, kk); END_LINES(p, e) } r = sdup(bstr(&b)); bfree(&b); }
  else if (sieq(t, "String") && sieq(mem, "Format")) { Buf b = { 0, 0, 0 }; for (const char *p = a0; *p;) { if (*p == '{' && is_digit(p[1])) { int k = (int)satoi(p + 1, &p); while (*p && *p != '}') p++; if (*p) p++; if (k + 1 < na) bput(&b, args[k + 1], -1); } else bputc(&b, *p++); } r = sdup(bstr(&b)); bfree(&b); }
  else if (sieq(t, "Math") && sieq(mem, "Max")) { s64 a = satoi(a0, 0), b = satoi(a1, 0); r = xalloc(32); sfmt(r, 32, "%L", a > b ? a : b); }
  else if (sieq(t, "Math") && sieq(mem, "Min")) { s64 a = satoi(a0, 0), b = satoi(a1, 0); r = xalloc(32); sfmt(r, 32, "%L", a < b ? a : b); }
  else if (sieq(t, "Math") && (sieq(mem, "Floor") || sieq(mem, "Round") || sieq(mem, "Ceiling") || sieq(mem, "Truncate"))) { r = xalloc(32); sfmt(r, 32, "%L", satoi(a0, 0)); }
  else if (sieq(t, "Convert") && sieq(mem, "ToInt32")) { r = xalloc(32); sfmt(r, 32, "%L", satoi(a0, 0)); }
  else if (sieq(t, "Convert") && sieq(mem, "ToBase64String")) { static const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; Buf b = { 0, 0, 0 }; const BYTE *p = (const BYTE *)a0; int n = slen(a0); for (int i = 0; i < n; i += 3) { u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0); bputc(&b, B[v >> 18]); bputc(&b, B[(v >> 12) & 63]); bputc(&b, i + 1 < n ? B[(v >> 6) & 63] : '='); bputc(&b, i + 2 < n ? B[v & 63] : '='); } r = sdup(bstr(&b)); bfree(&b); }
  else if (sieq(t, "Threading.Thread") && sieq(mem, "Sleep")) { Sleep((DWORD)satoi(a0, 0)); r = sdup(""); }
  else if (sieq(t, "Diagnostics.Process") && sieq(mem, "GetCurrentProcess")) { r = xalloc(16); sfmt(r, 16, "%u", (u32)GetCurrentProcessId()); }
  else if (sieq(t, "Net.ServicePointManager") || sieq(t, "Net.SecurityProtocolType") || sieq(t, "Net.WebClient") || sieq(t, "Net.Http.HttpClient")) { if (sieq(t, "Net.WebClient") || sieq(t, "Net.Http.HttpClient")) { errf("New-Object : no HTTP client in this helper ([%s]).\r\n", type); g_last_ok = 0; } r = sdup(""); }
  else if (sieq(t, "Security.Principal.WindowsIdentity") || sieq(t, "Security.Principal.WindowsPrincipal") || sieq(t, "Security.Principal.WindowsBuiltInRole")) r = sdup("False");
  else if (sieq(t, "Runtime.InteropServices.RuntimeInformation")) r = sdup(sieq(mem, "OSArchitecture") || sieq(mem, "ProcessArchitecture") ? "X64" : "Microsoft Windows 10.0.19045");
  else if (sieq(t, "Guid") && sieq(mem, "NewGuid")) { r = xalloc(40); u32 tc = (u32)GetTickCount(); sfmt(r, 40, "%08x-%04x-4%03x-8%03x-%012x", (u64)tc, (u64)(tc >> 16), (u64)(tc & 0xfff), (u64)((tc >> 4) & 0xfff), (u64)tc * 2654435761u); }
  else if (sieq(t, "DateTime") && (sieq(mem, "Now") || sieq(mem, "UtcNow"))) { SYSTEMTIME st; if (sieq(mem, "Now")) GetLocalTime(&st); else GetSystemTime(&st); r = xalloc(40); sfmt(r, 40, "%02d.%02d.%04d %02d:%02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond); }
  else if (sieq(t, "Version") && sieq(mem, "Parse")) r = sdup(a0);
  else if (sieq(t, "Uri") && sieq(mem, "EscapeDataString")) r = sdup(a0);
  else { errf("Unable to find type [%s] or its member '%s' in Nebula's powershell.exe.\r\n", type, mem); g_last_ok = 0; r = sdup(""); }
  for (int k = 0; k < na; k++) xfree(args[k]); return r;
}

/* find the matching close token */
static int match_close(Toks *ts, int open, int to) { int depth = 0; for (int j = open; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LBRACE || ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RBRACE || ty == T_RPAREN) { if (--depth == 0) return j; } } return to; }
/* run a pipeline (tokens from..to without statement terminators); returns exit status */
static void assign_var(const char *name, const char *op, char *v) {
  int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
  if (op[0] == '+') { const char *old = var_get(name); if (is_number(old) && is_number(v)) { char t[32]; sfmt(t, 32, "%L", satoi(old, 0) + satoi(v, 0)); var_set(name, t); } else { Buf nb = { 0, 0, 0 }; bput(&nb, old, -1); if (nb.n && !sstr(old, "\n") && 0) {} bput(&nb, v, -1); var_set(name, bstr(&nb)); bfree(&nb); } }
  else if (op[0] == '-') { char t[32]; sfmt(t, 32, "%L", satoi(var_get(name), 0) - satoi(v, 0)); var_set(name, t); }
  else var_set(name, v);
}
static int exec_pipeline(Toks *ts, int from, int to) {
  if (from >= to) return 0;
  /* assignment takes the whole statement (including any pipeline on the right-hand side) */
  if (ts->t[from].type == T_VAR && from + 1 < to && ts->t[from + 1].type == T_ASSIGN) { char *v = eval_expr(ts, from + 2, to); assign_var(ts->t[from].text, ts->t[from + 1].text, v); xfree(v); return 0; }
  /* split into stages at top-level pipes; detect redirections at the end */
  int stages[32][2]; int ns = 0, start = from, depth = 0; char *redir_out = 0; int redir_append = 0, redir_err_to_out = 0, redir_err_null = 0;
  for (int i = from; i <= to; i++) { int ty = i < to ? ts->t[i].type : T_PIPE; if (ty == T_LBRACE || ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RBRACE || ty == T_RPAREN) depth--; else if (!depth && ty == T_PIPE) { if (ns < 32) { stages[ns][0] = start; stages[ns][1] = i; ns++; } start = i + 1; } }
  /* redirections in the last stage */
  if (ns) { int a = stages[ns - 1][0], b = stages[ns - 1][1]; for (int i = a; i < b; i++) if (ts->t[i].type == T_REDIR) { const char *r = ts->t[i].text; if (seq(r, "2>&1") || seq(r, "*>&1")) redir_err_to_out = 1; else if ((r[0] == '2' || r[0] == '*') && i + 1 < b) { if (sieq(ts->t[i + 1].text, "null")) redir_err_null = 1; i++; } else if (i + 1 < b) { int j = i + 1; redir_out = tok_value(ts, &j, b); redir_append = sstr(r, ">>") != 0; if (sieq(redir_out, "null")) { xfree(redir_out); redir_out = sdup("NUL"); } i = j; } if (b == stages[ns - 1][1]) { stages[ns - 1][1] = i < b ? i : b; } }
    /* trim the redirection tokens from the stage */ for (int i = a; i < b; i++) if (ts->t[i].type == T_REDIR) { stages[ns - 1][1] = i; break; } }
  char *input = 0; int in_len = 0; int status = 0; HANDLE save_err = g_err;
  if (redir_err_null) g_err = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_RW, 0, OPEN_EXISTING, 0, 0);
  Buf *outer_capture = g_capture;
  for (int s = 0; s < ns && !g_quit; s++) {
    int a = stages[s][0], b = stages[s][1]; if (a >= b) continue;
    Buf cap = { 0, 0, 0 }; int last = s == ns - 1;
    if (!last || redir_out) g_capture = &cap; else g_capture = outer_capture;
    if (redir_err_to_out && g_capture) { /* route stderr into the capture: simplest honest approximation — swap handles for externals */ }
    Tok *t0 = &ts->t[a];
    /* assignment: $x = …  */
    if (t0->type == T_VAR && a + 1 < b && ts->t[a + 1].type == T_ASSIGN) {
      const char *op = ts->t[a + 1].text; char *v = eval_expr(ts, a + 2, b); int n = slen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
      if (op[0] == '+' ) { const char *old = var_get(t0->text); if (is_number(old) && is_number(v)) { char t[32]; sfmt(t, 32, "%L", satoi(old, 0) + satoi(v, 0)); var_set(t0->text, t); } else { Buf nb = { 0, 0, 0 }; bput(&nb, old, -1); bput(&nb, v, -1); var_set(t0->text, bstr(&nb)); bfree(&nb); } }
      else if (op[0] == '-') { char t[32]; sfmt(t, 32, "%L", satoi(var_get(t0->text), 0) - satoi(v, 0)); var_set(t0->text, t); }
      else var_set(t0->text, v);
      xfree(v); status = 0;
    }
    else if (t0->type == T_STATIC) { int j = a; char *v = eval_static(ts, &j, b); if (v[0]) emit_lines(v); xfree(v); status = g_last_ok ? 0 : 1; }
    else if (t0->type == T_VAR || t0->type == T_STR1 || t0->type == T_STR2 || t0->type == T_LPAREN || t0->type == T_SUBEXPR || t0->type == T_ARRAY || (t0->type == T_WORD && (is_number(t0->text) || seq(t0->text, "!") || sieq(t0->text, "-not")))) {
      /* expression statement: print its value, unless it is a pipeline input only */ char *v = eval_expr(ts, a, b); if (v[0] || !last) emit_lines(v); xfree(v); status = 0;
    }
    else {
      /* command: name + arguments (words, strings, vars, (sub)expressions, { blocks }) */
      Cmd c; memset(&c, 0, sizeof c); c.argv = xalloc(sizeof(char *) * 256); c.is_param = xalloc(sizeof(int) * 256); c.input = input; c.in_len = in_len;
      int i = a; int blk_from = -1, blk_to = -1;
      if (t0->type == T_WORD && seq(t0->text, "&") && a + 1 < b) { i = a + 1; int j = i; c.name = tok_value(ts, &j, b); i = j + 1; }
      else if (t0->type == T_WORD && seq(t0->text, ".") && a + 1 < b) { i = a + 1; int j = i; c.name = tok_value(ts, &j, b); i = j + 1; }
      else { int j = i; c.name = tok_value(ts, &j, b); i = j + 1; }
      for (; i < b && c.argc < 255; i++) {
        Tok *t = &ts->t[i];
        if (t->type == T_LBRACE) { int close = match_close(ts, i, b); if (blk_from < 0) { blk_from = i + 1; blk_to = close; } i = close; continue; }
        if (t->type == T_COMMA) continue;
        if (t->type == T_WORD && t->text[0] == '-' && t->text[1] && !is_digit(t->text[1]) && !(t->text[1] == '.' )) { /* -Parameter or -Parameter:value */ const char *colon = 0; for (const char *p = t->text; *p; p++) if (*p == ':') { colon = p; break; } if (colon && colon[1] && !(colon[1] == '$' && 0)) { c.argv[c.argc] = sdupn(t->text, (int)(colon - t->text)); c.is_param[c.argc++] = 1; if (colon[1] == '$') { const char *v = var_get(colon + 2); c.argv[c.argc] = sdup(v); } else c.argv[c.argc] = sdup(colon + 1); c.is_param[c.argc++] = 0; } else { c.argv[c.argc] = sdup(t->text); c.is_param[c.argc++] = 1; } continue; }
        int j = i; char *v = (t->type == T_STATIC) ? eval_static(ts, &j, b) : tok_value(ts, &j, b); i = j;
        /* a value containing newlines (array) → one argument per element for externals; keep as one for cmdlets */
        c.argv[c.argc] = v; c.is_param[c.argc++] = 0;
      }
      if (sieq(c.name, "Where-Object") || sieq(c.name, "where") || seq(c.name, "?") || sieq(c.name, "ForEach-Object") || sieq(c.name, "foreach") || seq(c.name, "%")) { /* blocks are expressions for these */ }
      status = run_cmd(&c, ts, blk_from, blk_to);
      for (int k = 0; k < c.argc; k++) xfree(c.argv[k]); xfree(c.argv); xfree(c.is_param); xfree(c.name);
    }
    if (!last || redir_out) { xfree(input); input = sdupn(bstr(&cap), cap.n); in_len = cap.n; bfree(&cap); }
    if (status) g_last_ok = 0; else if (!g_capture || g_capture == outer_capture) g_last_ok = 1;
  }
  g_capture = outer_capture;
  if (redir_out) { if (seq(redir_out, "NUL")) {} else { char full[1024]; full_path(redir_out, full, sizeof full); if (!write_file(full, input ? input : "", in_len, redir_append)) errf("Out-File : Could not open '%s'.\r\n", full); } xfree(redir_out); }
  if (redir_err_null) { CloseHandle(g_err); g_err = save_err; }
  xfree(input); return status;
}
static char *eval_pipeline_capture(Toks *ts, int from, int to) { Buf b = { 0, 0, 0 }; Buf *save = g_capture; g_capture = &b; exec_range(ts, from, to); g_capture = save; char *r = sdupn(bstr(&b), b.n); bfree(&b); return r; }

/* statements: if / try / foreach / while / function (unsupported) / pipelines separated by ; && || */
static int exec_range(Toks *ts, int from, int to) {
  int status = 0, i = from;
  while (i < to && !g_quit) {
    Tok *t = &ts->t[i];
    if (t->type == T_SEMI) { i++; continue; }
    if (t->type == T_WORD && (sieq(t->text, "if") || sieq(t->text, "elseif")) && i + 1 < to && ts->t[i + 1].type == T_LPAREN) {
      int taken = 0; int j = i;
      for (;;) {
        int cp = match_close(ts, j + 1, to); char *v = eval_expr(ts, j + 2, cp); int cond = truthy(v); xfree(v);
        int bo = cp + 1; if (bo >= to || ts->t[bo].type != T_LBRACE) { errs("ParserError: Missing statement block after if ( condition ).\r\n"); return 1; } int bc = match_close(ts, bo, to);
        if (cond && !taken) { status = exec_range(ts, bo + 1, bc); taken = 1; }
        j = bc + 1;
        if (j < to && ts->t[j].type == T_WORD && sieq(ts->t[j].text, "elseif")) continue;
        if (j < to && ts->t[j].type == T_WORD && sieq(ts->t[j].text, "else") && j + 1 < to && ts->t[j + 1].type == T_LBRACE) { int ec = match_close(ts, j + 1, to); if (!taken) status = exec_range(ts, j + 2, ec); j = ec + 1; }
        break;
      }
      i = j; continue;
    }
    if (t->type == T_WORD && sieq(t->text, "try") && i + 1 < to && ts->t[i + 1].type == T_LBRACE) {
      int bc = match_close(ts, i + 1, to); int ok_save = g_last_ok; g_last_ok = 1; status = exec_range(ts, i + 2, bc); int failed = !g_last_ok || status || g_quit == 4; if (g_quit == 4) g_quit = 0; int j = bc + 1;
      if (j < to && ts->t[j].type == T_WORD && sieq(ts->t[j].text, "catch")) { while (j < to && ts->t[j].type != T_LBRACE) j++; int cc = match_close(ts, j, to); if (failed) { var_set("_", "exception"); status = exec_range(ts, j + 1, cc); } j = cc + 1; }
      if (j < to && ts->t[j].type == T_WORD && sieq(ts->t[j].text, "finally") && j + 1 < to && ts->t[j + 1].type == T_LBRACE) { int fc = match_close(ts, j + 1, to); exec_range(ts, j + 2, fc); j = fc + 1; }
      if (!failed) g_last_ok = ok_save; i = j; continue;
    }
    if (t->type == T_WORD && sieq(t->text, "foreach") && i + 1 < to && ts->t[i + 1].type == T_LPAREN) {
      int cp = match_close(ts, i + 1, to); int k = i + 2; if (k < cp && ts->t[k].type == T_VAR) { char name[64]; scpy(name, sizeof name, ts->t[k].text); k++; if (k < cp && ts->t[k].type == T_WORD && sieq(ts->t[k].text, "in")) k++; char *list = eval_expr(ts, k, cp); int bo = cp + 1; int bc = bo < to && ts->t[bo].type == T_LBRACE ? match_close(ts, bo, to) : bo;
        FOR_LINES(list, p, e, n) char item[8192]; if (n > 8191) n = 8191; memcpy(item, p, (u64)n); item[n] = 0; var_set(name, item); status = exec_range(ts, bo + 1, bc); if (g_quit == 3) { g_quit = 0; break; } if (g_quit) break; END_LINES(p, e) xfree(list); i = bc + 1; continue; }
      i = cp + 1; continue;
    }
    if (t->type == T_WORD && sieq(t->text, "while") && i + 1 < to && ts->t[i + 1].type == T_LPAREN) {
      int cp = match_close(ts, i + 1, to); int bo = cp + 1; int bc = bo < to && ts->t[bo].type == T_LBRACE ? match_close(ts, bo, to) : bo; int guard = 0;
      for (;;) { char *v = eval_expr(ts, i + 2, cp); int c = truthy(v); xfree(v); if (!c || ++guard > 1000000) break; status = exec_range(ts, bo + 1, bc); if (g_quit == 3) { g_quit = 0; break; } if (g_quit) break; }
      i = bc + 1; continue;
    }
    if (t->type == T_WORD && sieq(t->text, "do") && i + 1 < to && ts->t[i + 1].type == T_LBRACE) { int bc = match_close(ts, i + 1, to); int j = bc + 1; int until = j < to && sieq(ts->t[j].text, "until"); int cp = j + 1 < to ? match_close(ts, j + 1, to) : to; int guard = 0; for (;;) { status = exec_range(ts, i + 2, bc); if (g_quit == 3) { g_quit = 0; break; } if (g_quit) break; char *v = eval_expr(ts, j + 2, cp); int c = truthy(v); xfree(v); if (until ? c : !c) break; if (++guard > 1000000) break; } i = cp + 1; continue; }
    if (t->type == T_WORD && (sieq(t->text, "function") || sieq(t->text, "filter") || sieq(t->text, "class") || sieq(t->text, "param") || sieq(t->text, "switch") || sieq(t->text, "workflow"))) { errf("Nebula powershell.exe: '%s' blocks are not supported; the block was skipped.\r\n", t->text); int j = i; while (j < to && ts->t[j].type != T_LBRACE && ts->t[j].type != T_LPAREN) j++; if (j < to && ts->t[j].type == T_LPAREN) { j = match_close(ts, j, to) + 1; while (j < to && ts->t[j].type != T_LBRACE) j++; } i = j < to ? match_close(ts, j, to) + 1 : to; g_last_ok = 0; continue; }
    if (t->type == T_LBRACE) { int bc = match_close(ts, i, to); status = exec_range(ts, i + 1, bc); i = bc + 1; continue; }
    /* a pipeline up to ; && || or a control keyword at depth 0 */
    int j = i, depth = 0, op = 0;
    for (; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LBRACE || ty == T_LPAREN || ty == T_SUBEXPR || ty == T_ARRAY) depth++; else if (ty == T_RBRACE || ty == T_RPAREN) { if (depth) depth--; else break; } else if (!depth && (ty == T_SEMI || ty == T_ANDAND || ty == T_OROR)) { op = ty; break; } }
    status = exec_pipeline(ts, i, j);
    if (op == T_ANDAND && status) { /* skip the next pipeline */ j++; depth = 0; for (; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LBRACE || ty == T_LPAREN) depth++; else if (ty == T_RBRACE || ty == T_RPAREN) depth--; else if (!depth && ty == T_SEMI) break; } }
    else if (op == T_OROR && !status) { j++; depth = 0; for (; j < to; j++) { int ty = ts->t[j].type; if (ty == T_LBRACE || ty == T_LPAREN) depth++; else if (ty == T_RBRACE || ty == T_RPAREN) depth--; else if (!depth && ty == T_SEMI) break; } }
    i = j + 1;
  }
  return status;
}

/* ------------------------------------------------------------------ command line ------------------------------------------------------------------ */
static int split_argv(const char *cl, char **argv, int max) {   /* Windows CommandLineToArgv rules */
  int argc = 0; const char *p = cl;
  while (*p && argc < max) {
    while (is_space(*p)) p++; if (!*p) break;
    Buf b = { 0, 0, 0 }; int inq = 0;
    while (*p && (inq || !is_space(*p))) {
      if (*p == '\\') { int nb = 0; while (*p == '\\') { nb++; p++; } if (*p == '"') { for (int k = 0; k < nb / 2; k++) bputc(&b, '\\'); if (nb & 1) { bputc(&b, '"'); p++; } else { inq = !inq; p++; } } else for (int k = 0; k < nb; k++) bputc(&b, '\\'); continue; }
      if (*p == '"') { if (inq && p[1] == '"') { bputc(&b, '"'); p += 2; continue; } inq = !inq; p++; continue; }
      bputc(&b, *p++);
    }
    argv[argc++] = sdup(bstr(&b)); bfree(&b);
  }
  return argc;
}
static char *decode_utf16_b64(const char *b64) {   /* -EncodedCommand: base64 of UTF-16LE */
  static signed char T[256]; if (!T['A']) { for (int i = 0; i < 256; i++) T[i] = -1; const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; i++) T[(BYTE)B[i]] = (signed char)i; T['='] = 0; }
  Buf raw = { 0, 0, 0 }; u32 acc = 0; int bits = 0; for (const char *p = b64; *p; p++) { if (T[(BYTE)*p] < 0 || *p == '=') continue; acc = (acc << 6) | (u32)T[(BYTE)*p]; bits += 6; if (bits >= 8) { bits -= 8; bputc(&raw, (char)((acc >> bits) & 0xff)); } }
  Buf out = { 0, 0, 0 }; for (int i = 0; i + 1 < raw.n; i += 2) { u32 cp = (BYTE)raw.p[i] | ((BYTE)raw.p[i + 1] << 8); if (cp < 0x80) bputc(&out, (char)cp); else if (cp < 0x800) { bputc(&out, (char)(0xC0 | (cp >> 6))); bputc(&out, (char)(0x80 | (cp & 63))); } else { bputc(&out, (char)(0xE0 | (cp >> 12))); bputc(&out, (char)(0x80 | ((cp >> 6) & 63))); bputc(&out, (char)(0x80 | (cp & 63))); } }
  char *r = sdup(bstr(&out)); bfree(&raw); bfree(&out); return r;
}
DI BOOL IsProcessorFeaturePresent(DWORD f);
/* Add-Type P/Invoke one-liners installers use to probe the CPU (opencode-ai postinstall: IsProcessorFeaturePresent(40) = AVX2) —
   there is no C# compiler here, so the single supported shape is answered by calling kernel32 directly. */
static int pinvoke_shortcut(const char *text) {
  const char *p = sstr(text, "::IsProcessorFeaturePresent("); if (!p || !sstr(text, "Add-Type")) return 0;
  u32 f = (u32)satoi(p + slen("::IsProcessorFeaturePresent("), 0); outline(IsProcessorFeaturePresent(f) ? "True" : "False"); return 1;
}
static int run_script_text(const char *text) { if (pinvoke_shortcut(text)) return 0; Toks ts = { 0, 0, 0 }; tokenize(text, &ts); int st = exec_range(&ts, 0, ts.n - 1); return g_quit == 1 ? g_exit_code : (st ? (g_last_exit ? g_last_exit : 1) : 0); }

void mainCRTStartup(void) {
  g_heap = GetProcessHeap(); g_in = GetStdHandle((DWORD)-10); g_out = GetStdHandle((DWORD)-11); g_err = GetStdHandle((DWORD)-12);
  GetModuleFileNameA(0, g_self, sizeof g_self); if (!g_self[0]) scpy(g_self, sizeof g_self, "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
  char *argv[256]; int argc = split_argv(GetCommandLineA(), argv, 256);
  int i = 1; const char *file = 0, *encoded = 0; Buf command = { 0, 0, 0 }; int have_command = 0, version = 0, help = 0;
  for (; i < argc; i++) {
    const char *a = argv[i]; if (a[0] != '-' && a[0] != '/') break;
    const char *o = a + 1;
    if (sieq(o, "NoProfile") || sieq(o, "nop") || sieq(o, "NoLogo") || sieq(o, "nol") || sieq(o, "NoExit") || sieq(o, "noe") || sieq(o, "Sta") || sieq(o, "Mta") || sieq(o, "NoInteractive") || sieq(o, "Login") || sieq(o, "l")) { if (sieq(o, "NoExit") || sieq(o, "noe")) g_noninteractive = -1; continue; }
    if (sieq(o, "NonInteractive") || sieq(o, "noni")) { g_noninteractive = 1; continue; }
    if (sieq(o, "ExecutionPolicy") || sieq(o, "ep") || sieq(o, "ex") || sieq(o, "WindowStyle") || sieq(o, "w") || sieq(o, "InputFormat") || sieq(o, "inp") || sieq(o, "if") || sieq(o, "OutputFormat") || sieq(o, "o") || sieq(o, "of") || sieq(o, "ConfigurationName") || sieq(o, "WorkingDirectory") || sieq(o, "wd") || sieq(o, "PSConsoleFile") || sieq(o, "psc") || sieq(o, "SettingsFile")) { if (sieq(o, "WorkingDirectory") || sieq(o, "wd")) { if (i + 1 < argc) SetCurrentDirectoryA(argv[i + 1]); } i++; continue; }
    if (sieq(o, "Version") || sieq(o, "v")) { if (i + 1 < argc && is_digit(argv[i + 1][0])) i++; version = 1; continue; }
    if (sieq(o, "Help") || sieq(o, "?") || sieq(o, "h")) { help = 1; continue; }
    if (sieq(o, "File") || sieq(o, "f")) { if (i + 1 < argc) file = argv[++i]; i++; break; }
    if (sieq(o, "EncodedCommand") || sieq(o, "ec") || sieq(o, "e") || sieq(o, "enc")) { if (i + 1 < argc) encoded = argv[++i]; continue; }
    if (sieq(o, "Command") || sieq(o, "c")) { have_command = 1; i++; break; }
    if (seq(a, "-")) { have_command = 1; break; }
    errf("powershell : The argument '%s' is not recognized by Nebula's powershell.exe.\r\n", a);
  }
  if (version) { outline("PowerShell 5.1.19041.1 (Nebula x64 emulator compatibility shell)"); ExitProcess(0); }
  if (help) { outline("powershell [-NoProfile] [-NonInteractive] [-ExecutionPolicy p] [-Version] [-File f [args]] [-EncodedCommand b64] [-Command | -c] script\r\nNebula's powershell.exe: file-system cmdlets, Expand-Archive/Compress-Archive, pipelines, if/try/foreach/while, external programs."); ExitProcess(0); }
  int rc = 0;
  if (encoded) { char *s = decode_utf16_b64(encoded); rc = run_script_text(s); ExitProcess((u32)rc); }
  if (file) {
    char full[1024]; full_path(file, full, sizeof full); int len; char *text = read_file(full, &len);
    if (!text) { errf("The argument '%s' to the -File parameter does not exist. Provide the path to an existing '.ps1' file as an argument to the -File parameter.\r\n", file); ExitProcess(-196608u); }
    dirname_of(full, g_script_root, sizeof g_script_root); Buf args = { 0, 0, 0 }; for (; i < argc; i++) { if (args.n) bputc(&args, '\n'); bput(&args, argv[i], -1); } var_set("args", bstr(&args)); var_set("MyInvocation", full);
    rc = run_script_text(text); ExitProcess((u32)rc);
  }
  if (have_command) {
    /* the rest of the command line is the script (joined with spaces, exactly like powershell.exe) */
    if (i < argc && seq(argv[i], "-")) { int len; char *s = read_all(g_in, &len); rc = run_script_text(s); ExitProcess((u32)rc); }
    for (; i < argc; i++) { if (command.n) bputc(&command, ' '); bput(&command, argv[i], -1); }
    const char *s = bstr(&command); int n = command.n; if (n >= 2 && s[0] == '{' && s[n - 1] == '}') { command.p[n - 1] = 0; s++; }
    rc = run_script_text(s); ExitProcess((u32)rc);
  }
  if (i < argc) { /* powershell script.ps1 args  → run the file */ Buf b = { 0, 0, 0 }; bput(&b, "& '", -1); bput(&b, argv[i], -1); bput(&b, "'", -1); for (int k = i + 1; k < argc; k++) { bput(&b, " '", -1); bput(&b, argv[k], -1); bput(&b, "'", -1); } rc = run_script_text(bstr(&b)); ExitProcess((u32)rc); }
  /* interactive */
  if (g_noninteractive == 1) { int len; char *s = read_all(g_in, &len); rc = run_script_text(s); ExitProcess((u32)rc); }
  outline("Nebula PowerShell-compatible shell (powershell.exe). Type 'help' for the supported subset, 'exit' to leave.");
  for (;;) { char d[1024]; cwd(d, sizeof d); hwrite(g_out, "PS ", 3); hwrite(g_out, d, slen(d)); hwrite(g_out, "> ", 2); char line[8192]; if (!read_line(g_in, line, sizeof line)) break; if (!line[0]) continue; Toks ts = { 0, 0, 0 }; tokenize(line, &ts); exec_range(&ts, 0, ts.n - 1); if (g_quit == 1) break; g_quit = 0; }
  ExitProcess((u32)g_exit_code);
}
