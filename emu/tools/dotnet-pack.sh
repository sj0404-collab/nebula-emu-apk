#!/usr/bin/env bash
# dotnet-pack.sh — builds the ".NET pack" served at /x64/dotnet/ (→ C:\Program Files\dotnet inside the emulator).
#
# Contents: the official Microsoft .NET Runtime (win-x64, the "dotnet-runtime-<ver>-win-x64.zip" from
# builds.dotnet.microsoft.com): dotnet.exe, host/fxr/<ver>/hostfxr.dll, shared/Microsoft.NETCore.App/<ver>/
# (hostpolicy, coreclr, clrjit, System.Private.CoreLib and the framework assemblies).  Nothing is modified — CoreCLR and
# the JIT run as real x64 code on the emulator; the emulator only provides the Win32 surface they call.
# The files are not committed: this script runs in CI (and locally) and writes public/x64/dotnet/<path>.gz (per-file
# gzip, the browser inflates with DecompressionStream) plus manifest.json {pack, version, source, zip_sha512, built,
# guest_dir, encoding, files:[{path,size,gz,gzsize,sha256}]}.
#
# Usage: emu/tools/dotnet-pack.sh [outdir=public/x64/dotnet] [extractdir]      (needs curl + unzip + gzip + python3)
#   DOTNET_RUNTIME_VERSION  (default 10.0.14)   DOTNET_RUNTIME_URL  (default: the official zip; file:// works)
#   extractdir: also leave the plain, uncompressed tree there (what emu/tests/dotnet.sh mounts as C:\Program Files\dotnet)
set -euo pipefail
OUT=${1:-public/x64/dotnet}
PLAIN=${2:-}
VER=${DOTNET_RUNTIME_VERSION:-10.0.14}
URL=${DOTNET_RUNTIME_URL:-https://builds.dotnet.microsoft.com/dotnet/Runtime/$VER/dotnet-runtime-$VER-win-x64.zip}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -n "$PLAIN" ] && { mkdir -p "$PLAIN"; PLAIN=$(cd "$PLAIN" && pwd); }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
echo "dotnet-pack: downloading $URL"
case "$URL" in file://*) cp "${URL#file://}" "$work/rt.zip" ;; *) curl -fL --retry 5 --retry-delay 3 -o "$work/rt.zip" "$URL" ;; esac
ls -la "$work/rt.zip"
mkdir -p "$work/rt"; unzip -q "$work/rt.zip" -d "$work/rt"
[ -f "$work/rt/dotnet.exe" ] || { echo "dotnet-pack: dotnet.exe not found in the zip" >&2; exit 1; }
# keep what the emulator uses: the muxer, hostfxr, the shared framework (drop PDBs / docs / native non-win files)
rm -rf "$OUT"/*; mkdir -p "$OUT"
n=0
while IFS= read -r -d '' f; do
  rel=${f#"$work/rt/"}
  case "$rel" in *.pdb|*.txt|*.md|LICENSE*|ThirdPartyNotices*) continue ;; esac
  mkdir -p "$OUT/$(dirname "$rel")"; gzip -9 -n -c "$f" > "$OUT/$rel.gz"; n=$((n + 1))
  if [ -n "$PLAIN" ]; then mkdir -p "$PLAIN/$(dirname "$rel")"; cp "$f" "$PLAIN/$rel"; fi
done < <(find "$work/rt" -type f -print0)
[ "$n" -gt 0 ] || { echo "dotnet-pack: nothing extracted" >&2; exit 1; }
python3 - "$OUT" "$URL" "$work/rt.zip" "$work/rt" "$VER" <<'EOF'
import sys, os, json, hashlib, time
out, url, zipf, src, ver = sys.argv[1:6]
files = []
for root, _, names in os.walk(src):
    for name in sorted(names):
        full = os.path.join(root, name); rel = os.path.relpath(full, src)
        gz = os.path.join(out, rel + '.gz')
        if not os.path.exists(gz): continue
        d = open(full, 'rb').read()
        files.append({ 'path': rel.replace(os.sep, '\\'), 'size': len(d), 'gz': rel.replace(os.sep, '/') + '.gz', 'gzsize': os.path.getsize(gz), 'sha256': hashlib.sha256(d).hexdigest() })
files.sort(key=lambda f: f['path'].lower())
man = { 'pack': 'Microsoft .NET Runtime ' + ver + ' (win-x64)', 'version': ver, 'source': url, 'zip_sha512': hashlib.sha512(open(zipf, 'rb').read()).hexdigest(),
        'built': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'guest_dir': 'C:\\Program Files\\dotnet', 'encoding': 'gzip', 'files': files }
json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
tot = sum(f['size'] for f in files); gz = sum(f['gzsize'] for f in files)
print(f"dotnet-pack: {len(files)} files, {tot / 1048576:.1f} MB ({gz / 1048576:.1f} MB gzipped) → {out}")
for f in files:
    if f['path'].count('\\') < 2 or f['path'].endswith(('coreclr.dll', 'clrjit.dll', 'hostpolicy.dll', 'System.Private.CoreLib.dll')): print(f"  {f['path']:70} {f['size']:>9}")
EOF
