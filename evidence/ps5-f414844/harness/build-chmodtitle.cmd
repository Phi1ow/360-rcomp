@echo off
set PATH=D:\ps5-toolchain\clang+llvm-18.1.8-x86_64-pc-windows-msvc\bin;%PATH%
cd /d D:\ps5-toolchain\chmodtitle
call D:\ps5-toolchain\ps5-payload-sdk\win\prospero-clang.cmd -fvisibility-nodllstorageclass=default -std=c11 -O2 -Wall -Wextra -Werror -o chmodtitle.elf main.c
echo exit=%ERRORLEVEL%
