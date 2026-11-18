#pragma once

// Disposable-compositor instrumentation: stop native animation ticks, then
// observe the values actually used for drawing. No wall-clock phase estimates.
#include <hyprland/src/animation/AnimationManager.hpp>
#if HYPRLIQUID_WINDOW_PRESENTATION
#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>
#endif
#include <map>

namespace NativeStyleTest {
using Tick = void (*)(Animation::CHyprAnimationManager*);
inline HANDLE handle = nullptr;
inline CFunctionHook* tickHook = nullptr;
inline CFunctionHook* stateHook = nullptr;
inline SP<HyprlandCompat::CCommand> command;
inline bool frozen = false;
inline uint64_t serial = 0, frame = 0;
inline std::map<uintptr_t, std::string> observations;

inline json_object* gradientJSON(const Config::CGradientValueData& gradient) {
    auto* result = json_object_new_object();
    json_object_object_add(result, "angle", json_object_new_double(gradient.m_angle));
    auto* colors = json_object_new_array();
    for (const auto& color : gradient.m_colors) {
        auto* rgba = json_object_new_array();
        for (double value : {color.r, color.g, color.b, color.a})
            json_object_array_add(rgba, json_object_new_double(value));
        json_object_array_add(colors, rgba);
    }
    json_object_object_add(result, "colors", colors);
    auto* gpuColors = json_object_new_array();
    for (float value : gradient.m_colorsOkLabA)
        json_object_array_add(gpuColors, json_object_new_double(value));
    json_object_object_add(result, "gpu_colors", gpuColors);
    return result;
}

inline void tick(Animation::CHyprAnimationManager* manager) {
    if (frozen) { manager->onTicked(); return; }
    ++serial;
    reinterpret_cast<Tick>(tickHook->m_original)(manager);
}

#if HYPRLIQUID_WINDOW_PRESENTATION
using State = CAnimatedDecorationGradient::SRenderState;
inline State observe(const CAnimatedDecorationGradient* gradient) {
    const auto state = reinterpret_cast<State (*)(const CAnimatedDecorationGradient*)>(stateHook->m_original)(gradient);
    auto* result = json_object_new_object();
    json_object_object_add(result, "current", gradientJSON(state.current));
    json_object_object_add(result, "previous", gradientJSON(state.previous));
    json_object_object_add(result, "progress", json_object_new_double(state.progress));
    json_object_object_add(result, "transitioning", json_object_new_boolean(state.transitioning));
    observations[reinterpret_cast<uintptr_t>(gradient)] = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
    json_object_put(result);
    return state;
}
#endif

inline void beginFrame() { ++frame; observations.clear(); }

inline std::string status() {
    auto* result = json_object_new_object();
    json_object_object_add(result, "frozen", json_object_new_boolean(frozen));
    json_object_object_add(result, "resolved_gradients", json_object_new_boolean(HYPRLIQUID_WINDOW_PRESENTATION));
    json_object_object_add(result, "tick_serial", json_object_new_uint64(serial));
    json_object_object_add(result, "frame", json_object_new_uint64(frame));
    auto* gradients = json_object_new_object();
#if HYPRLIQUID_WINDOW_PRESENTATION
    for (const auto& [id, text] : observations)
        json_object_object_add(gradients, std::format("{:x}", id).c_str(), json_tokener_parse(text.c_str()));
#else
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!HyprlandCompat::mapped(window)) continue;
        const auto add = [&](const char* name, const auto& current, const auto& previous, const auto& fade, const auto& angle) {
            auto* state = json_object_new_object();
            json_object_object_add(state, "current", gradientJSON(current));
            json_object_object_add(state, "previous", gradientJSON(previous));
            json_object_object_add(state, "progress", json_object_new_double(fade->value()));
            json_object_object_add(state, "transitioning", json_object_new_boolean(fade->isBeingAnimated()));
            json_object_object_add(state, "angle_progress", json_object_new_double(angle->value()));
            json_object_object_add(gradients, std::format("{:x}:{}", reinterpret_cast<uintptr_t>(window.get()), name).c_str(), state);
        };
        add("border", window->m_realBorderColor, window->m_realBorderColorPrevious, window->m_borderFadeAnimationProgress, window->m_borderAngleAnimationProgress);
        add("shadow", window->m_realShadowColor, window->m_realShadowColorPrevious, window->m_shadowFadeAnimationProgress, window->m_shadowAngleAnimationProgress);
        add("glow", window->m_realGlowColor, window->m_realGlowColorPrevious, window->m_glowFadeAnimationProgress, window->m_glowAngleAnimationProgress);
    }
#endif
    json_object_object_add(result, "gradients", gradients);
    const std::string text = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
    json_object_put(result);
    return text;
}

inline void unhook() {
    if (frozen) {
        frozen = false;
        Animation::mgr()->resetTickState();
        Animation::mgr()->scheduleTick();
    }
    if (stateHook) HyprlandAPI::removeFunctionHook(handle, stateHook);
    if (tickHook) HyprlandAPI::removeFunctionHook(handle, tickHook);
    stateHook = nullptr;
    tickHook = nullptr;
    observations.clear();
}

inline bool hook() {
    if (tickHook) return true;
    const auto address = dlsym(RTLD_DEFAULT, "_ZN9Animation21CHyprAnimationManager9frameTickEv");
    if (!address) return false;
    tickHook = HyprlandAPI::createFunctionHook(handle, address, reinterpret_cast<void*>(tick));
    if (!tickHook || !tickHook->hook()) { unhook(); return false; }
#if HYPRLIQUID_WINDOW_PRESENTATION
    const auto stateAddress = dlsym(RTLD_DEFAULT, "_ZNK27CAnimatedDecorationGradient11renderStateEv");
    if (!stateAddress) { unhook(); return false; }
    stateHook = HyprlandAPI::createFunctionHook(handle, stateAddress, reinterpret_cast<void*>(observe));
    if (!stateHook || !stateHook->hook()) { unhook(); return false; }
#endif
    return true;
}

inline void initialize(HANDLE owner) {
    handle = owner;
    command = HyprlandCompat::registerCommand(handle, "hyprliquid-test:animation", [](std::string request) {
        const auto split = request.find(' ');
        const auto action = split == std::string::npos ? "status" : request.substr(split + 1);
        if (action == "freeze") {
            if (!hook()) return std::string("{\"error\":\"animation hooks unavailable\"}");
            frozen = true;
        } else if (action == "advance") {
            if (!frozen || !tickHook) return std::string("{\"error\":\"freeze first\"}");
            Animation::mgr()->resetTickState();
            ++serial;
            reinterpret_cast<Tick>(tickHook->m_original)(Animation::mgr().get());
        } else if (action == "thaw") {
            frozen = false;
            Animation::mgr()->resetTickState();
            Animation::mgr()->scheduleTick();
        } else if (action != "status") return std::string("{\"error\":\"unknown action\"}");
        return status();
    });
}

inline void destroy() {
    unhook();
    if (command) HyprlandAPI::unregisterHyprCtlCommand(handle, command);
    command.reset();
}
}
