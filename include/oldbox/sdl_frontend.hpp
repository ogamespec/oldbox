#pragma once
#include "oldbox/machine.hpp"

namespace oldbox {
// Runs the SDL3 window/audio/input loop until the user quits or the machine stops.
int run_sdl_frontend(Machine& machine);
}
