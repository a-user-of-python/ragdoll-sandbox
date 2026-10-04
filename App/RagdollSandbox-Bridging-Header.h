// RagdollSandbox-Bridging-Header.h — exposes the C game API to Swift.
// HEADER_SEARCH_PATHS includes $(SRCROOT)/Game (see Xcode project).

#include "rs_game.h"

#include <os/proc.h>   /* os_proc_available_memory() for SystemInfo.swift */
