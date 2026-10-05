# Ragdoll Sandbox

An original 2D physics sandbox for iPad (Apple Silicon M1–M5). Spawn ragdolls,
build contraptions, wield weapons and tools, and mod it all with Lua.

**100% original code and assets.** Inspired by the physics-sandbox genre, but
nothing is copied from any existing game.

## Status

In development. iOS 17+, iPad only, landscape. Unsigned IPA built by GitHub
Actions (sideload with Sideloadly/AltStore).

## Layout

- `Engine/` — C++17 2D rigid-body physics (no Apple deps) + host tests
- `Game/` — game logic: ragdolls, weapons, damage, particles, tools (C API)
- `Renderer/` — native Metal 3 2D batch renderer (Metal 4 where available)
- `ThirdParty/lua/` — vendored PUC Lua 5.4.x
- `Scripting/` — Lua mod bindings + docs + sample mods
- `App/` — SwiftUI app: toolbar, spawn palette, settings, StikJIT, entitlements
- `.github/workflows/` — unsigned IPA build + host tests

See `DESIGN.md` for the architecture contract.

> Built with Muse — AI-assisted development.
