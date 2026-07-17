# FastCap executable

Place a Windows `fastcap.exe` build here:

```
external/fastcap/fastcap.exe
```

At build time, CMake copies it next to `circuit_capacity_calculator.exe` when
the file exists. The app also probes this source-tree path to enable the
`FastCap (external)` solver option.

FastCap is the MIT capacitance extractor by Nabors and White. If distributing a
binary, keep the upstream license text in this folder as `LICENSE`.
