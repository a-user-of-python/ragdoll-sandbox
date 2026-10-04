// ToolbarView.swift — left tool/weapon toolbar. Large high-contrast buttons.
// All original code.

import SwiftUI

struct ToolbarView: View {
    @ObservedObject var state: GameState

    // Grouped for a compact, scannable layout.
    private let groups: [(String, [Tool])] = [
        ("Tools",   [.grab, .delete, .freeze, .heal]),
        ("Guns",    [.pistol, .rifle, .shotgun, .sniper, .smg]),
        ("Heavy",   [.grenade, .rocket, .melee, .tnt, .fire]),
    ]

    var body: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(spacing: 10) {
                ForEach(groups, id: \.0) { group in
                    Text(group.0.uppercased())
                        .font(.system(size: 13, weight: .bold))
                        .foregroundColor(.white.opacity(0.6))
                    ForEach(group.1) { tool in
                        toolButton(tool)
                    }
                    Divider().background(Color.white.opacity(0.25))
                }
                // Context help for the selected tool.
                Text(state.selectedTool.help)
                    .font(.system(size: 14))
                    .foregroundColor(.white.opacity(0.85))
                    .multilineTextAlignment(.center)
                    .frame(width: 92)
                    .padding(.top, 4)
            }
            .padding(.vertical, 12)
            .padding(.horizontal, 8)
        }
        .frame(width: 116)
        .background(Color.black.opacity(0.55))
    }

    private func toolButton(_ tool: Tool) -> some View {
        let selected = state.selectedTool == tool && state.pendingSpawn == nil
        return Button(action: {
            state.selectedTool = tool
            state.pendingSpawn = nil
        }) {
            Text(tool.title)
                .font(.system(size: 19, weight: selected ? .bold : .regular))
                .foregroundColor(selected ? .black : .white)
                .frame(width: 96, height: 52)
                .background(selected ? Color.orange : Color.white.opacity(0.14))
                .cornerRadius(12)
        }
        .accessibilityLabel(tool.title)
    }
}
