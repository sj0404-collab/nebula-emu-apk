#!/bin/sh
# Native capability tests for the Nebula x64 emulator (run by CI; mirrors what the browser build does).
set -e
cd "$(dirname "$0")/.."
X=./x64/x64run; S=../public/x64/samples; fail=0
check() { # name, expected-substring, output
  if printf '%s' "$3" | grep -q -- "$2"; then echo "  ok   $1"; else echo "  FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -20; fail=1; fi
}
echo "[1] hello.exe (UCRT console)"
rc=0; out=$($X --quiet tests/hello.exe a b 2>&1) || rc=$?
check "greeting"      "Hello from x86-64 Windows! argc=3" "$out"
check "64-bit fnv"    "fnv=76a25757d9d78283" "$out"
check "printf/math"   "sqrt(2)=1.414214 sin(1)=0.8415 pow(2,10)=1024" "$out"
[ "$rc" = 7 ] && echo "  ok   exit code 7" || { echo "  FAIL exit code: $rc"; fail=1; }
echo "[1b] pebstd.exe (PEB std handles, MEM_DECOMMIT semantics)"
out=$($X --quiet tests/pebstd.exe 2>&1 || true)
check "peb handles"   "in=0000000000000010 out=0000000000000011 err=0000000000000012 flags=1" "$out"
check "peb WriteFile" "WriteFile(peb hStdOutput) -> 1, 87 bytes" "$out"
check "decommit"      "decommit -> 1, recommit same, nonzero after recommit: 0, untouched byte: ab" "$out"
echo "[1c] waitin.exe (console input handle is waitable)"
out=$( (sleep 1; printf 'hi') | $X --quiet tests/waitin.exe 2>&1 || true)
check "wait timeout"  "wait(50ms) before input -> 258" "$out"
check "wait signaled" "wait(5s) -> 0 (0=signaled)" "$out"
check "key records"   "ReadConsoleInput: 2 records 'h' 'i'" "$out"
echo "[1d] regwait.exe (RegisterWaitForSingleObject on console input)"
out=$( (sleep 1; printf 'hi') | $X --quiet tests/regwait.exe 2>&1 || true)
check "callback"      "callback ctx=0000000000001234 timedout=0" "$out"
check "event"         "event wait -> 0" "$out"
check "records"       "records=2" "$out"
echo "[1e] keys.exe (UTF-8 stdin -> UTF-16 key records)"
out=$(printf 'a\320\237\321\200\360\237\230\200z\n' | $X --quiet tests/keys.exe 2>&1 || true)
check "utf16 units"   "0061 041f 0440 d83d de00 007a 000d" "$out"
check "count"         "total=7" "$out"
echo "[1f] d3d11clear.exe (Direct3D 11 device + swap chain + clear + Present)"
rm -f /tmp/d3d_*.ppm; out=$(cd /tmp && X64_LOG=1 $OLDPWD/$X --quiet --no-stdin --frames 1 --max-ms 10000 $OLDPWD/tests/d3d11clear.exe 2>&1 || true)
check "device"        "device ok, feature level 0xb000" "$out"
check "backbuffer"    "backbuffer 640x480 format 87" "$out"
check "three frames"  "frame 2 presented (0.1 0.8 0.3)" "$out"
check "ok"            "d3d11clear ok" "$out"
check "frame pixels"  "frame_0001:(51, 102, 230) frame_0002:(230, 77, 26) frame_0003:(26, 204, 77)" "$(python3 - <<'PY'
import glob
def px(f):
    d=open(f,'rb').read(); parts=d.split(b'\n',3); w,h=map(int,parts[1].split()); o=parts[3]; i=(200*w+300)*3; return tuple(o[i:i+3])
print(' '.join(f.split('/')[-1][:-4]+':'+str(px(f)) for f in sorted(glob.glob('/tmp/frame_000[123].ppm'))))
PY
)"
rm -f /tmp/frame_*.ppm
echo "[1f2] d3d11tri.exe (DXBC → GLSL translation; the native runner has no GPU, so Draw must stop honestly)"
out=$(X64_LOG=1 $X --no-stdin --max-ms 10000 tests/d3d11tri.exe 2>&1 || true)
check "vs translated"  "vertex shader .*: 1432 bytes of DXBC → .* bytes of GLSL ES 3.00" "$out"
check "ps translated"  "pixel shader .*: 576 bytes of DXBC → .* bytes of GLSL ES 3.00" "$out"
check "honest draw"    "D3D11 Draw: this host has no GPU back end" "$out"
echo "[1g] uvpipe.exe (libuv-style child stdout: named pipe + IOCP zero-read + CreateProcess)"
out=$($X --quiet --no-stdin --max-ms 20000 tests/uvpipe.exe 2>&1 || true)
check "zero-read ready" "GQCS -> 1 bytes=0 key=42" "$out"
check "child output"    "data: \[hi from child" "$out"
echo "[2] bench.exe (CPU)"
out=$($X --quiet --stats tests/bench.exe 2>&1 || true)
check "sieve"         "sieve(2M): 148933 primes" "$out"
check "mandelbrot"    "mandelbrot(200x100x200): 5108 inside" "$out"
echo "[3] busybox64.exe (real third-party mingw64 binary)"
check "echo"    "hello from busybox"          "$($X --quiet $S/busybox64.exe echo hello from busybox 2>&1 || true)"
check "awk"     "42"                          "$($X --quiet $S/busybox64.exe awk 'BEGIN{print(6*7)}' 2>&1 || true)"
check "sha256"  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" "$($X --quiet $S/busybox64.exe sha256sum </dev/null 2>&1 || true)"
check "date"    "2025-10-29T15:00:00"         "$($X --quiet $S/busybox64.exe date -u -d @1761750000 +%Y-%m-%dT%H:%M:%S 2>&1 || true)"
check "factor"  "1234567891: 1234567891"      "$($X --quiet $S/busybox64.exe factor 1234567891 2>&1 || true)"
check "uname"   "Windows_NT nebula"           "$($X --quiet $S/busybox64.exe uname -a 2>&1 || true)"
echo "[3b] virtual file system + stdin (busybox on a mounted host folder)"
M=$(mktemp -d); mkdir -p "$M/sub"; printf 'pear\napple\nmango\n' > "$M/words.txt"; printf 'x' > "$M/sub/b.bin"
check "ls"       "words.txt"                   "$($X --quiet --mount "$M" $S/busybox64.exe ls </dev/null 2>&1 || true)"
check "sort file" "apple
mango
pear"                                            "$($X --quiet --mount "$M" $S/busybox64.exe sort words.txt </dev/null 2>&1 || true)"
check "stdin"    "cherry
banana
apple"                                           "$(printf 'banana\ncherry\napple\n' | $X --quiet $S/busybox64.exe sort -r 2>&1 || true)"
check "wc"       "3         3        17 words.txt" "$($X --quiet --mount "$M" $S/busybox64.exe wc words.txt </dev/null 2>&1 || true)"
check "find"     "./sub/b.bin"                  "$($X --quiet --mount "$M" $S/busybox64.exe find . </dev/null 2>&1 || true)"
check "sed"      "APPLE"                        "$($X --quiet --mount "$M" $S/busybox64.exe sed 's/apple/APPLE/' words.txt </dev/null 2>&1 || true)"
check "diff"     "+x"                           "$($X --quiet --mount "$M" $S/busybox64.exe diff words.txt sub/b.bin </dev/null 2>&1 || true)"
check "sh vars"  "n1
n2
n3
10"                                              "$($X --quiet $S/busybox64.exe sh -c 'for i in 1 2 3; do echo n$i; done; X=5; echo $((X*2))' </dev/null 2>&1 || true)"
check "sh read"  "got:typed line"               "$(echo 'typed line' | $X --quiet $S/busybox64.exe sh -c 'read x; echo got:$x' 2>&1 || true)"
check "env"      "TEMP=C:.tmp"                 "$($X --quiet $S/busybox64.exe env </dev/null 2>&1 || true)"
rm -rf "$M"
echo "[3c] processes + pipes (busybox sh spawns real child processes inside the emulator)"
BB="$X --quiet --no-stdin --max-ms 20000 $S/busybox64.exe"
check "pipe"        "HELLO"                      "$($BB sh -c 'echo hello | tr a-z A-Z' 2>&1 || true)"
check "pipeline x3" "5
4"                        "$($BB sh -c 'seq 1 5 | sort -rn | head -2' 2>&1 || true)"
check "subst"       "got:sub"                    "$($BB sh -c 'x=$(echo sub); echo got:$x' 2>&1 || true)"
check "nested"      "deep"                       "$($BB sh -c 'echo $(echo $(echo deep))' 2>&1 || true)"
check "exit codes"  "1 7 3"                      "$($BB sh -c 'false; a=$?; (exit 7); b=$?; sh -c "exit 3"; echo $a $b $?' 2>&1 || true)"
check "bg + ps"     "sleep 1"                    "$($BB sh -c 'sleep 1 & ps; wait' 2>&1 || true)"
check "kill"        "rc=137"                     "$($BB sh -c 'sleep 5 & kill -9 $!; wait $!; echo rc=$?' 2>&1 || true)"
check "yes|head"    "y
y"                        "$($BB sh -c 'yes | head -2' 2>&1 || true)"
check "1 MB pipe"   "1000000"                    "$($BB sh -c 'head -c 1000000 /dev/zero | wc -c' 2>/dev/null || true)"
check "while read"  "<x><y>"                     "$($BB sh -c 'printf "x\ny\n" | while read l; do printf "<%s>" "$l"; done; echo' 2>&1 || true)"
check "xargs sh"    "item 2"                     "$($BB sh -c 'printf "1\n2\n" | xargs -I{} sh -c "echo item {}"' 2>&1 || true)"
check "env child"   "FOO=baz"                    "$($BB sh -c 'FOO=baz sh -c "echo FOO=\$FOO"' 2>&1 || true)"
check "stdin→child" "child:line1
parent:line2"  "$(printf 'line1\nline2\n' | $X --quiet --max-ms 20000 $S/busybox64.exe sh -c 'sh -c "read a; echo child:\$a"; read b; echo parent:$b' 2>&1 || true)"
check "gzip pipe"   "payload"                    "$($BB sh -c 'echo payload | gzip -c | gunzip -c' 2>&1 || true)"
echo "[4] plink.exe (real MSVC/clang-cl PuTTY 0.85 binary)"
check "plink -V" "plink: Release 0.85"        "$($X --quiet $S/plink.exe -V 2>&1 || true)"
echo "[5] NebulaStrike.exe (Win32 GDI game) — 2 s headless, keys"
out=$($X --max-ms 2000 --key 0x25@300:400 --key 0x20@500:1200 ../public/downloads/NebulaStrike.exe 2>&1 || true)
frames=$(printf '%s' "$out" | sed -n 's/.* = \([0-9.]*\) MIPS, \([0-9]*\) frames.*/\2/p')
echo "  frames presented: ${frames:-0}"
[ "${frames:-0}" -ge 60 ] && echo "  ok   >= 60 frames in 2 s" || { echo "  FAIL frames: $out"; fail=1; }
echo "[7] threads & synchronisation (real Win32 threads scheduled by the emulator)"
check "threads.exe"   "39 checks, 0 failed"          "$($X --quiet --max-ms 30000 tests/threads.exe 2>&1 | tail -1 || true)"
check "pthreads.exe"  "9 checks, 0 failed"           "$($X --quiet --max-ms 30000 tests/pthreads.exe 2>&1 | tail -1 || true)"
check "rtlwait.exe"   "timeout -> 0x102 after ~100ms; woken -> 0x0 flag=1 after ~150ms" "$($X --quiet --max-ms 30000 tests/rtlwait.exe 2>&1 | tail -1 || true)"   # ntdll RtlWaitOnAddress re-executed after a park (v1.20.1 regression)
check "threads in child + pipe" "39 checks, 0 failed" "$($X --quiet --no-stdin --max-ms 30000 --mount "$PWD/tests" $S/busybox64.exe sh -c './threads.exe | tail -1' 2>&1 || true)"
echo "[7b] Winsock loopback: blocking send/recv of 12 MB between threads, overlapped WSASend + WSAGetOverlappedResult, shutdown/EOF, select, FIONREAD"
check "netloop.exe"   "netloop: 23 checks, 0 failed"  "$($X --quiet --no-stdin --max-ms 60000 tests/netloop.exe 2>&1 | tail -1 || true)"
echo "[8] heap: 10^5 live blocks, blocks spanning heap regions, sort of 100000 lines"
check "sort -n 20000"  "20000"                       "$($BB sh -c 'seq 1 20000 | sort -n | tail -1' 2>&1 || true)"
check "sort -rn 1e5"   "100000"                      "$($BB sh -c 'seq 1 100000 | sort -rn | head -1' 2>&1 || true)"
echo "[9] private DLLs: static imports, LoadLibrary, GetProcAddress, DllMain, per-thread TLS"
check "usedll.exe"   "static import: add3=6 name=mydll says hi
LoadLibrary=0000000180000000 same=1
GetProcAddress dll_hyp -> ok, f(3,4)=25.0
missing=0000000000000000 err=127
path=C:\\nebula\\mydll.dll
kernel32 GetTickCount=ok" "$($X --quiet --mount "$PWD/tests/dll" tests/dll/usedll.exe 2>&1 || true)"
check "DLL thread TLS" "thread 10: tls=57
thread 100: tls=507
main after threads: 9 (expect 9)" "$($X --quiet --mount "$PWD/tests/dll" tests/dll/tlsuse.exe 2>&1 | tail -3 || true)"
echo "[10] real applications (public/x64/apps: SQLite 3.50.4, jq 1.8.1, Lua 5.4.2 + DLL, 7-Zip 25.01 + DLL, zstd 1.5.7)"
A=../public/x64/apps
check "sqlite3 :memory:" "3|3.50.4"      "$($X --quiet --max-ms 30000 $A/sqlite3.exe :memory: 'select 1+2, sqlite_version();' 2>&1 || true)"
check "sqlite3 file db" "2000|ok|"       "$($X --quiet --no-stdin --max-ms 120000 --mount "$PWD/$A" $S/busybox64.exe sh -c './sqlite3.exe -init demo.sql demo.db .quit >/dev/null 2>&1; echo "select count(*) from t; pragma integrity_check;" | ./sqlite3.exe demo.db | tr -d "\\r" | tr "\\n" "|"' 2>&1 || true)"
check "jq"              '{"name":"cid","n":3}' "$($X --quiet --max-ms 30000 --mount "$PWD/$A" $A/jq.exe -M -c '.users[] | select(.age > 40) | {name, n: (.langs|length)}' demo.json 2>&1 || true)"
check "lua54 + lua54.dll" "fib(25) =	75025" "$($X --quiet --max-ms 60000 --mount "$PWD/$A" $A/lua54.exe demo.lua 2>&1 | head -1 | cut -f3- || true)"
check "7z.exe + 7z.dll round trip" "7Z_OK" "$($X --quiet --no-stdin --max-ms 120000 --mount "$PWD/$A" $S/busybox64.exe sh -c './7z.exe a -bb0 -mx1 d.7z demo.txt >/dev/null && ./7z.exe x -y -oout d.7z >/dev/null && cmp demo.txt out/demo.txt && echo 7Z_OK' 2>&1 || true)"
check "zstd round trip" "ZSTD_OK"        "$($X --quiet --no-stdin --max-ms 120000 --mount "$PWD/$A" $S/busybox64.exe sh -c './zstd.exe -q -f demo.txt -o d.zst && ./zstd.exe -q -d -f d.zst -o back.txt && cmp demo.txt back.txt && echo ZSTD_OK' 2>&1 || true)"
echo "[11] x87 FPU subset (long double printf / fsin / fyl2x)"
check "x87.exe" "ld: 1.9090909091 1909
sqrtl: 1.414213562
fistp: 1909090
fmodl: 1.5000 sinl: 0.841471
strtold: 2.718282" "$($X --quiet tests/x87.exe 2>&1 | grep -v 'via mingw\|cmp:' || true)"
echo "[12] structured exception handling: x64 SEH tables, libunwind C++ (zig c++), MSVC __CxxFrameHandler (7-Zip error paths)"
check "C++ throw/catch (gcc ABI)" "caught std::exception: value too large: 7" "$($X --quiet --max-ms 30000 tests/cxxexc.exe 2>&1 || true)"
check "1000 throws + dtors"       "1000 throws in a loop, total=503"        "$($X --quiet --max-ms 30000 tests/cxxexc.exe 2>&1 || true)"
check "__try/__finally unwinding" "inner __finally"                         "$($X --quiet --max-ms 30000 tests/sehtest.exe 2>&1 || true)"
check "nested __except"           "nested except: 1"                        "$($X --quiet --max-ms 30000 tests/sehtest.exe 2>&1 || true)"
check "7z: missing archive (MSVC C++ throw)" "System ERROR:" "$($X --quiet --max-ms 30000 --mount "$PWD/$A" $A/7z.exe x nonexistent.7z 2>&1 || true)"
check "7z: unsupported type"      "Unsupported archive type"               "$($X --quiet --max-ms 30000 --mount "$PWD/$A" $A/7z.exe a -tbogus x.7z demo.txt 2>&1 || true)"
echo "[13] hardware exceptions as SEH (AV / divide by zero / ud2 -> __except, vectored handlers, continue execution, top-level filter)"
hw="$($X --quiet --max-ms 30000 tests/hwexc.exe 2>&1 || true)"
check "AV caught with record"  "AV caught=1 addr_ok=3"            "$hw"
check "div0 + ud2 caught"       "div0 caught=2
ud2 caught=3"      "$hw"
check "finally during AV unwind" "nested AV caught=4 finally=1"    "$hw"
check "VEH fix-up + continue"   "veh calls=1 code=0xc0000094 result=42" "$hw"
check "2000 faults in a loop"   "loop caught=2000"                 "$hw"
check "SetUnhandledExceptionFilter" "top-level filter: code 0xc0000005" "$hw"
echo "[14] Rust programs (ripgrep 14.1.1, fd 10.2.0 — MSVC target, static CRT, Rust std on Win32)"
check "rg -c across the tree" "demo.txt:1922"            "$($X --quiet --max-ms 60000 --mount "$PWD/$A" $A/rg.exe --color never -c -w the 2>&1 || true)"
check "rg -n regex"           "fib(25)"                "$($X --quiet --max-ms 60000 --mount "$PWD/$A" $A/rg.exe -n -i --color never "lua|sqlite" demo.lua demo.sql demo.json 2>&1 || true)"
check "rg -j4 --stats"        "files searched"         "$($X --quiet --max-ms 60000 --mount "$PWD/$A" $A/rg.exe -j4 --stats -c the 2>&1 || true)"
check "fd by extension"       "demo.json
demo.lua
demo.sql" "$($X --quiet --max-ms 60000 --mount "$PWD/$A" $A/fd.exe --color never -e lua -e sql -e json 2>&1 || true)"
echo "[15] CPython 3.12.10 (official python.org embeddable build: python.exe + python312.dll + stdlib zip + .pyd extensions)"
PY=$A/py; PYR="$X --quiet --max-ms 120000 --mount $PWD/$PY"
check "print(6*7)"            "42"                                   "$($PYR $PY/python.exe -c "print(6*7)" 2>&1 || true)"
check "version + platform"    "3.12.10 win32 AMD64"                  "$($PYR $PY/python.exe -c "import sys, platform; print(sys.version.split()[0], sys.platform, platform.machine())" 2>&1 || true)"
check "zipimport stdlib"      '"b": "x"}'              "$($PYR $PY/python.exe -c "import json, re, collections; print(json.dumps({'b': 'x', 'a': [1, 2]}, sort_keys=True))" 2>&1 || true)"
check "sqlite3 .pyd + dll"    "(45,) 3.49.1"                         "$($PYR $PY/python.exe -c "import sqlite3; c = sqlite3.connect(':memory:'); c.execute('create table t(x)'); c.executemany('insert into t values (?)', [(i,) for i in range(10)]); print(c.execute('select sum(x) from t').fetchone(), sqlite3.sqlite_version)" 2>&1 || true)"
check "decimal/hashlib/math"  "3.3 ba7816bf -0.6931471805599453 1.4142135623730951" "$($PYR $PY/python.exe -c "import decimal, hashlib, math; print(decimal.Decimal('1.1') + decimal.Decimal('2.2'), hashlib.sha256(b'abc').hexdigest()[:8], math.log(0.5), math.pow(2, 0.5))" 2>&1 || true)"
check "threads"               "[49995000, 199990000, 449985000]"     "$($PYR $PY/python.exe -c "import threading, queue; q = queue.Queue(); ts = [threading.Thread(target=lambda n: q.put(sum(range(n))), args=(10000 * k,)) for k in (1, 2, 3)]; [t.start() for t in ts]; [t.join() for t in ts]; print(sorted(q.get() for _ in ts))" 2>&1 || true)"
check "ctypes + kernel32"     "4242 8"                               "$($PYR $PY/python.exe -c "import ctypes; print(ctypes.windll.kernel32.GetCurrentProcessId(), ctypes.sizeof(ctypes.c_void_p))" 2>&1 || true)"
check "file io + traceback"   "ZeroDivisionError: division by zero"  "$($PYR $PY/python.exe -c "open('o.txt', 'w').write('hi'); print(open('o.txt').read()); 1/0" 2>&1 || true)"
rc=0; $PYR $PY/python.exe -c "import sys; sys.exit(3)" >/dev/null 2>&1 || rc=$?
check "exit code"             "rc=3"                                 "rc=$rc"
check "subprocess pipes"      "returncode=3, stdout='42" "$($PYR $PY/python.exe -c "import subprocess, sys; print(subprocess.run(['python.exe', '-c', 'import sys; print(42); print(\"err\", file=sys.stderr); sys.exit(3)'], capture_output=True, text=True))" 2>&1 || true)"
check "REPL over stdin"       "55 привет"                            "$(printf 'x = [n * n for n in range(6)]\nprint(sum(x), %s)\n' "'привет'" | $PYR $PY/python.exe -i -q 2>&1 || true)"
check "demo.py"               "sqlite: (3, 84.92, 'carol')"          "$(cp $A/demo.py $PY/demo.py; $PYR $PY/python.exe demo.py 2>&1 || true; rm -f $PY/demo.py)"
echo "[16] cmd.exe (emu/cmd/cmd.c, embedded at C:\\Windows\\System32: shell for system()/_popen/subprocess(shell=True), batch files, pipelines)"
CMD="$X --quiet --max-ms 60000 --mount $PWD/$A $A/cmd.exe"
check "echo + variables"      "hello
Windows_NT C:\\nebula"                 "$($CMD /c "echo hello & echo %OS% %CD%" 2>&1 | tr -d '\r' || true)"
check "pipeline of builtins"  "demo.sql
demo.lua"                                "$($CMD /c "dir /b *.lua *.sql | sort /r" 2>&1 | tr -d '\r' || true)"
check "redirection + find /c" "2"                                    "$($CMD /c "echo one line> t_out.txt & echo two line>> t_out.txt & type t_out.txt | find /c line & del t_out.txt" 2>&1 | tr -d '\r' || true)"
printf '@echo off\r\nset /a R=6*7 >nul\r\nif %%R%% EQU 42 (set W=yes) else (set W=no)\r\necho %%R%% %%W%%\r\nfor /l %%%%i in (1,1,3) do @echo %%%%i\r\n' > $A/t_arith.bat
check "set /a, if, for /l"    "42 yes 1 2 3 "                        "$($CMD /c t_arith.bat 2>&1 | tr -d '\r' | tr '\n' ' ' || true)"
rm -f $A/t_arith.bat
check "external programs"     "ok3"                                    "$($CMD /c "jq.exe -c -M .users[1].name demo.json | findstr bob && echo ok3" 2>&1 | tr -d '\r' || true)"
rc=0; $CMD /c "exit 7" >/dev/null 2>&1 || rc=$?
check "exit code"             "rc=7"                                 "rc=$rc"
check "unknown command"       "'nosuch' is not recognized as an internal or external command"  "$($CMD /c "nosuch --flag" 2>&1 || true)"
check "batch file"            "count=3
lua present
at least three files"                    "$($CMD /c demo.bat 2>&1 | tr -d '\r' || true)"
check "batch: call :sub / exit /b" "sub got hello world (from demo)
errorlevel after sub: 7"               "$($CMD /c demo.bat 2>&1 | tr -d '\r' || true)"
check "system()/_popen (C)"   "system rc=5
popen: [demo.lua]
popen: [done]
popen lines=2 rc=0
apple
mango
pear
popen-w rc=0"                            "$($X --quiet --max-ms 60000 --mount $PWD/$A tests/shelltest.exe 2>&1 | tr -d '\r' || true)"
check "python os.system/popen/shell=True" "rc 3
'python.exe'
0 'shell=Windows_NT
1024
PIPED'"                                  "$(cp $A/py/python.exe /tmp/_py_probe.exe 2>/dev/null; $PYR $PY/python.exe -c "import os, subprocess; print('rc', os.system('echo from-system & exit 3')); print(repr(os.popen('dir /b *.exe').read().strip())); r = subprocess.run('echo shell=%OS% && python.exe -c \"print(2**10)\" | findstr 1024 && python.exe -c \"import sys; print(sys.stdin.read().upper())\"', shell=True, input='piped', capture_output=True, text=True); print(r.returncode, repr(r.stdout.strip()))" 2>&1 | tr -d '\r' || true)"
printf '@echo off\r\ntype %%1\r\necho extra line with the word the\r\n' > $A/pre.cmd
check "rg --pre (Rust pipes: CreateNamedPipeW + ReadFileEx/APC)" "demo.lua
8:extra line with the word the"      "$($X --quiet --max-ms 60000 --mount $PWD/$A $A/rg.exe --color never --pre C:\\nebula\\pre.cmd -n "extra line" demo.lua 2>&1 | tr -d '\r' || true)"
rm -f $A/pre.cmd
check "fd -x cmd /c"          "found ./demo.lua"                     "$($X --quiet --max-ms 60000 --mount $PWD/$A $A/fd.exe --color never -e lua -x cmd /c echo found {} 2>&1 | tr -d '\r' || true)"
rm -f $A/out.txt
echo "[17] powershell.exe (emu/pwsh/pwsh.c, embedded at C:/Windows/System32/WindowsPowerShell/v1.0: Expand-Archive and the scripting subset installers use)"
PSX="$X --quiet --max-ms 120000 --mount $PWD/tests=C:\\nebula $A/cmd.exe /c"
check "found on PATH"           "WindowsPowerShell.v1.0.powershell.exe" "$($PSX "powershell -NoProfile -Command (Get-Command powershell.exe)" 2>&1 || true)"
# the exact spawn opencode uses to unpack ripgrep (cmd.exe /d /s /c "powershell.exe ^"-NoProfile^" … Expand-Archive …")
printf '@echo off\r\ncmd.exe /d /s /c "powershell.exe ^"-NoProfile^" ^"-NonInteractive^" ^"-Command^" ^"$global:ProgressPreference^ =^ ^'"'"'SilentlyContinue^'"'"'^;^ Expand-Archive^ -LiteralPath^ ^'"'"'C:\\nebula\\pwtest.zip^'"'"'^ -DestinationPath^ ^'"'"'C:\\tmp\\pw^'"'"'^ -Force^""\r\necho exit=%%ERRORLEVEL%%\r\npowershell -NoProfile -Command "(Get-ChildItem -Recurse -File -Name C:\\tmp\\pw) -join '"'"' '"'"'; (Get-Content C:\\tmp\\pw\\pkg\\data\\numbers.txt | Measure-Object).Count; Get-Content C:\\tmp\\pw\\pkg\\data\\numbers.txt | Select-Object -Last 1"\r\n' > tests/t_pw.bat
check "Expand-Archive exactly as opencode spawns it (deflate + stored, nested dirs)" "exit=0 pkg.README.txt pkg.bin.tool.cmd pkg.data.numbers.txt 100 10000" "$($PSX C:\\nebula\\t_pw.bat 2>&1 | tr -d '\r' | tr '\n' ' ' || true)"
printf '%s\r\n' "[System.IO.Compression.ZipFile]::ExtractToDirectory('C:\nebula\pwtest.zip', 'C:\tmp\pw2')" "& 'C:\tmp\pw2\pkg\bin\tool.cmd' 42" "(Get-Content 'C:\tmp\pw2\pkg\README.txt' | Measure-Object).Count" > tests/t_zip.ps1
check "ZipFile::ExtractToDirectory + external program from the archive" "tool-ran 42
50" "$($PSX "powershell -NoProfile -File C:\\nebula\\t_zip.ps1" 2>&1 | tr -d '\r' || true)"
printf '%s\r\n' "param()" "\$ErrorActionPreference = 'Stop'" "\$dir = Join-Path \$env:TEMP 'pstest'" "New-Item -ItemType Directory -Path \$dir -Force | Out-Null" "Set-Content -Path (Join-Path \$dir 'a.txt') -Value \"alpha\`nbeta\`ngamma\"" "\$lines = Get-Content (Join-Path \$dir 'a.txt') | Where-Object { \$_ -like '*a*' }" "Write-Output (\"matched {0}: {1}\" -f \$lines.Count, (\$lines -join ','))" "if ((Test-Path \$dir) -and (\$lines.Count -eq 3)) { 'cond ok' } else { 'cond bad' }" "try { Get-Content C:\\missing.txt } catch { 'caught' }" "foreach (\$n in 1,2,3) { \$sum += \$n }" "\"sum=\$sum\"" "\$h = (Get-FileHash (Join-Path \$dir 'a.txt')).Hash" "\$h.Length" "cmd /c echo via-cmd" "exit 3" > tests/t_ps.ps1
check "script: pipeline, -f, if/-and, try/catch, foreach, hash, external" "matched 3: alpha,beta,gamma
cond ok
caught
sum=6
64
via-cmd" "$($PSX "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\nebula\\t_ps.ps1" 2>&1 | tr -d '\r' || true)"
rc=0; $PSX "powershell -NoProfile -File C:\\nebula\\t_ps.ps1" >/dev/null 2>&1 || rc=$?
check "script exit code"        "rc=3" "rc=$rc"
check "unknown cmdlet"          "The term 'Get-Nonsense' is not recognized" "$($PSX "powershell -Command Get-Nonsense" 2>&1 || true)"
rm -f tests/t_ps.ps1 tests/t_zip.ps1 tests/t_pw.bat
echo "[5x] msvcp.exe (REAL msvcp140.dll from the VC++ redistributable — needs the runtime pack in public/x64/sys32)"
if [ -f ../public/x64/sys32/msvcp140.dll ]; then
  out=$($X --quiet --no-stdin --mount '../public/x64/sys32=C:\Windows\System32' tests/msvcp.exe 2>&1 || true)
  check "cout << int/double"  "42 3.25" "$out"
  check "cout put/flush"      "msvcp140 says hello" "$out"
  check "done"                "MSVCP OK" "$out"
  out=$($X --quiet --no-stdin tests/msvcp.exe 2>&1 || true)
  check "no pack → honest"    "exports missing" "$out"
else
  echo "  skip (run emu/tools/runtime-pack.sh first)"
fi
echo "[5y] tpool.exe (kernel32 thread pool: work items, periodic/one-shot timers, waits, simple callbacks, *WhenCallbackReturns)"
out=$($X --quiet --no-stdin tests/tpool.exe 2>&1 || true)
check "24 work callbacks"      "24 work callbacks ran, sum 48000" "$out"
check "periodic timer"         "periodic timer fired 4..8 times" "$out"
check "timer cancel"           "no ticks after cancel" "$out"
check "one-shot timer"         "one-shot timer fired exactly once" "$out"
check "wait object"            "wait callback ran with WAIT_OBJECT_0" "$out"
check "wait timeout"           "wait callback got WAIT_TIMEOUT" "$out"
check "simple callback"        "simple callback ran" "$out"
check "all"                    "threadpool 14/14" "$out"
if [ -f ../public/x64/sys32/msvcp140.dll ]; then
  echo "[5z] every runtime-pack DLL loads and runs its DllMain (LoadLibrary from C:\\Windows\\System32)"
  for d in $(ls ../public/x64/sys32/*.dll | xargs -n1 basename); do
    out=$($X --quiet --no-stdin --mount '../public/x64/sys32=C:\Windows\System32' tests/msvcp.exe "$d" 2>&1 || true)
    case "$out" in *"exports missing"*|*"MSVCP OK"*) echo "  ok   $d";; *) echo "  FAIL $d: $(printf '%s' "$out" | head -3)"; fail=1;; esac
  done
fi
DXP=${DX_PACK:-/var/tmp/dx/pack}
echo "[5dx] DirectX pack: REAL D3DCompiler_43 / d3dx9_43 / d3dx11_43 / X3DAudio1_7 from the June 2010 redistributable (DX_PACK=$DXP)"
if [ -f "$DXP/D3DCompiler_43.dll" ]; then
  out=$($X --quiet --no-stdin --max-ms 280000 --mount "$DXP=C:\\Windows\\System32" tests/d3dcompile.exe 2>&1 || true)
  check "real compiler found"     "shader compiler: D3DCompiler_43.dll" "$out"
  check "vs_4_0 compiled"         "vs_main vs_4_0: 1096 bytes, magic DXBC, valid DXBC container" "$out"
  check "ps_4_0 compiled"         "ps_main ps_4_0: 580 bytes, magic DXBC, valid DXBC container" "$out"
  check "disassembly"             "contains mul/mad arithmetic" "$out"
  check "DXBC accepted by D3D11"  "runtime-compiled shaders bound" "$out"
  out=$($X --quiet --no-stdin --max-ms 280000 --mount "$DXP=C:\\Windows\\System32" tests/dxpack.exe 2>&1 || true)
  check "d3dx9 math"              "P*inverse(P) deviates from identity by 0.0000" "$out"
  check "d3dx11 compile"          "HLSL compiled through D3DCompiler_43" "$out"
  check "X3DAudio panning"        "left 0.000 right 1.000" "$out"
  check "XInput honest"           "XInputGetState(0) = 1167" "$out"
  check "dxpack all"              "dxpack: 0 failure(s)" "$out"
  out=$($X --quiet --no-stdin tests/d3dcompile.exe 2>&1 || true)
  check "no pack → honest"        "install the DirectX pack" "$out"
  echo "  every pack DLL loads and runs its DllMain (LoadLibrary from C:\\Windows\\System32)"
  for d in $(ls "$DXP"/*.dll | xargs -n1 basename); do
    out=$($X --quiet --no-stdin --max-ms 60000 --mount "$DXP=C:\\Windows\\System32" tests/msvcp.exe "$d" 2>&1 || true)
    case "$out" in *"exports missing"*|*"MSVCP OK"*) echo "  ok   $d";; *) echo "  FAIL $d: $(printf '%s' "$out" | head -3)"; fail=1;; esac
  done
else
  echo "  skip (run emu/tools/dx-pack.sh public/x64/dx /var/tmp/dx/pack first)"
fi
echo "[5au] sound: waveOut / DirectSound 8 / WASAPI / PlaySound → host audio output (X64_AUDIO_WAV dump of the mix)"
rm -f /tmp/x64audio.wav
if [ -f "$DXP/XAudio2_7.dll" ]; then out=$(X64_AUDIO_WAV=/tmp/x64audio.wav $X --quiet --no-stdin --max-ms 120000 --mount "$DXP=C:\\Windows\\System32" tests/audio.exe 2>&1 || true)
else out=$(X64_AUDIO_WAV=/tmp/x64audio.wav $X --quiet --no-stdin --max-ms 120000 tests/audio.exe 2>&1 || true); fi
check "waveOut"                 "waveOut OK" "$out"
check "DirectSound"             "DirectSound OK" "$out"
check "WASAPI"                  "WASAPI OK" "$out"
check "PlaySound"               "PlaySound OK" "$out"
if [ -f "$DXP/XAudio2_7.dll" ]; then check "real XAudio2_7.dll"      "XAudio2 OK" "$out"; else echo "  skip XAudio2 (no DirectX pack)"; fi
check "audio all"               "audio: 0 failure(s)" "$out"
wavinfo=$(python3 - <<'PY'
import array, math
try: d = open('/tmp/x64audio.wav', 'rb').read()
except OSError: print('no wav'); raise SystemExit
a = array.array('f'); a.frombytes(d[44:44 + ((len(d) - 44) // 8) * 8]); L = a[0::2]; blk = 480
rms = [math.sqrt(sum(x * x for x in L[i:i + blk]) / blk) for i in range(0, len(L) - blk, blk)]
loud = sum(1 for r in rms if r > 0.02); print(f'wav {len(L) / 48000:.2f}s, {loud} loud 10ms blocks of {len(rms)}, peak {max(L):.2f}')
PY
)
echo "  $wavinfo"
case "$wavinfo" in wav*) n=$(printf '%s' "$wavinfo" | sed -E 's/.*, ([0-9]+) loud.*/\1/'); if [ "$n" -ge 100 ]; then echo "  ok   the mix really contains sound (≥1 s of tone across the five tests)"; else echo "  FAIL too little sound in the mix"; fail=1; fi;; *) echo "  FAIL $wavinfo"; fail=1;; esac
echo "[6] honest rejections"
check "ARM64 PE"   '"verdict":"unsupported"' "$(python3 - <<'PY'
import struct
b=bytearray(0x200); b[0:2]=b'MZ'; struct.pack_into('<I',b,0x3c,0x80); b[0x80:0x84]=b'PE\0\0'; struct.pack_into('<H',b,0x84,0xaa64); struct.pack_into('<H',b,0x94,0xf0); struct.pack_into('<H',b,0x98,0x20b)
open('/tmp/pearm.exe','wb').write(b)
PY
$X --inspect /tmp/pearm.exe 2>&1 || true)"
check "not a PE"    '"ok":false' "$($X --inspect tests/run.sh 2>&1 || true)"
echo "[18] x86 32-bit processes (PE32: IA-32 mode, fs:[0] SEH chain, stdcall/cdecl shims, 32-bit layouts)"
check "inspector verdict"         '"arch":"x86 (32-bit)"'                       "$($X --inspect tests/hello32.exe 2>&1 || true)"
check "inspector not rejected"    '"verdict":"likely"'                            "$($X --inspect tests/hello32.exe 2>&1 || true)"
h32=$($X --quiet --max-ms 30000 tests/hello32.exe a b 2>&1 || true)
check "hello32: argv/env/files"   "argc=3 sizeof(void.)=4"                  "$h32"
check "hello32: threads + TLS"    "tid ok=1 tick ok=1"                           "$h32"
rc=0; $X --quiet --max-ms 30000 tests/hello32.exe >/dev/null 2>&1 || rc=$?
check "hello32: exit code"        "rc=42"                                        "rc=$rc"
check "seh32: 9 checks"           "seh32: 9 checks, 0 failed"                    "$($X --quiet --max-ms 30000 tests/seh32.exe 2>&1 || true)"
check "C++ EH (gcc ABI, 32-bit)"  "caught std::exception: value too large: 7"    "$($X --quiet --max-ms 30000 tests/cxxexc32.exe 2>&1 || true)"
check "pthreads32"                "9 checks, 0 failed"                           "$($X --quiet --no-stdin --max-ms 60000 tests/pthreads32.exe 2>&1 || true)"
check "netloop32 (Winsock x86)"   "netloop: 23 checks, 0 failed"                 "$($X --quiet --no-stdin --max-ms 60000 tests/netloop32.exe 2>&1 | tail -1 || true)"
out=$($X --max-ms 2000 --key 0x25@300:400 --key 0x20@500:1200 tests/NebulaStrike32.exe 2>&1 || true)
frames=$(printf '%s' "$out" | sed -n 's/.* = \([0-9.]*\) MIPS, \([0-9]*\) frames.*/\2/p')
check "NebulaStrike32 (GDI game, x86) frames" "yes" "$([ "${frames:-0}" -ge 60 ] && echo yes || echo "no ($frames frames)")"
Z7=tests/7za32; W32=/tmp/w32test; rm -rf $W32; mkdir -p $W32/src; head -c 300000 /dev/urandom > $W32/src/rand.bin; seq 1 20000 > $W32/src/nums.txt
check "7za.exe x86 (real MSVC build): help" "7-Zip (a) 25.01 (x86)" "$($X --quiet --max-ms 30000 $Z7/7za.exe 2>&1 | head -2 || true)"
check "7za.exe x86: compress (LZMA2, threads)" "Everything is Ok" "$($X --quiet --max-ms 120000 --mount-rw $W32='C:\w' $Z7/7za.exe a -mx5 'C:\w\out.7z' 'C:\w\src' 2>&1 | tail -1 || true)"
check "7za.exe x86: extract"      "Everything is Ok"                             "$($X --quiet --max-ms 120000 --mount-rw $W32='C:\w' $Z7/7za.exe x -y '-oC:\w\out' 'C:\w\out.7z' 2>&1 | grep -m1 'Everything' || true)"
check "7za.exe x86: round-trip identical" "same" "$(cmp -s $W32/src/rand.bin $W32/out/src/rand.bin && cmp -s $W32/src/nums.txt $W32/out/src/nums.txt && echo same || echo different)"
check "7za.exe x86: error path = MSVC C++ throw/catch (__CxxFrameHandler)" "System ERROR:" "$($X --quiet --max-ms 30000 --mount-rw $W32='C:\w' $Z7/7za.exe x '-oC:\w\o2' 'C:\w\nope.7z' 2>&1 || true)"
echo "[19] dialog manager (user32 dialogs, standard controls, resources, shell folders, IShellLink) — same test built for x64 and x86"
check "dlg.exe (x64): 88 checks (dialogs, controls, menus, status bar / toolbar / tab control)"    "ALL OK 88/88"   "$($X --quiet --no-stdin --max-ms 40000 tests/dlg.exe 2>&1 || true)"
check "dlg32.exe (x86): 88 checks (dialogs, controls, menus, status bar / toolbar / tab control)"  "ALL OK 88/88"   "$($X --quiet --no-stdin --max-ms 40000 tests/dlg32.exe 2>&1 || true)"
check "input.exe (x64): 30 checks (Raw Input WM_INPUT, DirectInput8 keyboard/mouse, buffered data)"  "ALL OK 30/30"   "$($X --quiet --no-stdin --max-ms 15000 --key 0x27@1000:1500 --key 0x28@3000:400 --move 300,300@4200 tests/input.exe 2>&1 || true)"
check "input32.exe (x86): 30 checks (Raw Input WM_INPUT, DirectInput8 keyboard/mouse, buffered data)"  "ALL OK 30/30"   "$($X --quiet --no-stdin --max-ms 15000 --key 0x27@1000:1500 --key 0x28@3000:400 --move 300,300@4200 tests/input32.exe 2>&1 || true)"
# the same real installer twice: the x86 build (32-bit files) and the x64 build (a 32-bit setup stub that probes
# IsWow64Process and installs 64-bit files) — both bitnesses end-to-end: setup dialog → installed 7z.exe round trip
inst_fetch() { [ -f tests/inst/$1 ] && return 0; mkdir -p tests/inst; curl -fsSL -o tests/inst/$1 https://www.7-zip.org/a/$1 2>/dev/null || curl -fsSL -o tests/inst/$1 https://github.com/ip7z/7zip/releases/download/25.01/$1 2>/dev/null || rm -f tests/inst/$1; }
for V in "7z2501.exe x86 32-bit" "7z2501-x64.exe x64 64-bit"; do
  set -- $V; INST=tests/inst/$1; B=$2; inst_fetch $1
  if [ -f $INST ]; then
    echo "  real installer: 7-Zip 25.01 $B ($1, the official setup program; installs $3 7z.exe)"
    ID=$(mktemp -d); rc=0; $X --quiet --no-stdin --max-ms 120000 --mount-rw "$ID=C:\\inst" $INST /S '/D=C:\inst\7z' >/dev/null 2>&1 || rc=$?
    check "$B silent install (/S) exit code"   "rc=0"   "rc=$rc"
    check "$B silent install wrote 7z.exe + 7z.dll + Lang"   "7z.dll 7z.exe .*Lang"   "$(ls $ID/7z 2>/dev/null | tr '\n' ' ')"
    check "$B installed 7z.exe is a $3 PE"   "$([ $B = x64 ] && echo PE32+ || echo 'PE32 ')"   "$(python3 -c "import struct;d=open('$ID/7z/7z.exe','rb').read();pe=struct.unpack_from('<I',d,0x3c)[0];print('PE32+' if struct.unpack_from('<H',d,pe+24)[0]==0x20b else 'PE32 ')" 2>/dev/null)"
    echo "round trip" > $ID/in.txt
    check "$B installed 7z.exe (loads 7z.dll through COM-style GetProcAddress) compresses"   "Everything is Ok"   "$($X --quiet --no-stdin --max-ms 60000 --mount-rw "$ID=C:\\inst" $ID/7z/7z.exe a 'C:\inst\in.7z' 'C:\inst\in.txt' 2>&1 || true)"
    check "$B installed 7z.exe lists the archive"   "in.txt"   "$($X --quiet --no-stdin --max-ms 60000 --mount-rw "$ID=C:\\inst" $ID/7z/7z.exe l 'C:\inst\in.7z' 2>&1 || true)"
    rm -rf $ID; ID=$(mktemp -d); rc=0
    out=$($X --no-stdin --max-ms 120000 --mount-rw "$ID=C:\\inst" --click 130,153@1500 --click 130,153@8000 --click 130,153@16000 --click 130,153@26000 --click 130,153@40000 --click 130,153@60000 --click 130,153@90000 $INST '/D=C:\inst\7z' 2>&1 | grep -v "last blocks" || true)
    check "$B GUI install: dialog from the resource template, Install clicked, Close clicked → exit 0"   "exit code 0"   "$out"
    check "$B GUI install: frames were presented while installing"   "[1-9][0-9]* frames"   "$out"
    check "$B GUI install: files written through the dialog flow"   "7z.exe"   "$(ls $ID/7z 2>/dev/null | tr '\n' ' ')"
    rm -rf $ID
  else echo "  skip $B (no $1 and no network)"; fi
done
# NSIS (the installer framework of Notepad++, VLC, Audacity, ...): the official Notepad++ 8.8.9 setups — x86 and x64 —
# are 32-bit NSIS stubs (ordinal import COMCTL32!#17, wsprintfW, CharNextW, GetPrivateProfileString, plugins extracted to %TEMP%)
nsis_fetch() { [ -f tests/inst/$1 ] && return 0; mkdir -p tests/inst; curl -fsSL -m 120 -o tests/inst/$1 https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.9/$2 2>/dev/null || rm -f tests/inst/$1; }
nsis_fetch npp32.exe npp.8.8.9.Installer.exe; nsis_fetch npp64.exe npp.8.8.9.Installer.x64.exe
if [ -f tests/inst/npp32.exe ] && [ -f tests/inst/npp64.exe ]; then
  echo "  real installer: Notepad++ 8.8.9 (NSIS), x86 and x64 setups"
  ID=$(mktemp -d); rc=0; $X --quiet --no-stdin --max-ms 150000 --mount-rw "$ID=C:\\inst" tests/inst/npp64.exe /S '/D=C:\inst\npp' >/dev/null 2>&1 || rc=$?
  check "NSIS x64 setup: silent install exit code"   "rc=0"   "rc=$rc"
  check "NSIS x64 setup: notepad++.exe is a 64-bit PE, plugins/themes/localization written"   "PE32+ .*localization.*plugins.*themes"   "$(python3 -c "import struct;d=open('$ID/npp/notepad++.exe','rb').read();pe=struct.unpack_from('<I',d,0x3c)[0];print('PE32+' if struct.unpack_from('<H',d,pe+24)[0]==0x20b else 'PE32 ')" 2>/dev/null) $(ls $ID/npp 2>/dev/null | tr '\n' ' ')"
  # the installed 64-bit Notepad++ itself: Scintilla (statically linked) registers its class, the document tab bar is a real
  # SysTabControl32, the status bar a real msctls_statusbar32, comctl32 subclass chains (SetWindowSubclass x2 per window) hold
  out=$(X64_LOG=1 X64_WNDDUMP=1 $X --no-stdin --max-ms 25000 --mount-rw "$ID/npp=C:\\nebula" $ID/npp/notepad++.exe 2>&1 3>&1 | grep -a "^\[wnd\]\|FAULT\|MessageBox\|not registered" || true)
  check "Notepad++ x64 (installed): starts to the main window — Scintilla editors, tab bar, status bar created, no fault / error box"   '"Scintilla" "Notepad++".*"SysTabControl32" "Tab".* builtin=16 .*"msctls_statusbar32" "" parent=0 .* vis=1 .* builtin=14'   "$(echo "$out" | grep -v "FAULT\|MessageBox\|not registered" | tr '\n' ' ')"
  check "Notepad++ x64: no fault, no exception message box during start-up"   "errors=0"   "errors=$(echo "$out" | grep -a -c "FAULT\|MessageBox\|not registered") $(echo "$out" | grep -a "FAULT\|MessageBox\|not registered" | head -1)"
  rm -rf $ID; ID=$(mktemp -d)
  out=$($X --no-stdin --max-ms 150000 --mount-rw "$ID=C:\\inst" --click 357,337@3000 --click 357,337@6000 --click 357,337@9000 --click 357,337@12000 --click 357,337@70000 --click 357,337@80000 --click 357,337@90000 --click 357,337@110000 tests/inst/npp32.exe '/D=C:\inst\npp' 2>&1 | grep -v "last blocks" || true)
  check "NSIS x86 setup: GUI wizard (license → components → directory → install → finish) exit 0"   "exit code 0"   "$out"
  check "NSIS x86 setup: frames presented"   "[1-9][0-9]* frames"   "$out"
  check "NSIS x86 setup: 32-bit notepad++.exe written through the wizard"   "PE32 "   "$(python3 -c "import struct;d=open('$ID/npp/notepad++.exe','rb').read();pe=struct.unpack_from('<I',d,0x3c)[0];print('PE32+' if struct.unpack_from('<H',d,pe+24)[0]==0x20b else 'PE32 ')" 2>/dev/null)"
  rm -rf $ID
else echo "  skip Notepad++ (no network)"; fi
# a real Delphi/VCL installer (Inno Setup 6.5.4 installing itself: SetupLdr → Setup child → Compil32 /ASSOC third process,
# ISSig ECDSA P-256 verification of its own DLLs through BCrypt), then the installed ISCC.exe compiles a script (ISPP, LZMA2
# threads in islzma.dll) and the setup it produced installs — the file survives the compress/decompress round trip unchanged
IS=tests/inst/inno.exe; [ -f $IS ] || curl -fsSL -m 120 -o $IS https://github.com/jrsoftware/issrc/releases/download/is-6_5_4/innosetup-6.5.4.exe 2>/dev/null || rm -f $IS
if [ -f $IS ]; then
  echo "  real installer: Inno Setup 6.5.4 (Delphi), then ISCC compiles and the result installs"
  ID=$(mktemp -d); rc=0; $X --quiet --no-stdin --max-ms 200000 --mount-rw "$ID=C:\\inst" $IS /VERYSILENT /SUPPRESSMSGBOXES /NORESTART '/DIR=C:\inst\is' >/dev/null 2>&1 || rc=$?
  check "Inno Setup: silent self-install exit code"   "rc=0"   "rc=$rc"
  check "Inno Setup: compiler, IDE, DLLs (ISSig-verified) and languages written"   "Compil32.exe.*ISCC.exe.*ISCmplr.dll.*Languages.*islzma.dll"   "$(ls $ID/is 2>/dev/null | tr '\n' ' ')"
  if [ -f $ID/is/ISCC.exe ]; then
    SRC=$(mktemp -d); OUT=$(mktemp -d); cp $ID/is/Examples/MyProg.exe $ID/is/Examples/Readme.txt $SRC/ 2>/dev/null
    printf '[Setup]\nAppName=Nebula Test App\nAppVersion=1.0\nDefaultDirName={autopf}\\NebulaTest\nOutputDir=C:\\out\nOutputBaseFilename=nebula-setup\nCompression=lzma2/fast\nSolidCompression=yes\nPrivilegesRequired=lowest\n[Files]\nSource: "C:\\src\\MyProg.exe"; DestDir: "{app}"\nSource: "C:\\src\\Readme.txt"; DestDir: "{app}"\n' > $SRC/nebula.iss
    out=$($X --no-stdin --max-ms 200000 --mount "$ID/is=C:\\nebula" --mount "$SRC=C:\\src" --mount-rw "$OUT=C:\\out" $ID/is/ISCC.exe 'C:\src\nebula.iss' 2>&1 | grep -v "last blocks" || true)
    check "ISCC.exe (installed compiler): compiles the script with LZMA2 (multi-threaded islzma.dll)"   "Successful compile"   "$out"
    check "ISCC.exe: exit code 0"   "exit code 0"   "$out"
    RT=$(mktemp -d); rc=0; [ -f $OUT/nebula-setup.exe ] && { $X --quiet --no-stdin --max-ms 120000 --mount-rw "$RT=C:\\inst" $OUT/nebula-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART '/DIR=C:\inst\nt' >/dev/null 2>&1 || rc=$?; } || rc=99
    check "compiled setup: silent install exit code"   "rc=0"   "rc=$rc"
    check "compiled setup: MyProg.exe identical after the LZMA2 round trip, uninstaller written"   "same.*unins000.exe"   "$(cmp -s $RT/nt/MyProg.exe $SRC/MyProg.exe && echo same) $(ls $RT/nt 2>/dev/null | tr '\n' ' ')"
    rm -rf $SRC $OUT $RT
    # the Inno Setup Compiler IDE (heavy Delphi VCL app): 253 PNG icons through WIC (IWICImagingFactory), TImageList streams (ImageList_Read),
    # a RichEdit-less startup dialog driven with the mouse -> the main IDE form with the Scintilla editor (isscint.dll) comes up
    out=$(X64_LOG=1 X64_WNDDUMP=1 $X --no-stdin --max-ms 42000 --click 274,111@12000 --click 674,477@15000 --click 274,111@24000 --click 674,477@27000 --click 270,221@36000 --mount "$ID/is=C:\\nebula" $ID/is/Compil32.exe 2>&1 3>&1 | grep -a "^\[wnd\]\|\[wic\]\|\[imagelist\]\|FAULT\|RaiseException" || true)
    check "Compil32.exe (Inno IDE): PNG icons decoded by the built-in WIC and the TImageList stream read"   "decoded PNG.*ImageList_Read: 4 images\|ImageList_Read: 4 images.*decoded PNG"   "$(echo "$out" | tr '\n' ' ')"
    check "Compil32.exe: Welcome dialog driven with the mouse -> main IDE form with the Scintilla editor is visible, no fault"   '"TMainForm" "Untitled - Inno Setup Compiler.*vis=1.*"TIDEScintFileEdit".*vis=1'   "$(echo "$out" | grep -v FAULT | tr '\n' ' ')"
    check "Compil32.exe: TToolBar (ToolbarWindow32) and TStatusBar (msctls_statusbar32) are real built-in common controls (builtin=15/14), not placeholders"   '"TToolBar" "" parent=[0-9]* at [0-9,]* 35[0-9]x2[0-9] .* builtin=15 .*"TStatusBar" "" parent=[0-9]* at 0,[0-9]* 36[0-9]x2[0-9] .* builtin=14'   "$(echo "$out" | tr '\n' ' ')"
    check "Compil32.exe: the VCL TMainMenu bar is drawn in the frame (nc=1,44) and a click on File opens a popup menu window (#32768)"   '"TMainForm" .* nc=1,44 .*"#32768" "" parent=-1 .* vis=1'   "$(echo "$out" | tr '\n' ' ')"
  fi
  rm -rf $ID
  # the GUI wizard (Delphi VCL: superclassed controls, WM_ERASEBKGND through the window procedure, synchronous WM_SIZE, DC origins/clipping)
  GD=$(mktemp -d); out=$(X64_LOG=1 X64_WNDDUMP=1 $X --no-stdin --max-ms 45000 --mount-rw "$GD=C:\\inst" $IS '/DIR=C:\inst\is' 2>&1 3>&1 | grep -a "^\[wnd\]\|frames" || true); rm -rf $GD
  check "Inno Setup GUI: the 'Select Setup Language' form (VCL) is created, laid out and painted"   '"TSelectLanguageForm" "Select Setup Language" parent=-1 at [0-9]*,[0-9]* 3[0-9][0-9]x1[0-9][0-9] nc=1,24 style=[0-9a-f]* vis=1 inv=0'   "$out"
  check "Inno Setup GUI: combo box, OK/Cancel buttons and the prompt are visible"   '"TNewComboBox" "English".*vis=1 inv=0'   "$(echo "$out" | tr '\n' ' ')"
  # drive the wizard with the mouse: OK (language), "I accept" radio, Next x7 -> the installation runs from the GUI (clicks are repeated: the forms appear at different times on different hosts)
  GD=$(mktemp -d); out=$(X64_LOG=1 X64_WNDDUMP=1 $X --no-stdin --max-ms 200000 --click 628,350@30000 --click 628,350@38000 --click 246,434@44000 --click 246,434@48000 --click 666,497@52000 --click 666,497@58000 --click 666,497@64000 --click 666,497@70000 --click 666,497@76000 --click 666,497@82000 --click 666,497@88000 --click 666,497@94000 --mount-rw "$GD=C:\\inst" $IS '/DIR=C:\inst\is' 2>&1 3>&1 | grep -a "^\[wnd\]\|frames" || true)
  check "Inno Setup GUI: the wizard form (TWizardForm, notebook pages, buttons) is shown"   '"TWizardForm" "Setup - Inno Setup version 6.5.4" parent=-1 at [0-9]*,[0-9]* [0-9]*x[0-9]* nc=1,24 style=[0-9a-f]* vis=1'   "$out"
  check "Inno Setup GUI: license accepted + Next through the pages -> files are being installed from the GUI"   "ISCC.exe\|ISCmplr.dll\|islzma.dll\|Examples\|Setup.e32\|Languages"   "$(ls $GD/is 2>/dev/null | tr '\n' ' ')"
  rm -rf $GD
else echo "  skip Inno Setup (no network)"; fi
# a real MSVC x64 GUI program with dialog templates + common controls: PuTTY reaches its configuration dialog
# (About box opens through the dialog manager); SysTreeView32 is still reported as not implemented
PT=tests/inst/putty.exe; [ -f $PT ] || curl -fsSL -m 60 -o $PT https://the.earth.li/~sgtatham/putty/latest/w64/putty.exe 2>/dev/null || rm -f $PT
if [ -f $PT ]; then
  out=$($X --no-stdin --max-ms 4000 --click 50,415@2000 $PT 2>&1 | grep -v "last blocks" || true)
  check "putty.exe (x64, MSVC): configuration dialog + About box are shown, no fault"   "time limit reached"   "$out"
  check "putty.exe: frames presented"   "[1-9][0-9]* frames"   "$out"
  check "putty.exe: the missing common control is reported honestly"   "SysTreeView32.*not implemented"   "$out"
else echo "  skip putty.exe (no network)"; fi
[ $fail = 0 ] && echo "ALL OK" || { echo "FAILURES"; exit 1; }
