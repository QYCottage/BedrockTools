#include "UiRenderPass.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/memory/Vtable.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>

namespace bedrocktools::core::uirender {
namespace {

using DrawTextFn = void (*)(void*, void*, const void*, const std::string&, const void*, std::uint8_t, float, const void*, const void*);
using FinishFn = void (*)(void*);
DrawTextFn drawTextOriginal = nullptr;
FinishFn finishOriginal = nullptr;
bedrocktools::hooks::Handle textHook = nullptr;
bedrocktools::hooks::Handle finishHook = nullptr;
std::array<Callback, 8> callbacks{};
std::atomic_size_t callbackCount{0};
std::mutex registrationMutex;
thread_local void* fontContext = nullptr;
thread_local void* currentFont = nullptr;
thread_local bool drawing = false;

void drawText(void* context, void* font, const void* rect, const std::string& text,
              const void* color, std::uint8_t alignment, float alpha,
              const void* measure, const void* caret) {
    if (!drawing) {
        fontContext = context;
        currentFont = font;
    }
    if (drawTextOriginal) drawTextOriginal(context, font, rect, text, color, alignment, alpha, measure, caret);
}

void finish(void* context) {
    if (!drawing && context) {
        drawing = true;
        void* font = fontContext == context ? currentFont : nullptr;
        const auto count = callbackCount.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index) callbacks[index](context, font);
        drawing = false;
    }
    if (fontContext == context) {
        fontContext = nullptr;
        currentFont = nullptr;
    }
    if (finishOriginal) finishOriginal(context);
}

bool install() {
    if (finishHook) return true;
    const auto target = pl::memory::resolveVtableFunction("24MinecraftUIRenderContext",
        bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFinish, "libminecraftpe.so");
    if (!target) return false;
    finishHook = bedrocktools::hooks::install(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(finish), reinterpret_cast<void**>(&finishOriginal));
    if (!finishHook) return false;
    const auto text = pl::memory::resolveVtableFunction("24MinecraftUIRenderContext",
        bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText, "libminecraftpe.so");
    if (text) textHook = bedrocktools::hooks::install(reinterpret_cast<void*>(text),
        reinterpret_cast<void*>(drawText), reinterpret_cast<void**>(&drawTextOriginal));
    return true;
}

}

bool subscribe(Callback callback) {
    if (!callback) return false;
    std::lock_guard lock(registrationMutex);
    const auto count = callbackCount.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < count; ++index) if (callbacks[index] == callback) return true;
    if (count == callbacks.size() || !install()) return false;
    callbacks[count] = callback;
    callbackCount.store(count + 1, std::memory_order_release);
    return true;
}

}
