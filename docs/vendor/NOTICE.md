# FM-1 browser update protocol

`fm1ota.js` and `fm1pkg.js` are derived from Skiff v1.3.2 at commit
`c4bb2b4e9015cf6244f98db1d319874e15eb0767` (`web/fm1ota.js` and `web/fm1pkg.js`),
as adapted in chinotto1/Fzfm. `fm1pkg.js` keeps the product, logical-image, and
SHA-256 helpers. Package mutation and firmware construction are omitted.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments.
GPL-3.0-only. The complete GPL text is at the repository root in `LICENSE`.
Source: https://github.com/illuminus5/skiff.

The port-name pattern also matches `fmdac`, so an FM-1 that enumerates under
that name is offered the same identity handshake as the other names in the
pattern. The page still installs only after the device answers with an
`FM-1_###` identity and the chosen package has been checked.

M-VAVE's own update loader can report a stock identity such as `ota-FM-1_015`
while the package is a later official build. The page accepts that stock
loader for a stock package, answers either finish address, and continues the
write if the loader restarts. A community loader still has to match its package.

No Skiff, SLOOP, or M-VAVE firmware binaries are bundled.
