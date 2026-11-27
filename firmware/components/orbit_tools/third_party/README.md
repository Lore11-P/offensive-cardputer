# lgfx_qrcode

- Origin: `firmware/components/lgfx/lgfx/utility/lgfx_qrcode.c` and `.h`
  (LovyanGFX, fork of ricmoo/QRCode, itself inspired by Nayuki).
- License: MIT (see the header of `lgfx_qrcode.c` / `lgfx_qrcode.h`).
- Local changes: only the `alloca` include, replaced with the portable
  `qrcode_alloca.h` wrapper in this directory. No encoder logic changed.
