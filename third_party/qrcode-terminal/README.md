# qrcode-terminal small-mode rendering

`render_qr()` in `src/cli.cpp` ports the half-block rendering convention from
qrcode-terminal 0.12.0 (Apache-2.0), by Gord Tanner and contributors:
https://github.com/gtanner/qrcode-terminal/blob/0.12.0/lib/main.js
https://www.npmjs.com/package/qrcode-terminal/v/0.12.0

C++ adaptation: 2026-10-22. Only the rendering convention is ported; encoding
uses Nayuki instead of qrcode-terminal's JavaScript QR encoder. The original
package license, including its bundled-code notice, is retained in LICENSE.
