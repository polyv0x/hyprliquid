#pragma once

#include "HyprlandCompat.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <algorithm>
#include <filesystem>
#include <string_view>

// Match the complete ABI signature. A future overload with extra arguments
// must not be called through one of our existing function-pointer types.
// PLUGIN_INIT separately verifies the compositor build and library ABI.
inline void* rendererSymbol(HANDLE handle, const std::string& name) {
    struct SSymbol { std::string_view name; const char* signature; };
    static constexpr SSymbol symbols[] = {
#if HYPRLIQUID_WINDOW_PRESENTATION
        {"setTarget", "_ZN27CAnimatedDecorationGradient9setTargetERKN6Config18CGradientValueDataEb"},
        {"rounding", "_ZN7Desktop4View19CWindowPresentation8roundingEv"},
#else
        {"rounding", "_ZN7Desktop4View7CWindow8roundingEv"},
#endif
        {"damageSurface", "_ZN6Render13IHyprRenderer13damageSurfaceEN9Hyprutils6Memory14CSharedPointerI18CWLSurfaceResourceEEddd"},
#if HYPRLIQUID_RENDER_CONTEXT
        {"renderLayer", "_ZN6Render13IHyprRenderer11renderLayerERNS_14CRenderContextEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS5_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSD_3_V212steady_clockENSD_8durationIlSt5ratioILl1ELl1000000000EEEEEEbbb"},
        {"renderWindow", "_ZN6Render13IHyprRenderer12renderWindowERNS_14CRenderContextEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS5_IN7Monitor8CMonitorEEERKNS_25SWindowRenderPresentationERKNSt6chrono10time_pointINSG_3_V212steady_clockENSG_8durationIlSt5ratioILl1ELl1000000000EEEEEEbNS_15eRenderPassModeEbb"},
        {"renderWorkspaceWindows", "_ZN6Render13IHyprRenderer22renderWorkspaceWindowsERNS_14CRenderContextEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEENS5_IN9Workspace12CHLWorkspaceEEERKNSt6chrono10time_pointINSC_3_V212steady_clockENSC_8durationIlSt5ratioILl1ELl1000000000EEEEEENS_10eSceneModeE"},
#else
        {"renderLayer", "_ZN6Render13IHyprRenderer11renderLayerEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbb"},
        {"renderWindow", "_ZN6Render13IHyprRenderer12renderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbNS_15eRenderPassModeEbb"},
        {"renderWorkspaceWindows", "_ZN6Render13IHyprRenderer22renderWorkspaceWindowsEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEENS3_I10CWorkspaceEERKNSt6chrono10time_pointINS9_3_V212steady_clockENS9_8durationIlSt5ratioILl1ELl1000000000EEEEEE"},
#endif
    };
    const auto expected = std::ranges::find_if(symbols, [&](const auto& symbol) { return symbol.name == name; });
    if (expected == std::end(symbols)) return nullptr;
    try {
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, name))
            if (match.signature == expected->signature)
                return match.address;
    } catch (const std::filesystem::filesystem_error&) {
        // Name enumeration canonicalizes /proc/self/exe, which fails after a
        // package update replaces the running executable. Resolve its export.
        return HyprlandAPI::getFunctionAddressFromSignature(handle, expected->signature);
    }
    return nullptr;
}
