# Renderer — Metal 2D batch renderer

Original code. Native Metal 3 baseline; runs on every Apple Silicon iPad.
No translation layers, no MoltenVK, no cross-platform GPU abstraction.

## Files

- `Renderer.swift` — `MetalRenderer` class (owns device, queue, pipelines,
  triple-buffered ring buffers). Public API:
  - `init?(mtkView: MTKView)`
  - `submit(items:particles:viewMatrix:)`
  - `setRenderScale(_:)` — 0.5 / 0.75 / 1.0 graphics setting
  - `viewDidResize()` — call on rotation / layout
  - `gridEnabled` — background grid toggle (default true)
- `Shaders.metal` — 6 instanced batches (grid, boxes, circles, segments,
  particles-alpha, particles-additive). Add `Shaders.metal` to the app target
  so `makeDefaultLibrary()` finds it.

## Draw calls per frame

| # | Pass | Blend |
|---|------|-------|
| 1 | grid background (opaque, AA grid 50u/250u) | off |
| 2 | boxes (instanced quads, edge outline) | alpha |
| 3 | circles (instanced quads, SDF edge — crisp at any zoom) | alpha |
| 4 | segments (quads built from p0→p1, round caps) | alpha |
| 5 | particles: blood/smoke/debris (soft radial falloff) | alpha |
| 6 | particles: fire/sparks | additive |

No vertex buffers anywhere — quads are generated from `[[vertex_id]]`
(triangle strips). Worst case 6 draw calls for 2000 items + 4000 particles.

## Struct layout contract

`RSRenderItem` / `RSParticle` mirror `Game/rs_game.h` exactly. Byte tables:

`RSRenderItem` (40 bytes; `MemoryLayout.stride == 40` asserted in `init`):

| offset | C field | Swift field | type |
|--------|---------|-------------|------|
| 0 | x | x | float |
| 4 | y | y | float |
| 8 | angle | angle | float |
| 12 | w | w | float |
| 16 | h | h | float |
| 20 | r,g,b,a | r,g,b,a | float ×4 |
| 36 | shape | shape | uint8 (0=box 1=circle 2=segment) |
| 37 | tint | tint | uint8 (0=normal 1=dead 2=burning) |
| 38–39 | padding | — | |

`RSParticle` (48 bytes; stride asserted):

| offset | C field | Swift field | type |
|--------|---------|-------------|------|
| 0 | x,y,vx,vy | x,y,vx,vy | float ×4 |
| 16 | life,maxLife,size | life,maxLife,size | float ×3 |
| 28 | r,g,b,a | r,g,b,a | float ×4 |
| 44 | type | type | uint8 (0=blood 1=fire 2=smoke 3=spark 4=debris) |
| 45–47 | padding | — | |

GPU structs (Swift ↔ Metal, must stay in sync):

- `RSInstance` 48 bytes: `posSize` float4 (xy=center|p0, zw=size|p1),
  `color` float4, `misc` float4 (x=angle, z=tint|type, w=particle life01).
- `RSUniforms` 112 bytes: `viewMatrix` float3x3, `ndcScale` float2,
  `time` float, `invViewMatrix` float3x3.

If the Game agent changes `rs_game.h`, the `precondition`s in `init` fail
loudly on first launch instead of rendering garbage.

Notes:
- Segments: `(x,y)` is p0, `(w,h)` is p1, `angle` unused. Line width is
  `RS_SEGMENT_WIDTH` (0.6 world units) in `Shaders.metal`.
- Particles are split on CPU: types 0/2/4 → alpha pass, 1/3 → additive pass.
- Tints are applied in-shader: 1=dead (desaturate+darken), 2=burning
  (orange flicker driven by the `time` uniform).

## Wiring (App agent)

```swift
import MetalKit

let mtkView = MTKView(frame: bounds)
guard let renderer = MetalRenderer(mtkView: mtkView) else { fatalError("no Metal") }

// The renderer configures the view: isPaused=true, framebufferOnly=true,
// no depth, bgra8Unorm. YOU drive frames with a display link:
let link = CADisplayLink(target: self, selector: #selector(tick))
link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
link.add(to: .main, forMode: .common)

@objc func tick() {
    RS_Step(world, 1/60)                       // game agent's C API
    let items = getRenderItems()               // [RSRenderItem]
    let parts = getParticles()                 // [RSParticle]
    renderer.submit(items: items, particles: parts, viewMatrix: cameraMatrix())
}

func cameraMatrix() -> simd_float3x3 {
    // world (y-up) -> centered y-up points. Camera at (cx,cy), zoom z:
    var m = simd_float3x3(diagonal: SIMD3<Float>(z, z, 1))
    m[2][0] = -cx * z
    m[2][1] = -cy * z
    return m
}

// rotation / layout:
override func viewDidLayoutSubviews() {
    super.viewDidLayoutSubviews()
    renderer.viewDidResize()
}

// settings:
renderer.setRenderScale(0.5)   // 50/75/100%
```

Rules:
- Call `submit` on the main thread, once per tick.
- `viewDidResize()` on every layout change (the renderer manages
  `drawableSize` itself because `autoResizeDrawable = false`).
- Item counts above budget are clamped (2048 boxes / 2048 circles /
  1024 segments / 4096 particles; doubled on iOS 26+).

## Metal 3 / Metal 4

Baseline is Metal 3 API only — the same code path runs on Metal 4 devices,
where the driver executes it natively. The one opportunistic use:
`isMetal4Era` (`#available(iOS 26, *)`) doubles the preallocated instance
ring buffers on Metal 4-era OS. No Metal 4-only symbols are referenced, so
this compiles on any Xcode. Rationale: a 6-draw-call 2D renderer gains
nothing from argument tables or other Metal 4-only features; adding them
would be complexity without benefit.

## Performance design

- Triple-buffered ring buffers (`storageModeShared`, zero-copy on Apple Silicon),
  semaphore-gated so the CPU never overwrites an in-flight frame.
- Instance data is written directly into mapped buffers — no per-frame
  Swift allocations in the hot path.
- 2000 items + 4000 particles ≈ 290 KB/frame of instance data; trivial.
- `framebufferOnly = true`, no depth/stencil, no MSAA.

## Mac verification checklist (cannot compile Metal on the Linux host)

1. Add `Renderer.swift` + `Shaders.metal` to the app target; build.
   Watch for Metal compiler errors in `Shaders.metal` (checked by eye here,
   but verify).
2. Run with Metal validation enabled (scheme → Diagnostics → Metal API
   Validation); exercise all shapes, tints, both particle blends.
3. Confirm 60fps in Instruments with 2000 items + 4000 particles.
4. Toggle `setRenderScale(0.5/0.75/1.0)` — image should get softer, fps same.
5. Rotate the iPad — `viewDidResize()` must keep the drawable correct.
6. Run on an iOS 17 device (Metal 3 path) and an iOS 26 device (Metal 4-era
   path); both must render identically.
