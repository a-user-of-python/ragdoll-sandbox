// GameState.swift — central app state: world, tool selection, camera, settings.
// All original code.

import SwiftUI
import simd

// MARK: - Tools

/// Every tool/weapon in the left toolbar.
enum Tool: String, CaseIterable, Identifiable {
    case grab
    case pistol, rifle, shotgun, sniper, smg
    case grenade, rocket, melee
    case tnt, fire
    case delete, freeze, heal

    var id: String { rawValue }

    var title: String {
        switch self {
        case .grab:    return "Grab"
        case .pistol:  return "Pistol"
        case .rifle:   return "Rifle"
        case .shotgun: return "Shotgun"
        case .sniper:  return "Sniper"
        case .smg:     return "SMG"
        case .grenade: return "Grenade"
        case .rocket:  return "Rocket"
        case .melee:   return "Bat"
        case .tnt:     return "TNT"
        case .fire:    return "Fire"
        case .delete:  return "Delete"
        case .freeze:  return "Freeze"
        case .heal:    return "Heal"
        }
    }

    /// Index into RS_FireHitscan's weapon parameter (0=pistol 1=rifle
    /// 2=shotgun 3=sniper 4=smg). nil for non-hitscan tools.
    var hitscanIndex: Int? {
        switch self {
        case .pistol:  return 0
        case .rifle:   return 1
        case .shotgun: return 2
        case .sniper:  return 3
        case .smg:     return 4
        default:       return nil
        }
    }

    /// Tools that use aim-drag-release (touch down = muzzle, drag = aim).
    var isAimed: Bool {
        switch self {
        case .pistol, .rifle, .shotgun, .sniper, .smg,
             .grenade, .rocket, .melee:
            return true
        default:
            return false
        }
    }

    /// One-line help shown in the toolbar footer.
    var help: String {
        switch self {
        case .grab:    return "Drag bodies around"
        case .pistol:  return "Drag to aim, release to fire"
        case .rifle:   return "Drag to aim, release to fire"
        case .shotgun: return "Drag to aim, release to fire"
        case .sniper:  return "Drag to aim, release to fire"
        case .smg:     return "Drag to spray"
        case .grenade: return "Drag to set throw, release"
        case .rocket:  return "Drag to aim, release to fire"
        case .melee:   return "Tap to swing the bat"
        case .tnt:     return "Tap to detonate"
        case .fire:    return "Drag to spread fire"
        case .delete:  return "Tap a body to remove it"
        case .freeze:  return "Tap a body to freeze/unfreeze"
        case .heal:    return "Tap a human to heal"
        }
    }
}

/// Things the spawn palette can place.
enum SpawnKind: String, CaseIterable, Identifiable {
    case human, crate, barrel, ball, plank
    var id: String { rawValue }
    var title: String {
        switch self {
        case .human:  return "Human"
        case .crate:  return "Crate"
        case .barrel: return "Barrel"
        case .ball:   return "Ball"
        case .plank:  return "Plank"
        }
    }
}

// MARK: - Camera

/// 2D camera: pan + zoom. World units = points, +x right, +y UP.
/// Screen points: UIKit convention, +y DOWN, origin top-left.
struct Camera {
    var center = SIMD2<Float>(0, 100)   // world point at screen center
    var scale: Float = 1.0             // screen points per world unit

    mutating func clampScale() {
        scale = min(max(scale, 0.2), 6.0)
    }

    /// Keep the camera near the world so it can't be flung irretrievably
    /// off-screen. (2026-10-04)
    mutating func clampCenter() {
        center.x = min(max(center.x, -4000), 4000)
        center.y = min(max(center.y, -4000), 4000)
    }

    func screenToWorld(_ p: CGPoint, viewport: CGSize) -> SIMD2<Float> {
        let fx = Float(p.x), fy = Float(p.y)
        let w = Float(viewport.width), h = Float(viewport.height)
        return SIMD2<Float>((fx - w * 0.5) / scale + center.x,
                            center.y + (h * 0.5 - fy) / scale)
    }

    func worldToScreen(_ w: SIMD2<Float>, viewport: CGSize) -> CGPoint {
        let vw = Float(viewport.width), vh = Float(viewport.height)
        let px = (w.x - center.x) * scale + vw * 0.5
        let py = vh * 0.5 - (w.y - center.y) * scale
        return CGPoint(x: CGFloat(px), y: CGFloat(py))
    }

    /// Column-major 3x3 mapping world -> NDC for the Metal vertex shader.
    func viewMatrix(viewport: CGSize) -> simd_float3x3 {
        let w = Float(viewport.width), h = Float(viewport.height)
        let sx = 2 * scale / w
        let sy = 2 * scale / h
        let c0 = SIMD3<Float>(sx, 0, 0)
        let c1 = SIMD3<Float>(0, sy, 0)
        let c2 = SIMD3<Float>(-sx * center.x, -sy * center.y, 1)
        return simd_float3x3(columns: (c0, c1, c2))
    }
}

// MARK: - Game state

/// Owns the RSWorld and all UI-facing state. Everything RS_* runs on the
/// main thread (display link + UI actions), so no locking is needed.
final class GameState: ObservableObject {
    let world: UnsafeMutablePointer<RSWorld>

    @Published var selectedTool: Tool = .grab
    @Published var pendingSpawn: SpawnKind? = nil
    @Published var isPaused: Bool = false
    @Published var showSettings: Bool = false
    @Published var showJITPrompt: Bool = false

    // Live stats (updated ~2x/sec from the frame loop).
    @Published var fps: Double = 0
    @Published var stepMs: Float = 0
    @Published var entityCount: Int = 0
    @Published var bodyCount: Int = 0

    // Graphics settings (persisted).
    @Published var renderScale: Double = 1.0 { didSet { save() } }
    @Published var particleBudget: Double = 4000 { didSet { save(); applyParticleBudget() } }
    @Published var physicsSubsteps: Int = 2 { didSet { save(); applySubsteps() } }
    @Published var bloodEnabled: Bool = true { didSet { save() } }
    @Published var showFPS: Bool = true { didSet { save() } }

    // Mods.
    @Published var modErrors: [String] = []
    @Published var modsVersion: Int = 0  // bump to refresh the mods list UI

    var camera = Camera()
    var frozenEntities = Set<UInt32>()

    private let defaults = UserDefaults.standard

    init() {
        guard let w = RS_CreateWorld() else {
            fatalError("RS_CreateWorld failed")
        }
        self.world = w
        // Restore persisted settings.
        if defaults.object(forKey: "rs.renderScale") != nil {
            renderScale = defaults.double(forKey: "rs.renderScale")
            particleBudget = defaults.double(forKey: "rs.particleBudget")
            physicsSubsteps = defaults.integer(forKey: "rs.physicsSubsteps")
            bloodEnabled = defaults.bool(forKey: "rs.bloodEnabled")
            showFPS = defaults.bool(forKey: "rs.showFPS")
            if physicsSubsteps < 1 || physicsSubsteps > 4 { physicsSubsteps = 2 }
            if renderScale < 0.5 || renderScale > 1.0 { renderScale = 1.0 }
        }
        applyParticleBudget()
        applySubsteps()
    }

    deinit {
        RS_DestroyWorld(world)
    }

    private func save() {
        defaults.set(renderScale, forKey: "rs.renderScale")
        defaults.set(particleBudget, forKey: "rs.particleBudget")
        defaults.set(physicsSubsteps, forKey: "rs.physicsSubsteps")
        defaults.set(bloodEnabled, forKey: "rs.bloodEnabled")
        defaults.set(showFPS, forKey: "rs.showFPS")
    }

    private func applyParticleBudget() {
        RS_SetParticleBudget(world, Int32(particleBudget))
    }

    private func applySubsteps() {
        RS_SetPhysicsSubsteps(world, Int32(physicsSubsteps))
    }

    // MARK: - App launch

    /// Called once from the App's onAppear.
    func appDidLaunch() {
        ModManager.ensureModsFolder()
        reloadMods()
        maybePromptForJIT()
    }

    func reloadMods() {
        modErrors = ModManager.loadEnabledMods(world: world)
        modsVersion += 1
    }

    private func maybePromptForJIT() {
        guard !defaults.bool(forKey: "rs.jitPrompted") else { return }
        defaults.set(true, forKey: "rs.jitPrompted")
        // Only prompt if StikJIT is installed but JIT isn't active yet.
        if StikJIT.status == .available {
            showJITPrompt = true
        }
    }

    // MARK: - UI actions (all real RS_* calls)

    func clearWorld() {
        RS_ClearWorld(world)
        frozenEntities.removeAll()
    }

    func spawn(_ kind: SpawnKind, at p: SIMD2<Float>) {
        switch kind {
        case .human:  RS_SpawnHuman(world, p.x, p.y)
        case .crate:  RS_SpawnCrate(world, p.x, p.y, 60)
        case .barrel: RS_SpawnBarrel(world, p.x, p.y)
        case .ball:   RS_SpawnBall(world, p.x, p.y, 30)
        case .plank:  RS_SpawnPlank(world, p.x, p.y, 160, 24)
        }
    }

    func toggleFreeze(_ e: UInt32) {
        if frozenEntities.contains(e) {
            frozenEntities.remove(e)
            RS_SetFrozen(world, e, 0)
        } else {
            frozenEntities.insert(e)
            RS_SetFrozen(world, e, 1)
        }
    }
}
