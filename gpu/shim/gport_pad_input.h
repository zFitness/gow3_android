// SPDX-License-Identifier: GPL-2.0-or-later
// spartanport: the pad bits the save list and the text box act on, split out of spartanport_overlay.cpp so the
// mapping can be checked without ImGui, Vulkan or a window.
#pragma once

#include <atomic>
#include <cstdint>

namespace GportPad {

/// The injected pad-file mask, in runtime_pad.c's own PS4 button layout.
inline constexpr std::uint32_t kUp = 0x10, kDown = 0x40, kCircle = 0x2000, kCross = 0x4000;
inline constexpr std::uint32_t kR3 = 0x4, kL2 = 0x100;

/// A press is an edge, not the level a pad file holds: only the bits that appeared since the
/// caller's previous read count, and `seen` is moved on so holding does not repeat the action.
inline std::uint32_t Edges(std::uint32_t published, std::atomic<std::uint32_t>& seen) {
    return published & ~seen.exchange(published);
}

/// Forget what was held before a dialog opened, so it is not read as a press on it.
inline void Forget(std::atomic<std::uint32_t>& seen, std::uint32_t published) {
    seen.store(published);
}

/// `finish`: 1 accept, 2 cancel; `move`: -1 up, 1 down. Both 0 when nothing is pressed.
inline void Map(std::uint32_t edges, int& move, int& finish) {
    move = (edges & kUp) ? -1 : (edges & kDown) ? 1 : 0;
    finish = (edges & kCross) ? 1 : (edges & kCircle) ? 2 : 0;
}

} // namespace GportPad
