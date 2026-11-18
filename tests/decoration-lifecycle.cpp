// Disposable-compositor fixture. Keep a window alive across hidden/unmapped
// states without linking the plugin, so dlclose really unloads its code.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include "../src/HyprlandCompat.hpp"
#include "../src/HyprlandIPCCompat.hpp"
#if HYPRLIQUID_WINDOW_PRESENTATION
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#endif
#include <hyprland/src/event/EventBus.hpp>
#include <algorithm>
#include <format>
#include <dlfcn.h>

namespace {
HANDLE handle;
CPluginSystem* plugins = nullptr;
SP<HyprlandCompat::CCommand> command;
PHLWINDOW window;
std::vector<IHyprWindowDecoration*> glass, native;
bool unmapped = false;

#if HYPRLIQUID_WINDOW_PRESENTATION
// Test-only instrumentation: preserve the unmapped-owner and pending-deleter
// assertions after Hyprland made these fields private. Explicit instantiation
// permits access without changing production visibility or compositor state.
template<class Tag, typename Tag::type Member>
struct PrivateMember { friend typename Tag::type member(Tag) { return Member; } };
struct MappedTag {
    using type = bool Desktop::View::CWindow::*;
    friend type member(MappedTag);
};
template struct PrivateMember<MappedTag, &Desktop::View::CWindow::m_isMapped>;
struct PendingTag {
    using type = std::vector<IHyprWindowDecoration*> Desktop::View::CWindowPresentation::*;
    friend type member(PendingTag);
};
template struct PrivateMember<PendingTag, &Desktop::View::CWindowPresentation::m_decosToRemove>;
#endif

bool& mappedFlag() {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window.get()->*member(MappedTag{});
#else
    return window->m_isMapped;
#endif
}

auto& pendingDecorations() {
#if HYPRLIQUID_WINDOW_PRESENTATION
    return window->presentation().*member(PendingTag{});
#else
    return window->m_decosToRemove;
#endif
}

CPlugin* glassPlugin() {
    for (auto* plugin : plugins->getAllPlugins())
        if (plugin->m_name == "hyprliquid") return plugin;
    return nullptr;
}

std::string status() {
    if (!window) return "idle";
    const auto present = [](const auto* pointer) {
        return std::ranges::any_of(HyprlandCompat::decorations(window), [pointer](const auto& deco) { return deco.get() == pointer; });
    };
    const auto retained = std::ranges::count_if(glass, present);
    const auto preserved = std::ranges::count_if(native, present);
    const auto pending = std::ranges::count_if(pendingDecorations(), [](const auto* pointer) {
        return std::ranges::find(glass, pointer) != glass.end();
    });
    const auto pendingNative = std::ranges::count_if(pendingDecorations(), [](const auto* pointer) {
        return std::ranges::find(native, pointer) != native.end();
    });
    return std::format("{} {} {} {} {}", retained, pending, preserved, native.size(), pendingNative);
}

void restore() {
    if (!window) return;
    if (unmapped) {
        mappedFlag() = true;
        Event::bus()->m_events.window.open.emit(window);
        unmapped = false;
    }
    window->setHidden(false);
#if HYPRLIQUID_WINDOW_PRESENTATION
    window->presentation().updateDecorations();
#else
    window->updateWindowDecos();
#endif
}

std::string dispatch(std::string request) {
    if (request.ends_with(" registry")) {
        const auto* plugin = glassPlugin();
        if (!plugin) return "0 0";
        size_t live = 0;
        for (const auto& candidate : Desktop::windowState()->windows())
            for (const auto& deco : HyprlandCompat::decorations(candidate))
                if (deco->getDisplayName() == "Hyprliquid") ++live;
        return std::format("{} {}", plugin->m_registeredDecorations.size(), live);
    }
    if (request.ends_with(" status")) return status();
    if (request.ends_with(" restore")) {
        restore();
        window.reset();
        glass.clear();
        native.clear();
        return "ok";
    }
    const bool unmap = request.ends_with(" unmapped");
    const bool reused = request.ends_with(" reused");
    if (!unmap && !reused && !request.ends_with(" hidden")) return "unknown request";
    if (window) return "already prepared";
    for (const auto& candidate : Desktop::windowState()->windows()) {
#if HYPRLIQUID_WINDOW_PRESENTATION
        const auto appID = candidate->metadata().appID();
#else
        const auto appID = candidate->m_class;
#endif
        if (appID == "hyprliquid-lifecycle" && HyprlandCompat::mapped(candidate) && !candidate->isHidden()) {
            window = candidate;
            break;
        }
    }
    if (!window) return "missing test window";
    for (const auto& deco : HyprlandCompat::decorations(window))
        (deco->getDisplayName() == "Hyprliquid" ? glass : native).push_back(deco.get());
    if (glass.size() != 1) return "expected one glass decoration";
    if (reused) {
        // Deterministically model a dead glass pointer whose allocation was
        // later reused for a native border. Hyprland retains the raw registry
        // entry until plugin unload, even after the original window is gone.
        auto* plugin = glassPlugin();
#if HYPRLIQUID_WINDOW_PRESENTATION
        auto* border = window->presentation().decoration(DECORATION_BORDER).get();
#else
        auto* border = window->getDecorationByType(DECORATION_BORDER);
#endif
        if (!plugin || !border) return "missing plugin or native border";
        plugin->m_registeredDecorations.push_back(border);
    }
    window->setHidden(true);
    if (unmap) {
        mappedFlag() = false;
        unmapped = true;
        Event::bus()->m_events.window.close.emit(window);
    }
    return "ok";
}
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE owner) {
    handle = owner;
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("decoration lifecycle fixture: Hyprland ABI mismatch");
    const auto* ownerSystem = static_cast<const decltype(g_pPluginSystem)*>(dlsym(RTLD_DEFAULT, "g_pPluginSystem"));
    if (!ownerSystem || !*ownerSystem) throw std::runtime_error("decoration lifecycle fixture: host plugins unavailable");
    plugins = ownerSystem->get();
    command = HyprlandCompat::registerCommand(handle, "hyprliquid-test:decorations", dispatch);
    return {"hyprliquid-decoration-lifecycle", "Decoration unload regression fixture", "hyprliquid", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
    command.reset();
    // A failed regression may contain unloaded deleters; do not invoke them.
    window.reset();
    glass.clear();
    native.clear();
}
