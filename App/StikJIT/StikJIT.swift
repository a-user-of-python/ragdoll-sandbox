// StikJIT.swift — JIT enablement via the StikJIT app URL scheme, plus a real
// status check. Self-contained; no dependency on the emulator codebase.
//
// Pattern (standard iOS mechanism, original implementation):
//   1. StikJIT (or StikDebug) attaches to this process as a debugger, which
//      sets the CS_DEBUGGED task flag and permits JIT page mapping.
//   2. We detect JIT capability functionally: mmap one page with
//      PROT_EXEC|MAP_JIT. It succeeds only when JIT is actually allowed.
//   3. Settings shows the status and offers to open StikJIT when installed
//      but JIT is not yet enabled.
//
// This app is fully native and does not require JIT; the status is reported
// because the project spec asks for it.

import UIKit
import Darwin

enum StikJIT {
    enum Status {
        case enabled            // JIT page mapping works right now
        case available          // StikJIT installed, JIT not enabled yet
        case unavailable        // StikJIT not installed
    }

    /// Cached probe result: canMapJITPage() does an mmap/munmap syscall pair,
    /// and SettingsView reads status several times per render. Refresh on
    /// demand via refreshStatus(). (2026-10-04)
    private static var cachedStatus: Status?
    static func refreshStatus() { cachedStatus = nil }

    static var status: Status {
        if let s = cachedStatus { return s }
        let s: Status
        if canMapJITPage() { s = .enabled }
        else { s = isStikJITInstalled ? .available : .unavailable }
        cachedStatus = s
        return s
    }

    static var statusText: String {
        switch status {
        case .enabled:     return "Enabled"
        case .available:   return "Installed — not enabled"
        case .unavailable: return "Not installed"
        }
    }

    /// True when the StikJIT URL scheme can be opened.
    /// Requires `stikjit` in LSApplicationQueriesSchemes (see Info.plist).
    static var isStikJITInstalled: Bool {
        guard let url = URL(string: "stikjit://") else { return false }
        return UIApplication.shared.canOpenURL(url)
    }

    /// Ask StikJIT to enable JIT for this app (attaches its debugger).
    static func requestEnable() {
        guard let url = URL(string: "stikjit://") else { return }
        UIApplication.shared.open(url, options: [:], completionHandler: nil)
    }

    // MARK: - Private

    /// Functional JIT probe: try to map one RWX page with MAP_JIT.
    /// Succeeds only if the process is allowed JIT (debugger-attached via
    /// StikJIT, or the dynamic-codesigning entitlement). Cleans up after
    /// itself; safe to call at any time.
    private static func canMapJITPage() -> Bool {
        let pageSize = Int(sysconf(Int32(_SC_PAGESIZE)))
        guard pageSize > 0 else { return false }
        let MAP_JIT_FLAG: Int32 = 0x800
        let raw = mmap(nil, pageSize,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANON | MAP_JIT_FLAG, -1, 0)
        guard let p = raw,
              p != UnsafeMutableRawPointer(bitPattern: -1) else {
            return false
        }
        munmap(p, pageSize)
        return true
    }
}
