#!/usr/bin/env bash
# runtime-pack.sh — builds the Windows "runtime pack" served at /x64/sys32/ (→ C:\Windows\System32 inside the emulator).
#
# Contents: the x64 DLLs of the Microsoft Visual C++ 2015–2022 Redistributable (msvcp140*, vcruntime140*, concrt140,
# vcomp140, vcamp140, vccorlib140, mfc140*, mfcm140*), extracted from the official vc_redist.x64.exe at build time.
# The files are not committed: this script runs in CI (and locally) and writes public/x64/sys32/<name>.dll plus
# manifest.json {files:[{name,size,sha256,version}], source, built}.  The emulator prefers these real DLLs over its
# built-in shims for the vendor-runtime names (see pe_dll_native_ok in emu/x64/pe.c); core OS DLLs stay built-in.
#
# Usage: emu/tools/runtime-pack.sh [outdir]      (needs curl + cabextract + python3)
set -euo pipefail
OUT=${1:-public/x64/sys32}
URL=${VC_REDIST_URL:-https://aka.ms/vs/17/release/vc_redist.x64.exe}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
echo "runtime-pack: downloading $URL"
curl -fL --retry 5 --retry-delay 3 -o "$work/vc_redist.x64.exe" "$URL"
ls -la "$work/vc_redist.x64.exe"
# the Burn bundle carries its payload as cabinets (a10..a13); each payload is itself a cabinet with <name>.dll_<arch> entries
cabextract -q -d "$work/s1" "$work/vc_redist.x64.exe"
for f in "$work"/s1/*; do cabextract -q -d "$work/s2" "$f" 2>/dev/null || true; done
mkdir -p "$OUT"
n=0
for f in "$work"/s2/*.dll_amd64; do b=$(basename "$f"); cp "$f" "$OUT/${b%_amd64}"; n=$((n + 1)); done
[ "$n" -gt 0 ] || { echo "runtime-pack: no amd64 DLLs found in the redistributable" >&2; exit 1; }
python3 - "$OUT" "$URL" "$work/vc_redist.x64.exe" <<'EOF'
import sys, os, json, hashlib, struct, time
out, url, bundle = sys.argv[1], sys.argv[2], sys.argv[3]
def file_version(data):
    # VS_FIXEDFILEINFO follows the "VS_VERSION_INFO" key; signature 0xFEEF04BD, then dwFileVersionMS/LS
    key = 'VS_VERSION_INFO'.encode('utf-16-le')
    i = data.find(key)
    while i >= 0:
        j = data.find(b'\xbd\x04\xef\xfe', i, i + 64)
        if j > 0:
            ms, ls = struct.unpack_from('<II', data, j + 8)
            return f'{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}'
        i = data.find(key, i + 1)
    return ''
files = []
for name in sorted(os.listdir(out)):
    if not name.lower().endswith('.dll'): continue
    d = open(os.path.join(out, name), 'rb').read()
    files.append({ 'name': name, 'size': len(d), 'sha256': hashlib.sha256(d).hexdigest(), 'version': file_version(d) })
man = { 'pack': 'Microsoft Visual C++ 2015-2022 Redistributable (x64)', 'source': url, 'bundle_sha256': hashlib.sha256(open(bundle, 'rb').read()).hexdigest(),
        'built': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'guest_dir': 'C:\\Windows\\System32', 'files': files }
json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print(f"runtime-pack: {len(files)} DLLs, {sum(f['size'] for f in files) / 1048576:.1f} MB → {out}")
for f in files: print(f"  {f['name']:32} {f['size']:>9}  {f['version']}")
EOF
