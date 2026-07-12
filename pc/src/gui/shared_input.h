// SharedInput — hands controller state from the GUI thread to a session.
//
// SDL wants controller/event polling on the thread that pumps events (the
// GUI thread), but HostApp/ClientApp read their InputBackend from their
// own loop threads. The GUI polls the real SDL pad every UI frame and
// stores the full state here; the adapter backend returns the latest copy
// to the session thread. Input packets carry full state, so the UI frame
// rate (~60 Hz) versus send rate (~120 Hz) mismatch is harmless.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <memory>
#include <mutex>

#include "input/input_backend.h"

namespace wsu {

class SharedInput {
  public:
    void set(const WsuInputState &state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        valid_ = true;
    }

    bool get(WsuInputState &out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_) return false;
        out = state_;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        valid_ = false;
    }

  private:
    mutable std::mutex mutex_;
    WsuInputState state_{};
    bool valid_ = false;
};

// InputBackend adapter reading from a SharedInput owned by the GUI.
class SharedInputBackend final : public InputBackend {
  public:
    explicit SharedInputBackend(const SharedInput &source)
        : source_(source) {}

    bool poll(WsuInputState &out) override { return source_.get(out); }
    const char *name() const override { return "gui-shared"; }

  private:
    const SharedInput &source_;
};

inline std::unique_ptr<InputBackend> makeSharedInputBackend(
    const SharedInput &source) {
    return std::make_unique<SharedInputBackend>(source);
}

} // namespace wsu
