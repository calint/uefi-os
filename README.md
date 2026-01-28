# osca x64 uefi

## intention

* experiment with uefi bootable image
* render graphics on screen
* keyboard
* pointing device
* timer
* tasks

## coding convention

* if possible, declare `auto` being the first word in a declaration
* for consistency declare pointers initiated with `nullptr` using `auto ptr =
  static_cast<type*>(nullptr)`
* use prefix increments only
* although anything that can be `const` should be, it introduces too much noise
  so it is sparsely used
