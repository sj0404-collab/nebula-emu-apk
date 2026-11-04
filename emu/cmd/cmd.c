/*
 * cmd.c — a small, freestanding cmd.exe for the Nebula x64 emulator.
 *
 * It is a real PE32+ console program (built with `zig cc -target x86_64-windows-gnu -nostdlib`, kernel32 only, ~40 KB)
 * that the emulator places at C:\Windows\System32\cmd.exe so that `system()`, `subprocess.run(shell=True)`, `fd -x`,
 * `rg --pre`, batch files and the interactive prompt work exactly the way programs expect on Windows.
 *
 * Implemented (the commonly used subset of cmd.exe):
 *   cmd [/c | /k] [/q] [/d] [/s] [/v:on] command        interactive prompt when no /c
 *   operators  &  &&  ||  |  ( … )   redirections  < > >> 2> 2>> 2>&1 1>&2    ^ escapes, "quotes"
 *   %VAR%  %VAR:a=b%  %VAR:~s,l%  %0-%9 %* %~fdpnx0  %CD% %DATE% %TIME% %RANDOM% %ERRORLEVEL%
 *   builtins: echo cd chdir dir type more copy move ren rename del erase mkdir md rmdir rd set path prompt
 *             if (exist / == / errorlevel / EQU NEQ LSS LEQ GTR GEQ, /i, not, else)  for (plain, /l, /d)  goto call shift
 *             setlocal endlocal exit [/b] rem pause cls ver vol title color start where sort find findstr timeout
 *             date /t time /t pushd popd help
 *   batch files: labels, goto, call :label, exit /b, echo on/off, @, %1…%9, shift
 * External programs are searched in the current directory and %PATH% with %PATHEXT% (.COM .EXE .BAT .CMD) and get
 * the inherited / redirected standard handles. Pipelines spawn one process per stage (builtins run as `cmd /c`).
 *
 * for /f supports tokens= delims= skip= eol= over files, strings and 'commands' (no usebackq); for /r and for /d exist.
 * Not implemented: delayed expansion (!var!), usebackq, string-escape subtleties of nested quotes, choice,
 * assoc/ftype, mklink, attrib, console colours. Public domain / CC0 — part of the nebula_gaming repository.
 */
typedef unsigned long DWORD; typedef unsigned short WORD; typedef unsigned char BYTE; typedef int BOOL; typedef void *HANDLE;
typedef unsigned long long u64; typedef long long s64; typedef unsigned int u32;
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
DI DWORD GetEnvironmentVariableA(const char *, char *, DWORD); DI BOOL SetEnvironmentVariableA(const char *, const char *); DI char *GetEnvironmentStringsA(void); DI BOOL FreeEnvironmentStringsA(char *);
DI BOOL CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, STARTUPINFOA *, PROCESS_INFORMATION *);
DI DWORD WaitForSingleObject(HANDLE, DWORD); DI BOOL GetExitCodeProcess(HANDLE, DWORD *); DI BOOL CreatePipe(HANDLE *, HANDLE *, SECURITY_ATTRIBUTES *, DWORD);
DI BOOL SetHandleInformation(HANDLE, DWORD, DWORD); DI DWORD GetTickCount(void); DI void GetLocalTime(SYSTEMTIME *); DI void Sleep(DWORD);
DI HANDLE GetProcessHeap(void); DI void *HeapAlloc(HANDLE, DWORD, u64); DI BOOL HeapFree(HANDLE, DWORD, void *); DI BOOL GetFileSizeEx(HANDLE, s64 *);
DI BOOL FileTimeToLocalFileTime(const FILETIME *, FILETIME *); DI BOOL FileTimeToSystemTime(const FILETIME *, SYSTEMTIME *); DI DWORD GetFileType(HANDLE);
DI BOOL GetConsoleMode(HANDLE, DWORD *); DI BOOL SetFilePointerEx(HANDLE, s64, s64 *, DWORD);
#define INVALID ((HANDLE)(s64)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_RW 3
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define ATTR_DIR 0x10
#define FILE_END 2

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
static int scmp(const char *a, const char *b) { while (*a && *b) { if (*a != *b) return (BYTE)*a - (BYTE)*b; a++; b++; } return (BYTE)*a - (BYTE)*b; }
static int snieq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) { if (lc(a[i]) != lc(b[i])) return 0; if (!a[i]) return 1; } return 1; }
static void scpy(char *d, int cap, const char *s) { int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static void scat(char *d, int cap, const char *s) { int n = slen(d); scpy(d + n, cap - n, s); }
static const char *sistr(const char *h, const char *n) { int ln = slen(n); if (!ln) return h; for (; *h; h++) if (snieq(h, n, ln)) return h; return 0; }
static const char *sstr(const char *h, const char *n) { int ln = slen(n); if (!ln) return h; for (; *h; h++) if (!memcmp(h, n, (u64)ln)) return h; return 0; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_delim(char c) { return is_space(c) || c == ',' || c == ';' || c == '='; }
static s64 satoi(const char *s, const char **end) { int neg = 0; while (is_space(*s)) s++; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; s64 v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; for (;;) { char c = lc(*s); if (c >= '0' && c <= '9') v = v * 16 + (c - '0'); else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10); else break; s++; } }
  else while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); if (end) *end = s; return neg ? -v : v; }
static int fmt_v(char *out, int cap, const char *f, __builtin_va_list ap) {
  int o = 0; char tmp[32];
#define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++; int width = 0, left = 0, zero = 0; if (*f == '-') { left = 1; f++; } if (*f == '0') { zero = 1; f++; } while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    const char *s = tmp; int n;
    if (*f == 's') { s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)"; }
    else if (*f == 'c') { tmp[0] = (char)__builtin_va_arg(ap, int); tmp[1] = 0; }
    else if (*f == 'd' || *f == 'u' || *f == 'x') { s64 v = *f == 'd' ? (s64)__builtin_va_arg(ap, int) : (s64)__builtin_va_arg(ap, u32); if (f[0] == 'd' && 0) {} char *p = tmp + 31; *p = 0; int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v; int base = *f == 'x' ? 16 : 10; do { int dg = (int)(u % (u64)base); *--p = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10); u /= (u64)base; } while (u); if (neg) *--p = '-'; s = p; }
    else if (*f == 'L') { s64 v = __builtin_va_arg(ap, s64); char *p = tmp + 31; *p = 0; int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v; do { *--p = (char)('0' + u % 10); u /= 10; } while (u); if (neg) *--p = '-'; s = p; }
    else if (*f == 'N') { /* number with thousands separators */ s64 v = __builtin_va_arg(ap, s64); char *p = tmp + 31; *p = 0; u64 u = (u64)v; int k = 0; do { if (k && k % 3 == 0) *--p = ','; *--p = (char)('0' + u % 10); u /= 10; k++; } while (u); s = p; }
    else if (*f == '%') { tmp[0] = '%'; tmp[1] = 0; }
    else { tmp[0] = '%'; tmp[1] = *f; tmp[2] = 0; }
    n = slen(s); if (!left) for (int i = n; i < width; i++) PUT(zero ? '0' : ' '); for (int i = 0; i < n; i++) PUT(s[i]); if (left) for (int i = n; i < width; i++) PUT(' ');
  }
  if (cap) out[o < cap ? o : cap - 1] = 0; return o;
#undef PUT
}
static int sfmt(char *out, int cap, const char *f, ...) { __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(out, cap, f, ap); __builtin_va_end(ap); return n; }

/* ------------------------------------------------------------------ state ------------------------------------------------------------------ */
static HANDLE g_in, g_out, g_err;          /* current standard handles (builtins honour redirections through these) */
static HANDLE g_con_in, g_con_out, g_con_err;
static int g_errorlevel, g_echo = 1, g_interactive, g_quit, g_quiet;
static char g_self[520];                   /* our own path, for `cmd /c` children in pipelines */
static u32 g_rand = 12345;
static char g_pushd[16][520]; static int g_npushd;
typedef struct Batch { const char *text; int len; int pos; int argc; char *argv[64]; int leave; char label_goto[128]; struct Batch *up; } Batch;
static Batch *g_batch;
static int g_depth;

static void hwrite(HANDLE h, const char *s, int n) { if (!h || h == INVALID) return; DWORD w; while (n > 0) { if (!WriteFile(h, s, (DWORD)n, &w, 0) || !w) return; s += w; n -= (int)w; } }
static void outs(const char *s) { hwrite(g_out, s, slen(s)); }
static void errs(const char *s) { hwrite(g_err, s, slen(s)); }
static void outf(const char *f, ...) { char b[4096]; __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(b, sizeof b, f, ap); __builtin_va_end(ap); hwrite(g_out, b, n < (int)sizeof b ? n : (int)sizeof b - 1); }
static void errf(const char *f, ...) { char b[4096]; __builtin_va_list ap; __builtin_va_start(ap, f); int n = fmt_v(b, sizeof b, f, ap); __builtin_va_end(ap); hwrite(g_err, b, n < (int)sizeof b ? n : (int)sizeof b - 1); }
static void *xalloc(u64 n) { void *p = HeapAlloc(GetProcessHeap(), 8, n); if (!p) { errs("Not enough memory.\r\n"); ExitProcess(8); } return p; }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* read one line (without CR/LF) from a handle; returns 0 at EOF */
static int read_line(HANDLE h, char *buf, int cap) {
  int n = 0; DWORD got; char c;
  for (;;) { if (!ReadFile(h, &c, 1, &got, 0) || got == 0) { if (!n) return 0; break; } if (c == '\n') break; if (c == 0x1a && n == 0) return 0; if (n + 1 < cap) buf[n++] = c; }
  while (n && (buf[n - 1] == '\r')) n--; buf[n] = 0; return 1;
}
/* read whole handle into a heap buffer */
static char *read_all(HANDLE h, int *len) { int cap = 65536, n = 0; char *b = xalloc((u64)cap); DWORD got; for (;;) { if (n + 4096 > cap) { cap *= 2; char *nb = xalloc((u64)cap); memcpy(nb, b, (u64)n); xfree(b); b = nb; } if (!ReadFile(h, b + n, 4096, &got, 0) || !got) break; n += (int)got; } b[n] = 0; *len = n; return b; }
static char *read_file(const char *path, int *len) { HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_RW, 0, OPEN_EXISTING, 0x80, 0); if (h == INVALID) return 0; char *b = read_all(h, len); CloseHandle(h); return b; }

static const char *win_error(DWORD e) {
  switch (e) { case 2: return "The system cannot find the file specified."; case 3: return "The system cannot find the path specified."; case 5: return "Access is denied."; case 32: return "The process cannot access the file because it is being used by another process.";
  case 80: case 183: return "A duplicate file name exists, or the file cannot be found."; case 145: return "The directory is not empty."; case 267: return "The directory name is invalid."; default: return "The system cannot find the file specified."; }
}
static void getenv_s(const char *name, char *out, int cap) { DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap); if (!n || n >= (DWORD)cap) out[0] = 0; }
static void cwd(char *out, int cap) { if (!GetCurrentDirectoryA((DWORD)cap, out)) scpy(out, cap, "C:\\"); }
static int exists(const char *p) { char t[1024]; scpy(t, sizeof t, p); int n = slen(t); while (n > 1 && (t[n - 1] == '\\' || t[n - 1] == '/') && t[n - 2] != ':') t[--n] = 0; return GetFileAttributesA(t) != 0xFFFFFFFFu; }
static int is_dir(const char *p) { char t[1024]; scpy(t, sizeof t, p); int n = slen(t); while (n > 1 && (t[n - 1] == '\\' || t[n - 1] == '/') && t[n - 2] != ':') t[--n] = 0; DWORD a = GetFileAttributesA(t); return a != 0xFFFFFFFFu && (a & ATTR_DIR); }
static int has_wild(const char *p) { for (; *p; p++) if (*p == '*' || *p == '?') return 1; return 0; }
static const char *basename_of(const char *p) { const char *b = p; for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1; return b; }
static void dirname_of(const char *p, char *out, int cap) { const char *b = basename_of(p); int n = (int)(b - p); if (n >= cap) n = cap - 1; memcpy(out, p, (u64)n); out[n] = 0; }
static void join_path(char *out, int cap, const char *dir, const char *name) { if (!dir[0]) { scpy(out, cap, name); return; } scpy(out, cap, dir); int n = slen(out); if (n && out[n - 1] != '\\' && out[n - 1] != '/' && out[n - 1] != ':') scat(out, cap, "\\"); scat(out, cap, name); }
static void fullpath(const char *p, char *out, int cap) { if (!GetFullPathNameA(p, (DWORD)cap, out, 0)) scpy(out, cap, p); }
static void unquote(char *s) { int n = slen(s); if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { memmove(s, s + 1, (u64)(n - 2)); s[n - 2] = 0; } }

/* ------------------------------------------------------------------ tokenizer helpers ------------------------------------------------------------------ */
/* next whitespace-delimited token (quotes keep spaces, quotes are kept in the token); returns pointer after token */
static const char *next_tok(const char *p, char *out, int cap) {
  while (is_space(*p)) p++; int o = 0, q = 0; if (!*p) { out[0] = 0; return p; }
  while (*p && (q || !is_space(*p))) { if (*p == '"') q = !q; if (o + 1 < cap) out[o++] = *p; p++; }
  out[o] = 0; return p;
}
/* like next_tok but also stops at , ; = (argument delimiters of built-ins) */
static const char *next_arg(const char *p, char *out, int cap) {
  while (is_delim(*p)) p++; int o = 0, q = 0; if (!*p) { out[0] = 0; return p; }
  while (*p && (q || !is_delim(*p))) { if (*p == '"') q = !q; if (o + 1 < cap) out[o++] = *p; p++; }
  out[o] = 0; return p;
}
static void skip_ws(const char **p) { while (is_space(**p)) (*p)++; }
static void trim(char *s) { int n = slen(s); while (n && is_space(s[n - 1])) s[--n] = 0; int i = 0; while (is_space(s[i])) i++; if (i) memmove(s, s + i, (u64)(n - i + 1)); }

/* ------------------------------------------------------------------ %variable% expansion ------------------------------------------------------------------ */
static void special_var(const char *name, char *out, int cap, int *found) {
  *found = 1; SYSTEMTIME t;
  if (sieq(name, "CD")) { cwd(out, cap); return; }
  if (sieq(name, "ERRORLEVEL")) { sfmt(out, cap, "%d", g_errorlevel); return; }
  if (sieq(name, "RANDOM")) { g_rand = g_rand * 1103515245u + 12345u; sfmt(out, cap, "%d", (int)((g_rand >> 16) & 0x7fff)); return; }
  if (sieq(name, "DATE")) { GetLocalTime(&t); static const char *dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }; sfmt(out, cap, "%s %02d/%02d/%d", dn[t.wDayOfWeek % 7], t.wMonth, t.wDay, t.wYear); return; }
  if (sieq(name, "TIME")) { GetLocalTime(&t); sfmt(out, cap, "%02d:%02d:%02d.%02d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds / 10); return; }
  if (sieq(name, "CMDCMDLINE")) { scpy(out, cap, GetCommandLineA()); return; }
  *found = 0;
}
static int lookup_var(const char *name, char *out, int cap) { DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap); if (n && n < (DWORD)cap) return 1; int f; special_var(name, out, cap, &f); return f; }
static void apply_modifiers(const char *mods, const char *argv, char *out, int cap) {
  char v[1024]; scpy(v, sizeof v, argv); unquote(v);
  if (!mods[0]) { scpy(out, cap, v); return; }
  char full[1024]; fullpath(v, full, sizeof full);
  int f = 0, d = 0, p = 0, n = 0, x = 0; for (; *mods; mods++) { char m = lc(*mods); if (m == 'f') f = 1; else if (m == 'd') d = 1; else if (m == 'p') p = 1; else if (m == 'n') n = 1; else if (m == 'x') x = 1; }
  if (f) { scpy(out, cap, full); return; }
  out[0] = 0; const char *base = basename_of(full); const char *dot = 0; for (const char *q = base; *q; q++) if (*q == '.') dot = q;
  if (d) { if (full[1] == ':') { char t[3] = { full[0], ':', 0 }; scat(out, cap, t); } }
  if (p) { char t[1024]; int off = full[1] == ':' ? 2 : 0; int len = (int)(base - full) - off; if (len < 0) len = 0; memcpy(t, full + off, (u64)len); t[len] = 0; scat(out, cap, t); }
  if (n) { char t[520]; int len = dot ? (int)(dot - base) : slen(base); memcpy(t, base, (u64)len); t[len] = 0; scat(out, cap, t); }
  if (x && dot) scat(out, cap, dot);
  if (!d && !p && !n && !x) scpy(out, cap, v);
}
static void expand(const char *in, char *out, int cap) {
  int o = 0; char val[8192], name[256];
#define APP(s) do { const char *_s = (s); while (*_s && o + 1 < cap) out[o++] = *_s++; } while (0)
  for (const char *p = in; *p;) {
    if (*p != '%') { if (o + 1 < cap) out[o++] = *p; p++; continue; }
    /* batch argument references */
    if (g_batch && (p[1] >= '0' && p[1] <= '9')) { int i = p[1] - '0'; if (i < g_batch->argc) APP(g_batch->argv[i]); p += 2; continue; }
    if (g_batch && p[1] == '*') { for (int i = 1; i < g_batch->argc; i++) { if (i > 1) APP(" "); APP(g_batch->argv[i]); } p += 2; continue; }
    if (g_batch && p[1] == '~') { const char *q = p + 2; char mods[8]; int mi = 0; while (*q && !(*q >= '0' && *q <= '9') && mi < 7) mods[mi++] = *q++; mods[mi] = 0; if (*q >= '0' && *q <= '9') { int i = *q - '0'; if (i < g_batch->argc) { apply_modifiers(mods, g_batch->argv[i], val, sizeof val); APP(val); } p = q + 1; continue; } }
    if (g_batch && p[1] == '%') { if (o + 1 < cap) out[o++] = '%'; p += 2; continue; }
    /* %NAME% with optional :a=b or :~s,l */
    const char *e = p + 1; while (*e && *e != '%') e++;
    if (!*e) { if (!g_batch) { if (o + 1 < cap) out[o++] = '%'; } p++; continue; }
    int nl = (int)(e - p - 1); if (nl <= 0 || nl >= (int)sizeof name) { if (!g_batch && nl == 0) { if (o + 1 < cap) out[o++] = '%'; } p = nl == 0 ? e + 1 : p + 1; if (nl == 0 && !g_batch) { if (o + 1 < cap) out[o++] = '%'; } continue; }
    memcpy(name, p + 1, (u64)nl); name[nl] = 0;
    char *colon = 0; for (char *q = name; *q; q++) if (*q == ':' && q != name) { colon = q; break; }
    if (colon) *colon = 0;
    if (!lookup_var(name, val, sizeof val)) { if (g_batch) { p = e + 1; } else { if (o + 1 < cap) out[o++] = '%'; p++; } continue; }
    if (colon) {
      const char *op = colon + 1;
      if (*op == '~') { const char *q; s64 s = satoi(op + 1, &q); int vl = slen(val); s64 l = vl; if (*q == ',') l = satoi(q + 1, &q); if (s < 0) s += vl; if (s < 0) s = 0; if (s > vl) s = vl; if (l < 0) l = vl + l - s; if (l > vl - s) l = vl - s; if (l < 0) l = 0; char t[8192]; memcpy(t, val + s, (u64)l); t[l] = 0; APP(t); }
      else { char from[1024], to[1024]; const char *eq = 0; for (const char *q = op; *q; q++) if (*q == '=') { eq = q; break; } if (eq) { int fl = (int)(eq - op); memcpy(from, op, (u64)fl); from[fl] = 0; scpy(to, sizeof to, eq + 1); int star = from[0] == '*'; const char *fr = star ? from + 1 : from; int frl = slen(fr); char t[8192]; int ti = 0; for (const char *q = val; *q;) { if (frl && snieq(q, fr, frl)) { if (star) { ti = 0; } for (const char *z = to; *z && ti + 1 < (int)sizeof t; z++) t[ti++] = *z; q += frl; if (star) { for (; *q && ti + 1 < (int)sizeof t; q++) t[ti++] = *q; break; } } else { if (ti + 1 < (int)sizeof t) t[ti++] = *q; q++; } } t[ti] = 0; APP(t); } else APP(val); }
    } else APP(val);
    p = e + 1;
  }
  out[o] = 0;
#undef APP
}

/* ------------------------------------------------------------------ wildcard matching ------------------------------------------------------------------ */
static int wild_match(const char *pat, const char *s) {
  for (;;) { if (*pat == '*') { while (*pat == '*') pat++; if (!*pat) return 1; for (; *s; s++) if (wild_match(pat, s)) return 1; return 0; } if (!*s) return !*pat || (*pat == '.' && !pat[1]); if (*pat == '?' || lc(*pat) == lc(*s)) { pat++; s++; continue; } return 0; }
}
typedef struct { char name[260]; u64 size; int dir; SYSTEMTIME mtime; } Entry;
static int list_dir(const char *pattern, Entry **out) {   /* pattern may be a dir, a file or contain wildcards; returns count */
  char pat[1024]; scpy(pat, sizeof pat, pattern[0] ? pattern : "*");
  if (is_dir(pat)) join_path(pat, sizeof pat, pattern[0] ? pattern : ".", "*");
  WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID) { *out = 0; return 0; }
  int cap = 256, n = 0; Entry *e = xalloc(sizeof(Entry) * (u64)cap);
  do { if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue; if (n == cap) { cap *= 2; Entry *ne = xalloc(sizeof(Entry) * (u64)cap); memcpy(ne, e, sizeof(Entry) * (u64)n); xfree(e); e = ne; }
    scpy(e[n].name, 260, fd.cFileName); e[n].size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; e[n].dir = (fd.dwFileAttributes & ATTR_DIR) != 0; FILETIME lt; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &e[n].mtime); n++; } while (FindNextFileA(h, &fd));
  FindClose(h);
  for (int i = 1; i < n; i++) { Entry t = e[i]; int j = i - 1; while (j >= 0 && sicmp(e[j].name, t.name) > 0) { e[j + 1] = e[j]; j--; } e[j + 1] = t; }
  *out = e; return n;
}

/* ------------------------------------------------------------------ external programs ------------------------------------------------------------------ */
static int try_exe(const char *base, char *out, int cap) {
  static const char *exts[] = { "", ".com", ".exe", ".bat", ".cmd", 0 };
  const char *bn = basename_of(base); int has_ext = 0; for (const char *q = bn; *q; q++) if (*q == '.') has_ext = 1;
  for (int i = has_ext ? 0 : 1; exts[i]; i++) { sfmt(out, cap, "%s%s", base, exts[i]); DWORD a = GetFileAttributesA(out); if (a != 0xFFFFFFFFu && !(a & ATTR_DIR)) return 1; }
  return 0;
}
static int find_program(const char *name, char *out, int cap) {
  char n[1024]; scpy(n, sizeof n, name); unquote(n);
  int has_sep = 0; for (const char *q = n; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') has_sep = 1;
  if (has_sep) return try_exe(n, out, cap);
  if (try_exe(n, out, cap)) return 1;
  char path[8192]; getenv_s("PATH", path, sizeof path);
  for (char *p = path; *p;) { char *e = p; while (*e && *e != ';') e++; char save = *e; *e = 0; if (*p) { char cand[1200]; char dir[1100]; scpy(dir, sizeof dir, p); unquote(dir); join_path(cand, sizeof cand, dir, n); if (try_exe(cand, out, cap)) return 1; } if (!save) break; p = e + 1; }
  return 0;
}
static int is_batch(const char *path) { int n = slen(path); return n > 4 && (sieq(path + n - 4, ".bat") || sieq(path + n - 4, ".cmd")); }
static void make_inheritable(HANDLE h) { SetHandleInformation(h, 1, 1); }
static int spawn(const char *app, const char *cmdline, HANDLE in, HANDLE out, HANDLE err, int wait, HANDLE *proc_out) {
  STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = 0x100; si.hStdInput = in; si.hStdOutput = out; si.hStdError = err;
  PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
  char cl[8192]; scpy(cl, sizeof cl, cmdline);
  if (!CreateProcessA(app, cl, 0, 0, 1, 0, 0, 0, &si, &pi)) { DWORD e = GetLastError(); errf("%s\r\n", win_error(e)); return e == 2 || e == 3 ? 9009 : 1; }
  CloseHandle(pi.hThread);
  if (!wait) { if (proc_out) *proc_out = pi.hProcess; else CloseHandle(pi.hProcess); return 0; }
  WaitForSingleObject(pi.hProcess, 0xFFFFFFFFu); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); return (int)code;
}

/* ------------------------------------------------------------------ builtins ------------------------------------------------------------------ */
static int run_line(const char *line);
static int run_batch_file(const char *path, const char *args);
static int builtin_dispatch(const char *cmd, const char *args, const char *raw_after_cmd);
int run_batch_ctx(Batch *b);

static int bi_echo(const char *raw) {   /* raw = everything after "echo" */
  if (*raw == '.' || *raw == ':' || *raw == '/' || *raw == '\\' || *raw == '[' || *raw == ']' || *raw == '+' || *raw == '(') { outf("%s\r\n", raw + 1); return 0; }
  const char *p = raw; if (is_space(*p)) p++;   /* exactly one separator is eaten; "echo   x" prints "  x" like cmd */
  char t[4096]; scpy(t, sizeof t, p); trim(t);
  if (!t[0]) { outf("ECHO is %s.\r\n", g_echo ? "on" : "off"); return 0; }
  if (sieq(t, "on")) { g_echo = 1; return 0; } if (sieq(t, "off")) { g_echo = 0; return 0; }
  outf("%s\r\n", p); return 0;
}
static int bi_cd(const char *args) {
  char a[1024]; const char *p = args; skip_ws(&p); if (snieq(p, "/d", 2) && (is_space(p[2]) || !p[2])) { p += 2; skip_ws(&p); }
  scpy(a, sizeof a, p); trim(a); unquote(a);
  if (!a[0]) { char d[1024]; cwd(d, sizeof d); outf("%s\r\n", d); return 0; }
  if (a[1] == ':' && !a[2]) { if (lc(a[0]) == 'c') return 0; errs("The system cannot find the drive specified.\r\n"); return 1; }
  if (!SetCurrentDirectoryA(a)) { errs("The system cannot find the path specified.\r\n"); return 1; }
  return 0;
}
static int bi_dir(const char *args) {
  int bare = 0, only_dirs = 0, no_dirs = 0; char pats[16][1024]; int np = 0; char t[1024];
  for (const char *p = args; ; ) { p = next_arg(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') { char o = lc(t[1]); if (o == 'b') bare = 1; else if (o == 'a') { const char *f = t + 2; if (*f == ':') f++; if (lc(*f) == 'd') only_dirs = 1; if (*f == '-' && lc(f[1]) == 'd') no_dirs = 1; } } else if (np < 16) { scpy(pats[np], 1024, t); unquote(pats[np]); np++; } }
  if (!np) { pats[0][0] = 0; np = 1; }
  int nf = 0, nd = 0, rc = 0, header_done = 0; u64 total = 0;
  for (int pi = 0; pi < np; pi++) {
    const char *pat = pats[pi]; Entry *e; int n = list_dir(pat, &e);
    char dir[1024]; if (pat[0] && !has_wild(pat) && is_dir(pat)) fullpath(pat, dir, sizeof dir); else if (pat[0]) { char tmp[1024]; dirname_of(pat, tmp, sizeof tmp); fullpath(tmp[0] ? tmp : ".", dir, sizeof dir); } else cwd(dir, sizeof dir);
    if (!bare && !header_done) { outf(" Volume in drive C is Nebula\r\n Volume Serial Number is 4E42-554C\r\n\r\n Directory of %s\r\n\r\n", dir); header_done = 1; }
    if (!n) { errs("File Not Found\r\n"); rc = 1; xfree(e); continue; }
    for (int i = 0; i < n; i++) { if ((only_dirs && !e[i].dir) || (no_dirs && e[i].dir)) continue;
      if (bare) { outf("%s\r\n", e[i].name); continue; }
      SYSTEMTIME *m = &e[i].mtime; int h12 = m->wHour % 12; if (!h12) h12 = 12;
      if (e[i].dir) { outf("%02d/%02d/%d  %02d:%02d %s    <DIR>          %s\r\n", m->wMonth, m->wDay, m->wYear, h12, m->wMinute, m->wHour < 12 ? "AM" : "PM", e[i].name); nd++; }
      else { char sz[32]; sfmt(sz, sizeof sz, "%N", (s64)e[i].size); outf("%02d/%02d/%d  %02d:%02d %s    %14s %s\r\n", m->wMonth, m->wDay, m->wYear, h12, m->wMinute, m->wHour < 12 ? "AM" : "PM", sz, e[i].name); nf++; total += e[i].size; } }
    xfree(e);
  }
  if (!bare && (nf || nd)) { char sz[32]; sfmt(sz, sizeof sz, "%N", (s64)total); outf("%16d File(s) %14s bytes\r\n%16d Dir(s)  %14s bytes free\r\n", nf, sz, nd, "1,073,741,824"); }
  return rc;
}
static int bi_type(const char *args, int allow_stdin) {
  char t[1024]; int any = 0, rc = 0;
  for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') continue; any = 1; unquote(t);
    Entry *e; int n = has_wild(t) ? list_dir(t, &e) : 0;
    if (has_wild(t)) { char dir[1024]; dirname_of(t, dir, sizeof dir); for (int i = 0; i < n; i++) { if (e[i].dir) continue; char fp[1200]; join_path(fp, sizeof fp, dir, e[i].name); int len; char *b = read_file(fp, &len); if (b) { outf("\r\n%s\r\n\r\n\r\n", fp); hwrite(g_out, b, len); xfree(b); } } xfree(e); continue; }
    int len; char *b = read_file(t, &len); if (!b || is_dir(t)) { errs("The system cannot find the file specified.\r\n"); rc = 1; xfree(b); continue; } hwrite(g_out, b, len); xfree(b); }
  if (!any) { if (!allow_stdin) { errs("The syntax of the command is incorrect.\r\n"); return 1; } int len; char *b = read_all(g_in, &len); hwrite(g_out, b, len); xfree(b); }
  return rc;
}
static int copy_one(const char *src, const char *dst, int move) {
  char d[1024]; scpy(d, sizeof d, dst); if (is_dir(d)) join_path(d, sizeof d, dst, basename_of(src));
  if (move) { if (!MoveFileExA(src, d, 1)) { errf("%s\r\n", win_error(GetLastError())); return 1; } }
  else if (!CopyFileA(src, d, 0)) { errf("%s\r\n", win_error(GetLastError())); return 1; }
  return 0;
}
static int bi_copy(const char *args, int move) {
  char srcs[16][1024]; int ns = 0; char t[1024]; const char *p = args; int yes = 0;
  for (;;) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') { if (lc(t[1]) == 'y') yes = 1; continue; } if (ns < 16) { scpy(srcs[ns], sizeof srcs[ns], t); unquote(srcs[ns]); ns++; } }
  (void)yes; if (ns < 1) { errs("The syntax of the command is incorrect.\r\n"); return 1; }
  const char *dst = ns >= 2 ? srcs[ns - 1] : "."; int nsrc = ns >= 2 ? ns - 1 : 1; int copied = 0, rc = 0;
  for (int i = 0; i < nsrc; i++) {
    if (has_wild(srcs[i])) { Entry *e; int n = list_dir(srcs[i], &e); char dir[1024]; dirname_of(srcs[i], dir, sizeof dir); for (int k = 0; k < n; k++) { if (e[k].dir) continue; char fp[1200]; join_path(fp, sizeof fp, dir, e[k].name); if (!copy_one(fp, dst, move)) copied++; else rc = 1; } xfree(e); if (!n) { errs("The system cannot find the file specified.\r\n"); rc = 1; } }
    else if (move && is_dir(srcs[i])) { if (!MoveFileExA(srcs[i], dst, 1)) { errf("%s\r\n", win_error(GetLastError())); rc = 1; } else copied++; }
    else if (!exists(srcs[i])) { errs("The system cannot find the file specified.\r\n"); rc = 1; }
    else if (!copy_one(srcs[i], dst, move)) copied++; else rc = 1;
  }
  if (!g_quiet) { if (move) outf("%8d file(s) moved.\r\n", copied); else outf("%8d file(s) copied.\r\n", copied); }
  return rc;
}
static int bi_ren(const char *args) {
  char a[1024], b[1024]; const char *p = next_tok(args, a, sizeof a); next_tok(p, b, sizeof b); unquote(a); unquote(b); if (!a[0] || !b[0]) { errs("The syntax of the command is incorrect.\r\n"); return 1; }
  char dst[1200]; const char *bn = basename_of(b); if (bn != b) scpy(dst, sizeof dst, b); else { char dir[1024]; dirname_of(a, dir, sizeof dir); join_path(dst, sizeof dst, dir, b); }
  if (!MoveFileExA(a, dst, 0)) { errf("%s\r\n", win_error(GetLastError())); return 1; } return 0;
}
static int del_tree(const char *dir) { Entry *e; int n = list_dir(dir, &e); int rc = 0; for (int i = 0; i < n; i++) { char fp[1200]; join_path(fp, sizeof fp, dir, e[i].name); if (e[i].dir) rc |= del_tree(fp); else if (!DeleteFileA(fp)) rc = 1; } xfree(e); if (!RemoveDirectoryA(dir)) rc = 1; return rc; }
static int bi_del(const char *args) {
  char t[1024]; int rc = 0, any = 0;
  for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') continue; unquote(t); any = 1;
    if (is_dir(t)) { join_path(t, sizeof t, t, "*"); }
    if (has_wild(t)) { Entry *e; int n = list_dir(t, &e); char dir[1024]; dirname_of(t, dir, sizeof dir); int done = 0; for (int i = 0; i < n; i++) { if (e[i].dir) continue; char fp[1200]; join_path(fp, sizeof fp, dir, e[i].name); if (DeleteFileA(fp)) done++; } xfree(e); if (!done && !n) { errf("Could Not Find %s\r\n", t); } continue; }
    if (!DeleteFileA(t)) { char full[1024]; fullpath(t, full, sizeof full); errf("Could Not Find %s\r\n", full); rc = 0; } }
  if (!any) { errs("The syntax of the command is incorrect.\r\n"); return 1; } return rc;
}
static int mkdir_p(const char *path) {
  char t[1024]; scpy(t, sizeof t, path); int n = slen(t); while (n > 1 && (t[n - 1] == '\\' || t[n - 1] == '/')) t[--n] = 0;
  if (is_dir(t)) return 2;
  for (int i = 1; t[i]; i++) if ((t[i] == '\\' || t[i] == '/') && t[i - 1] != ':') { t[i] = 0; if (!is_dir(t)) CreateDirectoryA(t, 0); t[i] = '\\'; }
  return CreateDirectoryA(t, 0) ? 0 : 1;
}
static int bi_mkdir(const char *args) { char t[1024]; int rc = 0, any = 0; for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; unquote(t); any = 1; int r = mkdir_p(t); if (r == 2) { errf("A subdirectory or file %s already exists.\r\n", t); rc = 1; } else if (r) { errf("%s\r\n", win_error(GetLastError())); rc = 1; } } if (!any) { errs("The syntax of the command is incorrect.\r\n"); return 1; } return rc; }
static int bi_rmdir(const char *args) { char t[1024]; int rc = 0, rec = 0, any = 0; for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') { if (lc(t[1]) == 's') rec = 1; continue; } unquote(t); any = 1; if (!is_dir(t)) { errs("The system cannot find the file specified.\r\n"); rc = 1; continue; } if (rec) { if (del_tree(t)) { errs("The directory is not empty.\r\n"); rc = 1; } } else if (!RemoveDirectoryA(t)) { errf("%s\r\n", win_error(GetLastError())); rc = 1; } } if (!any) { errs("The syntax of the command is incorrect.\r\n"); return 1; } return rc; }

/* set /a expression evaluator */
static const char *ex_p; static int ex_err;
static s64 ex_expr(void);
static s64 ex_atom(void) {
  skip_ws(&ex_p);
  if (*ex_p == '(') { ex_p++; s64 v = ex_expr(); skip_ws(&ex_p); if (*ex_p == ')') ex_p++; else ex_err = 1; return v; }
  if (*ex_p == '-') { ex_p++; return -ex_atom(); } if (*ex_p == '+') { ex_p++; return ex_atom(); } if (*ex_p == '!') { ex_p++; return !ex_atom(); } if (*ex_p == '~') { ex_p++; return ~ex_atom(); }
  if (*ex_p >= '0' && *ex_p <= '9') { const char *e; s64 v = satoi(ex_p, &e); ex_p = e; return v; }
  if ((*ex_p >= 'a' && *ex_p <= 'z') || (*ex_p >= 'A' && *ex_p <= 'Z') || *ex_p == '_') { char name[128]; int i = 0; while (((*ex_p >= 'a' && *ex_p <= 'z') || (*ex_p >= 'A' && *ex_p <= 'Z') || (*ex_p >= '0' && *ex_p <= '9') || *ex_p == '_') && i < 127) name[i++] = *ex_p++; name[i] = 0; char v[256]; if (!lookup_var(name, v, sizeof v)) return 0; return satoi(v, 0); }
  ex_err = 1; return 0;
}
static s64 ex_mul(void) { s64 v = ex_atom(); for (;;) { skip_ws(&ex_p); if (*ex_p == '*') { ex_p++; v *= ex_atom(); } else if (*ex_p == '/') { ex_p++; s64 d = ex_atom(); if (!d) { ex_err = 2; return 0; } v /= d; } else if (*ex_p == '%') { ex_p++; s64 d = ex_atom(); if (!d) { ex_err = 2; return 0; } v %= d; } else return v; } }
static s64 ex_add(void) { s64 v = ex_mul(); for (;;) { skip_ws(&ex_p); if (*ex_p == '+') { ex_p++; v += ex_mul(); } else if (*ex_p == '-') { ex_p++; v -= ex_mul(); } else return v; } }
static s64 ex_shift(void) { s64 v = ex_add(); for (;;) { skip_ws(&ex_p); if (ex_p[0] == '<' && ex_p[1] == '<') { ex_p += 2; v <<= ex_add(); } else if (ex_p[0] == '>' && ex_p[1] == '>') { ex_p += 2; v >>= ex_add(); } else return v; } }
static s64 ex_bit(void) { s64 v = ex_shift(); for (;;) { skip_ws(&ex_p); if (*ex_p == '&' && ex_p[1] != '&') { ex_p++; v &= ex_shift(); } else if (*ex_p == '^') { ex_p++; v ^= ex_shift(); } else if (*ex_p == '|' && ex_p[1] != '|') { ex_p++; v |= ex_shift(); } else return v; } }
static s64 ex_expr(void) {
  skip_ws(&ex_p); const char *save = ex_p;
  /* assignment: name [op]= expr */
  if ((*ex_p >= 'a' && *ex_p <= 'z') || (*ex_p >= 'A' && *ex_p <= 'Z') || *ex_p == '_') { char name[128]; int i = 0; while (((*ex_p >= 'a' && *ex_p <= 'z') || (*ex_p >= 'A' && *ex_p <= 'Z') || (*ex_p >= '0' && *ex_p <= '9') || *ex_p == '_') && i < 127) name[i++] = *ex_p++; name[i] = 0; skip_ws(&ex_p);
    char op = 0; if (ex_p[0] == '=') { ex_p++; } else if ((ex_p[0] == '+' || ex_p[0] == '-' || ex_p[0] == '*' || ex_p[0] == '/' || ex_p[0] == '%' || ex_p[0] == '&' || ex_p[0] == '|' || ex_p[0] == '^') && ex_p[1] == '=') { op = ex_p[0]; ex_p += 2; } else { ex_p = save; goto plain; }
    s64 r = ex_expr(); if (op) { char cv[256]; s64 old = lookup_var(name, cv, sizeof cv) ? satoi(cv, 0) : 0; switch (op) { case '+': r = old + r; break; case '-': r = old - r; break; case '*': r = old * r; break; case '/': r = r ? old / r : (ex_err = 2, 0); break; case '%': r = r ? old % r : (ex_err = 2, 0); break; case '&': r = old & r; break; case '|': r = old | r; break; case '^': r = old ^ r; break; } }
    char vs[32]; sfmt(vs, sizeof vs, "%L", r); SetEnvironmentVariableA(name, vs); skip_ws(&ex_p); if (*ex_p == ',') { ex_p++; return ex_expr(); } return r; }
plain:;
  s64 v = ex_bit(); skip_ws(&ex_p); if (*ex_p == ',') { ex_p++; return ex_expr(); } return v;
}
static int bi_set(const char *args) {
  const char *p = args; skip_ws(&p);
  if (!*p) { char *env = GetEnvironmentStringsA(); for (char *q = env; *q; q += slen(q) + 1) if (*q != '=') outf("%s\r\n", q); FreeEnvironmentStringsA(env); return 0; }
  if (snieq(p, "/a", 2) && (is_space(p[2]) || !p[2])) { p += 2; char e[4096]; scpy(e, sizeof e, p); trim(e); char *w = e, *r = e; while (*r) { if (*r != '"') *w++ = *r; r++; } *w = 0; ex_p = e; ex_err = 0; s64 v = ex_expr(); if (ex_err == 2) { errs("Divide by zero error.\r\n"); return 1073750993; } if (ex_err) { errs("Invalid number.  Numeric constants are either decimal (17),\r\nhexadecimal (0x11), or octal (021).\r\n"); return 1073750988; } if (!g_batch) outf("%L", v); return 0; }
  if (snieq(p, "/p", 2) && (is_space(p[2]) || !p[2])) { p += 2; skip_ws(&p); char nm[256]; int i = 0; while (*p && *p != '=' && i < 255) nm[i++] = *p++; nm[i] = 0; trim(nm); if (*p == '=') p++; outs(p); char line[4096]; if (!read_line(g_in, line, sizeof line)) return 1; if (!line[0]) return 1; SetEnvironmentVariableA(nm, line); return 0; }
  char t[8192]; scpy(t, sizeof t, p); char *e = t + slen(t); while (e > t && is_space(e[-1])) e--; *e = 0;
  if (t[0] == '"') { unquote(t); }
  char *eq = 0; for (char *q = t; *q; q++) if (*q == '=') { eq = q; break; }
  if (!eq) { /* show variables with this prefix */ char *env = GetEnvironmentStringsA(); int n = 0, pl = slen(t); for (char *q = env; *q; q += slen(q) + 1) if (*q != '=' && snieq(q, t, pl)) { outf("%s\r\n", q); n++; } FreeEnvironmentStringsA(env); if (!n) { errf("Environment variable %s not defined\r\n", t); return 1; } return 0; }
  *eq = 0; char *nm = t; while (is_space(*nm)) nm++; if (!*nm) { errs("The syntax of the command is incorrect.\r\n"); return 1; }
  SetEnvironmentVariableA(nm, eq[1] ? eq + 1 : 0); return 0;
}
static int cmp_op(const char *op, const char *a, const char *b, int icase) {
  const char *ea, *eb; s64 ia = satoi(a, &ea), ib = satoi(b, &eb); int numeric = a[0] && b[0] && !*ea && !*eb; int c = numeric ? (ia < ib ? -1 : ia > ib) : (icase ? sicmp(a, b) : scmp(a, b));
  if (sieq(op, "EQU")) return c == 0; if (sieq(op, "NEQ")) return c != 0; if (sieq(op, "LSS")) return c < 0; if (sieq(op, "LEQ")) return c <= 0; if (sieq(op, "GTR")) return c > 0; if (sieq(op, "GEQ")) return c >= 0; return 0;
}
/* find the end of a (…) group starting at p[0]=='('; returns pointer to the ')' or 0 */
static const char *group_end(const char *p) { int depth = 0, q = 0; for (; *p; p++) { if (*p == '^' && !q && p[1]) { p++; continue; } if (*p == '"') q = !q; if (q) continue; if (*p == '(') depth++; else if (*p == ')') { if (--depth == 0) return p; } } return 0; }
static int run_body(const char *p) { return run_line(p); }   /* a (group) is handled by the operator scanner + exec_simple */
static int bi_if(const char *args) {
  const char *p = args; skip_ws(&p); int neg = 0, icase = 0; char t[4096];
  for (;;) { skip_ws(&p); if (snieq(p, "not", 3) && is_space(p[3])) { neg = !neg; p += 3; } else if (snieq(p, "/i", 2) && is_space(p[2])) { icase = 1; p += 2; } else break; }
  int cond = 0; skip_ws(&p);
  if (snieq(p, "exist", 5) && is_space(p[5])) { p = next_tok(p + 5, t, sizeof t); unquote(t); if (has_wild(t)) { Entry *e; int n = list_dir(t, &e); xfree(e); cond = n > 0; } else cond = exists(t); }
  else if (snieq(p, "errorlevel", 10) && is_space(p[10])) { p = next_tok(p + 10, t, sizeof t); cond = g_errorlevel >= satoi(t, 0); }
  else if (snieq(p, "defined", 7) && is_space(p[7])) { p = next_tok(p + 7, t, sizeof t); char vv[8192]; cond = GetEnvironmentVariableA(t, vv, sizeof vv) != 0; }
  else if (snieq(p, "cmdextversion", 13)) { p = next_tok(p + 13, t, sizeof t); cond = satoi(t, 0) <= 2; }
  else { /* a==b or a OP b */
    char a[4096]; const char *q = p; int o = 0, qq = 0; while (*q && (qq || (!is_space(*q) && !(q[0] == '=' && q[1] == '=')))) { if (*q == '"') qq = !qq; if (o + 1 < (int)sizeof a) a[o++] = *q; q++; } a[o] = 0;
    skip_ws(&q);
    if (q[0] == '=' && q[1] == '=') { q += 2; skip_ws(&q); char b[4096]; o = 0; qq = 0; while (*q && (qq || !is_space(*q))) { if (*q == '"') qq = !qq; if (o + 1 < (int)sizeof b) b[o++] = *q; q++; } b[o] = 0; cond = icase ? sieq(a, b) : seq(a, b); p = q; }
    else { char op[8]; q = next_tok(q, op, sizeof op); char b[4096]; q = next_tok(q, b, sizeof b); if (!(sieq(op, "EQU") || sieq(op, "NEQ") || sieq(op, "LSS") || sieq(op, "LEQ") || sieq(op, "GTR") || sieq(op, "GEQ"))) { errf("%s was unexpected at this time.\r\n", op); return 1; } char ua[4096], ub[4096]; scpy(ua, sizeof ua, a); scpy(ub, sizeof ub, b); unquote(ua); unquote(ub); cond = cmp_op(op, ua, ub, icase); p = q; }
  }
  if (neg) cond = !cond;
  skip_ws(&p);
  /* then-part: (group) [else (group)] or a single command to end of line */
  if (*p == '(') { const char *e = group_end(p); if (!e) { errs("The syntax of the command is incorrect.\r\n"); return 1; } char then_[8192]; int n = (int)(e - p - 1); if (n >= (int)sizeof then_) n = sizeof then_ - 1; memcpy(then_, p + 1, (u64)n); then_[n] = 0; const char *rest = e + 1; skip_ws(&rest);
    if (snieq(rest, "else", 4) && (is_space(rest[4]) || rest[4] == '(')) { rest += 4; if (cond) return run_line(then_); return run_body(rest); }
    if (cond) return run_line(then_); return g_errorlevel; }
  /* single command; an `else` after it only makes sense on one line as  cmd else cmd  (rare) — support "... else ..." split at top level */
  { const char *el = 0; int q = 0; for (const char *s = p; *s; s++) { if (*s == '"') q = !q; if (!q && is_space(*s) && snieq(s + 1, "else", 4) && (is_space(s[5]) || s[5] == '(')) { el = s; break; } }
    if (el) { char then_[8192]; int n = (int)(el - p); if (n >= (int)sizeof then_) n = sizeof then_ - 1; memcpy(then_, p, (u64)n); then_[n] = 0; if (cond) return run_line(then_); return run_body(el + 5); } }
  if (cond) return run_line(p); return g_errorlevel;
}
static void replace_var(const char *in, const char *var, const char *val, char *out, int cap) { int vl = slen(var), o = 0; for (const char *p = in; *p;) { if (*p == '%' && snieq(p + 1, var, vl) && (!g_batch || 1)) { for (const char *v = val; *v && o + 1 < cap; v++) out[o++] = *v; p += 1 + vl; continue; } if (*p == '%' && p[1] == '%' && snieq(p + 2, var, vl)) { for (const char *v = val; *v && o + 1 < cap; v++) out[o++] = *v; p += 2 + vl; continue; } if (o + 1 < cap) out[o++] = *p; p++; } out[o] = 0; }
static int bi_for(const char *args) {
  const char *p = args; skip_ws(&p); int mode_l = 0, mode_d = 0, mode_f = 0;
  for (;;) { skip_ws(&p); if (snieq(p, "/l", 2) && is_space(p[2])) { mode_l = 1; p += 2; } else if (snieq(p, "/d", 2) && is_space(p[2])) { mode_d = 1; p += 2; } else if (snieq(p, "/f", 2) && is_space(p[2])) { mode_f = 1; p += 2; } else if (snieq(p, "/r", 2) && is_space(p[2])) { p += 2; } else break; }
  skip_ws(&p); char opts[1024] = ""; if (mode_f && *p == '"') { const char *e = p + 1; while (*e && *e != '"') e++; int n = (int)(e - p - 1); if (n > 1000) n = 1000; memcpy(opts, p + 1, (u64)n); opts[n] = 0; p = *e ? e + 1 : e; skip_ws(&p); }
  if (*p != '%') { errs("The syntax of the command is incorrect.\r\n"); return 1; } p++; if (*p == '%') p++; char var[2] = { *p, 0 }; p++; skip_ws(&p);
  if (!snieq(p, "in", 2)) { errs("in was unexpected at this time.\r\n"); return 1; } p += 2; skip_ws(&p);
  if (*p != '(') { errs("( was unexpected at this time.\r\n"); return 1; } const char *e = group_end(p); if (!e) { errs("The syntax of the command is incorrect.\r\n"); return 1; }
  char set[8192]; int n = (int)(e - p - 1); if (n >= (int)sizeof set) n = sizeof set - 1; memcpy(set, p + 1, (u64)n); set[n] = 0; p = e + 1; skip_ws(&p);
  if (!snieq(p, "do", 2) || !(is_space(p[2]) || p[2] == '(')) { errs("do was unexpected at this time.\r\n"); return 1; } p += 2; skip_ws(&p);
  const char *body = p; int rc = 0; char cmd[16384];
  if (mode_l) { const char *q; s64 a = satoi(set, &q); if (*q == ',') q++; s64 st = satoi(q, &q); if (*q == ',') q++; s64 b = satoi(q, &q); if (!st) return 0; for (s64 i = a; st > 0 ? i <= b : i >= b; i += st) { char v[32]; sfmt(v, sizeof v, "%L", i); replace_var(body, var, v, cmd, sizeof cmd); rc = run_body(cmd); if (g_quit || (g_batch && (g_batch->leave || g_batch->label_goto[0]))) break; } return rc; }
  if (mode_f) {
    char delims[64] = " \t"; int tok = 1, skip = 0; char eol = ';';
    for (const char *o = opts; *o;) { if (snieq(o, "tokens=", 7)) { o += 7; if (*o == '*') tok = -1; else tok = (int)satoi(o, 0); while (*o && *o != ' ') o++; } else if (snieq(o, "delims=", 7)) { o += 7; int i = 0; while (*o && *o != ' ' && i < 63) delims[i++] = *o++; delims[i] = 0; if (!i) delims[0] = 0; } else if (snieq(o, "skip=", 5)) { o += 5; skip = (int)satoi(o, 0); while (*o && *o != ' ') o++; } else if (snieq(o, "eol=", 4)) { o += 4; eol = *o ? *o++ : 0; } else if (snieq(o, "usebackq", 8)) { o += 8; } else o++; while (*o == ' ') o++; }
    char *text = 0; int len = 0; char ts[8192]; scpy(ts, sizeof ts, set); trim(ts);
    if (ts[0] == '"') { unquote(ts); text = xalloc((u64)slen(ts) + 1); scpy(text, slen(ts) + 1, ts); len = slen(text); }
    else if (ts[0] == '\'') { /* command output */ int l = slen(ts); if (l >= 2 && ts[l - 1] == '\'') ts[l - 1] = 0; HANDLE r, w; SECURITY_ATTRIBUTES sa = { sizeof sa, 0, 1 }; if (!CreatePipe(&r, &w, &sa, 0)) return 1; SetHandleInformation(r, 1, 0); char cl[8192]; sfmt(cl, sizeof cl, "\"%s\" /c %s", g_self, ts + 1); HANDLE ph; spawn(g_self, cl, g_in, w, g_err, 0, &ph); CloseHandle(w); text = read_all(r, &len); CloseHandle(r); if (ph) { WaitForSingleObject(ph, 0xFFFFFFFFu); CloseHandle(ph); } }
    else { char fn[1024]; next_tok(ts, fn, sizeof fn); unquote(fn); text = read_file(fn, &len); if (!text) { errs("The system cannot find the file specified.\r\n"); return 1; } }
    int lineno = 0; for (char *lp = text; lp && *lp;) { char *le = lp; while (*le && *le != '\n') le++; char save = *le; *le = 0; int ll = slen(lp); if (ll && lp[ll - 1] == '\r') lp[ll - 1] = 0; char line[8192]; scpy(line, sizeof line, lp); *le = save; lp = save ? le + 1 : le; if (lineno++ < skip) continue; if (!line[0] || (eol && line[0] == eol)) continue;
      /* split into tokens by delims */ char *toks[32]; int nt = 0; char *s = line; while (*s && nt < 32) { while (*s && (delims[0] ? sstr(delims, (char[]){ *s, 0 }) != 0 : 0)) s++; if (!*s) break; toks[nt++] = s; if (!delims[0]) { s += slen(s); break; } while (*s && !sstr(delims, (char[]){ *s, 0 })) s++; if (*s) *s++ = 0; }
      char val[8192]; if (tok == -1) scpy(val, sizeof val, line); else if (tok >= 1 && tok <= nt) scpy(val, sizeof val, toks[tok - 1]); else continue;
      replace_var(body, var, val, cmd, sizeof cmd); rc = run_body(cmd); if (g_quit || (g_batch && (g_batch->leave || g_batch->label_goto[0]))) break; }
    xfree(text); return rc;
  }
  char item[1024]; for (const char *q = set; ; ) { q = next_arg(q, item, sizeof item); if (!item[0]) break; char it[1024]; scpy(it, sizeof it, item); unquote(it);
    if (has_wild(it)) { Entry *en; int cnt = list_dir(it, &en); char dir[1024]; dirname_of(it, dir, sizeof dir); for (int i = 0; i < cnt; i++) { if (mode_d ? !en[i].dir : en[i].dir) continue; char fp[1200]; join_path(fp, sizeof fp, dir, en[i].name); replace_var(body, var, fp, cmd, sizeof cmd); rc = run_body(cmd); if (g_quit || (g_batch && (g_batch->leave || g_batch->label_goto[0]))) break; } xfree(en); }
    else { replace_var(body, var, item, cmd, sizeof cmd); rc = run_body(cmd); }
    if (g_quit || (g_batch && (g_batch->leave || g_batch->label_goto[0]))) break; }
  return rc;
}
static int bi_sort(const char *args) {
  int rev = 0; char fn[1024] = ""; char t[1024]; for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') { if (lc(t[1]) == 'r') rev = 1; } else { scpy(fn, sizeof fn, t); unquote(fn); } }
  int len; char *text = fn[0] ? read_file(fn, &len) : read_all(g_in, &len); if (!text) { errs("The system cannot find the file specified.\r\n"); return 1; }
  int cap = 1024, n = 0; char **lines = xalloc(sizeof(char *) * (u64)cap); for (char *p = text; *p;) { char *e = p; while (*e && *e != '\n') e++; char *nx = *e ? e + 1 : e; if (e > p && e[-1] == '\r') e--; *e = 0; if (n == cap) { cap *= 2; char **nl = xalloc(sizeof(char *) * (u64)cap); memcpy(nl, lines, sizeof(char *) * (u64)n); xfree(lines); lines = nl; } lines[n++] = p; p = nx; }
  for (int i = 1; i < n; i++) { char *t2 = lines[i]; int j = i - 1; while (j >= 0 && (rev ? sicmp(lines[j], t2) < 0 : sicmp(lines[j], t2) > 0)) { lines[j + 1] = lines[j]; j--; } lines[j + 1] = t2; }
  for (int i = 0; i < n; i++) outf("%s\r\n", lines[i]); xfree(lines); xfree(text); return 0;
}
static int bi_find(const char *args, int findstr) {
  int icase = 0, count = 0, invert = 0, number = 0; char pats[16][512]; int np = 0; char files[16][1024]; int nf = 0; char t[1024];
  for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break;
    if (t[0] == '/') { char o = lc(t[1]); if (o == 'i') icase = 1; else if (o == 'c' && !findstr) count = 1; else if (o == 'c' && t[2] == ':') { if (np < 16) { scpy(pats[np], 512, t + 3); unquote(pats[np]); np++; } } else if (o == 'v') invert = 1; else if (o == 'n') number = 1; continue; }
    if (!np && !findstr) { scpy(pats[0], 512, t); unquote(pats[0]); np = 1; continue; }
    if (findstr && !np) { char tt[512]; scpy(tt, sizeof tt, t); unquote(tt); for (char *s = tt; *s;) { char *e = s; while (*e && *e != ' ') e++; char sv = *e; *e = 0; if (np < 16 && *s) scpy(pats[np++], 512, s); if (!sv) break; s = e + 1; } continue; }
    if (nf < 16) { scpy(files[nf], 1024, t); unquote(files[nf]); nf++; } }
  if (!np) { errs(findstr ? "FINDSTR: Bad command line\r\n" : "FIND: Parameter format not correct\r\n"); return 2; }
  int found_any = 0;
  for (int f = 0; f < (nf ? nf : 1); f++) {
    int len; char *text; const char *label = nf ? files[f] : 0;
    if (nf) { if (has_wild(files[f])) { Entry *e; int n = list_dir(files[f], &e); char dir[1024]; dirname_of(files[f], dir, sizeof dir); for (int i = 0; i < n; i++) { if (e[i].dir) continue; char fp[1200]; join_path(fp, sizeof fp, dir, e[i].name); text = read_file(fp, &len); if (!text) continue; int ln = 0, cnt = 0; for (char *p = text; *p;) { char *e2 = p; while (*e2 && *e2 != '\n') e2++; char *nx = *e2 ? e2 + 1 : e2; if (e2 > p && e2[-1] == '\r') e2--; *e2 = 0; ln++; int m = 0; for (int k = 0; k < np; k++) if (icase ? sistr(p, pats[k]) != 0 : sstr(p, pats[k]) != 0) m = 1; if (m != invert) { cnt++; found_any = 1; if (!count) { if (number) outf("%s:%d:%s\r\n", fp, ln, p); else outf("%s:%s\r\n", fp, p); } } p = nx; } xfree(text); } xfree(e); continue; }
      text = read_file(files[f], &len); if (!text) { errf("%s: Cannot open %s\r\n", findstr ? "FINDSTR" : "File not found - ", files[f]); continue; } }
    else text = read_all(g_in, &len);
    if (!findstr && !count && label) outf("\r\n---------- %s\r\n", label);
    int ln = 0, cnt = 0;
    for (char *p = text; *p;) { char *e = p; while (*e && *e != '\n') e++; char *nx = *e ? e + 1 : e; if (e > p && e[-1] == '\r') e--; *e = 0; ln++; int m = 0; for (int k = 0; k < np; k++) if (icase ? sistr(p, pats[k]) != 0 : sstr(p, pats[k]) != 0) m = 1; if (m != invert) { cnt++; found_any = 1; if (!count) { if (number) outf("%d:%s\r\n", ln, p); else if (findstr && nf > 1) outf("%s:%s\r\n", label, p); else outf("%s\r\n", p); } } p = nx; }
    if (count) { if (label) outf("\r\n---------- %s: %d\r\n", label, cnt); else outf("%d\r\n", cnt); }
    xfree(text);
  }
  return found_any ? 0 : 1;
}
static int bi_where(const char *args) { char t[1024]; int rc = 1; for (const char *p = args; ; ) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/') continue; char out[1024]; if (find_program(t, out, sizeof out)) { char full[1024]; fullpath(out, full, sizeof full); outf("%s\r\n", full); rc = 0; } else errf("INFO: Could not find files for the given pattern(s).\r\n"); } return rc; }
static int bi_start(const char *args) {
  const char *p = args; char t[1024]; char title[1024] = ""; int wait = 0;
  for (;;) { skip_ws(&p); if (*p == '"') { const char *q = next_tok(p, t, sizeof t); if (!title[0]) { scpy(title, sizeof title, t); p = q; continue; } break; } if (*p == '/') { p = next_tok(p, t, sizeof t); if (sieq(t, "/wait")) wait = 1; continue; } break; }
  skip_ws(&p); if (!*p) return 0;
  char line[8192]; scpy(line, sizeof line, p); char first[1024]; next_tok(line, first, sizeof first); char exe[1024];
  if (find_program(first, exe, sizeof exe) && !is_batch(exe)) { HANDLE ph = 0; int rc = spawn(exe, line, g_in, g_out, g_err, wait, &ph); if (ph) CloseHandle(ph); return rc; }
  char cl[8192]; sfmt(cl, sizeof cl, "\"%s\" /c %s", g_self, line); HANDLE ph = 0; int rc = spawn(g_self, cl, g_in, g_out, g_err, wait, &ph); if (ph) CloseHandle(ph); return rc;
}
static int bi_help(void) { outs("For more information on a specific command, type HELP command-name\r\nCD CHDIR CLS COPY DATE DEL DIR ECHO ERASE EXIT FIND FINDSTR FOR GOTO IF MD MKDIR MORE MOVE PATH PAUSE POPD PROMPT\r\nPUSHD RD REM REN RENAME RMDIR SET SETLOCAL SHIFT SORT START TIME TITLE TYPE VER VOL WHERE\r\n(Nebula x64 cmd.exe — a small re-implementation; external programs are searched in %PATH%)\r\n"); return 0; }

static int builtin_dispatch(const char *cmd, const char *args, const char *raw_after) {
  if (sieq(cmd, "echo")) return bi_echo(raw_after);
  if (sieq(cmd, "cd") || sieq(cmd, "chdir")) return bi_cd(args);
  if (sieq(cmd, "dir")) return bi_dir(args);
  if (sieq(cmd, "type")) return bi_type(args, 0);
  if (sieq(cmd, "more")) return bi_type(args, 1);
  if (sieq(cmd, "copy")) return bi_copy(args, 0);
  if (sieq(cmd, "move")) return bi_copy(args, 1);
  if (sieq(cmd, "ren") || sieq(cmd, "rename")) return bi_ren(args);
  if (sieq(cmd, "del") || sieq(cmd, "erase")) return bi_del(args);
  if (sieq(cmd, "mkdir") || sieq(cmd, "md")) return bi_mkdir(args);
  if (sieq(cmd, "rmdir") || sieq(cmd, "rd")) return bi_rmdir(args);
  if (sieq(cmd, "set")) return bi_set(args);
  if (sieq(cmd, "path")) { const char *p = args; skip_ws(&p); if (!*p) { char v[8192]; getenv_s("PATH", v, sizeof v); outf("PATH=%s\r\n", v); return 0; } if (*p == '=') p++; SetEnvironmentVariableA("PATH", p); return 0; }
  if (sieq(cmd, "prompt")) { const char *p = args; skip_ws(&p); SetEnvironmentVariableA("PROMPT", *p ? p : 0); return 0; }
  if (sieq(cmd, "if")) return bi_if(args);
  if (sieq(cmd, "for")) return bi_for(args);
  if (sieq(cmd, "rem") || sieq(cmd, "::")) return g_errorlevel;
  if (sieq(cmd, "cls")) { outs("\x1b[2J\x1b[H"); return 0; }
  if (sieq(cmd, "ver")) { outs("\r\nMicrosoft Windows [Version 10.0.19045.6466] (Nebula x64 cmd)\r\n"); return 0; }
  if (sieq(cmd, "vol")) { outs(" Volume in drive C is Nebula\r\n Volume Serial Number is 4E42-554C\r\n"); return 0; }
  if (sieq(cmd, "title") || sieq(cmd, "color") || sieq(cmd, "mode") || sieq(cmd, "chcp") || sieq(cmd, "verify") || sieq(cmd, "break")) { if (sieq(cmd, "chcp")) outs("Active code page: 65001\r\n"); return 0; }
  if (sieq(cmd, "pause")) { outs("Press any key to continue . . . "); char l[256]; read_line(g_in, l, sizeof l); outs("\r\n"); return 0; }
  if (sieq(cmd, "date")) { SYSTEMTIME t; GetLocalTime(&t); static const char *dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }; outf("%s%s %02d/%02d/%d\r\n", sistr(args, "/t") ? "" : "The current date is: ", dn[t.wDayOfWeek % 7], t.wMonth, t.wDay, t.wYear); return 0; }
  if (sieq(cmd, "time")) { SYSTEMTIME t; GetLocalTime(&t); if (sistr(args, "/t")) outf("%02d:%02d\r\n", t.wHour, t.wMinute); else outf("The current time is: %02d:%02d:%02d.%02d\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds / 10); return 0; }
  if (sieq(cmd, "timeout") || sieq(cmd, "sleep")) { char t[64]; const char *p = args; s64 secs = 0; for (;;) { p = next_tok(p, t, sizeof t); if (!t[0]) break; if (t[0] == '/' && lc(t[1]) == 't') { p = next_tok(p, t, sizeof t); secs = satoi(t, 0); } else if (t[0] != '/') secs = satoi(t, 0); } if (secs > 0) Sleep((DWORD)(secs * 1000)); return 0; }
  if (sieq(cmd, "pushd")) { char d[520]; cwd(d, sizeof d); const char *p = args; skip_ws(&p); if (*p) { char a[1024]; scpy(a, sizeof a, p); trim(a); unquote(a); if (!SetCurrentDirectoryA(a)) { errs("The system cannot find the path specified.\r\n"); return 1; } } if (g_npushd < 16) scpy(g_pushd[g_npushd++], 520, d); return 0; }
  if (sieq(cmd, "popd")) { if (g_npushd) SetCurrentDirectoryA(g_pushd[--g_npushd]); return 0; }
  if (sieq(cmd, "sort")) return bi_sort(args);
  if (sieq(cmd, "find")) return bi_find(args, 0);
  if (sieq(cmd, "findstr")) return bi_find(args, 1);
  if (sieq(cmd, "where")) return bi_where(args);
  if (sieq(cmd, "start")) return bi_start(args);
  if (sieq(cmd, "help")) return bi_help();
  if (sieq(cmd, "setlocal") || sieq(cmd, "endlocal")) return 0;
  if (sieq(cmd, "shift")) { if (g_batch && g_batch->argc > 1) { for (int i = 1; i + 1 < g_batch->argc; i++) g_batch->argv[i] = g_batch->argv[i + 1]; g_batch->argc--; } return g_errorlevel; }
  if (sieq(cmd, "goto")) { if (!g_batch) return 1; char l[128]; next_tok(args, l, sizeof l); if (l[0] == ':') { if (sieq(l, ":eof")) { g_batch->leave = 1; return g_errorlevel; } memmove(l, l + 1, (u64)slen(l)); } scpy(g_batch->label_goto, sizeof g_batch->label_goto, l); return g_errorlevel; }
  if (sieq(cmd, "exit")) { const char *p = args; skip_ws(&p); int b = 0; if (snieq(p, "/b", 2)) { b = 1; p += 2; skip_ws(&p); } int code = *p ? (int)satoi(p, 0) : g_errorlevel; if (b && g_batch) { g_batch->leave = 1; return code; } g_quit = 1; return code; }
  if (sieq(cmd, "call")) { const char *p = args; skip_ws(&p); if (*p == ':') { if (!g_batch) return 1; char l[128]; const char *rest = next_tok(p, l, sizeof l); /* run the current batch from the label with new arguments */ Batch nb; memset(&nb, 0, sizeof nb); nb.text = g_batch->text; nb.len = g_batch->len; nb.up = g_batch; nb.argv[0] = g_batch->argv[0]; nb.argc = 1; static char argbuf[64][8192]; char *ab = argbuf[g_depth & 63]; scpy(ab, 8192, rest); for (char *q = ab; ; ) { char tk[1024]; const char *nq = next_tok(q, tk, sizeof tk); if (!tk[0]) break; int n = slen(tk); char *slot = q; memcpy(slot, tk, (u64)n + 1); nb.argv[nb.argc++] = slot; q = (char *)nq; if (nb.argc >= 63) break; } scpy(nb.label_goto, sizeof nb.label_goto, l + 1); Batch *save = g_batch; g_batch = &nb; g_depth++; int rc = run_batch_ctx(&nb); g_depth--; g_batch = save; return rc; }
    char ex2[16384]; expand(p, ex2, sizeof ex2); p = ex2;   /* call performs a second expansion pass */
    char first[1024]; const char *rest = next_tok(p, first, sizeof first); char exe[1024]; if (find_program(first, exe, sizeof exe) && is_batch(exe)) return run_batch_file(exe, rest); return run_line(p); }
  return -1;
}

/* ------------------------------------------------------------------ command execution ------------------------------------------------------------------ */
typedef struct { char text[8192]; char in[1024], out[1024]; int append, err_to_out, out_to_err; char err[1024]; int err_append; } Simple;
static void parse_redirs(const char *seg, Simple *s) {
  memset(s, 0, sizeof *s); int o = 0, q = 0;
  for (const char *p = seg; *p;) {
    if (*p == '"') { q = !q; s->text[o++] = *p++; continue; }
    if (q) { s->text[o++] = *p++; continue; }
    int fdn = 1; const char *r = p; if ((*r == '1' || *r == '2') && (r[1] == '>' )) { fdn = *r - '0'; r++; }
    if (*r == '<') { r++; skip_ws(&r); char t[1024]; r = next_tok(r, t, sizeof t); unquote(t); scpy(s->in, sizeof s->in, t); p = r; while (o && s->text[o - 1] == ' ') o--; s->text[o++] = ' '; continue; }
    if (*r == '>') { r++; int app = 0; if (*r == '>') { app = 1; r++; } skip_ws(&r);
      if (*r == '&' && (r[1] == '1' || r[1] == '2')) { if (fdn == 2 && r[1] == '1') s->err_to_out = 1; if (fdn == 1 && r[1] == '2') s->out_to_err = 1; r += 2; }
      else { char t[1024]; r = next_tok(r, t, sizeof t); unquote(t); if (fdn == 2) { scpy(s->err, sizeof s->err, t); s->err_append = app; } else { scpy(s->out, sizeof s->out, t); s->append = app; } }
      p = r; while (o && s->text[o - 1] == ' ') o--; s->text[o++] = ' '; continue; }
    s->text[o++] = *p++; if (o >= (int)sizeof s->text - 2) break;
  }
  s->text[o] = 0; trim(s->text);
}
static HANDLE open_redir(const char *path, int write, int append) {
  SECURITY_ATTRIBUTES sa = { sizeof sa, 0, 1 };
  if (!write) { HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_RW, &sa, OPEN_EXISTING, 0x80, 0); if (h == INVALID) errs("The system cannot find the file specified.\r\n"); return h; }
  HANDLE h = CreateFileA(path, GENERIC_WRITE | (append ? 4u : 0u), FILE_SHARE_RW, &sa, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0x80, 0); if (h == INVALID) { errs("The system cannot find the path specified.\r\n"); return h; }
  if (append) SetFilePointerEx(h, 0, 0, FILE_END);
  return h;
}
/* run one simple command (builtin or external) with the given std handles; returns exit code */
static int exec_simple(const char *seg, HANDLE in, HANDLE out, HANDLE err) {
  Simple s; parse_redirs(seg, &s);
  HANDLE hin = in, hout = out, herr = err, cin = 0, cout = 0, cerr = 0; int rc = 0;
  if (s.in[0]) { cin = open_redir(s.in, 0, 0); if (cin == INVALID) return 1; hin = cin; }
  if (s.out[0]) { cout = open_redir(s.out, 1, s.append); if (cout == INVALID) { if (cin) CloseHandle(cin); return 1; } hout = cout; }
  if (s.err[0]) { cerr = open_redir(s.err, 1, s.err_append); if (cerr == INVALID) { if (cin) CloseHandle(cin); if (cout) CloseHandle(cout); return 1; } herr = cerr; }
  if (s.err_to_out) herr = hout; if (s.out_to_err) hout = herr;
  const char *p = s.text; skip_ws(&p);
  if (!*p) rc = g_errorlevel;
  else if (*p == '(') { const char *e = group_end(p); if (!e) { errs("The syntax of the command is incorrect.\r\n"); rc = 1; } else { char inner[8192]; int n = (int)(e - p - 1); if (n >= (int)sizeof inner) n = sizeof inner - 1; memcpy(inner, p + 1, (u64)n); inner[n] = 0; HANDLE si = g_in, so = g_out, se = g_err; g_in = hin; g_out = hout; g_err = herr; rc = run_line(inner); g_in = si; g_out = so; g_err = se; } }
  else {
    char name[1024]; int i = 0; if (*p == '"') { p++; while (*p && *p != '"' && i < 1023) name[i++] = *p++; if (*p == '"') p++; name[i] = 0; }
    else { while (*p && !is_space(*p) && *p != '(' && *p != '=' && *p != ',' && *p != ';' && i < 1023) name[i++] = *p++; name[i] = 0;
      /* "echo.", "cd..", "cd\", "dir/b", "::comment": split the internal command from the separator that follows it */
      static const char *splitters[] = { "echo", "cd", "chdir", "dir", "type", "rem", "md", "mkdir", "rd", "rmdir", "del", "erase", "copy", "move", "set", "ver", "cls", "vol", "path", "popd", "pushd", "::", 0 };
      for (int k = 0; splitters[k]; k++) { int l = slen(splitters[k]); if (i > l && snieq(name, splitters[k], l) && (name[l] == '.' || name[l] == ':' || name[l] == '/' || name[l] == '\\' || name[l] == '[' || name[l] == ']' || name[l] == '+')) { if (!seq(splitters[k], "::") || 1) { p -= i - l; i = l; name[i] = 0; } break; } } }
    const char *raw_after = p; char args[8192]; scpy(args, sizeof args, p); trim(args);
    /* strip a trailing argument-separator after the command word (cmd allows "dir,x" "cd\dir") */
    char lname[1024]; scpy(lname, sizeof lname, name); for (char *q = lname; *q; q++) *q = lc(*q);
    HANDLE si = g_in, so = g_out, se = g_err; g_in = hin; g_out = hout; g_err = herr;
    if (lname[0] == ':' && lname[1] == ':') rc = g_errorlevel;
    else { rc = builtin_dispatch(name, args, raw_after);
      if (rc == -1) { char exe[1024];
        if (!find_program(name, exe, sizeof exe)) { errf("'%s' is not recognized as an internal or external command,\r\noperable program or batch file.\r\n", name); rc = 9009; }
        else if (is_batch(exe)) rc = run_batch_file(exe, args);
        else rc = spawn(exe, s.text, hin, hout, herr, 1, 0); } }
    g_in = si; g_out = so; g_err = se;
  }
  if (cin) CloseHandle(cin); if (cout) CloseHandle(cout); if (cerr) CloseHandle(cerr);
  return rc;
}
static int is_builtin_name(const char *seg) { char n[1024]; const char *p = seg; skip_ws(&p); int i = 0; while (*p && !is_space(*p) && *p != '.' && *p != '(' && i < 1023) n[i++] = *p++; n[i] = 0; static const char *bi[] = { "echo", "dir", "type", "more", "set", "cd", "chdir", "if", "for", "sort", "find", "findstr", "where", "ver", "vol", "date", "time", "copy", "move", "del", "erase", "ren", "rename", "mkdir", "md", "rmdir", "rd", "path", "call", "help", "pushd", "popd", "cls", "rem", 0 }; for (int k = 0; bi[k]; k++) if (sieq(n, bi[k])) return 1; char exe[1024]; return find_program(n, exe, sizeof exe) && is_batch(exe); }
static int run_pipeline(char **segs, int n) {
  HANDLE prev_r = 0; HANDLE procs[64]; int np = 0; int rc = 0;
  for (int i = 0; i < n; i++) {
    HANDLE r = 0, w = 0; if (i + 1 < n) { SECURITY_ATTRIBUTES sa = { sizeof sa, 0, 1 }; if (!CreatePipe(&r, &w, &sa, 0)) { errs("The pipe has been ended.\r\n"); return 1; } SetHandleInformation(r, 1, 0); }
    HANDLE in = prev_r ? prev_r : g_in, out = w ? w : g_out;
    if (prev_r) SetHandleInformation(prev_r, 1, 1);   /* the previous stage's read end is this child's stdin: it must be inheritable now */
    /* each stage runs as a child `cmd /c <stage>` when it is a builtin or has redirections, else the program directly */
    Simple s; parse_redirs(segs[i], &s); char first[1024]; next_tok(s.text, first, sizeof first); char exe[1024]; int direct = !is_builtin_name(segs[i]) && !s.in[0] && !s.out[0] && !s.err[0] && !s.err_to_out && find_program(first, exe, sizeof exe) && !is_batch(exe);
    HANDLE ph = 0;
    if (direct) rc = spawn(exe, s.text, in, out, g_err, 0, &ph);
    else { char cl[8192]; sfmt(cl, sizeof cl, "\"%s\" /c %s", g_self, segs[i]); rc = spawn(g_self, cl, in, out, g_err, 0, &ph); }
    if (ph && np < 64) procs[np++] = ph;
    if (prev_r) CloseHandle(prev_r); if (w) CloseHandle(w); prev_r = r;
    if (!ph) { if (r) { CloseHandle(r); prev_r = 0; } break; }
  }
  if (prev_r) CloseHandle(prev_r);
  DWORD code = (DWORD)rc; for (int i = 0; i < np; i++) { WaitForSingleObject(procs[i], 0xFFFFFFFFu); GetExitCodeProcess(procs[i], &code); CloseHandle(procs[i]); }
  return (int)code;
}
/* split a line at top-level operators and execute with && || & semantics */
static int g_nest;
static int run_line_inner(const char *line);
static int run_line(const char *line) { if (g_nest > 48) { errs("****** Batch recursion exceeds stack limits ******\r\n"); return 1; } g_nest++; int rc = run_line_inner(line); g_nest--; return rc; }
static int run_line_inner(const char *line) {
  const char *p = line; skip_ws(&p); if (*p == '@') { p++; skip_ws(&p); }
  if (!*p) return g_errorlevel;
  if (g_depth > 200) { errs("****** Batch recursion exceeds stack limits ******\r\n"); return 1; }
  /* commands that swallow the rest of the line (up to a newline of a multi-line block) */
  { char w[16]; int i = 0; const char *q = p; while (*q && !is_space(*q) && *q != '(' && i < 15) w[i++] = *q++; w[i] = 0;
    if (sieq(w, "rem") || (w[0] == ':' && w[1] == ':')) { const char *nl = q; while (*nl && *nl != '\n') nl++; return *nl ? run_line(nl + 1) : g_errorlevel; }
    if (sieq(w, "if") || sieq(w, "for")) { int rc = builtin_dispatch(w, q, q); g_errorlevel = rc; return rc; } }
  /* a leading group: run it, then continue with the operator after it */
  char seg[8192]; int o = 0; int q = 0; const char *s = p; const char *rest = 0; int op = 0;   /* op: 1 '&', 2 '&&', 3 '||', 4 '|' */
  char *segs[64]; int nseg = 0; static char segbuf[64][8192]; int depth = 0;
  for (;; s++) {
    char c = *s;
    if (c == '^' && !q && s[1]) { s++; seg[o++] = *s; continue; }
    if (c == '"') q = !q;
    if (!q) { if (c == '(') depth++; else if (c == ')') { if (depth > 0) depth--; }
      if (depth == 0) { if (c == '&' && s[1] == '&') { op = 2; rest = s + 2; break; } if (c == '|' && s[1] == '|') { op = 3; rest = s + 2; break; } if (c == '&' || c == '\n') { op = 1; rest = s + 1; break; } if (c == '|') { op = 4; rest = s + 1; break; } } }
    if (!c) break;
    if (o < (int)sizeof seg - 1) seg[o++] = c;
  }
  seg[o] = 0;
  if (op == 4) { /* collect pipeline stages */
    scpy(segbuf[0], 8192, seg); segs[nseg++] = segbuf[0];
    const char *t = rest; for (;;) { skip_ws(&t); int oo = 0, qq = 0, dd = 0; const char *u = t; int nop = 0; const char *nrest = 0;
      for (;; u++) { char c = *u; if (c == '^' && !qq && u[1]) { u++; segbuf[nseg][oo++] = *u; continue; } if (c == '"') qq = !qq; if (!qq) { if (c == '(') dd++; else if (c == ')') { if (dd > 0) dd--; } if (dd == 0) { if (c == '&' && u[1] == '&') { nop = 2; nrest = u + 2; break; } if (c == '|' && u[1] == '|') { nop = 3; nrest = u + 2; break; } if (c == '&' || c == '\n') { nop = 1; nrest = u + 1; break; } if (c == '|') { nop = 4; nrest = u + 1; break; } } } if (!c) break; if (oo < 8191) segbuf[nseg][oo++] = c; }
      segbuf[nseg][oo] = 0; segs[nseg] = segbuf[nseg]; nseg++; if (nop == 4 && nseg < 63) { t = nrest; continue; } op = nop; rest = nrest; break; }
    int rc = run_pipeline(segs, nseg); g_errorlevel = rc;
    if (g_quit || !rest) return rc;
    if (op == 1 || (op == 2 && rc == 0) || (op == 3 && rc != 0)) return run_line(rest);
    return rc;
  }
  int rc = exec_simple(seg, g_in, g_out, g_err); g_errorlevel = rc;
  if (g_quit || !rest || (g_batch && (g_batch->leave || g_batch->label_goto[0]))) return rc;
  if (op == 1 || (op == 2 && rc == 0) || (op == 3 && rc != 0)) return run_line(rest);
  return rc;
}

/* ------------------------------------------------------------------ batch files ------------------------------------------------------------------ */
static int paren_depth(const char *s) { int d = 0, q = 0; const char *p = s; skip_ws(&p); { char w[8]; int i = 0; const char *t = p; while (*t && !is_space(*t) && i < 7) w[i++] = *t++; w[i] = 0; if (sieq(w, "rem")) return 0; } for (; *p; p++) { if (*p == '^' && !q && p[1]) { p++; continue; } if (*p == '"') q = !q; if (q) continue; if (*p == '(') d++; else if (*p == ')') d--; } return d; }
static int find_label(Batch *b, const char *label) {
  for (int i = 0; i < b->len;) { int j = i; while (j < b->len && b->text[j] != '\n') j++; int k = i; while (k < j && is_space(b->text[k])) k++; if (k < j && b->text[k] == ':') { k++; int e = k; while (e < j && !is_space(b->text[e])) e++; int n = e - k; if (n == slen(label) && snieq(b->text + k, label, n)) return j < b->len ? j + 1 : j; } i = j + 1; }
  return -1;
}
int run_batch_ctx(Batch *b) {
  int rc = g_errorlevel; char line[16384], ex[16384];
  if (b->label_goto[0]) { int pos = find_label(b, b->label_goto); b->label_goto[0] = 0; if (pos < 0) { errs("The system cannot find the batch label specified\r\n"); return 1; } b->pos = pos; }
  while (b->pos < b->len && !b->leave && !g_quit) {
    int j = b->pos; while (j < b->len && b->text[j] != '\n') j++; int n = j - b->pos; if (n >= (int)sizeof line) n = sizeof line - 1; memcpy(line, b->text + b->pos, (u64)n); line[n] = 0; b->pos = j + 1;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
    /* a block that opens "(" and does not close it on this line continues on the following lines (joined with newlines = '&') */
    while (paren_depth(line) > 0 && b->pos < b->len) { int k = b->pos; while (k < b->len && b->text[k] != '\n') k++; int m = k - b->pos; if (n + 1 + m >= (int)sizeof line) m = sizeof line - 2 - n; if (m < 0) m = 0; line[n++] = '\n'; memcpy(line + n, b->text + b->pos, (u64)m); n += m; line[n] = 0; b->pos = k + 1; while (n && line[n - 1] == '\r') line[--n] = 0; }
    const char *p = line; skip_ws(&p); if (!*p || *p == ':') continue;
    int silent = *p == '@'; if (silent) { p++; skip_ws(&p); }
    expand(p, ex, sizeof ex);
    if (g_echo && !silent) { char d[520]; cwd(d, sizeof d); char shown[16384]; int o = 0; for (const char *q = ex; *q && o < (int)sizeof shown - 4; q++) { if (*q == '\n') { shown[o++] = ' '; shown[o++] = '&'; shown[o++] = ' '; } else shown[o++] = *q; } shown[o] = 0; outf("\r\n%s>%s\r\n", d, shown); }
    rc = run_line(ex);
    if (b->label_goto[0]) { int pos = find_label(b, b->label_goto); b->label_goto[0] = 0; if (pos < 0) { errf("The system cannot find the batch label specified - %s\r\n", b->label_goto); b->leave = 1; rc = 1; break; } b->pos = pos; }
  }
  return rc;
}
static int run_batch_file(const char *path, const char *args) {
  int len; char *text = read_file(path, &len); if (!text) { errs("The system cannot find the file specified.\r\n"); return 1; }
  Batch b; memset(&b, 0, sizeof b); b.text = text; b.len = len; b.up = g_batch;
  char *argbuf = xalloc(8192); char self[1024]; scpy(self, sizeof self, path); b.argv[0] = self; b.argc = 1;
  scpy(argbuf, 8192, args ? args : ""); for (char *q = argbuf; b.argc < 63; ) { char tk[1024]; const char *nq = next_tok(q, tk, sizeof tk); if (!tk[0]) break; memcpy(q, tk, (u64)slen(tk) + 1); b.argv[b.argc++] = q; q = (char *)nq; }
  Batch *save = g_batch; g_batch = &b; g_depth++; int save_echo = g_echo;
  int rc = run_batch_ctx(&b);
  g_depth--; g_batch = save; if (!save) g_echo = save_echo;
  xfree(argbuf); xfree(text); return rc;
}

/* ------------------------------------------------------------------ main ------------------------------------------------------------------ */
static void prompt(void) {
  char d[520]; cwd(d, sizeof d); char pr[256]; getenv_s("PROMPT", pr, sizeof pr); if (!pr[0]) { outf("%s>", d); return; }
  char out[1024]; int o = 0; for (const char *p = pr; *p && o < 1000; p++) { if (*p == '$' && p[1]) { p++; char c = lc(*p); if (c == 'p') { for (const char *q = d; *q; q++) out[o++] = *q; } else if (c == 'g') out[o++] = '>'; else if (c == 'l') out[o++] = '<'; else if (c == 'n') out[o++] = 'C'; else if (c == '_') { out[o++] = '\r'; out[o++] = '\n'; } else if (c == '$') out[o++] = '$'; else if (c == 's') out[o++] = ' '; else if (c == 'b') out[o++] = '|'; else if (c == 'q') out[o++] = '='; else if (c == 'e') out[o++] = 27; } else out[o++] = *p; } out[o] = 0; outs(out);
}
void mainCRTStartup(void) {
  g_con_in = g_in = GetStdHandle((DWORD)-10); g_con_out = g_out = GetStdHandle((DWORD)-11); g_con_err = g_err = GetStdHandle((DWORD)-12);
  g_rand = GetTickCount() ^ 0x5bd1e995u;
  const char *cl = GetCommandLineA(); char tok[1024]; const char *p = next_tok(cl, tok, sizeof tok);
  { char t[1024]; scpy(t, sizeof t, tok); unquote(t); char exe[1024]; if (!find_program(t, exe, sizeof exe)) scpy(exe, sizeof exe, t); fullpath(exe, g_self, sizeof g_self); if (!exists(g_self)) scpy(g_self, sizeof g_self, "C:\\Windows\\System32\\cmd.exe"); }
  { char v[8]; if (!GetEnvironmentVariableA("COMSPEC", v, sizeof v)) SetEnvironmentVariableA("COMSPEC", g_self); if (!GetEnvironmentVariableA("PATHEXT", v, sizeof v)) SetEnvironmentVariableA("PATHEXT", ".COM;.EXE;.BAT;.CMD"); }
  int mode_c = 0, mode_k = 0, strip = 0; const char *command = 0;
  for (;;) { skip_ws(&p); if (*p != '/') break; const char *q = next_tok(p, tok, sizeof tok); char o = lc(tok[1]);
    if (o == 'c' || o == 'k') { if (o == 'c') mode_c = 1; else mode_k = 1; command = tok[2] ? p + 2 : q; break; }
    if (o == 'q') g_echo = 0; else if (o == 's') strip = 1; /* /d /v /e /a /u /t are accepted and ignored */
    p = q; }
  if (command) { skip_ws(&command); }
  char cmdbuf[8192];
  if (command) {
    scpy(cmdbuf, sizeof cmdbuf, command); int n = slen(cmdbuf); while (n && is_space(cmdbuf[n - 1])) cmdbuf[--n] = 0;
    /* cmd's quote rule: if the first character is a quote, strip it and the last quote (unless the two-quote exception keeps them) */
    if (cmdbuf[0] == '"') { int nq = 0; for (char *q = cmdbuf; *q; q++) if (*q == '"') nq++; int special = 0; for (char *q = cmdbuf; *q; q++) if (*q == '&' || *q == '<' || *q == '>' || *q == '(' || *q == ')' || *q == '@' || *q == '^' || *q == '|') special = 1;
      int keep = !strip && nq == 2 && !special; if (keep) { char inner[8192]; scpy(inner, sizeof inner, cmdbuf + 1); char *e = inner; while (*e && *e != '"') e++; char save = *e; *e = 0; char exe[1024]; int isprog = find_program(inner, exe, sizeof exe); *e = save; keep = isprog; }
      if (!keep) { char *last = 0; for (char *q = cmdbuf; *q; q++) if (*q == '"') last = q; if (last && last != cmdbuf) memmove(last, last + 1, (u64)slen(last)); memmove(cmdbuf, cmdbuf + 1, (u64)slen(cmdbuf)); } }
    char ex[16384]; expand(cmdbuf, ex, sizeof ex); g_errorlevel = run_line(ex);
    if (mode_c || g_quit) ExitProcess((u32)g_errorlevel);
  }
  (void)mode_k;
  g_interactive = 1;
  if (!command && g_echo) outs("Microsoft Windows [Version 10.0.19045.6466]\r\n(c) Nebula x64 cmd.exe - a small re-implementation for the wasm64 emulator. Type HELP for commands.\r\n\r\n");
  char line[8192], ex[16384];
  DWORD cm; int console = GetConsoleMode(g_con_in, &cm) != 0;
  while (!g_quit) {
    if (g_echo) prompt();
    if (!read_line(g_in, line, sizeof line)) break;
    if (!console && g_echo) outf("%s\r\n", line);   /* like cmd: commands read from a file/pipe are echoed after the prompt */
    while (paren_depth(line) > 0) { char more[4096]; outs("More? "); if (!read_line(g_in, more, sizeof more)) break; if (!console && g_echo) outf("%s\r\n", more); int n = slen(line); if (n + 1 + slen(more) < (int)sizeof line) { line[n] = '\n'; scpy(line + n + 1, (int)sizeof line - n - 1, more); } }
    const char *q = line; skip_ws(&q); if (!*q) { if (g_echo) outs("\r\n"); continue; }
    expand(q, ex, sizeof ex); g_errorlevel = run_line(ex);
    if (g_echo && !g_quit) outs("\r\n");
  }
  ExitProcess((u32)g_errorlevel);
}
