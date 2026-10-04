// MetalGameView.swift — UIView hosting the MTKView, the CADisplayLink frame
// loop (RS_Step + gather + renderer.submit), camera gestures, and tool input.
// All original code.

import UIKit
import MetalKit
import simd

/// Weak CADisplayLink target: breaks the displayLink -> target -> displayLink
/// retain cycle so MetalGameView.deinit actually runs. (2026-10-04)
private final class DisplayLinkProxy {
    weak var owner: MetalGameView?
    init(owner: MetalGameView) { self.owner = owner }
    @objc func tick(_ link: CADisplayLink) { owner?.frameTick(link) }
}

/// Container view: owns the MTKView, the Renderer, the display link, and all
/// touch handling. Single-finger = active tool (or spawn placement);
/// two-finger = pan + pinch zoom.
final class MetalGameView: UIView {    var state: GameState

    private var mtkView: MTKView!
    private var renderer: MetalRenderer!
    private var displayLink: CADisplayLink!
    private var lastRenderScale: Float = -1

    // Reusable gather buffers (no per-frame allocation).
    private var itemBuf: UnsafeMutablePointer<RSRenderItem>!
    private var particleBuf: UnsafeMutablePointer<RSParticle>!
    private var particleScratch: UnsafeMutablePointer<RSParticle>!
    private let maxItems: Int32 = 4096
    private let maxParticles: Int32 = 8192

    // Single-touch tool state.
    private var trackingTouch: UITouch?
    private var grabHandle: UInt32 = 0
    private var aiming = false
    private var aimStart = SIMD2<Float>.zero
    private var aimCurrent = SIMD2<Float>.zero
    private var lastSpray = CFTimeInterval(0)
    private var aimLayer: CAShapeLayer!

    // Two-finger gesture state.
    private var gestureTouches: [UITouch] = []
    private var gestureStartDist: CGFloat = 0
    private var gestureStartScale: Float = 1
    private var gestureAnchorWorld = SIMD2<Float>.zero

    // Stat publish throttle.
    private var lastStatPublish = CFTimeInterval(0)
    private var fpsEMA: Double = 60

    init(state: GameState) {
        self.state = state
        super.init(frame: .zero)
        setup()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) not supported") }

    private func setup() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("Metal is not supported on this device")
        }
        mtkView = MTKView(frame: bounds, device: device)
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isOpaque = true
        addSubview(mtkView)
        NSLayoutConstraint.activate([
            mtkView.leadingAnchor.constraint(equalTo: leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: trailingAnchor),
            mtkView.topAnchor.constraint(equalTo: topAnchor),
            mtkView.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])

        // MetalRenderer configures the MTKView itself (paused, manual
        // drawables, no auto-resize) and presents inside submit().
        guard let r = MetalRenderer(mtkView: mtkView) else {
            fatalError("MetalRenderer init failed (see console for shader errors)")
        }
        renderer = r

        aimLayer = CAShapeLayer()
        aimLayer.strokeColor = UIColor.systemOrange.cgColor
        aimLayer.fillColor = UIColor.clear.cgColor
        aimLayer.lineWidth = 3
        aimLayer.isHidden = true
        layer.addSublayer(aimLayer)

        itemBuf = UnsafeMutablePointer<RSRenderItem>.allocate(capacity: Int(maxItems))
        particleBuf = UnsafeMutablePointer<RSParticle>.allocate(capacity: Int(maxParticles))
        particleScratch = UnsafeMutablePointer<RSParticle>.allocate(capacity: Int(maxParticles))

        isMultipleTouchEnabled = true

        // Weak proxy breaks the CADisplayLink -> target -> displayLink retain
        // cycle (2026-10-04: deinit was dead code before this).
        displayLink = CADisplayLink(target: DisplayLinkProxy(owner: self),
                                    selector: #selector(DisplayLinkProxy.tick(_:)))
        // Game targets 60fps; 120Hz ProMotion doubles CPU/GPU cost for no benefit.
        displayLink.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        displayLink.add(to: .main, forMode: .common)
    }

    deinit {
        displayLink.invalidate()
        itemBuf.deallocate()
        particleBuf.deallocate()
        particleScratch.deallocate()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        // Rotation / split-view: the renderer re-derives the drawable size.
        renderer?.viewDidResize()
    }

    // MARK: - Frame loop

    /// Called by DisplayLinkProxy (weak target). Was @objc/private on self,
    /// which created the retain cycle.
    func frameTick(_ link: CADisplayLink) {
        tick(link)
    }

    private func tick(_ link: CADisplayLink) {
        let rawDt = link.duration > 0 ? link.duration : 1.0 / 60.0
        let dt = min(max(rawDt, 1.0 / 240.0), 1.0 / 20.0)
        fpsEMA += (1.0 / rawDt - fpsEMA) * 0.05

        let world = state.world
        if !state.isPaused {
            RS_Step(world, Float(dt))
        }

        // Gather render items.
        let nItems = Int(RS_GetRenderItems(world, itemBuf, maxItems))
        let nParticles: Int
        if state.bloodEnabled {
            nParticles = Int(RS_GetParticles(world, particleBuf, maxParticles))
        } else {
            // Blood toggle is App-side: filter type 0 (blood) during copy.
            let raw = particleScratch!
            let count = Int(RS_GetParticles(world, raw, maxParticles))
            var kept = 0
            for i in 0..<count where raw[i].type != 0 {
                particleBuf[kept] = raw[i]
                kept += 1
            }
            nParticles = kept
        }
        let items = UnsafeBufferPointer(start: itemBuf, count: nItems)
        let particles = UnsafeBufferPointer(start: particleBuf, count: nParticles)

        let matrix = state.camera.viewMatrix(viewport: bounds.size)

        // Render scale is owned by the renderer (it sizes the drawable).
        let rs = Float(state.renderScale)
        if rs != lastRenderScale {
            lastRenderScale = rs
            renderer.setRenderScale(rs)
        }
        // submit() encodes AND presents via the MTKView's current drawable.
        renderer.submit(items: items, particles: particles, viewMatrix: matrix)

        // Publish stats ~2x/sec to avoid SwiftUI churn.
        let now = CACurrentMediaTime()
        if now - lastStatPublish > 0.5 {
            lastStatPublish = now
            state.fps = fpsEMA
            state.stepMs = RS_GetStepMs(world)
            state.entityCount = Int(RS_GetEntityCount(world))
            state.bodyCount = Int(RS_GetBodyCount(world))
        }
    }

    // MARK: - Touch handling
    // Clean state machine (rewritten 2026-10-04; the old one could never
    // reach the two-finger state and misfired tools on gesture end):
    //   1 active touch  -> tool touch (trackingTouch)
    //   2+ active touches -> pan/pinch gesture (first two fingers); tool cancelled
    // A finger that participated in a gesture never auto-starts a tool touch
    // when the gesture ends (prevents stray explosions/deletions).
    private var activeTouches: [UITouch] = []
    private var toolSuppressed = Set<UITouch>()

    private func worldPoint(_ touch: UITouch) -> SIMD2<Float> {
        state.camera.screenToWorld(touch.location(in: self), viewport: bounds.size)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches.sorted(by: { $0.timestamp < $1.timestamp }) {
            if !activeTouches.contains(t) { activeTouches.append(t) }
        }
        reconcileTouches()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        if gestureTouches.count == 2 {
            updateGesture()
            return
        }
        if let t = trackingTouch, touches.contains(t) {
            moveToolTouch(t)
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        endTouches(touches, cancelled: false)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        endTouches(touches, cancelled: true)
    }

    private func endTouches(_ touches: Set<UITouch>, cancelled: Bool) {
        for t in touches {
            activeTouches.removeAll { $0 == t }
            toolSuppressed.remove(t)
            if t == trackingTouch {
                trackingTouch = nil
                endToolTouch(t, cancelled: cancelled)
            }
        }
        gestureTouches.removeAll { touches.contains($0) }
        reconcileTouches()
    }

    private func reconcileTouches() {
        if activeTouches.count >= 2 {
            if trackingTouch != nil { cancelToolTouch() }
            let pair = Array(activeTouches.prefix(2))
            if pair != gestureTouches {
                gestureTouches = pair
                toolSuppressed.formUnion(pair)
                beginGesture()
            }
        } else {
            gestureTouches.removeAll()
            if activeTouches.count == 1, trackingTouch == nil {
                let t = activeTouches[0]
                if !toolSuppressed.contains(t) {
                    trackingTouch = t
                    beginToolTouch(t)
                }
            }
            if activeTouches.isEmpty { toolSuppressed.removeAll() }
        }
    }

    // MARK: Tool touches

    private func beginToolTouch(_ t: UITouch) {
        let p = worldPoint(t)
        let world = state.world

        // Spawn placement takes precedence over the selected tool.
        if let kind = state.pendingSpawn {
            state.spawn(kind, at: p)
            return
        }

        switch state.selectedTool {
        case .grab:
            grabHandle = RS_GrabBegin(world, p.x, p.y)
        case .tnt:
            RS_Explode(world, p.x, p.y, 150, 1200)
        case .fire:
            RS_Ignite(world, p.x, p.y, 60)
        case .delete:
            let e = RS_EntityAtPoint(world, p.x, p.y)
            if e != 0 { RS_Despawn(world, e) }
        case .freeze:
            let e = RS_EntityAtPoint(world, p.x, p.y)
            if e != 0 { state.toggleFreeze(e) }
        case .heal:
            let e = RS_EntityAtPoint(world, p.x, p.y)
            if e != 0 { RS_HealHuman(world, e) }
        default:
            break
        }

        if state.selectedTool.isAimed {
            aiming = true
            aimStart = p
            aimCurrent = p
            updateAimOverlay()
            // SMG sprays immediately on touch-down too.
            if state.selectedTool == .smg {
                spray(at: p, angle: 0)
                lastSpray = CACurrentMediaTime()
            }
        }
    }

    private func moveToolTouch(_ t: UITouch) {
        let p = worldPoint(t)
        let world = state.world
        switch state.selectedTool {
        case .grab:
            if grabHandle != 0 { RS_GrabMove(world, grabHandle, p.x, p.y) }
        case .fire:
            RS_Ignite(world, p.x, p.y, 60)
        case .smg where aiming:
            // Spray while dragging: throttle to ~11 shots/sec.
            let now = CACurrentMediaTime()
            if now - lastSpray > 0.09 {
                lastSpray = now
                spray(at: p, angle: aimAngle())
            }
        default:
            break
        }
        if aiming {
            aimCurrent = p
            updateAimOverlay()
        }
    }

    private func endToolTouch(_ t: UITouch, cancelled: Bool) {
        let world = state.world
        if grabHandle != 0 {
            RS_GrabEnd(world, grabHandle)
            grabHandle = 0
        }
        if aiming && !cancelled {
            let p = worldPoint(t)
            let angle = aimAngle(at: p)
            fireAimed(from: aimStart, angle: angle)
        }
        aiming = false
        aimLayer.isHidden = true
    }

    private func cancelToolTouch() {
        if grabHandle != 0 {
            RS_GrabEnd(state.world, grabHandle)
            grabHandle = 0
        }
        trackingTouch = nil
        aiming = false
        aimLayer.isHidden = true
    }

    private func aimAngle(at p: SIMD2<Float>? = nil) -> Float {
        let cur = p ?? aimCurrent
        let d = cur - aimStart
        if simd_length(d) < 20 { return 0 }   // tap without drag: fire right
        return atan2(d.y, d.x)
    }

    private func spray(at p: SIMD2<Float>, angle: Float) {
        RS_FireHitscan(state.world, p.x, p.y, angle, 4)
    }

    /// Release-to-fire for aimed tools. Documented UX: touch down sets the
    /// muzzle, drag aims (orange line), release fires.
    private func fireAimed(from muzzle: SIMD2<Float>, angle: Float) {
        let world = state.world
        let tool = state.selectedTool
        if let idx = tool.hitscanIndex, tool != .smg {
            RS_FireHitscan(world, muzzle.x, muzzle.y, angle, Int32(idx))
            return
        }
        switch tool {
        case .smg:
            break // sprayed during the drag already
        case .grenade:
            let d = aimCurrent - aimStart
            // Drag vector -> throw velocity (tuned for the world scale).
            RS_ThrowGrenade(world, muzzle.x, muzzle.y, d.x * 6.0, d.y * 6.0)
        case .rocket:
            RS_FireRocket(world, muzzle.x, muzzle.y, angle)
        case .melee:
            RS_MeleeSwing(world, muzzle.x, muzzle.y, angle, 110, 45)
        default:
            break
        }
    }

    private func updateAimOverlay() {
        guard aiming, state.selectedTool.isAimed else {
            aimLayer.isHidden = true
            return
        }
        let a = state.camera.worldToScreen(aimStart, viewport: bounds.size)
        let b = state.camera.worldToScreen(aimCurrent, viewport: bounds.size)
        let path = CGMutablePath()
        path.move(to: a)
        path.addLine(to: b)
        path.addEllipse(in: CGRect(x: a.x - 8, y: a.y - 8, width: 16, height: 16))
        aimLayer.path = path
        aimLayer.isHidden = false
    }

    // MARK: Pan / pinch

    private func beginGesture() {
        guard gestureTouches.count == 2 else { return }
        let a = gestureTouches[0].location(in: self)
        let b = gestureTouches[1].location(in: self)
        gestureStartDist = hypot(a.x - b.x, a.y - b.y)
        gestureStartScale = state.camera.scale
        let mid = CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
        gestureAnchorWorld = state.camera.screenToWorld(mid, viewport: bounds.size)
    }

    private func updateGesture() {
        guard gestureTouches.count == 2, gestureStartDist > 0 else { return }
        let a = gestureTouches[0].location(in: self)
        let b = gestureTouches[1].location(in: self)
        let dist = hypot(a.x - b.x, a.y - b.y)
        let mid = CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)

        // Pinch zoom ANCHORED at the pinch point (2026-10-04: previously
        // zoomed around the view center, so content slid away from fingers).
        state.camera.scale = gestureStartScale * Float(dist / gestureStartDist)
        state.camera.clampScale()
        // Keep the world point from gesture start under the current midpoint;
        // this handles both zoom-around-point and two-finger pan.
        let w = gestureAnchorWorld
        let vw = Float(bounds.size.width), vh = Float(bounds.size.height)
        state.camera.center.x = w.x - (Float(mid.x) - vw * 0.5) / state.camera.scale
        state.camera.center.y = w.y - (vh * 0.5 - Float(mid.y)) / state.camera.scale
        state.camera.clampCenter()
    }
}
