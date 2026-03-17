#pragma once
#include "app_events.h"

// Stateless event → action mapper.
// Translates user input events into high-level app actions
// that the Shell (MainWindow) executes.
class AppController {
public:
    ActionList HandleKeyDown(const KeyDownEvent& event) const;
    ActionList HandleMouseWheel(const MouseWheelEvent& event) const;
};
