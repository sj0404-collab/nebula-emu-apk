#!/usr/bin/env bash
# dn.sh [extra x64run opts…] — run a .NET app inside the emulator with the extracted runtime at /var/tmp/dn/dotnet
# usage: emu/tools/dn.sh [--app host-dir=C:\app] <exe-or-dll (guest path or host path)> args…   output → /tmp/dn.out
cd "$(dirname "$0")/../.."
X64_LOG=1 timeout "${T:-180}" ./emu/x64/x64run --no-stdin --max-ms "${MS:-170000}" --mount '/var/tmp/dn/dotnet=C:\Program Files\dotnet' --mount '/var/tmp/dn/app=C:\app' --mount '/var/tmp/dn/fx=C:\fx' --mount-rw '/var/tmp/dn/tmp=C:\tmp' "$@" > /tmp/dn.out 2>&1 3>&1
echo "[rc $?]" >> /tmp/dn.out
grep -av "GetProcAddress\|loaded\|last blocks\|thread\|LoadLibrary\|mounted" /tmp/dn.out | tail -${N:-15}
