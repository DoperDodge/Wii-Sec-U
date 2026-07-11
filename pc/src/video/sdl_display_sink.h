// SdlDisplaySink — the "see the Wii U" window (PLAN.md §4B.2).
//
// Receives assembled frames (possibly on a receive thread), decodes them
// to RGB24 immediately, and hands the newest frame to the window on the
// next pump() from the app's run loop — latest-wins, so a slow display
// never backs up the network path. Audio (PCM16 BE from the wire) is
// queued straight into an SDL audio device with a small cap to stay
// near-live instead of drifting.
//
// Compiled only when WSU_WITH_SDL is defined.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <memory>
#include <string>

#include "video/video_sink.h"

namespace wsu {

// Returns nullptr when built without SDL support.
std::unique_ptr<VideoSink> makeSdlDisplaySink(const std::string &title);

} // namespace wsu
