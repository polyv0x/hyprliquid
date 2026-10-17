#include "RuntimeControl.hpp"
#include "Globals.hpp"

#include <hyprland/src/state/MonitorState.hpp>
#include <json-c/json.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
SP<SHyprCtlCommand> command;
constexpr auto COMMAND = "hyprliquid:control";
constexpr int API = 1;

Json own(json_object* value) { return Json(value, json_object_put); }
json_object* object() { return json_object_new_object(); }
json_object* string(const std::string& value) { return json_object_new_string_len(value.data(), value.size()); }
void put(json_object* target, const char* key, json_object* value) { json_object_object_add(target, key, value); }
json_object* get(json_object* target, const char* key) { return json_object_object_get(target, key); }
bool has(json_object* target, const char* key) { json_object* value; return json_object_object_get_ex(target, key, &value); }
void require(bool condition, const std::string& message) { if (!condition) throw std::invalid_argument(message); }
std::string text(json_object* value) {
    require(json_object_is_type(value, json_type_string), "expected a string");
    return {json_object_get_string(value), static_cast<size_t>(json_object_get_string_len(value))};
}
void keys(json_object* value, std::initializer_list<std::string_view> allowed) {
    require(json_object_is_type(value, json_type_object), "expected an object");
    json_object_object_foreach(value, key, item) {
        (void)item;
        require(std::ranges::find(allowed, key) != allowed.end(), std::string("unknown field: ") + key);
    }
}

std::string configuredPreset() {
    const auto preset = readStringConfig(g_pGlobalState->config.defaultPreset);
    return preset.empty() ? "default" : std::string(preset);
}
bool configuredDark() { return readStringConfig(g_pGlobalState->config.defaultTheme) != "light"; }
bool configuredEnabled() {
    const auto ptr = g_pGlobalState->config.enabled;
    return ptr && *ptr && **ptr;
}
bool validPreset(std::string name) {
    std::unordered_set<std::string> visited;
    for (int depth = 0; depth < MAX_PRESET_INHERITANCE_DEPTH; ++depth) {
        const auto it = g_pGlobalState->customPresets.find(name);
        if (it == g_pGlobalState->customPresets.end() || !visited.insert(name).second) return false;
        if (it->second.inherits.empty()) return true;
        name = it->second.inherits;
    }
    return false;
}
json_object* values(const SMaterial& material) {
    auto* result = object();
#define GLASS_PARAM(name, value, lo, hi) put(result, #name, json_object_new_double(material.name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
    return result;
}
json_object* profileJson(const SRuntimeProfile& profile) {
    auto* result = object();
    if (profile.enabled) put(result, "enabled", json_object_new_boolean(*profile.enabled));
    if (profile.basePreset) put(result, "base_preset", string(*profile.basePreset));
    if (profile.theme) put(result, "theme", string(*profile.theme));
    auto* overrides = object();
#define GLASS_PARAM(name, value, lo, hi) if (profile.overrides.name != SENTINEL_FLOAT) put(overrides, #name, json_object_new_double(profile.overrides.name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
    put(result, "overrides", overrides);
    return result;
}
bool active(const SRuntimeProfile& profile) {
    if (profile.enabled || profile.basePreset || profile.theme) return true;
#define GLASS_PARAM(name, value, lo, hi) if (profile.overrides.name != SENTINEL_FLOAT) return true;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    return false;
}
json_object* state() {
    const auto& runtime = g_pGlobalState->runtimeControl;
    const auto& config = g_pGlobalState->config;
    const auto& presets = g_pGlobalState->customPresets;
    const auto base = runtimeBasePreset();
    const auto configured = configuredPreset();
    auto* result = object();
    put(result, "epoch", string(runtime.epoch));
    put(result, "revision", json_object_new_int64(runtime.revision));
    put(result, "generation", json_object_new_int64(runtime.generation));
    put(result, "enabled", json_object_new_boolean(runtimeEnabled()));
    put(result, "base_preset", string(base));
    put(result, "theme", string(runtimeThemeIsDark() ? "dark" : "light"));
    put(result, "active", json_object_new_boolean(active(runtime.profile)));
    put(result, "panel", profileJson(runtime.profile));
    put(result, "inherited", values(resolveMaterial({base, runtimeThemeIsDark(), config, presets})));
    put(result, "material", values(resolveMaterial({base, runtimeThemeIsDark(), config, presets, &runtime.profile.overrides})));
    auto* baseline = object();
    put(baseline, "enabled", json_object_new_boolean(configuredEnabled()));
    put(baseline, "base_preset", string(configured));
    put(baseline, "theme", string(configuredDark() ? "dark" : "light"));
    put(baseline, "material", values(resolveMaterial({configured, configuredDark(), config, presets})));
    put(result, "baseline", baseline);
    return result;
}
void describe(json_object* result) {
    auto* capabilities = json_object_new_array();
    for (const auto* name : {"default_material", "atomic_apply", "revision_guard", "field_reset"})
        json_object_array_add(capabilities, string(name));
    put(result, "capabilities", capabilities);
    auto* parameters = json_object_new_array();
#define GLASS_PARAM(name, value, lo, hi) { \
    auto* item = object(); \
    put(item, "name", string(#name)); \
    put(item, "default", json_object_new_double(value)); \
    put(item, "min", json_object_new_double(lo)); \
    put(item, "max", json_object_new_double(hi)); \
    json_object_array_add(parameters, item); \
}
#include "MaterialParams.inc"
#undef GLASS_PARAM
    put(result, "parameters", parameters);
    auto* presets = json_object_new_array();
    std::vector<std::string> names;
    for (const auto& [name, preset] : g_pGlobalState->customPresets) names.push_back(name);
    std::ranges::sort(names);
    for (const auto& name : names) {
        auto* item = object();
        put(item, "id", string(name));
        put(item, "inherits", string(g_pGlobalState->customPresets.at(name).inherits));
        put(item, "valid", json_object_new_boolean(validPreset(name)));
        json_object_array_add(presets, item);
    }
    put(result, "presets", presets);
}
void setField(SPresetValues& target, const std::string& key, json_object* item) {
#define GLASS_PARAM(name, value, lo, hi) if (key == #name) { \
    if (!item) { target.name = SENTINEL_FLOAT; return; } \
    require(json_object_is_type(item, json_type_double) || json_object_is_type(item, json_type_int), key + ": expected a number"); \
    const double number = json_object_get_double(item); \
    require(std::isfinite(number) && static_cast<float>(number) >= lo && static_cast<float>(number) <= hi, key + ": outside valid range"); \
    target.name = static_cast<float>(number); return; \
}
#include "MaterialParams.inc"
#undef GLASS_PARAM
    throw std::invalid_argument("unknown material parameter: " + key);
}
void readProfile(SRuntimeProfile& target, json_object* input) {
    keys(input, {"enabled", "base_preset", "theme", "overrides", "reset_fields"});
    if (has(input, "enabled")) {
        auto* v = get(input, "enabled");
        require(!v || json_object_is_type(v, json_type_boolean), "enabled: expected boolean or null");
        target.enabled = v ? std::optional<bool>(json_object_get_boolean(v)) : std::nullopt;
    }
    if (has(input, "base_preset")) {
        auto* v = get(input, "base_preset");
        target.basePreset = v ? std::optional<std::string>(text(v)) : std::nullopt;
    }
    if (has(input, "theme")) {
        auto* v = get(input, "theme");
        target.theme = v ? std::optional<std::string>(text(v)) : std::nullopt;
        require(!target.theme || *target.theme == "dark" || *target.theme == "light", "theme: expected dark or light");
    }
    if (has(input, "overrides")) {
        auto* overrides = get(input, "overrides");
        require(json_object_is_type(overrides, json_type_object), "overrides: expected an object");
        json_object_object_foreach(overrides, key, item) { setField(target.overrides, key, item); }
    }
    if (has(input, "reset_fields")) {
        auto* resets = get(input, "reset_fields");
        require(json_object_is_type(resets, json_type_array), "reset_fields: expected an array");
        for (size_t i = 0; i < json_object_array_length(resets); ++i)
            setField(target.overrides, text(json_object_array_get_idx(resets, i)), nullptr);
    }
    require(validPreset(target.basePreset.value_or(configuredPreset())), "base_preset: missing, cyclic, or excessively deep inheritance");
}
void repaint() {
    for (const auto& monitor : State::monitorState()->monitors()) {
        g_pGlobalState->bumpSceneGeneration(monitor);
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame();
    }
}
bool guard(json_object* request) {
    const bool epoch = has(request, "if_epoch"), revision = has(request, "if_revision");
    require(epoch == revision, "if_epoch and if_revision must be supplied together");
    if (!epoch) return true;
    auto* value = get(request, "if_revision");
    require(json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 0, "if_revision: expected a nonnegative integer");
    return text(get(request, "if_epoch")) == g_pGlobalState->runtimeControl.epoch &&
        static_cast<uint64_t>(json_object_get_int64(value)) == g_pGlobalState->runtimeControl.revision;
}
std::string dispatch(eHyprCtlOutputFormat, std::string request) {
    auto response = own(object());
    put(response.get(), "api", json_object_new_int(API));
    put(response.get(), "ok", json_object_new_boolean(false));
    try {
        require(request.starts_with(std::string(COMMAND) + " "), "expected a JSON request after hyprliquid:control");
        const auto payload = request.substr(std::char_traits<char>::length(COMMAND) + 1);
        require(payload.size() <= 16384, "request too large");
        auto tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(json_tokener_new_ex(16), json_tokener_free);
        json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        auto input = own(json_tokener_parse_ex(tokener.get(), payload.c_str(), payload.size() + 1));
        require(json_tokener_get_error(tokener.get()) == json_tokener_success && input, "invalid JSON");
        const auto op = text(get(input.get(), "op"));
        if (op == "describe" || op == "state") {
            keys(input.get(), {"op"});
            if (op == "describe") describe(response.get());
        } else if (op == "apply" || op == "reset") {
            if (op == "reset") keys(input.get(), {"op", "if_epoch", "if_revision"});
            else keys(input.get(), {"op", "if_epoch", "if_revision", "profile", "patch"});
            if (!guard(input.get())) {
                put(response.get(), "code", string("revision_conflict"));
                throw std::invalid_argument("renderer changed; read state before applying again");
            }
            auto next = g_pGlobalState->runtimeControl.profile;
            if (op == "reset") next = {};
            else {
                require(has(input.get(), "profile") != has(input.get(), "patch"), "supply exactly one of profile or patch");
                if (has(input.get(), "profile")) next = {};
                readProfile(next, get(input.get(), has(input.get(), "profile") ? "profile" : "patch"));
            }
            // Validation is complete. There are no partial writes on failure.
            g_pGlobalState->runtimeControl.profile = std::move(next);
            ++g_pGlobalState->runtimeControl.revision;
            repaint();
        } else throw std::invalid_argument("unknown operation: " + op);
        put(response.get(), "ok", json_object_new_boolean(true));
    } catch (const std::exception& error) {
        if (!has(response.get(), "code")) put(response.get(), "code", string("invalid_request"));
        put(response.get(), "error", string(error.what()));
    }
    put(response.get(), "state", state());
    return json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN);
}
} // namespace

bool runtimeEnabled() { return g_pGlobalState->runtimeControl.profile.enabled.value_or(configuredEnabled()); }
std::string runtimeBasePreset() { return g_pGlobalState->runtimeControl.profile.basePreset.value_or(configuredPreset()); }
bool runtimeThemeIsDark() {
    const auto& theme = g_pGlobalState->runtimeControl.profile.theme;
    return theme ? *theme == "dark" : configuredDark();
}
void runtimeBaselineChanged(bool reloaded) {
    if (!g_pGlobalState) return;
    auto& runtime = g_pGlobalState->runtimeControl;
    runtime.profile = {};
    ++runtime.revision;
    if (reloaded) ++runtime.generation;
    repaint();
}
void registerRuntimeControl() {
    g_pGlobalState->runtimeControl.epoch = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    command = HyprlandAPI::registerHyprCtlCommand(PHANDLE, {COMMAND, false, dispatch});
    if (!command) throw std::runtime_error("hyprliquid: could not register runtime control IPC");
}
void unregisterRuntimeControl() {
    if (command) HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, command);
    command.reset();
}
