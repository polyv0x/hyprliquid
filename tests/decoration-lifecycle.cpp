// Disposable-compositor fixture. Keep a window alive across hidden/unmapped
// states without linking the plugin, so dlclose really unloads its code.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <algorithm>
#include <format>
#include <dlfcn.h>

namespace {
HANDLE handle;
CPluginSystem* plugins = nullptr;
SP<SHyprCtlCommand> command;
PHLWINDOW window;
std::vector<IHyprWindowDecoration*> glass, native;
bool unmapped = false;

CPlugin* glassPlugin() {
    for (auto* plugin : plugins->getAllPlugins())
        if (plugin->m_name == "hyprliquid") return plugin;
    return nullptr;
}

std::string status() {
    if (!window) return "idle";
    const auto present = [](const auto* pointer) {
        return std::ranges::any_of(window->m_windowDecorations, [pointer](const auto& deco) { return deco.get() == pointer; });
    };
    const auto retained = std::ranges::count_if(glass, present);
    const auto preserved = std::ranges::count_if(native, present);
    const auto pending = std::ranges::count_if(window->m_decosToRemove, [](const auto* pointer) {
        return std::ranges::find(glass, pointer) != glass.end();
    });
    const auto pendingNative = std::ranges::count_if(window->m_decosToRemove, [](const auto* pointer) {
        return std::ranges::find(native, pointer) != native.end();
    });
    return std::format("{} {} {} {} {}", retained, pending, preserved, native.size(), pendingNative);
}

void restore() {
    if (!window) return;
    if (unmapped) {
        window->m_isMapped = true;
        Event::bus()->m_events.window.open.emit(window);
        unmapped = false;
    }
    window->setHidden(false);
    window->updateWindowDecos();
}

std::string dispatch(eHyprCtlOutputFormat, std::string request) {
    if (request.ends_with(" registry")) {
        const auto* plugin = glassPlugin();
        if (!plugin) return "0 0";
        size_t live = 0;
        for (const auto& candidate : Desktop::windowState()->windows())
            for (const auto& deco : candidate->m_windowDecorations)
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
        if (candidate->m_class == "hyprliquid-lifecycle" && candidate->m_isMapped && !candidate->isHidden()) {
            window = candidate;
            break;
        }
    }
    if (!window) return "missing test window";
    for (const auto& deco : window->m_windowDecorations)
        (deco->getDisplayName() == "Hyprliquid" ? glass : native).push_back(deco.get());
    if (glass.size() != 1) return "expected one glass decoration";
    if (reused) {
        // Deterministically model a dead glass pointer whose allocation was
        // later reused for a native border. Hyprland retains the raw registry
        // entry until plugin unload, even after the original window is gone.
        auto* plugin = glassPlugin();
        auto* border = window->getDecorationByType(DECORATION_BORDER);
        if (!plugin || !border) return "missing plugin or native border";
        plugin->m_registeredDecorations.push_back(border);
    }
    window->setHidden(true);
    if (unmap) {
        window->m_isMapped = false;
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
    command = HyprlandAPI::registerHyprCtlCommand(handle, {"hyprliquid-test:decorations", false, dispatch});
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
