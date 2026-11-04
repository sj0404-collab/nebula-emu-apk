#!/bin/sh
# .NET capability tests for the Nebula x64 emulator: the REAL Microsoft .NET 10 runtime (CoreCLR + RyuJIT, win-x64)
# runs managed programs on the emulator.  Needs the .NET pack unpacked (emu/tools/dotnet-pack.sh <out> <plaindir>) and
# the test apps built with the .NET SDK (emu/tests/dotnet/app → net10.0, emu/tests/dotnet/fx → net48).
# usage: emu/tests/dotnet.sh [packdir=/var/tmp/dn/pack] [appdir=/var/tmp/dn/app] [fxdir=/var/tmp/dn/fx] [cscdir=/var/tmp/dn/csc/tasks/netcore/bincore]
# cscdir = tasks/netcore/bincore of the Microsoft.Net.Compilers.Toolset NuGet package (the IL-only Roslyn csc.dll); D8 is skipped without it.
set -e
cd "$(dirname "$0")/.."
PACK=${1:-/var/tmp/dn/pack}; APP=${2:-/var/tmp/dn/app}; FX=${3:-/var/tmp/dn/fx}; CSC=${4:-/var/tmp/dn/csc/tasks/netcore/bincore}
[ -f "$PACK/dotnet.exe" ] || { echo "dotnet.sh: no dotnet.exe in $PACK (run emu/tools/dotnet-pack.sh public/x64/dotnet $PACK)"; exit 2; }
[ -f "$APP/nebula-dotnet-test.dll" ] || { echo "dotnet.sh: no nebula-dotnet-test.dll in $APP (dotnet publish emu/tests/dotnet/app -c Release -o $APP)"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
xr() { timeout 300 ./x64/x64run --quiet --no-stdin --max-ms 170000 --mount "$PACK=C:\\Program Files\\dotnet" --mount "$APP=C:\\app" --mount-rw "$TMP=C:\\tmp" "$@"; }
fail=0
check() { # name, expected-substring, output
  if printf '%s' "$3" | grep -q -F -- "$2"; then echo "  ok   $1"; else echo "  FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -30; fail=1; fi
}
code() { [ "$1" = "$2" ] && echo "  ok   exit code $2" || { echo "  FAIL exit code: $1 (expected $2)"; fail=1; }; }
run() { rc=0; out=$(xr "$@" 2>&1) || rc=$?; }

echo "[D1] dotnet.exe exec app.dll — LINQ, exceptions (NRE as hardware fault → managed), JSON, reflection + dynamic, Regex, GC"
run "$PACK/dotnet.exe" 'C:\app\nebula-dotnet-test.dll' linq-throw-nre-json-reflect-regex-gc
check "banner"      "mode linq-throw-nre-json-reflect-regex-gc" "$out"
check "linq"        "primes 303" "$out"
check "nre"         "caught NRE" "$out"
check "throw"       "caught boom" "$out"
check "regex"       "regex 12,345,64" "$out"
check "reflection"  "reflect List\`1 1 6 Main,Run,Steps" "$out"
check "json"        'json {"name":"nebula","items":[2,3,5],"ok":true}' "$out"
check "json back"   "back nebula" "$out"
check "gc"          "gc ok gen0=1" "$out"
code "$rc" 3
echo "[D2] apphost (nebula-dotnet-test.exe, native launcher → hostfxr) — culture, process spawn (cmd.exe), async + JSON"
run "$APP/nebula-dotnet-test.exe" async-culture-proc-json-gc
check "culture"     "culture Invariant Language (Invariant Country) |" "$out"
check "upper"       "| STRAßE |" "$out"
check "timezone"    "tz=Coordinated Universal Time" "$out"
check "process"     "proc child-ok" "$out"
check "json"        "back nebula" "$out"
code "$rc" 3
echo "[D3] GC with other threads: a spinning background thread is suspended and redirected; thread pool; blocking threads"
run "$PACK/dotnet.exe" 'C:\app\nebula-dotnet-test.dll' bgspin-alloc-gc
check "gc+spin"     "gc ok gen0=" "$out"
code "$rc" 3
run "$PACK/dotnet.exe" 'C:\app\nebula-dotnet-test.dll' taskrun-bgsleep-alloc-gc
check "gc+pool"     "gc ok gen0=" "$out"
code "$rc" 3
echo "[D4] HttpClient (SocketsHttpHandler on Winsock IOCP: WSASend/WSARecv overlapped + completion port)"
run "$PACK/dotnet.exe" 'C:\app\nebula-dotnet-test.dll' http
if printf '%s' "$out" | grep -q "http fail"; then echo "  skip (no network: $(printf '%s' "$out" | grep 'http fail' | cut -c1-120))"; else check "http" "http " "$out"; check "bytes" " bytes" "$out"; code "$rc" 3; fi
echo "[D5] managed dll started directly → redirected to dotnet.exe exec"
run "$APP/nebula-dotnet-test.dll" throw-gc
check "redirect"    "caught boom" "$out"
code "$rc" 3
if [ -f "$FX/fx.exe" ]; then
  echo "[D6] classic .NET Framework 4.8 exe (AnyCPU, IL only) — runs on the .NET 10 runtime with a generated runtimeconfig"
  rc=0; out=$(xr --mount "$FX=C:\\fx" "$FX/fx.exe" a b 2>&1) || rc=$?
  check "hello"       "Hello from a .NET Framework 4.8 exe on 10.0." "$out"
  check "linq"        "linq: b,a" "$out"
  check "exception"   "caught: fx boom" "$out"
  check "file io"     "file: written by net48 app" "$out"
  check "args"        "args: a b" "$out"
  code "$rc" 5
else echo "[D6] fx.exe: skip (not built)"; fi
if [ -f "$CSC/csc.dll" ]; then
  echo "[D8] Roslyn: the real C# compiler (csc.dll, Microsoft.Net.Compilers.Toolset) compiles a program on the emulator, then the result runs (BCrypt hashing, C++ EH inside CoreCLR, ~13 G instructions)"
  FW=$(ls -d "$PACK"/shared/Microsoft.NETCore.App/*/ | head -1); FW=${FW%/}; FWV=${FW##*/}
  printf 'using System;\nusing System.Linq;\nclass P { static int Main(string[] a) { Console.WriteLine("hello from csc inside the emulator: " + string.Join(",", Enumerable.Range(1,5).Select(x => x*x))); return 42; } }\n' > "$TMP/hello.cs"
  { printf '/nologo\n/nostdlib+\n/target:exe\n/out:C:\\tmp\\hello.dll\n'
    for r in System.Runtime System.Private.CoreLib System.Console System.Linq System.Collections; do printf '/r:"C:\\Program Files\\dotnet\\shared\\Microsoft.NETCore.App\\%s\\%s.dll"\n' "$FWV" "$r"; done
    printf 'C:\\tmp\\hello.cs\n'; } > "$TMP/csc.rsp"
  rc=0; out=$(timeout 1500 ./x64/x64run --quiet --no-stdin --max-ms 1400000 --env DOTNET_gcServer=0 --mount "$PACK=C:\\Program Files\\dotnet" --mount "$CSC=C:\\csc" --mount-rw "$TMP=C:\\tmp" "$PACK/dotnet.exe" 'C:\csc\csc.dll' '@C:\tmp\csc.rsp' 2>&1) || rc=$?
  [ -z "$out" ] && echo "  ok   csc: no diagnostics" || { echo "  FAIL csc output:"; printf '%s\n' "$out" | head -20; fail=1; }
  code "$rc" 0
  if [ -f "$TMP/hello.dll" ]; then
    echo "  ok   hello.dll written ($(wc -c < "$TMP/hello.dll") bytes)"
    cp "$APP/nebula-dotnet-test.runtimeconfig.json" "$TMP/hello.runtimeconfig.json"
    run "$PACK/dotnet.exe" 'C:\tmp\hello.dll'
    check "compiled program runs" "hello from csc inside the emulator: 1,4,9,16,25" "$out"
    code "$rc" 42
    [ "$(wc -c < "$TMP/hello.runtimeconfig.json")" -gt 0 ] && echo "  ok   read-only mapping of a write-back file left intact" || { echo "  FAIL hello.runtimeconfig.json was truncated by the read-only mapping write-back"; fail=1; }
  else echo "  FAIL hello.dll not produced"; fail=1; fi
else echo "[D8] csc: skip (no csc.dll in $CSC)"; fi
echo "[D7] honest rejection without the pack"
rc=0; out=$(timeout 60 ./x64/x64run --quiet --no-stdin --mount "$APP=C:\\app" "$APP/nebula-dotnet-test.dll" gc 2>&1) || rc=$?
check "message"     "is a .NET assembly: install the .NET runtime pack" "$out"
[ "$fail" = 0 ] && echo "ALL OK" || { echo "FAILURES"; exit 1; }
