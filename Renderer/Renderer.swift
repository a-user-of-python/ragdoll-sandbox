// Renderer.swift — Ragdoll Sandbox 2D Metal batch renderer.
// Original code. Metal 3 baseline; runs natively on every Apple Silicon iPad.
//
// Wiring (see README.md):
//   1. App creates MTKView, then `let renderer = MetalRenderer(mtkView: view)`.
//   2. App runs its own CADisplayLink at 60fps; each tick it steps the game,
//      then calls `renderer.submit(items:particles:viewMatrix:)`.
//   3. The view is driven entirely by submit(); keep `isPaused = true`.
//   4. On rotation/layout call `renderer.viewDidResize()`.

import Foundation
import Metal
import MetalKit
import QuartzCore
import simd

// MARK: - C struct mirrors
//
// Layout contract with Game/rs_game.h (imported via the App bridging header).
// The Game agent owns rs_game.h; the stride preconditions in
// MetalRenderer.init verify the C structs still match.

// NOTE (app-shell integration, 2026-10-04): the Swift mirrors of RSRenderItem /
// RSParticle that were here have been removed. Game/rs_game.h is imported into
// Swift via App/RagdollSandbox-Bridging-Header.h, so the C structs ARE the
// Swift structs — keeping both declarations is a compile error ("invalid
// redeclaration"). Field layout is identical (stride preconditions in
// MetalRenderer.init still verify it), so no other change was needed.

// MARK: - GPU structs (must match Shaders.metal)

/// Must match Shaders.metal `RSInstance`. 48 bytes.
private struct RSInstance {
    var posSize: SIMD4<Float>  // xy: center|p0, zw: size|p1
    var color: SIMD4<Float>    // rgba
    var misc: SIMD4<Float>    // x: angle, y: -, z: tint, w: particle life01
}

/// Must match Shaders.metal `RSUniforms`. 112 bytes.
private struct RSUniforms {
    var viewMatrix: simd_float3x3 // world -> centered y-up points
    var ndcScale: SIMD2<Float>    // 2 / drawableSizeInPoints
    var time: Float
    var pad: Float = 0
    var invViewMatrix: simd_float3x3
}

// MARK: - Renderer

public final class MetalRenderer {

    // True on Metal 4-era OS (iOS 26+). Used ONLY to size the preallocated
    // ring buffers; the draw path is identical Metal 3 API everywhere, which
    // Metal 4 drivers execute natively. Clean fallback: same code, smaller
    // budgets on older OS.
    private static let isMetal4Era: Bool = {
        if #available(iOS 26, *) { return true }
        return false
    }()

    private struct Budget { let boxes, circles, segments, particles: Int }
    private static var budget: Budget {
        isMetal4Era
            ? Budget(boxes: 4096, circles: 4096, segments: 2048, particles: 8192)
            : Budget(boxes: 2048, circles: 2048, segments: 1024, particles: 4096)
    }

    private static let framesInFlight = 3

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private weak var mtkView: MTKView?
    private let semaphore = DispatchSemaphore(value: framesInFlight)
    private var frameIndex: Int = 0
    private var renderScale: Float = 1.0
    public var gridEnabled: Bool = true

    // Ring buffers: [slot][kind]
    private var boxBufs: [MTLBuffer] = []
    private var circleBufs: [MTLBuffer] = []
    private var segmentBufs: [MTLBuffer] = []
    private var particleBufs: [MTLBuffer] = []
    private var uniformBufs: [MTLBuffer] = []

    private var pipeBox: MTLRenderPipelineState!
    private var pipeCircle: MTLRenderPipelineState!
    private var pipeSegment: MTLRenderPipelineState!
    private var pipeParticleAlpha: MTLRenderPipelineState!
    private var pipeParticleAdd: MTLRenderPipelineState!
    private var pipeGrid: MTLRenderPipelineState!

    // MARK: init

    public init?(mtkView: MTKView) {
        // Layout contract with Game/rs_game.h. If the C header changes,
        // these fire on first launch instead of corrupting frames.
        precondition(MemoryLayout<RSRenderItem>.stride == 40,
                     "RSRenderItem layout drifted from rs_game.h (want 40 bytes)")
        precondition(MemoryLayout<RSParticle>.stride == 48,
                     "RSParticle layout drifted from rs_game.h (want 48 bytes)")
        precondition(MemoryLayout<RSInstance>.stride == 48)
        precondition(MemoryLayout<RSUniforms>.stride == 112)

        guard let device = mtkView.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        self.mtkView = mtkView

        // The renderer owns presentation timing; the App drives submit()
        // from its own display link.
        mtkView.device = device
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.depthStencilPixelFormat = .invalid
        mtkView.sampleCount = 1
        mtkView.framebufferOnly = true
        mtkView.isPaused = true
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 60
        mtkView.autoResizeDrawable = false

        let b = Self.budget
        func makeBuffer(_ instances: Int) -> MTLBuffer? {
            device.makeBuffer(length: instances * MemoryLayout<RSInstance>.stride,
                              options: .storageModeShared)
        }
        for _ in 0..<Self.framesInFlight {
            guard let bx = makeBuffer(b.boxes),
                  let ci = makeBuffer(b.circles),
                  let sg = makeBuffer(b.segments),
                  let pa = makeBuffer(b.particles),
                  let un = device.makeBuffer(length: MemoryLayout<RSUniforms>.stride,
                                             options: .storageModeShared)
            else { return nil }
            boxBufs.append(bx); circleBufs.append(ci); segmentBufs.append(sg)
            particleBufs.append(pa); uniformBufs.append(un)
        }

        guard let library = device.makeDefaultLibrary() else {
            print("[MetalRenderer] makeDefaultLibrary failed: is Shaders.metal in the app target?")
            return nil
        }
        func pipe(_ vs: String, _ fs: String, blend: Bool, additive: Bool) -> MTLRenderPipelineState? {
            guard let vfn = library.makeFunction(name: vs),
                  let ffn = library.makeFunction(name: fs) else {
                print("[MetalRenderer] missing shader function \(vs)/\(fs)")
                return nil
            }
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = vfn
            d.fragmentFunction = ffn
            d.colorAttachments[0].pixelFormat = mtkView.colorPixelFormat
            if blend {
                d.colorAttachments[0].isBlendingEnabled = true
                d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
                d.colorAttachments[0].destinationRGBBlendFactor = additive ? .one : .oneMinusSourceAlpha
                d.colorAttachments[0].sourceAlphaBlendFactor = .sourceAlpha
                d.colorAttachments[0].destinationAlphaBlendFactor = additive ? .one : .oneMinusSourceAlpha
            }
            return try? device.makeRenderPipelineState(descriptor: d)
        }
        guard let pBox = pipe("vs_box", "fs_box", blend: true, additive: false),
              let pCircle = pipe("vs_circle", "fs_circle", blend: true, additive: false),
              let pSeg = pipe("vs_segment", "fs_segment", blend: true, additive: false),
              let pPAlpha = pipe("vs_particle", "fs_particle", blend: true, additive: false),
              let pPAdd = pipe("vs_particle", "fs_particle", blend: true, additive: true),
              let pGrid = pipe("vs_grid", "fs_grid", blend: false, additive: false)
        else { return nil }
        pipeBox = pBox; pipeCircle = pCircle; pipeSegment = pSeg
        pipeParticleAlpha = pPAlpha; pipeParticleAdd = pPAdd; pipeGrid = pGrid

        updateDrawableSize()
    }

    // MARK: public API

    /// Graphics setting: 0.5 / 0.75 / 1.0. Clamped to [0.25, 1.0].
    public func setRenderScale(_ s: Float) {
        renderScale = min(max(s, 0.25), 1.0)
        updateDrawableSize()
    }

    /// App must call this from viewDidLayoutSubviews / on rotation.
    public func viewDidResize() {
        updateDrawableSize()
    }

    /// Encode and present one frame. Call once per tick on the MAIN thread
    /// (e.g. the App's CADisplayLink; MTKView.currentDrawable is main-thread
    /// only). Copies instance data into the current ring slot; no allocations
    /// in the hot path beyond the semaphore wait.
    public func submit(items: [RSRenderItem],
                       particles: [RSParticle],
                       viewMatrix: simd_float3x3) {
        guard let view = mtkView else { return }
        semaphore.wait()

        let slot = frameIndex % Self.framesInFlight
        frameIndex &+= 1

        guard let drawable = view.currentDrawable,
              let rpd = view.currentRenderPassDescriptor,
              let cmd = queue.makeCommandBuffer() else {
            semaphore.signal()
            return
        }

        let b = Self.budget
        let nBox = fillBoxes(items, into: boxBufs[slot], cap: b.boxes)
        let nCircle = fillCircles(items, into: circleBufs[slot], cap: b.circles)
        let nSeg = fillSegments(items, into: segmentBufs[slot], cap: b.segments)
        let (nAlpha, nAdd) = fillParticles(particles, into: particleBufs[slot], cap: b.particles)
        writeUniforms(into: uniformBufs[slot], viewMatrix: viewMatrix, view: view)

        guard let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else {
            semaphore.signal()
            return
        }
        enc.setVertexBuffer(uniformBufs[slot], offset: 0, index: 0)
        enc.setFragmentBuffer(uniformBufs[slot], offset: 0, index: 0)

        if gridEnabled {
            enc.setRenderPipelineState(pipeGrid)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        if nBox > 0 {
            enc.setRenderPipelineState(pipeBox)
            enc.setVertexBuffer(boxBufs[slot], offset: 0, index: 1)
            enc.setFragmentBuffer(boxBufs[slot], offset: 0, index: 1)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4,
                               instanceCount: nBox)
        }
        if nCircle > 0 {
            enc.setRenderPipelineState(pipeCircle)
            enc.setVertexBuffer(circleBufs[slot], offset: 0, index: 1)
            enc.setFragmentBuffer(circleBufs[slot], offset: 0, index: 1)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4,
                               instanceCount: nCircle)
        }
        if nSeg > 0 {
            enc.setRenderPipelineState(pipeSegment)
            enc.setVertexBuffer(segmentBufs[slot], offset: 0, index: 1)
            enc.setFragmentBuffer(segmentBufs[slot], offset: 0, index: 1)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4,
                               instanceCount: nSeg)
        }
        if nAlpha > 0 || nAdd > 0 {
            enc.setVertexBuffer(particleBufs[slot], offset: 0, index: 1)
            enc.setFragmentBuffer(particleBufs[slot], offset: 0, index: 1)
            if nAlpha > 0 {
                enc.setRenderPipelineState(pipeParticleAlpha)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4,
                                   instanceCount: nAlpha)
            }
            if nAdd > 0 {
                enc.setRenderPipelineState(pipeParticleAdd)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4,
                                   instanceCount: nAdd, baseInstance: nAlpha)
            }
        }
        enc.endEncoding()

        cmd.present(drawable)
        cmd.addCompletedHandler { [semaphore] _ in semaphore.signal() }
        cmd.commit()
    }

    // MARK: internals

    private func updateDrawableSize() {
        guard let view = mtkView else { return }
        let bounds = view.bounds.size
        guard bounds.width > 0, bounds.height > 0 else { return }
        let cs = view.contentScaleFactor
        view.drawableSize = CGSize(width: bounds.width * cs * CGFloat(renderScale),
                                   height: bounds.height * cs * CGFloat(renderScale))
    }

    private func writeUniforms(into buf: MTLBuffer, viewMatrix: simd_float3x3, view: MTKView) {
        let px = view.drawableSize
        let cs = max(view.contentScaleFactor, 1)
        let pts = SIMD2<Float>(Float(px.width) / Float(cs), Float(px.height) / Float(cs))
        var u = RSUniforms(viewMatrix: viewMatrix,
                           ndcScale: SIMD2<Float>(2, 2) / pts,
                           time: Float(CACurrentMediaTime()),
                           invViewMatrix: simd_inverse(viewMatrix))
        memcpy(buf.contents(), &u, MemoryLayout<RSUniforms>.stride)
    }

    // Each fill* writes RSInstance records straight into the ring buffer and
    // returns the clamped instance count. No intermediate allocations.

    private func fillBoxes(_ items: [RSRenderItem], into buf: MTLBuffer, cap: Int) -> Int {
        var p = buf.contents().assumingMemoryBound(to: RSInstance.self)
        var n = 0
        for it in items where it.shape == 0 && n < cap {
            p.pointee = RSInstance(
                posSize: SIMD4(it.x, it.y, it.w, it.h),
                color: SIMD4(it.r, it.g, it.b, it.a),
                misc: SIMD4(it.angle, 0, Float(it.tint), 0))
            p = p.advanced(by: 1); n &+= 1
        }
        return n
    }

    private func fillCircles(_ items: [RSRenderItem], into buf: MTLBuffer, cap: Int) -> Int {
        var p = buf.contents().assumingMemoryBound(to: RSInstance.self)
        var n = 0
        for it in items where it.shape == 1 && n < cap {
            p.pointee = RSInstance(
                posSize: SIMD4(it.x, it.y, it.w, it.h),
                color: SIMD4(it.r, it.g, it.b, it.a),
                misc: SIMD4(0, 1, Float(it.tint), 0))
            p = p.advanced(by: 1); n &+= 1
        }
        return n
    }

    private func fillSegments(_ items: [RSRenderItem], into buf: MTLBuffer, cap: Int) -> Int {
        var p = buf.contents().assumingMemoryBound(to: RSInstance.self)
        var n = 0
        // shape==2: (x,y)=p0, (w,h)=p1, angle unused. Width is RS_SEGMENT_WIDTH
        // in Shaders.metal (world units).
        for it in items where it.shape == 2 && n < cap {
            p.pointee = RSInstance(
                posSize: SIMD4(it.x, it.y, it.w, it.h),
                color: SIMD4(it.r, it.g, it.b, it.a),
                misc: SIMD4(0, 2, Float(it.tint), 0))
            p = p.advanced(by: 1); n &+= 1
        }
        return n
    }

    /// Splits particles into alpha-blended [0, nAlpha) then additive
    /// [nAlpha, nAlpha+nAdd), matching the two draw calls in submit().
    private func fillParticles(_ parts: [RSParticle], into buf: MTLBuffer, cap: Int) -> (Int, Int) {
        var p = buf.contents().assumingMemoryBound(to: RSInstance.self)
        var nAlpha = 0, nAdd = 0
        // Pass 1: alpha types (blood 0, smoke 2, debris 4)
        for pt in parts where (pt.type == 0 || pt.type == 2 || pt.type == 4)
                                && (nAlpha + nAdd) < cap {
            let life01 = pt.maxLife > 0 ? min(max(pt.life / pt.maxLife, 0), 1) : 0
            p.pointee = RSInstance(
                posSize: SIMD4(pt.x, pt.y, pt.size, pt.size),
                color: SIMD4(pt.r, pt.g, pt.b, pt.a),
                misc: SIMD4(0, 0, Float(pt.type), life01))
            p = p.advanced(by: 1); nAlpha &+= 1
        }
        // Pass 2: additive types (fire 1, spark 3)
        for pt in parts where (pt.type == 1 || pt.type == 3)
                                && (nAlpha + nAdd) < cap {
            let life01 = pt.maxLife > 0 ? min(max(pt.life / pt.maxLife, 0), 1) : 0
            p.pointee = RSInstance(
                posSize: SIMD4(pt.x, pt.y, pt.size, pt.size),
                color: SIMD4(pt.r, pt.g, pt.b, pt.a),
                misc: SIMD4(0, 0, Float(pt.type), life01))
            p = p.advanced(by: 1); nAdd &+= 1
        }
        return (nAlpha, nAdd)
    }
}
