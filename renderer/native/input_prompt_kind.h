#pragma once
#include <cstdint>

namespace Doritos::Graphics {
enum class InputPromptKind : uint8_t {
    None, Space, Enter, Escape, X, Y, Q, E, Shift, Ctrl, Pause, Tab,
    ButtonsAtlas, ControlsDiagram,
};
}
