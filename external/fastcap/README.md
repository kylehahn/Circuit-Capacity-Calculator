FastCap
=======

This directory contains the WRCad FastCap executable kept as a fast, fixed-panel
solver option.

Files:

- `fastcap.exe` is from the FastCap2-WRCad distribution.
- `README.FastCap-WRCad.txt` and `README.mit.txt` are copied from that source.

The application uses this backend when the UI solver is set to:

```text
FastCap (fixed panels)
```

Unlike FasterCap's adaptive refinement mode, this backend solves the generated
panels directly. It is usually faster for quick comparisons, but accuracy is
controlled mostly by the panel sizes chosen in the application.
