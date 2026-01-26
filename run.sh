#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

FLAGS="-Wfatal-errors -Werror"
WARNINGS="-Weverything -Wno-c++98-compat -Wno-reserved-macro-identifier \
    -Wno-reserved-identifier -Wno-reserved-macro-identifier \
    -Wno-unsafe-buffer-usage -Wno-pre-c++20-compat-pedantic \
    -Wno-missing-prototypes -Wno-padded -Wno-c++98-compat-pedantic \
    -Wno-language-extension-token -Wno-undef -Wno-unused-variable \
    -Wno-unused-function"

clang++ -std=c++26 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -mno-red-zone \
    -fno-builtin \
    -I /usr/include/efi/ \
    $FLAGS \
    $WARNINGS \
    -c src/uefi.cpp -o uefi.o

clang++ -std=c+26 -target x86_64-unknown-windows-msvc \
    $FLAGS \
    $WARNINGS \
    -c src/kernel_asm.s -o kernel_asm.o

clang++ -std=c++26 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -mno-red-zone \
    -fno-exceptions -fno-rtti \
    -fno-builtin \
    -I /usr/include/efi/ \
    $FLAGS \
    $WARNINGS \
    -c src/kernel.cpp -o kernel.o

clang++ -std=c++26 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -mno-red-zone \
    -fno-exceptions -fno-rtti \
    -fno-builtin \
    -I /usr/include/efi/ \
    $FLAGS \
    $WARNINGS \
    -c src/osca.cpp -o osca.o

clang -target x86_64-unknown-windows-msvc \
    -nostdlib \
    -fuse-ld=lld \
    -Wl,-entry:efi_main \
    -Wl,-subsystem:efi_application \
    -o esp/EFI/BOOT/BOOTX64.EFI \
    uefi.o kernel_asm.o kernel.o osca.o

qemu-system-x86_64 -m 1G -vga std -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive format=raw,file=fat:rw:esp # -full-screen

# sudo qemu-system-x86_64 -m 1G -serial stdio -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd -drive format=raw,file=/dev/sda
