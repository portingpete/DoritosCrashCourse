#pragma once
#include <cstdint>

namespace Doritos::Graphics {
struct GuiPointerContext {
    uint32_t screen = 0, root = 0;
    uint64_t rootHandle = 0;
    bool operator==(const GuiPointerContext&) const = default;
};
enum class GuiPointerMapping { Flat, Pending, Miss, Hit };
struct GuiPointerResult {
    GuiPointerMapping mapping = GuiPointerMapping::Flat;
    float x = 0, y = 0;
};
// The guest GUI thread supplies the current input screen identity. Renderer
// snapshots are published only after the matching frame has been presented.
void setTitleGuiPointerContext(const GuiPointerContext& context);
GuiPointerResult mapTitleGuiPointer(const GuiPointerContext& context, float x, float y);
GuiPointerResult mapTitlePresentationPointer(float x, float y);
}
