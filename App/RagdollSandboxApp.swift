// RagdollSandboxApp.swift — app entry point. All original code.

import SwiftUI

@main
struct RagdollSandboxApp: App {
    @StateObject private var state = GameState()

    var body: some Scene {
        WindowGroup {
            GameView(state: state)
                .onAppear {
                    state.appDidLaunch()
                }
                .statusBar(hidden: true)
        }
    }
}
