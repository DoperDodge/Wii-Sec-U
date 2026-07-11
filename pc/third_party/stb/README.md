# Vendored: stb single-file libraries

From https://github.com/nothings/stb (public domain / MIT dual license):

- `stb_image.h` v2.30 — baseline JPEG decoding for the MJPEG stream
  (`pc/src/video/frame_decoder.cpp`). Only JPEG support is compiled in.
- `stb_image_write.h` v1.16 — JPEG *encoding*, used exclusively by tests
  to generate valid MJPEG input without a console.

Vendored (rather than a package dependency) so the PC app keeps building
on Windows/Linux CI with zero external libraries.
