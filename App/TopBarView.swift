// TopBarView.swift — pause, clear, stats, settings gear.
// All original code.

import SwiftUI

struct TopBarView: View {
    @ObservedObject var state: GameState

    var body: some View {
        HStack(spacing: 14) {
            Button(action: { state.isPaused.toggle() }) {
                Image(systemName: state.isPaused ? "play.fill" : "pause.fill")
                    .font(.system(size: 24))
                    .foregroundColor(.white)
                    .frame(width: 56, height: 56)
                    .background(Color.white.opacity(0.16))
                    .cornerRadius(14)
            }
            .accessibilityLabel(state.isPaused ? "Play" : "Pause")

            Button(action: { state.clearWorld() }) {
                Image(systemName: "trash.fill")
                    .font(.system(size: 24))
                    .foregroundColor(.white)
                    .frame(width: 56, height: 56)
                    .background(Color.red.opacity(0.55))
                    .cornerRadius(14)
            }
            .accessibilityLabel("Clear world")

            Text("\(state.entityCount) entities · \(state.bodyCount) bodies")
                .font(.system(size: 18, weight: .medium, design: .monospaced))
                .foregroundColor(.white.opacity(0.9))

            Spacer()

            Text("RAGDOLL SANDBOX")
                .font(.system(size: 20, weight: .black))
                .foregroundColor(.white.opacity(0.85))
                .tracking(2)

            Spacer()

            Button(action: { state.showSettings = true }) {
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 24))
                    .foregroundColor(.white)
                    .frame(width: 56, height: 56)
                    .background(Color.white.opacity(0.16))
                    .cornerRadius(14)
            }
            .accessibilityLabel("Settings")
        }
        .padding(.horizontal, 14)
        .padding(.top, 10)
    }
}
