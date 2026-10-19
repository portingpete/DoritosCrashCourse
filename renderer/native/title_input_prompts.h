#pragma once
#include "input_prompt_kind.h"
#include <string_view>

namespace Doritos::Graphics {
struct TitleInputPrompt {
    InputPromptKind kind = InputPromptKind::None;
    bool alphaMask = false;
};
// Exact shipped TEXTUREMASTER filenames, rather than shader names or a
// substring match. Color and the separate authored opacity texture use the
// same prompt layout.
inline TitleInputPrompt titleInputPrompt(std::string_view name) {
    struct Entry { std::string_view color, mask; InputPromptKind kind; };
    constexpr Entry entries[] = {
        {"gui/xbox_buttons.jpg;xbox_buttons_a.jpg", "", InputPromptKind::ButtonsAtlas},
        {"gui/controls.jpg;controls_a.jpg", "", InputPromptKind::ControlsDiagram},
        {"gui/button_lb.jpg;button_lb_a.jpg", "", InputPromptKind::Q},
        {"gui/button_rb.jpg;button_rb_a.jpg", "", InputPromptKind::E},
        {"gui/button_lt.jpg;button_lt_a.jpg", "", InputPromptKind::Shift},
        {"gui/button_rt.jpg;button_rt_a.jpg", "", InputPromptKind::Ctrl},
        {"gui/button_start.jpg;button_start_a.jpg", "", InputPromptKind::Pause},
        {"gui/xbox_buttons.jpg", "gui/xbox_buttons_a.jpg", InputPromptKind::ButtonsAtlas},
        {"gui/controls.jpg", "gui/controls_a.jpg", InputPromptKind::ControlsDiagram},
        {"gui/button_lb.jpg", "gui/button_lb_a.jpg", InputPromptKind::Q},
        {"gui/button_rb.jpg", "gui/button_rb_a.jpg", InputPromptKind::E},
        {"gui/button_lt.jpg", "gui/button_lt_a.jpg", InputPromptKind::Shift},
        {"gui/button_rt.jpg", "gui/button_rt_a.jpg", InputPromptKind::Ctrl},
        {"gui/button_start.jpg", "gui/button_start_a.jpg", InputPromptKind::Pause},
    };
    for (const auto& entry : entries) {
        if (name == entry.color) return {entry.kind, false};
        if (!entry.mask.empty() && name == entry.mask) return {entry.kind, true};
    }
    return {};
}
}
