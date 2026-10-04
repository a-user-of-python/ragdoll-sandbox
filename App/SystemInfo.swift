// SystemInfo.swift — device model, GPU name, memory reporting for Settings.
// All original code.

import Foundation
import Metal
import Darwin

enum SystemInfo {
    /// e.g. "iPad13,4".
    static var deviceModel: String {
        var sysinfo = utsname()
        uname(&sysinfo)
        let mirror = Mirror(reflecting: sysinfo.machine)
        return mirror.children.compactMap { $0.value as? Int8 }
            .prefix(while: { $0 != 0 })
            .map { Character(UnicodeScalar(UInt8($0))) }
            .map(String.init)
            .joined()
    }

    /// e.g. "Apple M3".
    static var gpuName: String {
        MTLCreateSystemDefaultDevice()?.name ?? "Unknown"
    }

    /// Total physical RAM, e.g. "8.0 GB".
    static var totalMemoryText: String {
        let bytes = ProcessInfo.processInfo.physicalMemory
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }

    /// Currently available memory via os_proc_available_memory().
    static var availableMemoryText: String {
        let bytes = os_proc_available_memory()
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }
}
