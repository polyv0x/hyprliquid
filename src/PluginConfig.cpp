#include "PluginConfig.hpp"
#include "BuiltInPresets.hpp"
#include "Globals.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

// ── Config registration ──────────────────────────────────────────────────────

namespace {

template <typename T, typename Default>
static void addConfigValue(HANDLE handle, const char* name, Default defaultValue) {
    HyprlandAPI::addConfigValueV2(handle,
        Config::Values::makeConfigValue<T>(name, "", defaultValue, Config::Values::valueOptions_t<T>{}));
}

} // namespace

// Forward declarations for Lua handlers (need access to file-static preset/layer data below)
static int handleLuaPreset(lua_State* L);
static int handleLuaLayer(lua_State* L);
static int handleLuaConfig(lua_State* L);

std::optional<ELayerMaskMode> parseLayerMaskMode(std::string_view value) {
    if (value == "auto")   return ELayerMaskMode::AUTO;
    if (value == "alpha")  return ELayerMaskMode::ALPHA;
    if (value == "region") return ELayerMaskMode::REGION;
    return std::nullopt;
}

void registerConfig(HANDLE handle) {
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::ENABLED, Config::INTEGER{1});
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::MANAGE_WINDOW_BLUR, Config::INTEGER{1});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::DEFAULT_THEME, Config::STRING{"dark"});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::DEFAULT_PRESET, Config::STRING{"frosted"});

    // Layer surface support
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::LAYERS_ENABLED, Config::INTEGER{0});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_NAMESPACES, Config::STRING{});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_EXCLUDE_NAMESPACES, Config::STRING{});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_PRESET, Config::STRING{});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_NAMESPACE_PRESETS, Config::STRING{});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_NAMESPACE_MASK_THRESHOLDS, Config::STRING{});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_NAMESPACE_LIVE_RESAMPLE, Config::STRING{});
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::LAYERS_LIVE_RESAMPLE, Config::INTEGER{1});
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::LAYERS_LIVE_RESAMPLE_FPS, Config::INTEGER{30});
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::LAYERS_FORCE_LIVE_RESAMPLE, Config::INTEGER{0});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_MASK_MODE, Config::STRING{"auto"});
    addConfigValue<Config::Values::String>(handle, ConfigKeys::LAYERS_NAMESPACE_MASK_MODES, Config::STRING{});
    addConfigValue<Config::Values::Int>(handle, ConfigKeys::LAYERS_MANAGE_BLUR, Config::INTEGER{1});

    // A single parameter list keeps config, presets and shader uploads in sync.
#define GLASS_PARAM(name, value, lo, hi) \
    addConfigValue<Config::Values::Float>(handle, "plugin:hyprliquid:" #name, Config::FLOAT{value}); \
    addConfigValue<Config::Values::Float>(handle, "plugin:hyprliquid:dark:" #name, Config::FLOAT{SENTINEL_FLOAT}); \
    addConfigValue<Config::Values::Float>(handle, "plugin:hyprliquid:light:" #name, Config::FLOAT{SENTINEL_FLOAT});
#include "MaterialParams.inc"
#undef GLASS_PARAM

    // Legacy config keyword plus Lua-config callbacks for custom presets and layers.
    HyprlandAPI::addConfigKeyword(handle, ConfigKeys::PRESET_KEYWORD, handlePresetKeyword, Hyprlang::SHandlerOptions{});
    HyprlandAPI::addLuaFunction(handle, "hyprliquid", "preset", handleLuaPreset);
    HyprlandAPI::addLuaFunction(handle, "hyprliquid", "layer", handleLuaLayer);
    HyprlandAPI::addLuaFunction(handle, "hyprliquid", "config", handleLuaConfig);
}

// ── Config pointer initialization ────────────────────────────────────────────

template <typename T>
static auto* getStaticPtr(HANDLE /*handle*/, const char* key) {
    return reinterpret_cast<T* const*>(Config::mgr()->getConfigValue(key).dataptr);
}

static StringConfigPtr getStringPtr(HANDLE /*handle*/, const char* key) {
    const auto value = Config::mgr()->getConfigValue(key);
    return {.dataptr = value.dataptr, .type = value.type};
}

void initConfigPointers(HANDLE handle, SPluginConfig& config) {
    config.enabled          = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::ENABLED);
    config.manageWindowBlur = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::MANAGE_WINDOW_BLUR);
    config.defaultTheme  = getStringPtr(handle, ConfigKeys::DEFAULT_THEME);
    config.defaultPreset = getStringPtr(handle, ConfigKeys::DEFAULT_PRESET);

    config.layersEnabled           = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::LAYERS_ENABLED);
    config.layersNamespaces        = getStringPtr(handle, ConfigKeys::LAYERS_NAMESPACES);
    config.layersExcludeNamespaces = getStringPtr(handle, ConfigKeys::LAYERS_EXCLUDE_NAMESPACES);
    config.layersPreset            = getStringPtr(handle, ConfigKeys::LAYERS_PRESET);
    config.layersNamespacePresets         = getStringPtr(handle, ConfigKeys::LAYERS_NAMESPACE_PRESETS);
    config.layersNamespaceMaskThresholds = getStringPtr(handle, ConfigKeys::LAYERS_NAMESPACE_MASK_THRESHOLDS);
    config.layersNamespaceLiveResample = getStringPtr(handle, ConfigKeys::LAYERS_NAMESPACE_LIVE_RESAMPLE);
    config.layersLiveResample      = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::LAYERS_LIVE_RESAMPLE);
    config.layersLiveResampleFps   = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::LAYERS_LIVE_RESAMPLE_FPS);
    config.layersForceLiveResample = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::LAYERS_FORCE_LIVE_RESAMPLE);
    config.layersMaskMode           = getStringPtr(handle, ConfigKeys::LAYERS_MASK_MODE);
    config.layersNamespaceMaskModes = getStringPtr(handle, ConfigKeys::LAYERS_NAMESPACE_MASK_MODES);
    config.layersManageBlur         = getStaticPtr<Hyprlang::INT>(handle, ConfigKeys::LAYERS_MANAGE_BLUR);

#define GLASS_PARAM(name, value, lo, hi) \
    config.global.name = getStaticPtr<Hyprlang::FLOAT>(handle, "plugin:hyprliquid:" #name); \
    config.dark.name = getStaticPtr<Hyprlang::FLOAT>(handle, "plugin:hyprliquid:dark:" #name); \
    config.light.name = getStaticPtr<Hyprlang::FLOAT>(handle, "plugin:hyprliquid:light:" #name);
#include "MaterialParams.inc"
#undef GLASS_PARAM
}

// ── Preset keyword parsing ───────────────────────────────────────────────────

// Presets built during config parse, swapped into g_pGlobalState on configReloaded
static std::unordered_map<std::string, SCustomPreset> s_pendingPresets;

static std::string_view trim(std::string_view str) {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.remove_prefix(1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))  str.remove_suffix(1);
    return str;
}

static bool setPresetField(SPresetValues& values, std::string_view key, std::string_view valueStr) {
    float parsed = 0.0f;
    auto [ptr, ec] = std::from_chars(valueStr.data(), valueStr.data() + valueStr.size(), parsed);
    if (ec != std::errc{} || ptr != valueStr.data() + valueStr.size() || !std::isfinite(parsed))
        return false;
#define GLASS_PARAM(name, value, lo, hi) \
    if (key == #name) { \
        if (parsed < lo || parsed > hi) return false; \
        values.name = parsed; return true; \
    }
#include "MaterialParams.inc"
#undef GLASS_PARAM
    return false;
}

static bool isSet(float value) {
    return std::isfinite(value) && value != static_cast<float>(SENTINEL_FLOAT);
}

static void mergePresetValues(SPresetValues& target, const SPresetValues& overrides) {
#define GLASS_PARAM(name, value, lo, hi) if (isSet(overrides.name)) target.name = overrides.name;
#include "MaterialParams.inc"
#undef GLASS_PARAM
}

Hyprlang::CParseResult handlePresetKeyword(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;
    std::string_view       input(value);

    std::string presetName;
    std::string variant;     // "", "dark", or "light"
    std::string inherits;
    SPresetValues parsedValues;

    // Split on ',' and parse key:value tokens
    while (!input.empty()) {
        auto commaPos = input.find(',');
        auto token = trim(input.substr(0, commaPos));
        input = (commaPos == std::string_view::npos) ? std::string_view{} : input.substr(commaPos + 1);

        if (token.empty()) continue;

        auto colonPos = token.find(':');
        if (colonPos == std::string_view::npos) {
            result.setError(std::format("preset: invalid token '{}' (expected key:value)", token).c_str());
            return result;
        }

        auto key = trim(token.substr(0, colonPos));
        auto val = trim(token.substr(colonPos + 1));

        if (key == "name") {
            // val is "presetname" or "presetname:dark" or "presetname:light"
            auto variantSep = val.find(':');
            if (variantSep != std::string_view::npos) {
                presetName = std::string(val.substr(0, variantSep));
                variant = std::string(val.substr(variantSep + 1));
                if (variant != "dark" && variant != "light") {
                    result.setError(std::format("preset: invalid variant '{}' (expected dark or light)", variant).c_str());
                    return result;
                }
            } else {
                presetName = std::string(val);
            }
        } else if (key == "inherits") {
            inherits = std::string(val);
        } else {
            if (!setPresetField(parsedValues, key, val)) {
                result.setError(std::format("preset: unknown or invalid setting '{}:{}'", key, val).c_str());
                return result;
            }
        }
    }

    if (presetName.empty()) {
        result.setError("preset: missing required 'name' field");
        return result;
    }

    // Normalize names so presets defined as "firefox*" (old workaround for
    // dynamic tags) and "firefox" register under the same key.
    presetName = stripDynamicTagMarker(presetName);
    inherits   = stripDynamicTagMarker(inherits);

    // Get or create the preset entry
    auto& preset = s_pendingPresets[presetName];
    preset.name = presetName;

    if (!inherits.empty())
        preset.inherits = inherits;

    // Merge parsed values into the correct layer (additive across multiple lines)
    if (variant == "dark")
        mergePresetValues(preset.dark, parsedValues);
    else if (variant == "light")
        mergePresetValues(preset.light, parsedValues);
    else
        mergePresetValues(preset.shared, parsedValues);

    return result;
}

void clearPendingPresets() {
    s_pendingPresets.clear();
}

void commitPendingPresets() {
    if (!g_pGlobalState) return;

    // Start with built-in presets, then merge user-defined overrides (non-sentinel fields win)
    auto merged = BuiltInPresets::getAll();
    for (auto& [name, userPreset] : s_pendingPresets) {
        if (auto it = merged.find(name); it != merged.end()) {
            it->second.hidden = false;
            if (!userPreset.inherits.empty())
                it->second.inherits = userPreset.inherits;
            mergePresetValues(it->second.shared, userPreset.shared);
            // Explicit shared user values override both built-in theme defaults.
            // A user's theme-specific value still takes precedence below.
            mergePresetValues(it->second.dark, userPreset.shared);
            mergePresetValues(it->second.light, userPreset.shared);
            mergePresetValues(it->second.dark, userPreset.dark);
            mergePresetValues(it->second.light, userPreset.light);
        } else {
            merged[name] = std::move(userPreset);
        }
    }

    g_pGlobalState->customPresets = std::move(merged);
    s_pendingPresets.clear();
}

// ── Lua config handler ──────────────────────────────────────────────────────
// hyprliquid.config({...}) forwards to a single hl.config({ plugin = { hyprliquid = ... } }).
// Keys are validated first so unknown options are reported with the user's file and line instead of '[C]'.
// Lua errors longjmp, so they are raised only while no C++ object lives in our frames.

static WP<Config::Lua::CConfigManager> luaConfigManager() {
    return dynamicPointerCast<Config::Lua::CConfigManager>(WP<Config::IConfigManager>(Config::mgr()));
}

struct SLuaConfigContext {
    lua_State*                                       L;
    WP<Config::Lua::CConfigManager>                  luaMgr;
    std::string                                      where;
    std::vector<std::pair<std::string, std::string>> applied; // {option path, dotted key}

    void report(const std::string& msg) const {
        if (!luaMgr) return;
        luaMgr->addError(where.empty() ? "hyprliquid.config: " + msg : where + ": hyprliquid.config: " + msg);
    }
};

// Copies the known options of the table at srcIdx into the table at dstIdx (absolute indices).
static void copyValidatedTable(SLuaConfigContext& ctx, int srcIdx, int dstIdx, const std::string& dottedPrefix, int depth = 0) {
    lua_State* L = ctx.L;
    // options are at most two levels deep; the cap also ends self-referencing tables
    if (depth > 4 || !lua_checkstack(L, 5)) {
        ctx.report(dottedPrefix.empty() ? std::string{"nesting too deep"} : std::format("nesting too deep in '{}'", dottedPrefix));
        return;
    }

    lua_pushnil(L);
    while (lua_next(L, srcIdx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            ctx.report(dottedPrefix.empty() ? std::string{"ignoring non-string key"} : std::format("ignoring non-string key in '{}'", dottedPrefix));
            lua_pop(L, 1);
            continue;
        }

        // same normalisation as Hyprland's luaConfigValueName
        std::string key = lua_tostring(L, -2);
        std::replace(key.begin(), key.end(), ':', '.');
        std::replace(key.begin(), key.end(), '-', '_');
        const std::string dotted = dottedPrefix.empty() ? key : dottedPrefix + "." + key;

        std::string optionPath = "plugin:hyprliquid:" + dotted;
        std::replace(optionPath.begin(), optionPath.end(), '.', ':');
        const bool known = Config::mgr()->getConfigValue(optionPath).dataptr != nullptr;
        const bool table = lua_istable(L, -1);

        if (!known) {
            if (table) {
                lua_newtable(L);
                const int subDst = lua_gettop(L);
                copyValidatedTable(ctx, subDst - 1, subDst, dotted, depth + 1);
                lua_setfield(L, dstIdx, key.c_str());
            } else {
                ctx.report(std::format("unknown option '{}'", dotted));
            }
        } else if (table) {
            ctx.report(std::format("option '{}' expects a value, not a table", dotted));
        } else {
            lua_pushvalue(L, -1);
            lua_setfield(L, dstIdx, key.c_str());
            ctx.applied.emplace_back(std::move(optionPath), dotted);
        }
        lua_pop(L, 1);
    }
}

// Stack on entry: [T, ..., hl, config]. Returns true with [T, ..., hl], or false with the error message on top.
static bool forwardLuaConfig(lua_State* L) {
    SLuaConfigContext ctx{.L = L, .luaMgr = luaConfigManager()};

    lua_Debug ar{};
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0) {
        const char* src = ar.source;
        if (src && *src == '@')
            ++src;
        ctx.where = std::format("{}:{}", src ? src : "?", ar.currentline);
    }

    lua_newtable(L); // root
    lua_newtable(L); // plugin
    lua_newtable(L); // hyprliquid
    copyValidatedTable(ctx, 1, lua_gettop(L), "");
    lua_setfield(L, -2, "hyprliquid");
    lua_setfield(L, -2, "plugin");

    const std::string errorsBefore = ctx.luaMgr ? ctx.luaMgr->getErrors() : std::string{};
    if (lua_pcall(L, 1, 0, 0) != LUA_OK)
        return false;

    // hl.config reports type and range errors itself; only look for silent no-ops while the config file is parsed.
    if (ctx.luaMgr && !ctx.luaMgr->isDynamicParse() && ctx.luaMgr->getErrors() == errorsBefore) {
        for (const auto& [optionPath, dotted] : ctx.applied) {
            if (!Config::mgr()->getConfigValue(optionPath).setByUser)
                ctx.report(std::format("option '{}' was not applied", dotted));
        }
    }
    return true;
}

static int handleLuaConfig(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_istable(L, 1))
        return luaL_error(L, "hyprliquid.config: expected a table");

    lua_getglobal(L, "hl");
    if (!lua_istable(L, -1))
        return luaL_error(L, "hyprliquid.config: hl.config is not available");
    lua_getfield(L, -1, "config");
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "hyprliquid.config: hl.config is not available");

    if (!forwardLuaConfig(L))
        return lua_error(L);
    lua_pop(L, 1); // hl
    if (const auto manager = luaConfigManager(); manager && manager->isDynamicParse())
        runtimeBaselineChanged(false);
    return 0;
}

// ── Lua preset handler (table + string) ─────────────────────────────────────

static void readPresetValuesFromTable(lua_State* L, int tableIdx, SPresetValues& values) {
    lua_pushnil(L);
    while (lua_next(L, tableIdx) != 0) {
        if (lua_isstring(L, -2) && lua_isnumber(L, -1)) {
            const char* key = lua_tostring(L, -2);
            std::string valStr = std::to_string(lua_tonumber(L, -1));
            setPresetField(values, key, valStr);
        }
        lua_pop(L, 1);
    }
}

static int handleLuaPreset(lua_State* L) {
    int nargs = lua_gettop(L);

    // Legacy: preset("name:clear, glass_opacity:0.8, ...")
    if (nargs == 1 && lua_isstring(L, 1)) {
        const auto result = handlePresetKeyword(ConfigKeys::PRESET_KEYWORD, lua_tostring(L, 1));
        if (result.error)
            return luaL_error(L, "%s", result.getError());
        return 0;
    }

    // Table: preset("clear", { glass_opacity = 0.8, dark = { brightness = 0.7 }, ... })
    if (nargs == 2 && lua_isstring(L, 1) && lua_istable(L, 2)) {
        std::string baseName = stripDynamicTagMarker(lua_tostring(L, 1));
        auto& preset = s_pendingPresets[baseName];
        preset.name = baseName;

        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (!lua_isstring(L, -2)) { lua_pop(L, 1); continue; }
            const char* key = lua_tostring(L, -2);

            if (strcmp(key, "inherits") == 0 && lua_isstring(L, -1)) {
                preset.inherits = stripDynamicTagMarker(lua_tostring(L, -1));
            } else if (strcmp(key, "dark") == 0 && lua_istable(L, -1)) {
                readPresetValuesFromTable(L, lua_gettop(L), preset.dark);
            } else if (strcmp(key, "light") == 0 && lua_istable(L, -1)) {
                readPresetValuesFromTable(L, lua_gettop(L), preset.light);
            } else if (lua_isnumber(L, -1)) {
                std::string valStr = std::to_string(lua_tonumber(L, -1));
                setPresetField(preset.shared, key, valStr);
            }
            lua_pop(L, 1);
        }
        return 0;
    }

    return luaL_error(L, "hyprliquid.preset: expected (string) or (name, table)");
}

// ── Lua layer handler ───────────────────────────────────────────────────────

struct SPendingLayer {
    std::string                   ns;
    std::string                   preset;
    float                         maskThreshold = -1.0f;
    bool                          exclude       = false;
    int                           liveResample  = -1; // -1 = not set
    std::optional<ELayerMaskMode> maskMode;
};

static std::vector<SPendingLayer> s_pendingLayers;

static int handleLuaLayer(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
        return luaL_error(L, "hyprliquid.layer: first argument must be a namespace string");

    SPendingLayer entry;
    entry.ns = lua_tostring(L, 1);

    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "exclude");
        if (lua_isboolean(L, -1) && lua_toboolean(L, -1))
            entry.exclude = true;
        lua_pop(L, 1);

        lua_getfield(L, 2, "preset");
        if (lua_isstring(L, -1))
            entry.preset = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "mask_threshold");
        if (lua_isnumber(L, -1))
            entry.maskThreshold = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 2, "live_resample");
        if (lua_isboolean(L, -1))
            entry.liveResample = lua_toboolean(L, -1) ? 1 : 0;
        lua_pop(L, 1);

        lua_getfield(L, 2, "mask_mode");
        if (lua_isstring(L, -1))
            entry.maskMode = parseLayerMaskMode(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    s_pendingLayers.push_back(std::move(entry));
    return 0;
}

void clearPendingLayers() {
    s_pendingLayers.clear();
}

void commitPendingLayers() {
    if (!g_pGlobalState) return;
    for (const auto& entry : s_pendingLayers) {
        if (entry.exclude) {
            g_pGlobalState->layerNamespaceExclude.insert(entry.ns);
        } else {
            g_pGlobalState->layerNamespaceFilter.insert(entry.ns);
            if (!entry.preset.empty())
                g_pGlobalState->layerNamespacePresets[entry.ns] = entry.preset;
            if (entry.maskThreshold >= 0.0f)
                g_pGlobalState->layerNamespaceMaskThresholds[entry.ns] = entry.maskThreshold;
            if (entry.liveResample >= 0)
                g_pGlobalState->layerNamespaceLiveResample[entry.ns] = entry.liveResample != 0;
            if (entry.maskMode)
                g_pGlobalState->layerNamespaceMaskModes[entry.ns] = *entry.maskMode;
        }
    }
    s_pendingLayers.clear();
}

void validateConfig() {
    if (!g_pGlobalState) return;

    const auto& config = g_pGlobalState->config;

    const auto theme = readStringConfig(config.defaultTheme);
    if (theme != "dark" && theme != "light") {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprliquid] Invalid default_theme '") + std::string(theme) + "', expected 'dark' or 'light'. Falling back to 'dark'."},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    const auto maskMode = readStringConfig(config.layersMaskMode);
    if (!parseLayerMaskMode(maskMode)) {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprliquid] Invalid layers:mask_mode '") + std::string(maskMode) + "', expected 'auto', 'alpha', or 'region'. Falling back to 'auto'."},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    const auto preset = readStringConfig(config.defaultPreset);
    if (!preset.empty() && preset != "default") {
        const auto& presets = g_pGlobalState->customPresets;
        if (presets.find(std::string(preset)) == presets.end()) {
            HyprlandAPI::addNotificationV2(PHANDLE, {
                {"text", std::string("[hyprliquid] Unknown default_preset '") + std::string(preset) + "'. Using 'default' resolution chain."},
                {"time", (uint64_t)5000},
                {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
            });
        }
    }
}

// ── Preset-aware resolution ──────────────────────────────────────────────────

static float resolvePresetFloatImpl(
    const std::string& presetName, bool isDark,
    float SPresetValues::* presetField,
    Hyprlang::FLOAT* const* SOverridableConfig::* configField,
    const SPluginConfig& config,
    const std::unordered_map<std::string, SCustomPreset>& customPresets,
    float hardcodedDefault, int depth
) {
    if (depth < MAX_PRESET_INHERITANCE_DEPTH) {
        if (auto it = customPresets.find(presetName); it != customPresets.end()) {
            const auto& preset = it->second;

            const auto& themeVariant = isDark ? preset.dark : preset.light;
            if (isSet(themeVariant.*presetField)) return themeVariant.*presetField;

            if (isSet(preset.shared.*presetField)) return preset.shared.*presetField;

            if (!preset.inherits.empty())
                return resolvePresetFloatImpl(preset.inherits, isDark, presetField, configField,
                                             config, customPresets, hardcodedDefault, depth + (preset.hidden ? 0 : 1));
        }
    }

    // Built-in theme override
    const auto& themeConfig = isDark ? config.dark : config.light;
    if (auto ptr = themeConfig.*configField; ptr && *ptr) {
        const float themeValue = static_cast<float>(**ptr);
        if (isSet(themeValue)) return themeValue;
    }

    // Global
    if (auto ptr = config.global.*configField; ptr && *ptr) {
        const float globalValue = static_cast<float>(**ptr);
        if (isSet(globalValue)) return globalValue;
    }

    return hardcodedDefault;
}

float resolvePresetFloat(
    const SResolveContext& context,
    float SPresetValues::* presetField,
    Hyprlang::FLOAT* const* SOverridableConfig::* configField,
    float hardcodedDefault
) {
    if (context.runtimeOverrides && isSet(context.runtimeOverrides->*presetField))
        return context.runtimeOverrides->*presetField;
    return resolvePresetFloatImpl(context.presetName, context.isDark, presetField, configField,
                                 context.config, context.customPresets, hardcodedDefault, 0);
}

SMaterial resolveMaterial(const SResolveContext& context) {
    SMaterial material;
#define GLASS_PARAM(name, value, lo, hi) \
    material.name = resolvePresetFloat(context, &SPresetValues::name, &SOverridableConfig::name, value);
#include "MaterialParams.inc"
#undef GLASS_PARAM
    sanitizeMaterial(material);
    return material;
}
