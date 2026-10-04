// ModManager.swift — Documents/Mods folder management and Lua mod loading.
// All original code.

import Foundation

/// Manages user Lua mods in Documents/Mods. Calls RS_RunLuaFile for each
/// enabled mod; collects errors for the Settings screen.
enum ModManager {
    private static let enabledKey = "rs.enabledMods"
    static let folderName = "Mods"

    static var modsDirectory: URL {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent(folderName, isDirectory: true)
    }

    /// Create Documents/Mods on first launch and seed the bundled sample mod.
    static func ensureModsFolder() {
        let fm = FileManager.default
        let dir = modsDirectory
        if !fm.fileExists(atPath: dir.path) {
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        let sample = dir.appendingPathComponent("sample_mod.lua")
        if !fm.fileExists(atPath: sample.path),
           let bundled = Bundle.main.url(forResource: "sample_mod", withExtension: "lua") {
            try? fm.copyItem(at: bundled, to: sample)
        }
    }

    static func availableMods() -> [String] {
        let fm = FileManager.default
        let urls = (try? fm.contentsOfDirectory(at: modsDirectory,
                                                includingPropertiesForKeys: nil)) ?? []
        return urls.filter { $0.pathExtension.lowercased() == "lua" }
                   .map { $0.deletingPathExtension().lastPathComponent }
                   .sorted()
    }

    static func isEnabled(_ name: String) -> Bool {
        // Default: all mods enabled.
        let dict = UserDefaults.standard.dictionary(forKey: enabledKey) as? [String: Bool]
        return dict?[name] ?? true
    }

    static func setEnabled(_ name: String, _ enabled: Bool) {
        var dict = UserDefaults.standard.dictionary(forKey: enabledKey) as? [String: Bool] ?? [:]
        dict[name] = enabled
        UserDefaults.standard.set(dict, forKey: enabledKey)
    }

    /// Run every enabled mod against the world. Returns error strings.
    @discardableResult
    static func loadEnabledMods(world: UnsafeMutablePointer<RSWorld>) -> [String] {
        var errors: [String] = []
        for name in availableMods() where isEnabled(name) {
            let url = modsDirectory
                .appendingPathComponent(name)
                .appendingPathExtension("lua")
            let rc = url.path.withCString { RS_RunLuaFile(world, $0) }
            if rc != 0 {
                let detail = RS_GetLuaError(world).map { String(cString: $0) } ?? "unknown error"
                errors.append("\(name).lua: \(detail)")
            }
        }
        return errors
    }
}
