#!/bin/sh
# Сборка qrcode1c.dll (Win32, x86) и тестового стенда кросс-компилятором mingw-w64.
# Нужны пакеты: mingw-w64 mingw-w64-tools wine32 (из него stdole2.tlb для библиотеки типов).
set -eu
cd "$(dirname "$0")"

# Вариант компилятора с потоками win32: не тянет libwinpthread-1.dll.
CC=i686-w64-mingw32-gcc-win32
CXX=i686-w64-mingw32-g++-win32
WINDRES=i686-w64-mingw32-windres
WIDL=i686-w64-mingw32-widl
STDOLE_DIR=/usr/lib/i386-linux-gnu/wine/i386-windows

# Windows XP SP3 и новее: API не новее XP, версия подсистемы PE 5.1.
WINVER="-D_WIN32_WINNT=0x0501 -DWINVER=0x0501"
CFLAGS="-O2 -DNDEBUG $WINVER -Wall -Wextra"
LDFLAGS="-static -static-libgcc -static-libstdc++ -s \
  -Wl,--major-subsystem-version,5,--minor-subsystem-version,1 \
  -Wl,--major-os-version,5,--minor-os-version,1 \
  -Wl,--enable-stdcall-fixup"

mkdir -p build
$WIDL -t -L "$STDOLE_DIR" -o build/qrcode1c.tlb src/qrcode1c.idl
$WINDRES --codepage=65001 -I src -I build -i src/qrcode1c.rc -o build/qrcode1c_res.o
$CC $CFLAGS -std=c99 -c third_party/qrcodegen/qrcodegen.c -o build/qrcodegen.o
# Компоновка через gcc, а не g++: без libstdc++ (см. комментарий у __cxa_pure_virtual).
$CC $CFLAGS -x c++ -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -I third_party/qrcodegen \
  -shared -o build/qrcode1c.dll src/qrcode1c.cpp src/png.cpp -x none build/qrcodegen.o build/qrcode1c_res.o \
  src/qrcode1c.def $LDFLAGS -lole32 -loleaut32 -luuid -ladvapi32 -lshlwapi

$CXX $CFLAGS -municode -o build/harness.exe test/harness.cpp \
  $LDFLAGS -lole32 -loleaut32 -luuid -ladvapi32 -lshell32

echo "OK: build/qrcode1c.dll"
