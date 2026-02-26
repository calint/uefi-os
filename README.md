# osca x64 uefi

## intention

* minimalist uefi bootable kernel
* render graphics on screen
* keyboard
* timer
* main core schedules jobs executed by remaining cores

## scope

* x86-64 only
* uefi gop required
* acpi 2.0+
* assumes compliant modern firmware and hardware
* no legacy bios
* ps/2 keyboard required
* requires more than 1 logical processor
* all memory allocated up-front
* correctness assumed; exceptions reboot the system

## coding convention (opinionated)

* if possible, declare `auto` being the first word in a declaration
  * applies to functions, inline, constexpr, const etc
* `nullptr` declarations are done in non-auto way due to syntax noise
* trailing return type on functions
* use prefix increments
* everything that can be `const` declared as such
* private class members suffixed with `_`
* right to left type notation, e.g. `auto const* ptr`

## repository

* tagged versions have been tested on asus computers with 4GB, 16GB and 32GB
* committed code works in qemu

## tools used

* `clang++`: 21.1.6
* `lld`: 21.1.6
* `qemu-system-x86_64`: 10.2.0
