#include "NativeDecorationState.hpp"

#if HYPRLIQUID_WINDOW_PRESENTATION
#include "Globals.hpp"
#include "RendererSymbols.hpp"
#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprInnerGlowDecoration.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <bit>
#include <typeinfo>
#include <unordered_map>

namespace NativeDecorationState {
namespace {
using SetTarget = void (*)(CAnimatedDecorationGradient*, const Config::CGradientValueData&, bool);
CFunctionHook* targetHook = nullptr;
struct SBinding {
    WP<IHyprWindowDecoration> owner;
    CAnimatedDecorationGradient* gradient;
};
std::unordered_map<IHyprWindowDecoration*, SBinding> bindings;
struct SProbe {
    CAnimatedDecorationGradient* gradient = nullptr;
    size_t calls = 0;
};
// Trivial TLS does not leave a destructor in an unloaded plugin.
thread_local SProbe* probe = nullptr;

void setTarget(CAnimatedDecorationGradient* gradient, const Config::CGradientValueData& value, bool transition) {
    if (probe) {
        probe->gradient = gradient;
        ++probe->calls;
        return;
    }
    reinterpret_cast<SetTarget>(targetHook->m_original)(gradient, value, transition);
}
}

bool initialize() {
    const auto address = rendererSymbol(PHANDLE, "setTarget");
    if (!address) return false;
    targetHook = HyprlandAPI::createFunctionHook(PHANDLE, address, reinterpret_cast<void*>(setTarget));
    if (targetHook && targetHook->hook()) return true;
    destroy();
    return false;
}

void destroy() {
    bindings.clear();
    if (targetHook) HyprlandAPI::removeFunctionHook(PHANDLE, targetHook);
    targetHook = nullptr;
}

bool appendKey(std::vector<uint64_t>& key, const SP<IHyprWindowDecoration>& decoration) {
    if (!targetHook || !decoration) return false;
    // A custom subclass may have a different updateState contract. Probe only
    // the three native implementations, each of which sets exactly one target.
    const auto& type = typeid(*decoration);
    if (type != typeid(CHyprBorderDecoration) && type != typeid(CHyprDropShadowDecoration) &&
        type != typeid(CHyprInnerGlowDecoration)) return false;

    std::erase_if(bindings, [](const auto& entry) { return !entry.second.owner; });
    auto it = bindings.find(decoration.get());
    if (it == bindings.end()) {
        if (probe) return false;
        SProbe discovered;
        {
            probe = &discovered;
            Hyprutils::Utils::CScopeGuard restore([] { probe = nullptr; });
            // Obtain the by-value gradient through public native methods.
            // Suppressing this probe's setter preserves its targets and fade.
            decoration->updateState();
        }
        if (discovered.calls != 1 || !discovered.gradient) return false;
        it = bindings.emplace(decoration.get(), SBinding{decoration, discovered.gradient}).first;
    }
    const auto owner = it->second.owner.lock();
    if (owner != decoration) return false;
    const auto state = it->second.gradient->renderState();
    const auto number = [&](double value) { key.push_back(std::bit_cast<uint64_t>(value)); };
    key.push_back(reinterpret_cast<uintptr_t>(decoration.get()));
    key.push_back(decoration->getDecorationType());
    for (const auto* gradient : {&state.current, &state.previous}) {
        number(gradient->m_angle);
        key.push_back(gradient->m_colors.size());
        for (const auto& color : gradient->m_colors) {
            number(color.r); number(color.g); number(color.b); number(color.a);
        }
    }
    number(state.progress);
    key.push_back(state.transitioning);
    return true;
}
}
#else
namespace NativeDecorationState {
bool initialize() { return true; }
void destroy() {}
bool appendKey(std::vector<uint64_t>&, const SP<IHyprWindowDecoration>&) { return true; }
}
#endif
