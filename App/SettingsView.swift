// SettingsView.swift — graphics, system status (JIT/memory), and mods.
// Large high-contrast type throughout. All original code.

import SwiftUI

struct SettingsView: View {
    @ObservedObject var state: GameState
    @State private var mods: [String] = []

    var body: some View {
        NavigationStack {
            Form {
                graphicsSection
                systemSection
                modsSection
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { state.showSettings = false }
                        .font(.system(size: 20, weight: .bold))
                }
            }
        }
        .onAppear { refreshMods() }
    }

    // MARK: - Graphics

    private var graphicsSection: some View {
        Section(header: Text("Graphics").font(.system(size: 18, weight: .bold))) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Render scale — lower this on older iPads for more speed")
                    .font(.system(size: 16))
                    .foregroundColor(.secondary)
                Picker("Render scale", selection: $state.renderScale) {
                    Text("50%").tag(0.5)
                    Text("75%").tag(0.75)
                    Text("100%").tag(1.0)
                }
                .pickerStyle(.segmented)
            }
            .padding(.vertical, 4)

            VStack(alignment: .leading, spacing: 8) {
                Text("Particle budget: \(Int(state.particleBudget))")
                    .font(.system(size: 18))
                Slider(value: $state.particleBudget, in: 500...8000, step: 100)
                    .accessibilityLabel("Particle budget")
            }
            .padding(.vertical, 4)

            VStack(alignment: .leading, spacing: 8) {
                Text("Physics substeps — higher is more accurate, slower")
                    .font(.system(size: 16))
                    .foregroundColor(.secondary)
                Picker("Physics substeps", selection: $state.physicsSubsteps) {
                    Text("1").tag(1)
                    Text("2").tag(2)
                    Text("4").tag(4)
                }
                .pickerStyle(.segmented)
            }
            .padding(.vertical, 4)

            Toggle("Blood effects", isOn: $state.bloodEnabled)
                .font(.system(size: 20))
            Toggle("FPS counter", isOn: $state.showFPS)
                .font(.system(size: 20))
        }
    }

    // MARK: - System

    private var systemSection: some View {
        Section(header: Text("System").font(.system(size: 18, weight: .bold))) {
            statusRow("Device", SystemInfo.deviceModel)
            statusRow("Renderer", "Native Metal")
            statusRow("GPU", SystemInfo.gpuName)
            HStack {
                Text("JIT").font(.system(size: 20))
                Spacer()
                Text(StikJIT.statusText)
                    .font(.system(size: 20, weight: .bold))
                    .foregroundColor(StikJIT.status == .enabled ? .green : .orange)
            }
            if StikJIT.status != .enabled {
                Button(StikJIT.status == .available ? "Open StikJIT to enable" : "About JIT") {
                    if StikJIT.status == .available {
                        StikJIT.requestEnable()
                    }
                }
                .font(.system(size: 18))
                .disabled(StikJIT.status == .unavailable)
            }
            statusRow("Available memory", SystemInfo.availableMemoryText)
            VStack(alignment: .leading, spacing: 4) {
                Text("Expanded memory")
                    .font(.system(size: 20))
                Text("increased-memory-limit entitlement is declared in this build. iPadOS grants it on install for supported devices.")
                    .font(.system(size: 15))
                    .foregroundColor(.secondary)
            }
            .padding(.vertical, 4)
            statusRow("Physics step", String(format: "%.2f ms", state.stepMs))
            statusRow("Frame rate", String(format: "%.0f fps", state.fps))
        }
    }

    private func statusRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.system(size: 20))
            Spacer()
            Text(value)
                .font(.system(size: 20, weight: .medium))
                .foregroundColor(.secondary)
        }
    }

    // MARK: - Mods

    private var modsSection: some View {
        Section(header: Text("Mods (Lua)").font(.system(size: 18, weight: .bold))) {
            if mods.isEmpty {
                Text("No mods found. Drop .lua files into the app's Documents/Mods folder (Files app).")
                    .font(.system(size: 16))
                    .foregroundColor(.secondary)
            }
            ForEach(mods, id: \.self) { name in
                Toggle(name, isOn: Binding(
                    get: { ModManager.isEnabled(name) },
                    set: { ModManager.setEnabled(name, $0); state.reloadMods() }
                ))
                .font(.system(size: 18))
            }
            Button("Reload mods") {
                state.reloadMods()
                refreshMods()
            }
            .font(.system(size: 20, weight: .bold))
            ForEach(Array(state.modErrors.enumerated()), id: \.offset) { _, err in
                Text(err)
                    .font(.system(size: 15, design: .monospaced))
                    .foregroundColor(.red)
            }
        }
    }

    private func refreshMods() {
        mods = ModManager.availableMods()
    }
}
