#pragma once
#include <cstdint>
#include <string>

namespace Doritos::Graphics {
inline std::wstring captureRequestEventName(uint32_t pid, uint64_t creationTime100ns) {
    return L"Local\\DoritosNativeCapture-" + std::to_wstring(pid) + L"-" + std::to_wstring(creationTime100ns);
}
}
