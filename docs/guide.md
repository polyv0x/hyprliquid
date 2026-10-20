# Hyprliquid guide

Hyprliquid adds glass effects to transparent Hyprland windows. It blurs and
refracts the desktop behind each window while keeping text and application
content sharp. You can choose presets per window and optionally enable glass
for panels and other layer surfaces.

## Build

For installation with `hyprpm` or a fresh source checkout, start with the
[installation instructions](../README.md#installation).

Targets **Hyprland 0.56.2**. Build against the headers for the running compositor;
the plugin checks the full Hyprland build/ABI string when loaded.

Requirements: a C++23 compiler compatible with your Hyprland build, GNU Make,
Python 3, pkg-config, pixman, libdrm and json-c development files, Hyprland
development headers, and their dependencies.
`pkg-config --cflags hyprland pixman-1 libdrm json-c` must locate the correct
headers. After updating Hyprland's headers, use `make -B -j4` to rebuild all
objects.

```sh
make -j4
```

This creates `hyprliquid.so`. Shaders are embedded at build time; the installed
plugin needs no external shader files. `hyprpm.toml` also supports building this
repository through hyprpm.

## Load

Unload any other backdrop-effect plugins before loading:

```sh
hyprctl plugin load /absolute/path/to/hyprliquid/hyprliquid.so
hyprctl plugin list
```

Multiple backdrop-effect plugins can interfere with the same rendering path.
A manual load lasts for the current session. Use `hyprctl plugin unload` with
the same absolute path to unload it.

For a manual installation, load it at startup from `~/.config/hypr/hyprland.lua`:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("hyprctl plugin load /absolute/path/to/hyprliquid/hyprliquid.so")
end)
```

Replace the example path with the absolute path to your built plugin. In a legacy
`hyprland.conf`, use `plugin = /absolute/path/to/hyprliquid/hyprliquid.so` instead.
For a `hyprpm` installation, use the `hyprpm reload` startup command from the
[README](../README.md#hyprpm-recommended) instead of loading a second copy.

## Configure

Add the contents of [`examples/hyprliquid.lua`](../examples/hyprliquid.lua) to your
Hyprland Lua configuration, or load that file with `dofile`. The plugin must be
loaded for `hl.plugin.hyprliquid` to exist. It reloads the configuration on load.

```lua
if hl.plugin.hyprliquid then
    local glass = hl.plugin.hyprliquid
    glass.config({ default_preset = "clear-edge" })
    glass.preset("my_glass", {
        inherits = "soft-glass",
        blur_sigma = 12,
        inner_refraction_amount = -10,
        rim_opacity = 0.6,
    })
end
hl.window_rule({ name = "terminal-glass", match = { class = "^kitty$" },
                 tag = "+hyprliquid_preset_my_glass" })
```

The default is `clear-edge` in dark mode, with a frosted center, a clear refractive
border, and reflection falloff set to `1`. Select `frosted` for an even blur with
no added rim, aberration, or refraction.

| Preset | Appearance |
| --- | --- |
| `frosted` | Soft, even blur without added refraction or rim lighting |
| `soft-glass` | Gentle refraction, soft blur, and rim highlights |
| `clear-glass` | Sharp backdrop with strong refraction and color fringes |
| `frosted-glass` | Strong lens refraction with blur across the whole pane |
| `clear-edge` | Frosted center with a clear, strongly refractive border; default |

Every preset has dark and light variants. Dark uses deeper face shading and
softer rim highlights; light lifts the face and highlights. Blur, refraction,
and edge geometry stay the same. Select a variant with `default_theme`, a window
theme tag, or the Noctalia panel's Theme selector. **Follow Noctalia** tracks the
shell's dark/light mode. Explicit panel slider overrides take precedence over the variant; reset
an overridden field to inherit its theme value again.

Older names remain accepted in configurations, window tags, and saved profiles:
`default`/`measured` map to `frosted`, `platform` to `soft-glass`, and `lens` to
`clear-glass`. The retired `lighter`, `darker`, `ultra_darker`, and `no_bleed`
variants now select `soft-glass`. `frosted-full` maps to `frosted-glass`, and
`frosted-clear-edge` maps to `clear-edge`.
An alias explicitly customized with `glass.preset(...)` remains a separate,
visible user preset.

Distances and blur sigma use logical pixels and scale with the output;
`effect_scale` adjusts their overall size. The shape follows the actual window's
size, corner radius, and rounding power, including per-window rules. Set corner
curvature through Hyprland's `rounding_power`; the material has no independent
shape override.

Glass can reflect neighbouring windows, including overlapping floating windows.
Reflections follow window movement and stacking order. Native shadows and borders
remain visible.

`neighbor_contribution` controls how strongly neighbouring windows appear in
reflections. Its default, `1`, gives them full strength; `0` removes their
contribution outside the receiving window. Intermediate values soften their
presence without changing the pane's frost or opacity. Windows directly beneath
the glass remain visible according to their normal stacking order.

`neighbor_falloff` (Reflection falloff in the control panel) softens detail into
frost toward the center of the pane. `0` uses the preset's usual blur transition;
higher values, up to the default of `1`, soften detail faster. It also affects
wallpaper and windows underneath, so it works with a single window.
`blur_sigma` sets the final softness; with zero blur, falloff has no visual effect.
Neither neighbour control applies to layer surfaces.

Common settings:

| Setting | Default | Purpose |
| --- | --- | --- |
| `enabled` | `true` | Apply glass to windows |
| `manage_window_blur` | `true` | Suppress Hyprland's separate blur on managed windows |
| `default_preset` | `clear-edge` | Material preset |
| `glass_opacity` | `1` | Mix the material with the original backdrop |
| `effect_scale` | `1` | Scale the material's pixel distances |
| `neighbor_contribution` | `1` | Contribution of adjacent neighbours, from `0` to `1` |
| `neighbor_falloff` | `1` | Transfer reflection detail into frost, from `0` (off) to `1` (fast) |
| `default_theme` | `dark` | Select `dark` or `light` overrides |
| `layers.enabled` | `false` | Enable optional layer-surface glass |

Window tags are `hyprliquid_enabled`, `hyprliquid_disabled`,
`hyprliquid_preset_NAME`, and `hyprliquid_theme_dark`/`hyprliquid_theme_light`.
The disabled tag wins. Preset values resolve through the selected theme variant,
shared preset, inherited preset, theme configuration, and global configuration.
To change a field supplied by a preset, override it with `glass.preset(...)`.
Shared user values also override built-in dark/light defaults; an explicit
`dark = { ... }` or `light = { ... }` customization wins for that theme.

Hyprliquid enables `decoration.shadow.enabled` when loaded or when the
configuration is reloaded.

Global values can be changed live, for example:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "clear-glass" })'
hyprctl eval 'hl.plugin.hyprliquid.config({ enabled = false })'
hyprctl configerrors
```

Layer surfaces can be selected with `glass.layer("noctalia-panel", { preset =
"soft-glass" })` after enabling `layers`. The masking modes are `auto`,
`alpha`, and `region`; `auto` uses a client's background-effect region when
available and otherwise uses its alpha. Layer glass uses rectangular outlines,
so rims may not follow custom-shaped panels.
Layer glass is suspended while the session is locked, including layers allowed
above the lock screen. It refreshes after unlocking.

## Live control panels

The [Noctalia control panel](https://github.com/polyv0x/hyprliquid-cpanel) lets you
choose presets, switch themes, adjust materials, and save profiles without
editing your configuration. Saved panel settings override the plugin defaults;
use **Reset to configuration** to return to your Hyprland configuration and
plugin defaults.

## Display compatibility

The materials support **SDR** rendering. HDR color management is not calibrated.
