// src/kernel.cpp
#include <stdint.h>

extern "C" {
    auto kernel_main(uint32_t* fb, uint32_t stride) -> void {
        // at this point, we are the only thing running on the cpu
        // draw a simple red square to prove we are in the kernel
        for (uint32_t y = 200; y < 300; y++) {
            for (uint32_t x = 200; x < 300; x++) {
                fb[y * stride + x] = 0x00FF0000;
            }
        }

        while (true) {
            __asm__("hlt");
        }
    }
}
