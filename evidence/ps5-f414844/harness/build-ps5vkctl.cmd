@echo off
set PATH=D:\ps5-toolchain\clang+llvm-18.1.8-x86_64-pc-windows-msvc\bin;%PATH%
cd /d D:\ps5-toolchain\ps5vkctl
call D:\ps5-toolchain\ps5-payload-sdk\win\prospero-clang.cmd -fvisibility-nodllstorageclass=default -std=c11 -O2 -g -Wall -Wextra -Werror -o ps5vkctl.elf main.c -lSceSystemService -lSceUserService
echo exit=%ERRORLEVEL%
