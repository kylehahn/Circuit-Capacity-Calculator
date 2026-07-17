FasterCap
=========

This directory contains the external FasterCap solver used by Circuit Capacity
Calculator on Windows.

Files:

- `FasterCap.exe` and `libgomp_64-1.dll` are from the FastFieldSolvers bundle.
- `LICENSE.FastFieldSolvers.txt` is the license text distributed with that
  bundle.
- `History.txt` is the FasterCap release history from the same bundle.

The application launches FasterCap as a subprocess in console mode:

```text
FasterCap.exe <generated-input-file> -b
```

FasterCap reads the generated FastCap/FasterCap panel input and prints the
capacitance matrix, which the application parses from stdout.
