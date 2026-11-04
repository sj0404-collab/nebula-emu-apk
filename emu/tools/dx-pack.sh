#!/usr/bin/env bash
# dx-pack.sh — builds the "DirectX pack" served at /x64/dx/ (→ C:\Windows\System32 inside the emulator).
#
# Contents: the x64 DLLs of the official Microsoft DirectX End-User Runtimes (June 2010) that are pure user-mode code and
# therefore run on the emulator as-is: the HLSL shader compilers D3DCompiler_33…43, the D3DX helper libraries d3dx9_24…43,
# d3dx10_33…43, d3dx11_42/43, the compute helpers d3dcsx_42/43, X3DAudio1_3…7 and — since v1.19 — the audio engines
# XAudio2_0…7, XAPOFX1_0…5 and xactengine3_0…7 (they sit on top of the emulator's WASAPI/mmdevapi layer, which plays through
# the device's sound output).  NOT included: xactengine2_x (XACT 2, DirectSound-era, no users) and xinput1_x (HID
# enumeration — the emulator answers XInput itself: "no controller connected").
# Nothing is modified: the cabinets inside directx_Jun2010_redist.exe are extracted with cabextract, every DLL is gzipped
# per file (the browser inflates with DecompressionStream) and listed in manifest.json {pack, source, bundle_sha256, built,
# guest_dir, encoding, files:[{path,size,gz,gzsize,sha256,version}]} — the same layout as the .NET pack.
#
# Usage: emu/tools/dx-pack.sh [outdir=public/x64/dx] [plaindir]      (needs curl + cabextract + gzip + python3)
set -euo pipefail
OUT=${1:-public/x64/dx}; PLAIN=${2:-}
URL=${DX_REDIST_URL:-https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
if [ -n "${DX_REDIST_FILE:-}" ] && [ -f "$DX_REDIST_FILE" ]; then cp "$DX_REDIST_FILE" "$work/redist.exe"; else
  echo "dx-pack: downloading $URL"; curl -fL --retry 5 --retry-delay 3 -o "$work/redist.exe" "$URL"; fi
ls -la "$work/redist.exe"
cabextract -q -d "$work/s1" "$work/redist.exe"
mkdir -p "$work/dll"
for cab in "$work"/s1/*_x64.cab; do
  case "$(basename "$cab")" in
    *D3DCompiler_*|*d3dx9_*|*d3dx10_*|*d3dx11_*|*d3dcsx_*|*X3DAudio_*|*XAudio_*|*XACT_*|*xact_*) cabextract -q -d "$work/dll" "$cab" 2>/dev/null || true ;;
  esac
done
rm -rf "$OUT"; mkdir -p "$OUT"; [ -n "$PLAIN" ] && mkdir -p "$PLAIN"
n=0
for f in "$work"/dll/*.dll; do
  b=$(basename "$f")
  case "$b" in xactengine2_*.dll|xinput*.dll) continue ;; esac   # XACT2 / XInput stay out (see the header)
  gzip -9 -n -c "$f" > "$OUT/$b.gz"; [ -n "$PLAIN" ] && cp "$f" "$PLAIN/$b"; n=$((n + 1))
done
[ "$n" -gt 0 ] || { echo "dx-pack: no x64 DLLs found in the redistributable" >&2; exit 1; }
python3 - "$OUT" "$URL" "$work/redist.exe" "$work/dll" <<'EOF'
import sys, os, json, hashlib, struct, time
out, url, bundle, plain = sys.argv[1:5]
def file_version(data):
    key = 'VS_VERSION_INFO'.encode('utf-16-le'); i = data.find(key)
    while i >= 0:
        j = data.find(b'\xbd\x04\xef\xfe', i, i + 64)
        if j > 0:
            ms, ls = struct.unpack_from('<II', data, j + 8); return f'{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}'
        i = data.find(key, i + 1)
    return ''
files = []
for name in sorted(os.listdir(plain), key=str.lower):
    if not name.lower().endswith('.dll') or not os.path.exists(os.path.join(out, name + '.gz')): continue
    d = open(os.path.join(plain, name), 'rb').read(); gz = os.path.getsize(os.path.join(out, name + '.gz'))
    files.append({ 'path': name, 'size': len(d), 'gz': name + '.gz', 'gzsize': gz, 'sha256': hashlib.sha256(d).hexdigest(), 'version': file_version(d) })
man = { 'pack': 'Microsoft DirectX End-User Runtimes (June 2010), x64 user-mode DLLs', 'source': url, 'bundle_sha256': hashlib.sha256(open(bundle, 'rb').read()).hexdigest(),
        'built': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'guest_dir': 'C:\\Windows\\System32', 'encoding': 'gzip', 'files': files }
json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
tot = sum(f['size'] for f in files); gz = sum(f['gzsize'] for f in files)
print(f"dx-pack: {len(files)} DLLs, {tot / 1048576:.1f} MB ({gz / 1048576:.1f} MB gzipped) → {out}")
for f in files: print(f"  {f['path']:24} {f['size']:>9}  {f['version']}")
EOF
