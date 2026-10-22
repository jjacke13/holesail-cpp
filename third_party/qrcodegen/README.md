# QR Code generator

Unmodified C++ sources from Project Nayuki, version 1.8.0 (MIT).

Source: https://github.com/nayuki/QR-Code-generator/tree/720f62bddb7226106071d4728c292cb1df519ceb/cpp

Only `qrcodegen.cpp` and `qrcodegen.hpp` are compiled. `LICENSE` reproduces
the license in their headers and is installed alongside the binary.

The terminal renderer follows `qrcode-terminal` 0.12.0 small mode, as used by
JS holesail 2.4.1: one byte segment, error correction L, foreground light
modules, half-block characters, and a one-module border. Nayuki uses its
own standards-compliant automatic mask selection; matrices can differ
from JS without changing the encoded URL.
