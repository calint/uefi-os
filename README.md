# osca x64 uefi

## intention

* experiment with uefi bootable image
* render graphics on screen
* keyboard
* pointing device
* timer
* tasks

## scope

* x86-64 only
* UEFI GOP required
* ACPI 2.0+
* modern PC firmware
* no legacy BIOS
* no broken firmware tolerance
* real-time system with all memory allocated up-front
* program assumed correct and exceptions should reboot

## coding convention (opinionated)

* if possible, declare `auto` being the first word in a declaration
  * applies to functions, inline, constexpr, const etc
  * do not differentiate using pointer signature, e.g. `auto*`
* trailing return type on functions
* `nullptr` declarations are done in non-auto way due to syntax noise
* use prefix increments
* although anything that can be `const` should be declared as such, it
  introduces "noise", sparsely used

## repository

* tagged versions have been tested on asus computers with 4GB, 16GB and 32GB
* committed code works in qemu

## tools used

* `clang++`: 21.1.6
* `lld`: 21.1.6
* `qemu-system-x86_64`: 10.2.0
