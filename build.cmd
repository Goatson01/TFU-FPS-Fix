@echo off
rem Builds build\dinput8.dll (32-bit Windows DLL) with LLVM for Windows
rem (clang, lld-link and llvm-dlltool must be on PATH, e.g. after "winget install LLVM.LLVM").
setlocal
cd /d "%~dp0"
if not exist build mkdir build
llvm-dlltool -m i386 -k -d src\kernel32.def -l build\kernel32.lib || goto :fail
llvm-dlltool -m i386 -k -d src\user32.def -l build\user32.lib || goto :fail
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -c src\tfufix.c -o build\tfufix.obj || goto :fail
lld-link /dll /entry:DllMain /nodefaultlib /machine:x86 /safeseh:no /Brepro /def:src\dinput8.def build\tfufix.obj build\kernel32.lib build\user32.lib /out:build\dinput8.dll || goto :fail
echo Built build\dinput8.dll
exit /b 0
:fail
echo Build failed.
exit /b 1
