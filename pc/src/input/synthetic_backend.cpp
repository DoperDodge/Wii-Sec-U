// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "input/input_backend.h"
#include "util/time_util.h"

namespace wsu {

namespace {

class SyntheticBackend final : public InputBackend {
  public:
    explicit SyntheticBackend(bool scripted) : scripted_(scripted) {}

    bool poll(WsuInputState &out) override {
        wsu_input_state_init(&out, 0);
        if (!scripted_) return true;

        // Deterministic pattern keyed to wall time: left stick walks a
        // square (period 2 s), A held on odd seconds.
        uint32_t t = nowMs();
        uint32_t phase = (t / 500) % 4;
        switch (phase) {
        case 0: out.lx = WSU_STICK_MAX; break;
        case 1: out.ly = WSU_STICK_MAX; break;
        case 2: out.lx = -WSU_STICK_MAX; break;
        default: out.ly = -WSU_STICK_MAX; break;
        }
        if ((t / 1000) % 2 == 1) out.buttons |= WSU_BTN_A;
        return true;
    }

    const char *name() const override {
        return scripted_ ? "synthetic-scripted" : "synthetic-neutral";
    }

  private:
    bool scripted_;
};

} // namespace

std::unique_ptr<InputBackend> makeSyntheticBackend(bool scripted) {
    return std::make_unique<SyntheticBackend>(scripted);
}

#if !defined(WSU_WITH_SDL)
std::unique_ptr<InputBackend> makeSdlBackend() { return nullptr; }
#endif

} // namespace wsu
