# WebSid (vendored)

`src/` is **verbatim** upstream `wothke/websid` (master, downloaded 2026-11-22;
the pristine archive is in `../upstream/websid-master.zip`). Do not edit it —
refresh it by re-downloading that zip.

The only addition is `win/`:

- `win/compat.h` — MinGW shim, force-included with `-include`. `src/base.h` does
  `typedef __uint32_t uint32_t;` outside its EMSCRIPTEN branch; `__uint32_t` is
  a glibc name that does not exist on MinGW, and base.h never pulls `stdint.h`.
  The header includes `stdint.h` first and defines `__uint32_t`, so the upstream
  sources need no patching.

Build (done by the project makefile, `make tools`):

    -DRPI4                      enables recordPokeSID(SYS_CYCLES(), reg, value)
                                inside SID::writeMem() -- the capture hook this
                                project uses to record register streams
    -std=gnu11 / -std=gnu++11   per file type
    -fno-rtti                   upstream's own flag (raspi/Makefile)

Adapter: `../../src/sid_websid.cpp` defines `recordPokeSID()` and the two public
entry points declared in `../../src/sid_websid.h`.
