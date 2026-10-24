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
    glass.config({ default_preset = "frosted-glass" })
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

The default is `frosted-glass` in dark mode, with strong lens refraction, blur
across the whole pane, and reflection falloff set to `1`. The preset list starts
with `frosted-glass`, followed by `frosted-glass-clear-edge`; remaining entries
are alphabetical. Select `frosted` for an even blur with no added rim,
aberration, or refraction.

| Preset | Appearance |
| --- | --- |
| `frosted-glass` | Strong lens refraction with blur across the whole pane; default |
| `frosted-glass-clear-edge` | Frosted center with a clear, strongly refractive border |
| `clear-glass` | Sharp backdrop with strong refraction and color fringes |
| `frosted` | Soft, even blur without added refraction or rim lighting |
| `frosted-liquid` | Liquid refraction, five-stop frost at blur sigma 19, directional key/fill highlights and a material shadow |
| `frosted-liquid-medium` | The same Liquid material with blur sigma 6; preserves the original Liquid medium appearance |
| `soft-glass` | Gentle refraction, soft blur, and rim highlights |

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
`clear-edge` and `frosted-clear-edge` map to `frosted-glass-clear-edge`.
`liquid-medium` follows the rename to
`frosted-liquid` (sigma 19); select `frosted-liquid-medium` to retain sigma 6.
An alias explicitly customized with `glass.preset(...)` remains a separate,
visible user preset.

Distances and blur sigma use logical pixels and scale with the output;
`effect_scale` adjusts their overall size. The shape follows the actual window's
size, corner radius, and rounding power, including per-window rules. Set corner
curvature through Hyprland's `rounding_power`; the material has no independent
shape override.

Glass can reflect neighbouring windows, including overlapping floating windows.
Reflections follow window movement and stacking order. Borders remain native.
The Liquid family owns its outer shadow; the other presets retain native shadows.

`neighbor_contribution` controls how strongly neighbouring windows appear in
reflections. Its default, `1`, gives them full strength; `0` removes their
contribution outside the receiving window. Intermediate values soften their
presence without changing the pane's frost or opacity. Windows directly beneath
the glass remain visible according to their normal stacking order.

`neighbor_falloff` (Reflection falloff in the control panel) softens detail into
frost toward the center of the pane. `0` uses the preset's usual blur transition;
higher values, up to the default of `1`, soften detail faster. It also affects
wallpaper and windows underneath, so it works with a single window.
For the legacy presets, `blur_sigma` sets the final softness; with zero blur,
falloff has no visual effect. Layers have no neighboring-window contribution;
Liquid layers can use falloff as a local detail control.

### Liquid material

`frosted-liquid` and `frosted-liquid-medium` select the `Liquid` material family.
They differ only in blur sigma: 19 and 6, respectively. Custom presets inherit
the family along with their parent values:

```lua
glass.preset("my_liquid", {
    inherits = "frosted-liquid",
    glass_opacity = 0.9,
    effect_scale = 1.0,
    neighbor_falloff = 0.0,
})
glass.config({ default_preset = "my_liquid", default_theme = "dark" })
```

`frosted-liquid-medium` retains the original Liquid appearance with no overrides.
Both presets expose 21 scalar controls. All existing blur and refraction
controls apply to Liquid:

| Controls | Liquid defaults | Meaning |
| --- | --- | --- |
| `blur_sigma` | 19; 6 for `frosted-liquid-medium` | Gaussian blur sigma; 0 disables blur while retaining refraction |
| `blur_alpha0` | 1 | Edge blur strength; scales the four near-edge stops, preserving their relative weights |
| `blur_alpha3` | 1 | Deep-interior blur strength |
| `blur_dist3` | 83 | Interior blur depth; scales the five signed distances by this value divided by 83 |
| `inner_refraction_amount`, `inner_refraction_inv_height` | -60, 0.05 | Signed inner displacement and inverse height; the default height is 20 |
| `outer_refraction_amount`, `outer_refraction_inv_height` | 0, 0.05 | Signed outer displacement and inverse height; amount 0 disables this ray |
| `refraction_threshold0`, `refraction_threshold1` | 0, 1 | Near/far interior depths for the outer-ray blend; become signed distances `[-threshold1, -threshold0]` |
| `refraction_opacity` | 1 | Refraction strength: multiplies both displacement amounts, leaving face color and blur intact |
| `face_opacity` | 1 | Strength of the material's face color treatment |
| `rim_width`, `rim_opacity` | 1, 1 | Key/fill highlight band height and color/alpha strength |
| `display_angle_degrees` | 0 | Rotate Liquid's key/fill highlights, from -180° to 180° |
| `shadow_amount` | 0.05 dark, 0.15 light | Opacity of the local outer shadow |
| `shadow_offset` | 8 | Vertical shadow offset in logical units, positive downward |
| `glass_opacity`, `effect_scale` | 1, 1 | Overall opacity and material distance scale |
| `neighbor_contribution`, `neighbor_falloff` | 1, 0 | Neighbor strength and reflection falloff |

These mappings adapt the existing sliders to Liquid's equations. Changing an
outer height has no visible effect while its amount is zero. Resetting controls
removes their overrides and restores the selected preset; the generic parameter
defaults are not Liquid's defaults. Saved overrides for newly supported controls
now affect the image. Reselect the desired preset in the panel to clear those edits.

The optional global boolean `animate_display_angle` defaults to `false`. When
false, Liquid uses the configured angle continuously. When true, window size
transitions sweep from 0° through the configured angle and back to 0°, leaving
idle windows at 0°. The sweep uses the window's actual interpolated size, so it
follows Hyprland's duration and Bézier or spring curve without a separate timer.
Interrupted resizes continue from the last rendered angle. Spring overshoot is
bounded to the configured angle; disabling native animations completes the
sweep immediately. Pure moves and workspace slides do not start a sweep.
Layer surfaces always use their static material angle.

```lua
glass.preset("tilting-liquid", {
    inherits = "frosted-liquid",
    display_angle_degrees = 30,
})
glass.config({ default_preset = "tilting-liquid", animate_display_angle = true })
```

The toggle is global for Liquid windows, including explicitly tagged presets;
each window uses its own resolved angle as the sweep's peak. The default 0°
angle and disabled toggle preserve existing rendering.

The remaining legacy color, bleed, aberration, coverage, rim-softness, and
shadow-height fields are inapplicable and retain their saved values. Clients
can read applicability and the effective material through the
[control API](control-api.md). The companion panel may require an update to hide
inapplicable sliders and label Liquid's refraction strength and highlights.

Dark and light select their respective material variants. The API names the
light variant's environment `default`. The window shape, rounding power, and
output scale come from Hyprland. `effect_scale` scales material distances once,
independently of texture density. Output transforms also rotate the lighting and
shadow coordinates.

The renderer retains the exact destination crop and a separate half-resolution
material backdrop, then filters a copy. Inner and outer refraction sample
separately with signed-distance blur weights. All material inputs compose
neighbors before reduction. Reduced neighbor contribution protects real
underlying windows on both refraction rays. Exterior shadows only darken the
exact destination at each pixel; they never displace or copy neighboring content
outside the glass.

For windows, the material owns the outer shadow and temporarily suppresses only
that window's native shadow. Switching family, disabling glass, and unloading
restore the previous native property. The output quad expands by 105 material
units while retaining the original window shape. Sampling uses conservative
padding of 268 pixels for `frosted-liquid`, or 209 for `frosted-liquid-medium`,
at scale 1, including a conservative sampling margin. Movement and teardown
damage the old and new bounds. Gaussian shadow tails are truncated at four radii.
Layers retain their surface shadows and masks; their rectangular SDF cannot make
highlights follow arbitrary rounded or disconnected surface alpha.

Adjacent panes occlude each other's exterior shadows regardless of focus draw
order. Overlapping floating windows keep native shadow occlusion order. The
auxiliary neighbor capture contains material bodies and content, without shadows.

The base variants are defined in [materials/liquid.json](../materials/liquid.json).
`scripts/embed-material.py` generates `LiquidMaterialData.hpp`; the constants,
material metadata, and shaders are embedded in the library. Runtime rendering
needs no external material files.

The material uses square-root refraction and directional key/fill highlights:

- Blur radius is Gaussian sigma. A variance-corrected prefilter and sharp/blur
  mix supply the five-stop blur transition. Repeated stops are right-continuous.
- Face color uses luma, range, saturation, and normal fill in the framebuffer's
  working color space.
- Highlight colors use a matrix with alpha-scaled bias and premultiplied
  source-over. Highlight response coefficients remain fixed at zero; display
  angle defaults to zero and rotates only key/fill lighting.
- Highlight normals are normalized native-shape derivatives. Refraction uses
  the existing stabilized optical normal to handle small corners and opposing
  edges. Shadow coverage uses a Gaussian with the material's radius, offset and
  opacity, applied as local darkening. Shadow displacement and color fields are
  inactive; the shadow never copies content outside the pane.
- Adaptive backdrop feedback, headroom conversion, and calibrated HDR remain
  outside this SDR implementation. Inactive and unset settings remain in the
  structured material data.

The frozen native baseline verifies pixel-identical `frosted-liquid-medium`
output in both themes across scales, corners, and fades; separate edits verify all 16 newly
supported controls change the image. The independent GLES reference covers the
body and highlights in both environments at scales 1, 1.5, and 2. Separate shadow checks verify local
darkening, offset and opacity at four scales, including 1.25. Analytic checks
isolate refraction, blur boundaries, color, highlights, and underlay rays;
legacy render comparisons retain the same numerical expectations.

Mesa software-rendered step-response measurements (10–90% transition width in
logical pixels) compare the filtered blur with a direct Gaussian:

| Output scale | Native filtered pyramid | Direct reference Gaussian |
| --- | --- | --- |
| 1 | 16.55 | 15.77 |
| 1.5 | 16.05 | 15.69 |
| 2 | 16.60 | 15.60 |

A disposable compositor audit of one 700×540 pane at scale 1 reported 5.71 MiB
of active plugin buffers for `frosted-glass-clear-edge` and 6.64 MiB for `frosted-liquid-medium`.
The Liquid half-sharp target was 559×479 (1,071,044 bytes in the tested 32-bit
format). Recent median CPU submission times were 0.202 ms and 0.266 ms,
respectively; these are observations on the test machine, not GPU timings or
a performance guarantee. Idle retained bytes depend on recent sizes and expire
through the existing pool. The runner writes details to
`build/liquid-buffer-audit.json` and captures representative images there.

Common settings:

| Setting | Default | Purpose |
| --- | --- | --- |
| `enabled` | `true` | Apply glass to windows |
| `manage_window_blur` | `true` | Suppress Hyprland's separate blur on managed windows |
| `animate_display_angle` | `false` | Sweep Liquid highlights during native window size transitions |
| `default_preset` | `frosted-glass` | Material preset |
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
