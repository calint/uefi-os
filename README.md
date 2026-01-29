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

## coding convention

* if possible, declare `auto` being the first word in a declaration
* this applies to functions, inline, constexpr, const etc
* `nullptr` declarations are done in non-auto way due to syntax noise
* prefix increments only used
* although anything that can be `const` should be declared as such, it
  introduces too much noise so it is sparsely used
