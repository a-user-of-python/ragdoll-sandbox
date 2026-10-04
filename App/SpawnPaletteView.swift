// SpawnPaletteView.swift — right-side spawn palette. Tap an item to arm
// placement, then tap the world to place it. Tap again to disarm.
// All original code.

import SwiftUI

struct SpawnPaletteView: View {
    @ObservedObject var state: GameState

    var body: some View {
        VStack(spacing: 10) {
            Text("SPAWN")
                .font(.system(size: 13, weight: .bold))
                .foregroundColor(.white.opacity(0.6))
            ForEach(SpawnKind.allCases) { kind in
                spawnButton(kind)
            }
            Spacer()
            if state.pendingSpawn != nil {
                Text("Tap the world\nto place")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.orange)
                    .multilineTextAlignment(.center)
            }
        }
        .padding(.vertical, 12)
        .padding(.horizontal, 8)
        .frame(width: 116)
        .background(Color.black.opacity(0.55))
    }

    private func spawnButton(_ kind: SpawnKind) -> some View {
        let armed = state.pendingSpawn == kind
        return Button(action: {
            // Toggle arming.
            state.pendingSpawn = (state.pendingSpawn == kind) ? nil : kind
        }) {
            Text(kind.title)
                .font(.system(size: 19, weight: armed ? .bold : .regular))
                .foregroundColor(armed ? .black : .white)
                .frame(width: 96, height: 52)
                .background(armed ? Color.green : Color.white.opacity(0.14))
                .cornerRadius(12)
        }
        .accessibilityLabel("Spawn \(kind.title)")
    }
}
