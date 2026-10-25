// Disposable-compositor fixture. Keep a window alive across hidden/unmapped
// states without linking the plugin, so dlclose really unloads its code.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <algorithm>
#include <format>

namespace {
HANDLE handle;
SP<SHyprCtlCommand> command;
PHLWINDOW window;
std::vector<IHyprWindowDecoration*> glass, native;
bool unmapped = false;

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
    return std::format("{} {} {} {}", retained, pending, preserved, native.size());
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
    if (request.ends_with(" status")) return status();
    if (request.ends_with(" restore")) {
        restore();
        window.reset();
        glass.clear();
        native.clear();
        return "ok";
    }
    const bool unmap = request.ends_with(" unmapped");
    if (!unmap && !request.ends_with(" hidden")) return "unknown request";
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
