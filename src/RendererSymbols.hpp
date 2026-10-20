#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <filesystem>
#include <string_view>

// Hyprland's name search canonicalizes /proc/self/exe, which fails after a
// package update replaces the running executable. Resolve the known signatures
// directly in that case. PLUGIN_INIT checks the running build/ABI before use.
inline void* rendererSymbol(HANDLE handle, const std::string& name, std::string_view demangled) {
    try {
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, name))
            if (match.demangled.contains(demangled))
                return match.address;
    } catch (const std::filesystem::filesystem_error&) {
        struct SFallback { std::string_view name; const char* signature; };
        static constexpr SFallback symbols[] = {
            {"renderLayer", "_ZN6Render13IHyprRenderer11renderLayerEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEE"
                            "NS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbb"},
            {"damageSurface", "_ZN6Render13IHyprRenderer13damageSurfaceEN9Hyprutils6Memory14CSharedPointerI18CWLSurfaceResourceEEddd"},
            {"renderWindow", "_ZN6Render13IHyprRenderer12renderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEE"
                             "NS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbNS_15eRenderPassModeEbb"},
            {"renderWorkspaceWindows", "_ZN6Render13IHyprRenderer22renderWorkspaceWindowsEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEE"
                                       "NS3_I10CWorkspaceEERKNSt6chrono10time_pointINS9_3_V212steady_clockENS9_8durationIlSt5ratioILl1ELl1000000000EEEEEE"},
        };
        for (const auto& symbol : symbols)
            if (symbol.name == name)
                return HyprlandAPI::getFunctionAddressFromSignature(handle, symbol.signature);
    }
    return nullptr;
}
