#include "video_menu_entry.h"
#include "input_services.h"
#include "runtime.h"
#include "renderer/native/video_settings_ui.h"
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_82259748);
PPC_FUNC(sub_82275390);
PPC_FUNC(sub_82275410);

namespace Doritos::Native {
namespace {
constexpr uint32_t groupTable = 0x8214f714;
constexpr uint32_t groupMasterTable = 0x8215bf6c;
constexpr char videoMaster[] = "menus/help_and_options/buttons/native_video_settings";
constexpr char parentMaster[] = "menus/help_and_options/buttons/buttons";

bool span(uint32_t address, uint32_t bytes) {
    return memory && address && !(address & 3) && memory->valid(address, bytes);
}
template<size_t N> bool namedMaster(uint32_t master, const char (&name)[N]) {
    if (!span(master, 384) || memory->read32(master) != groupMasterTable) return false;
    const uint32_t text = memory->read32(master + 8);
    return text && memory->valid(text, N) && !std::memcmp(memory->base() + text, name, N);
}
bool ownedCurrentTile(uint32_t group) {
    if (!span(0x82b5ca98, 4) || !span(0x82bab138, 4) || !span(0x82b5ab1c, 4)) return false;
    const uint32_t gui = memory->read32(0x82b5ca98);
    const uint32_t input = memory->read32(0x82bab138);
    const uint32_t manager = memory->read32(0x82b5ab1c);
    if (!span(gui, 16424) || memory->read32(gui) != 0x8213bb34 ||
        !span(input, 176) || memory->read32(input) != 0x8214fd54 ||
        int32_t(memory->read32(input + 12)) > 0 || !span(manager, 92)) return false;
    const uint32_t user = memory->read32(gui + 16420);
    if (user >= 4) return false;
    const uint32_t layer = memory->read32(manager + 56 + user * 4);
    const uint32_t screen = memory->read32(gui + 7768);
    if (!span(layer, 200) || memory->read32(layer) != 0x8215e8fc ||
        !memory->base()[layer + 196] || !(memory->read32(layer + 116) & (1u << user)) ||
        !span(screen, 104) || memory->read32(group + 124) != screen ||
        memory->read32(group + 168) != layer || memory->base()[group + 128] ||
        !memory->base()[group + 173]) return false;
    const uint32_t parent = memory->read32(group + 104);
    if (!span(parent, 176) || memory->read32(parent) != groupTable ||
        memory->read32(parent + 124) != screen ||
        !namedMaster(memory->read32(parent + 120), parentMaster)) return false;
    const uint32_t begin = memory->read32(layer + 140), end = memory->read32(layer + 144);
    if (end <= begin || (end - begin) % 4 || (end - begin) / 4 > 4096 || !span(begin, end - begin)) return false;
    for (uint32_t address = begin; address < end; address += 4)
        if (memory->read32(address) == group) return true;
    return false;
}
bool enabled(PPCContext& ctx, uint8_t* base, uint32_t group) {
    if (ctx.r1.u32 < 1024 || !span(ctx.r1.u32 - 1024, 1024)) return false;
    // Preserve the original ancestor-aware Visible/Enabled predicates rather
    // than guessing from a single cached byte during menu animation.
    PPCContext callback = ctx;
    struct RestoreFpscr {
        PPCContext& original;
        ~RestoreFpscr() { original.fpscr.setcsr(original.fpscr.csr); }
    } restore{ctx};
    callback.r1.u64 = ctx.r1.u32 - 512;
    memory->write32(callback.r1.u32, ctx.r1.u32);
    callback.r3.u64 = group;
    sub_82275390(callback, base);
    const bool visible = callback.r3.u8 != 0;
    callback.r3.u64 = group;
    if (visible) sub_82275410(callback, base);
    return visible && callback.r3.u8 != 0;
}
}

bool handleNativeVideoMenuCommand(PPCContext& ctx, uint8_t* base) {
    if (!memory || base != memory->base() || ctx.r4.u32 != 3) return false;
    const uint32_t group = ctx.r3.u32;
    if (!span(group, 176) || memory->read32(group) != groupTable ||
        !namedMaster(memory->read32(group + 120), videoMaster)) return false;
    const bool open = nativeInputFocused() && ownedCurrentTile(group) && enabled(ctx, base, group);
    ctx.r3.u64 = 0;
    if (open) {
        // Post to the native window thread; never wait in the guest GUI update.
        Doritos::Graphics::showNativeVideoSettings();
        ctx.r3.u64 = 1;
    }
    return true;
}
}

// Original RunCmd Lua adapter 8226F1B8 -> GUIGROUP VT+328/82259678 ->
// 82259748(group r3, index r4). The cloned tile's actual Command0 invokes
// me.RunCmd(3), so mouse, keyboard and controller share this exact entry.
PPC_FUNC(sub_82259748) {
    if (!Doritos::Native::handleNativeVideoMenuCommand(ctx, base))
        __imp__sub_82259748(ctx, base);
}
