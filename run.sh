#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

#!/bin/bash
set -e

mkdir -p esp/EFI/BOOT

clang++ -std=c++23 -target x86_64-unknown-windows-msvc \
    -ffreestanding -fno-stack-protector -fno-exceptions -fno-rtti \
    -mno-red-zone -c src/kernel.cpp -o kernel.o

clang -std=c23 -target x86_64-unknown-windows-msvc \
    -ffreestanding -mno-red-zone -fno-stack-protector \
    -I /usr/include/efi/ -c src/main.c -o main.o

clang -target x86_64-unknown-windows-msvc \
    -nostdlib \
    -fuse-ld=lld \
    -Wl,-entry:efi_main \
    -Wl,-subsystem:efi_application \
    -o esp/EFI/BOOT/BOOTX64.EFI \
    main.o kernel.o

qemu-system-x86_64 -m 1G -vga std -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive format=raw,file=fat:rw:esp -full-screen
