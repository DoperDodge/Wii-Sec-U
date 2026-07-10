// SDL game-controller backend: normalizes any pad SDL recognizes (Switch
// Pro, 8BitDo, Joy-Con pairs, generic XInput...) into WsuInputState.
// Compiled only when WSU_WITH_SDL is defined; see pc/CMakeLists.txt.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#if defined(WSU_WITH_SDL)

#include <SDL.h>

#include "input/input_backend.h"
#include "util/log.h"

namespace wsu {

namespace {

constexpr const char *kTag = "sdl-input";

class SdlBackend final : public InputBackend {
  public:
    SdlBackend() {
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        openFirstController();
    }

    ~SdlBackend() override {
        if (controller_ != nullptr) SDL_GameControllerClose(controller_);
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }

    bool poll(WsuInputState &out) override {
        SDL_GameControllerUpdate();
        if (controller_ == nullptr ||
            SDL_GameControllerGetAttached(controller_) == SDL_FALSE) {
            if (controller_ != nullptr) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                logWarn(kTag, "controller detached");
            }
            openFirstController();
            if (controller_ == nullptr) return false;
        }

        wsu_input_state_init(&out, 0);
        auto btn = [&](SDL_GameControllerButton b) {
            return SDL_GameControllerGetButton(controller_, b) != 0;
        };
        auto axis = [&](SDL_GameControllerAxis a) {
            return SDL_GameControllerGetAxis(controller_, a);
        };

        // SDL positions map by location: SDL "A" (south) is Nintendo B on
        // Nintendo pads SDL already relabels; we use positional mapping so
        // the physical layout matches the Wii U's.
        if (btn(SDL_CONTROLLER_BUTTON_A)) out.buttons |= WSU_BTN_A;
        if (btn(SDL_CONTROLLER_BUTTON_B)) out.buttons |= WSU_BTN_B;
        if (btn(SDL_CONTROLLER_BUTTON_X)) out.buttons |= WSU_BTN_X;
        if (btn(SDL_CONTROLLER_BUTTON_Y)) out.buttons |= WSU_BTN_Y;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) out.buttons |= WSU_BTN_UP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) out.buttons |= WSU_BTN_DOWN;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) out.buttons |= WSU_BTN_LEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            out.buttons |= WSU_BTN_RIGHT;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) out.buttons |= WSU_BTN_L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
            out.buttons |= WSU_BTN_R;
        if (btn(SDL_CONTROLLER_BUTTON_START)) out.buttons |= WSU_BTN_PLUS;
        if (btn(SDL_CONTROLLER_BUTTON_BACK)) out.buttons |= WSU_BTN_MINUS;
        if (btn(SDL_CONTROLLER_BUTTON_GUIDE)) out.buttons |= WSU_BTN_HOME;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))
            out.buttons |= WSU_BTN_STICK_L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK))
            out.buttons |= WSU_BTN_STICK_R;

        // SDL Y axes grow downward; the Wii U convention is +Y up.
        out.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
        out.ly = static_cast<int16_t>(
            -std::max<int>(axis(SDL_CONTROLLER_AXIS_LEFTY), -32767));
        out.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        out.ry = static_cast<int16_t>(
            -std::max<int>(axis(SDL_CONTROLLER_AXIS_RIGHTY), -32767));

        int16_t lt = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t rt = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        out.lt = static_cast<uint8_t>(lt >> 7);
        out.rt = static_cast<uint8_t>(rt >> 7);
        if (lt > 16384) out.buttons |= WSU_BTN_ZL;
        if (rt > 16384) out.buttons |= WSU_BTN_ZR;

        return true;
    }

    const char *name() const override { return "sdl"; }

  private:
    void openFirstController() {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i) == SDL_TRUE) {
                controller_ = SDL_GameControllerOpen(i);
                if (controller_ != nullptr) {
                    logInfo(kTag, "using controller: %s",
                            SDL_GameControllerName(controller_));
                    return;
                }
            }
        }
    }

    SDL_GameController *controller_ = nullptr;
};

} // namespace

std::unique_ptr<InputBackend> makeSdlBackend() {
    return std::make_unique<SdlBackend>();
}

} // namespace wsu

#endif // WSU_WITH_SDL
