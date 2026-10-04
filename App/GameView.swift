// GameView.swift — SwiftUI wrapper around MetalGameView plus HUD overlays.
// All original code.

import SwiftUI

/// UIViewRepresentable bridge for the Metal game view.
struct GameViewRepresentable: UIViewRepresentable {
    @ObservedObject var state: GameState

    func makeUIView(context: Context) -> MetalGameView {
        MetalGameView(state: state)
    }

    func updateUIView(_ uiView: MetalGameView, context: Context) {
        // GameState is a reference type shared with the view; nothing to push.
        // Kept for settings-driven updates (render scale is read per-frame).
    }
}

/// Root game screen: Metal view + toolbar + spawn palette + top bar.
struct GameView: View {
    @ObservedObject var state: GameState

    var body: some View {
        ZStack {
            GameViewRepresentable(state: state)
                .ignoresSafeArea()

            VStack(spacing: 0) {
                TopBarView(state: state)
                Spacer()
                if state.showFPS {
                    HStack {
                        Spacer()
                        Text(String(format: "%.0f fps", state.fps))
                            .font(.system(size: 22, weight: .bold, design: .monospaced))
                            .foregroundColor(.white)
                            .padding(8)
                            .background(Color.black.opacity(0.55))
                            .cornerRadius(10)
                            .padding()
                    }
                }
            }

            HStack(spacing: 0) {
                ToolbarView(state: state)
                Spacer()
                SpawnPaletteView(state: state)
            }
            .ignoresSafeArea()

            if state.showSettings {
                SettingsView(state: state)
            }
        }
        .alert("Enable JIT?", isPresented: $state.showJITPrompt) {
            Button("Open StikJIT") { StikJIT.requestEnable() }
            Button("Later", role: .cancel) {}
        } message: {
            Text("StikJIT is installed. This app is fully native and does not need JIT; enabling it has no effect on game performance. Status is shown for information.")
        }
    }
}
