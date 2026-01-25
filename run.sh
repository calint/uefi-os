#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

clang++ -std=c++26 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -mno-red-zone \
    -I /usr/include/efi/ \
    -c src/uefi.cpp -o uefi.o

clang++ -std=c+26 -target x86_64-unknown-windows-msvc \
    -c src/kernel_asm.s -o kernel_asm.o

clang++ -std=c++26 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -mno-red-zone \
    -fno-exceptions -fno-rtti \
    -c src/kernel.cpp -o kernel.o

clang -target x86_64-unknown-windows-msvc \
    -nostdlib \
    -fuse-ld=lld \
    -Wl,-entry:efi_main \
    -Wl,-subsystem:efi_application \
    -o esp/EFI/BOOT/BOOTX64.EFI \
    uefi.o kernel.o kernel_asm.o

qemu-system-x86_64 -m 1G -vga std -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive format=raw,file=fat:rw:esp -full-screen
