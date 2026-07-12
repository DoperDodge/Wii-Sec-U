// wsu-app — graphical Wii-Sec-U (see gui_app.h). The CLI `wsu` remains
// for scripting/headless use; this is the app people double-click.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "gui/gui_app.h"

int main(int, char **) {
    wsu::GuiApp app;
    return app.run();
}
