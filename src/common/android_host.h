// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The Android host runs this linux-x64 build as guest code inside an Android app, under FEXCore.
// Nothing about the build is Android: each seam is selected at runtime by a variable the launcher
// sets, so none of them is reachable on a desktop host.
namespace Common::AndroidHost {

// No SDL window: the surface is VK_EXT_headless_surface, which the host backs with its window.
bool WindowSelected();
// Output through AAudio, via the host's guest-side libaaudio.so.
bool AudioSelected();
// Pads come from the host's pad bridge instead of SDL.
bool InputSelected();
// The host window's size in pixels, from SHADPS4_HOST_WINDOW_SIZE=WIDTHxHEIGHT.
bool WindowSize(s32* width, s32* height);

struct PadWireState {
    u32 buttons;
    u8 left_x; // 0..255, 128 centred, Y grows downward
    u8 left_y;
    u8 right_x;
    u8 right_y;
    u8 left_trigger;
    u8 right_trigger;
    u8 connected;
    u8 reserved;
};
static_assert(sizeof(PadWireState) == 12);

enum PadWireButton : u32 {
    PadWireUp = 1u << 0,
    PadWireDown = 1u << 1,
    PadWireLeft = 1u << 2,
    PadWireRight = 1u << 3,
    PadWireCross = 1u << 4,
    PadWireCircle = 1u << 5,
    PadWireSquare = 1u << 6,
    PadWireTriangle = 1u << 7,
    PadWireL1 = 1u << 8,
    PadWireR1 = 1u << 9,
    PadWireL2 = 1u << 10,
    PadWireR2 = 1u << 11,
    PadWireL3 = 1u << 12,
    PadWireR3 = 1u << 13,
    PadWireOptions = 1u << 14,
    PadWireTouchPad = 1u << 15,
};

/// 1 when a pad was written, 0 when there is none, negative when the host refused the read.
s64 PadRead(PadWireState* state);
/// Returns once the host has recorded the request.
void PadRumble(u8 large_motor, u8 small_motor);

/// A started AAudio output stream of interleaved float samples, or nullptr.
void* AAudioOpenOutput(u32 sample_rate, u32 channels, u32 latency_ms);
void AAudioCloseOutput(void* stream);
/// Hands every frame to the device, waiting between non-blocking writes while it is full.
bool AAudioWrite(void* stream, const float* samples, u32 frames, u32 channels);

} // namespace Common::AndroidHost
