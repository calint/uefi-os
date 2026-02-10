#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

FLAGS="-std=c++26 -target x86_64-unknown-windows-msvc -Wfatal-errors -Werror"
ASMFLAGS=""
CPPFLAGS="-ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone \
    -fno-exceptions -fno-rtti \
    -O3 -g -gdwarf"
WARNINGS="-Weverything \
    -Wno-c++98-compat \
    -Wno-c++98-compat-pedantic \
    -Wno-pre-c++20-compat-pedantic \
    -Wno-c99-extensions \
    -Wno-reserved-identifier \
    -Wno-reserved-macro-identifier \
    -Wno-unsafe-buffer-usage \
    -Wno-missing-prototypes \
    -Wno-language-extension-token \
    -Wno-gnu-anonymous-struct \
    -Wno-undef \
    -Wno-padded \
    -Wno-unused-variable \
    -Wno-unused-function \
    -Wno-unused-argument \
    -Wno-unneeded-member-function \
    -Wno-unique-object-duplication \
    -Wno-gnu-alignof-expression \
    "

clang++ $FLAGS $CPPFLAGS $WARNINGS \
    -I /usr/include/efi/ \
    -c src/uefi.cpp -o uefi.o

clang++ $FLAGS $ASMFLAGS $WARNINGS \
    -c src/kernel_asm.s -o kernel_asm.o

clang++ $FLAGS $CPPFLAGS $WARNINGS \
    -I /usr/include/efi/ \
    -c src/kernel.cpp -o kernel.o

clang++ $FLAGS $CPPFLAGS $WARNINGS \
    -I /usr/include/efi/ \
    -c src/osca.cpp -o osca.o

clang++ -target x86_64-unknown-windows-msvc \
    -nostdlib \
    -fuse-ld=lld \
    -Wl,-entry:efi_main \
    -Wl,-subsystem:efi_application \
    -o esp/EFI/BOOT/BOOTX64.EFI \
    uefi.o kernel_asm.o kernel.o osca.o

qemu-system-x86_64 -enable-kvm -m 16G -vga std -serial stdio \
    -smp 4,sockets=1,cores=2,threads=2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive format=raw,file=fat:rw:esp # -full-screen
