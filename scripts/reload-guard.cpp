// One-time compatibility bridge for upgrading hyprliquid versions that left
// dead decoration pointers in Hyprland's plugin registry. Load immediately
// before unloading the old hyprliquid, then unload this bridge. The plugin's
// own PLUGIN_EXIT destroys its owned decorations, including hidden windows.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <dlfcn.h>
#include <format>

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE) {
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("hyprliquid reload guard: Hyprland ABI mismatch");
    // This library hides its inline globals; use the compositor's owner.
    const auto* owner = static_cast<const decltype(g_pPluginSystem)*>(dlsym(RTLD_DEFAULT, "g_pPluginSystem"));
    if (!owner || !*owner) throw std::runtime_error("hyprliquid reload guard: plugin registry unavailable");
    for (auto* plugin : (*owner)->getAllPlugins()) {
        if (plugin->m_name != "hyprliquid" || plugin->m_author != "polyv0x" || plugin->m_version != "0.1.0") continue;
        const auto count = plugin->m_registeredDecorations.size();
        plugin->m_registeredDecorations.clear();
        return {"hyprliquid-reload-guard", std::format("Retired {} legacy decoration registrations", count), "hyprliquid", "1"};
    }
    throw std::runtime_error("hyprliquid reload guard: compatible loaded plugin not found");
}
APICALL EXPORT void PLUGIN_EXIT() {}
