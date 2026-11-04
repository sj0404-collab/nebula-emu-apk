#!/bin/sh
# native-only quick build
cd "$(dirname "$0")" && cc -O2 -D_FORTIFY_SOURCE=2 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-misleading-indentation -o x64run lib.c cpu.c pe.c win32.c gdi.c vfs.c api.c dxbc.c host_native.c main_native.c -lm
