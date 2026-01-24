#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

clang -std=c23 -target x86_64-unknown-windows-msvc \
    -ffreestanding \
    -mno-red-zone \
    -fno-stack-protector \
    -nostdlib \
    -fuse-ld=lld \
    -Wl,-entry:efi_main \
    -Wl,-subsystem:efi_application \
    -I /usr/include/efi/ \
    -o esp/EFI/BOOT/BOOTX64.EFI \
    src/main.c

qemu-system-x86_64 -m 1G -vga std -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive format=raw,file=fat:rw:esp
