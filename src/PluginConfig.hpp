#pragma once

#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include "Material.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>

inline constexpr std::string_view CONFIG_PREFIX = "plugin:hyprliquid:";

// Window tags for theme and preset selection
inline constexpr std::string_view TAG_THEME_PREFIX  = "hyprliquid_theme_";
inline constexpr std::string_view TAG_PRESET_PREFIX = "hyprliquid_preset_";

// Window tags for per-window enable/disable. Override the global `enabled` setting.
// `hyprliquid_disabled` always wins if both are present.
inline constexpr std::string_view TAG_ENABLED  = "hyprliquid_enabled";
inline constexpr std::string_view TAG_DISABLED = "hyprliquid_disabled";

// Hyprland stores dynamic tags (`tagwindow` dispatcher, dynamic window rules)
// with a trailing '*'. CTagKeeper::isTagged() normalizes this for exact lookups,
// but code iterating getTags() or registering preset names must strip it itself
// so "firefox" and "firefox*" refer to the same preset.
inline std::string stripDynamicTagMarker(std::string_view tag) {
    if (tag.ends_with('*'))
        tag.remove_suffix(1);
    return std::string(tag);
}

// Sentinel: "not set by user, inherit from parent layer"

inline constexpr int MAX_PRESET_INHERITANCE_DEPTH = 8;

// Layer glass masking strategy: how hyprliquid decides which pixels of a layer
// surface get the glass effect.
enum class ELayerMaskMode { AUTO, ALPHA, REGION };

// Parses "auto"/"alpha"/"region"; nullopt for anything else.
[[nodiscard]] std::optional<ELayerMaskMode> parseLayerMaskMode(std::string_view value);

namespace ConfigKeys {

// Global-only
inline constexpr auto ENABLED            = "plugin:hyprliquid:enabled";
inline constexpr auto DEFAULT_THEME      = "plugin:hyprliquid:default_theme";
inline constexpr auto DEFAULT_PRESET     = "plugin:hyprliquid:default_preset";
inline constexpr auto MANAGE_WINDOW_BLUR = "plugin:hyprliquid:manage_window_blur";

// Preset keyword, registered as unscoped because Hyprlang does not dispatch
// scoped keyword handlers inside the plugin special category.
inline constexpr auto PRESET_KEYWORD = "hyprliquid_preset";

// Layer surface support
inline constexpr auto LAYERS_ENABLED            = "plugin:hyprliquid:layers:enabled";
inline constexpr auto LAYERS_NAMESPACES         = "plugin:hyprliquid:layers:namespaces";
inline constexpr auto LAYERS_EXCLUDE_NAMESPACES = "plugin:hyprliquid:layers:exclude_namespaces";
inline constexpr auto LAYERS_PRESET             = "plugin:hyprliquid:layers:preset";
inline constexpr auto LAYERS_NAMESPACE_PRESETS          = "plugin:hyprliquid:layers:namespace_presets";
inline constexpr auto LAYERS_NAMESPACE_MASK_THRESHOLDS  = "plugin:hyprliquid:layers:namespace_mask_thresholds";
inline constexpr auto LAYERS_NAMESPACE_LIVE_RESAMPLE    = "plugin:hyprliquid:layers:namespace_live_resample";
inline constexpr auto LAYERS_LIVE_RESAMPLE              = "plugin:hyprliquid:layers:live_resample";
inline constexpr auto LAYERS_LIVE_RESAMPLE_FPS          = "plugin:hyprliquid:layers:live_resample_fps";
inline constexpr auto LAYERS_FORCE_LIVE_RESAMPLE        = "plugin:hyprliquid:layers:force_live_resample";
inline constexpr auto LAYERS_MASK_MODE                  = "plugin:hyprliquid:layers:mask_mode";
inline constexpr auto LAYERS_NAMESPACE_MASK_MODES       = "plugin:hyprliquid:layers:namespace_mask_modes";
inline constexpr auto LAYERS_MANAGE_BLUR                = "plugin:hyprliquid:layers:manage_blur";

} // namespace ConfigKeys

// Cached pointers for a single config layer (built-in dark/light/global)
struct SOverridableConfig {
#define GLASS_PARAM(name, value, lo, hi) Hyprlang::FLOAT* const* name = nullptr;
#include "MaterialParams.inc"
#undef GLASS_PARAM
};

// Plain values for a user-defined preset layer (all sentinel = not set → inherit)


struct StringConfigPtr {
    void* const*          dataptr = nullptr;
    const std::type_info* type    = nullptr;
};

inline std::string_view readStringConfig(const StringConfigPtr& ptr) {
    if (!ptr.dataptr || !ptr.type)
        return {};

    if (*ptr.type == typeid(Config::STRING)) {
        const auto* value = *reinterpret_cast<Config::STRING* const*>(ptr.dataptr);
        return value ? std::string_view(*value) : std::string_view{};
    }

    if (*ptr.type == typeid(Hyprlang::STRING)) {
        const auto value = *reinterpret_cast<Hyprlang::STRING const*>(ptr.dataptr);
        return value ? std::string_view(value) : std::string_view{};
    }

    return {};
}

struct SPluginConfig {
    Hyprlang::INT* const* enabled          = nullptr;
    // Glass replaces Hyprland's blur for glassed windows: when set, the plugin
    // marks them with the noblur window property so Hyprland composites them
    // against the live framebuffer (which contains the glass) instead of its
    // pre-frame cached blur.
    Hyprlang::INT* const* manageWindowBlur = nullptr;
    StringConfigPtr      defaultTheme;
    StringConfigPtr      defaultPreset;

    Hyprlang::INT* const* layersEnabled                  = nullptr;
    StringConfigPtr       layersNamespaces;
    StringConfigPtr       layersExcludeNamespaces;
    StringConfigPtr       layersPreset;
    StringConfigPtr       layersNamespacePresets;
    StringConfigPtr       layersNamespaceMaskThresholds;
    StringConfigPtr       layersNamespaceLiveResample;
    Hyprlang::INT* const* layersLiveResample             = nullptr;
    Hyprlang::INT* const* layersLiveResampleFps          = nullptr;
    Hyprlang::INT* const* layersForceLiveResample        = nullptr;
    StringConfigPtr       layersMaskMode;
    StringConfigPtr       layersNamespaceMaskModes;
    Hyprlang::INT* const* layersManageBlur               = nullptr;

    SOverridableConfig global;
    SOverridableConfig dark;
    SOverridableConfig light;
};

// Context for preset-aware value resolution
struct SResolveContext {
    const std::string&                                    presetName;
    bool                                                  isDark;
    const SPluginConfig&                                  config;
    const std::unordered_map<std::string, SCustomPreset>& customPresets;
    const SPresetValues*                                  runtimeOverrides = nullptr;
};

// Preset-aware resolution: preset chain → built-in theme → global → hardcoded
[[nodiscard]] float resolvePresetFloat(
    const SResolveContext& context,
    float SPresetValues::* presetField,
    Hyprlang::FLOAT* const* SOverridableConfig::* configField,
    float hardcodedDefault = static_cast<float>(SENTINEL_FLOAT));

[[nodiscard]] SMaterial resolveMaterial(const SResolveContext& context);

void registerConfig(HANDLE handle);
void initConfigPointers(HANDLE handle, SPluginConfig& config);

// Preset keyword handler (registered via addConfigKeyword)
Hyprlang::CParseResult handlePresetKeyword(const char* command, const char* value);

// Clear pending presets/layers before config re-parse (called from preConfigReload callback)
void clearPendingPresets();
void clearPendingLayers();

// Swap pending data into active maps (called from configReloaded callback)
void commitPendingPresets();
void commitPendingLayers();

// Validate config values and notify user of misconfigurations
void validateConfig();
