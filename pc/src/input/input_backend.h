// Controller input backends. The real SDL backend (any Nintendo pad via
// SDL_GameController, PLAN.md §4B.1) lives behind WSU_WITH_SDL; the
// synthetic backend generates deterministic input for tests and demos.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <memory>
#include <string>

#include "core/controller_state.h"

namespace wsu {

class InputBackend {
  public:
    virtual ~InputBackend() = default;

    // Fills `out` with the current full controller state. Returns false if
    // no state is available this tick (nothing is sent then).
    virtual bool poll(WsuInputState &out) = 0;

    virtual const char *name() const = 0;
};

// Emits neutral state, or a scripted "wiggle" pattern (sticks tracing a
// square, A pressed on alternating seconds) when scripted=true — visible
// on the console and assertable in tests.
std::unique_ptr<InputBackend> makeSyntheticBackend(bool scripted);

// nullptr when built without SDL support.
std::unique_ptr<InputBackend> makeSdlBackend();

} // namespace wsu
