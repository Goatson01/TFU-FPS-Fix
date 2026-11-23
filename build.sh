#!/bin/sh
# Builds build/dinput8.dll (32-bit Windows DLL) with LLVM: clang, lld-link and llvm-dlltool.
# Works on Linux, macOS and WSL. Builds are reproducible (/Brepro): the same source always
# produces a byte-identical DLL.
set -e
cd "$(dirname "$0")"
mkdir -p build
llvm-dlltool -m i386 -k -d src/kernel32.def -l build/kernel32.lib
llvm-dlltool -m i386 -k -d src/user32.def -l build/user32.lib
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
      -mno-stack-arg-probe -Wall -c src/tfufix.c -o build/tfufix.obj
lld-link /dll /entry:DllMain /nodefaultlib /machine:x86 /safeseh:no /Brepro \
         /def:src/dinput8.def build/tfufix.obj build/kernel32.lib build/user32.lib \
         /out:build/dinput8.dll
echo "Built build/dinput8.dll"
