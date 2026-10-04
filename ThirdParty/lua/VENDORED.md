# Vendored PUC Lua

- **Version:** 5.4.9 (released 10 Aug 2026)
- **Upstream:** https://www.lua.org/ftp/lua-5.4.9.tar.gz
- **SHA-256:** `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`
- **License:** MIT (see `README` and `doc/readme.html`; copyright headers in each
  source file). Copyright © 1994–2026 Lua.org, PUC-Rio.

## What is included

- `src/`: all Lua library + core sources (`*.c`, `*.h`) **except** `lua.c` and
  `luac.c` (the standalone interpreter and compiler mains). Ragdoll Sandbox
  embeds Lua as a library; it does not ship the `lua`/`luac` binaries.
- `doc/`: upstream documentation, including the full reference manual
  (`manual.html`) and license text (`readme.html`).
- `README`: upstream readme.

## What is excluded / modified

- Nothing in `src/` or `doc/` is modified. The tree is byte-identical to the
  upstream tarball apart from the removal of `lua.c`/`luac.c`.
- This file (`VENDORED.md`) is the only addition.

## Building

`Scripting/CMakeLists.txt` compiles `src/*.c` into a static library (`lua`).
Lua is pure ISO C and also compiles cleanly as C++.
