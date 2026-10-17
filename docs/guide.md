# Hyprliquid guide

A Hyprland plugin with native glass materials written in GLSL ES 3.00.
It captures the desktop behind each window, retains a sharp copy, produces a
Gaussian blur, and draws the glass before Hyprland composites the application's
pixels. Transparent window backgrounds reveal the effect; opaque text and
application content stay sharp.

The blur filters the background through progressively smaller images before
applying its Gaussian kernel, then reconstructs the result through those same
levels. Increasing `blur_sigma` or `effect_scale` therefore keeps the samples
close together instead of exposing a grid of widely spaced samples. Windows
and layer surfaces use the same blur pipeline.

The compositor integration supports window selection, fades, fullscreen handling,
per-window presets, and optional layer surfaces.

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
    glass.config({ default_preset = "platform" })
    glass.preset("my_glass", {
        inherits = "platform",
        blur_sigma = 12,
        inner_refraction_amount = -10,
        rim_opacity = 0.6,
    })
end
hl.window_rule({ name = "terminal-glass", match = { class = "^kitty$" },
                 tag = "+hyprliquid_preset_my_glass" })
```

The default is `measured`, a frosted material with no added rim, aberration, or
refraction. Select `platform` or `lens` for stronger refraction and highlights.

| Preset | Appearance |
| --- | --- |
| `measured` | Soft frosted blur |
| `platform` | Refraction with rim highlights |
| `lighter` | Brighter face and rim |
| `darker` | Lower face opacity and subdued highlights |
| `ultra_darker` | Lower face opacity with stronger edge darkening |
| `lens` | Strong refraction and chromatic aberration |
| `no_bleed` | Platform glass without edge bleed |

All scalar material controls, defaults, and bounds are listed in
[`src/MaterialParams.inc`](../src/MaterialParams.inc).
Distances and blur sigma use logical pixels and scale with the output;
`effect_scale` adjusts their overall size. The shape follows the actual window's
size, corner radius, and rounding power, including per-window rules. Set corner
curvature through Hyprland's `rounding_power`; the material has no independent
shape override.

Refraction and edge bleed use a smoothed optical direction inside the glass.
The sampling footprint grows with depth, blending adjacent edge directions where
a small corner radius would otherwise produce a sharp diagonal seam. The native
outline, coverage, material depth and rim lighting still follow the window's
exact shape. This happens automatically and does not change saved profiles.

Window glass captures its backdrop before the window's own native shadow is
drawn. This prevents strong refraction, especially `lens`, from pulling the
shadow's clipped edge into the pane as an inset outline. Native shadows and
borders still render normally after the glass.

Adjacent, non-overlapping tiled windows refract a completed scene of their
neighbours from the same frame. Focusing a different tile no longer removes it
from another pane's refraction. The capture includes each pane's base glass and
client content; it does not recursively capture reflections of reflections.
Sampling inside a window still uses its real backdrop, avoiding self-refraction.
Floating windows, overlapping tiles and transformed windows retain native
stacking behavior. Popups and native borders/shadows are not duplicated into the
neighbour capture.

This adds one offscreen tiled-window pass and two reusable monitor-sized textures
when at least two tiles and one glass pane are visible. Those frames repaint the
monitor to keep the captured background free of old window pixels. It does not
schedule extra frames on an idle desktop. Single-window and disabled-glass scenes
skip the capture.

Native controls:

| Setting | Default | Purpose |
| --- | --- | --- |
| `enabled` | `true` | Apply glass to windows |
| `manage_window_blur` | `true` | Suppress Hyprland's separate blur on managed windows |
| `default_preset` | `measured` | Material preset |
| `glass_opacity` | `1` | Mix the material with the original backdrop |
| `effect_scale` | `1` | Scale the material's pixel distances |
| `default_theme` | `dark` | Select `dark` or `light` overrides |
| `layers.enabled` | `false` | Enable optional layer-surface glass |

Window tags are `hyprliquid_enabled`, `hyprliquid_disabled`,
`hyprliquid_preset_NAME`, and `hyprliquid_theme_dark`/`hyprliquid_theme_light`.
The disabled tag wins. Preset values resolve through the selected theme variant,
shared preset, inherited preset, theme configuration, and global configuration.
To change a field supplied by a preset, override it with `glass.preset(...)`.
Signed values, including negative inner refraction and coverage offsets, are
preserved throughout that chain.

The window integration requires Hyprland's shadow pass and enables
`decoration.shadow.enabled` when loaded or when the configuration is reloaded.

Global values can be changed live, for example:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "lens" })'
hyprctl eval 'hl.plugin.hyprliquid.config({ enabled = false })'
hyprctl configerrors
```

Layer surfaces can be selected with `glass.layer("noctalia-panel", { preset =
"platform" })` after enabling `layers`. The masking modes are `auto`,
`alpha`, and `region`; `auto` uses a client's background-effect region when
available and otherwise uses its alpha. This remains an analytic rectangular
material: it does not reconstruct an arbitrary layer's outline for the rim.

## Validation

```sh
make test
python3 scripts/test-nested.py
```

`make test` needs Mesa surfaceless EGL and GLES3. It compiles both the reference
fixtures and the native shaders. With the original outline-based optics selected
in the test harness, it compares all seven materials at 1×, 1.5× and 2× scale and
rounding powers from 1 to 10. The production optical path is checked for corner
continuity at 0px and 12px radii, including fractional scaling, and for unchanged
straight edges, coverage and rim lighting. Blur and fade-to-zero are also checked.
The reference fixtures are retained in `tests/reference`. Rendered comparisons
are written to `build/*-native.ppm`, `build/*-webgl.ppm` and `build/corners-*.ppm`.

The integration test needs a running Wayland desktop, Hyprland, Kitty, and grim.
It opens a disposable nested compositor with a separate configuration and IPC
socket, renders transparent windows, switches presets, exercises fullscreen and
screen-edge sampling, and unloads/reloads the plugin. It also compares adjacent
panes across focus changes, checks that neighbour repaints reach earlier-drawn
panes at native/fractional scale and on a rotated output, and closes a neighbour
to check capture cleanup. Set `HYPRLIQUID_TEST_PLUGIN` to an alternative library
path to run the same regression against a previous build. Screenshots and logs go in
`build/`. It does not install the plugin into the parent compositor.

The materials are validated for **SDR** rendering. HDR color management is not
calibrated.

## Demo captures

Run `make demo` from a Wayland session. In addition to the build dependencies,
this needs Mesa EGL/GLES3, PySide6, Hyprland, and grim.

`demo/backdrop.frag` renders the colored grid and rounded primitives through
native EGL. The second scene replaces the cylinder and octahedron with a sphere
and cone, with a third variation using darker studio lighting for dark-mode
examples. `demo/window.py` supplies an opaque backdrop and transparent windows
with sharp text. A separate Hyprland compositor loads the plugin and draws the
glass on a 1920×1200 virtual output; grim captures the actual compositor output.
The foreground application does not paint or simulate the glass.

The eight PNGs in `screenshots/` cover `platform`, `measured`, `lens`, the classic
primitive scene, and a three-window comparison, plus `darker`, `ultra_darker`,
and a dark-mode comparison of three materials. Captures are deliberately
excluded from Git. Upload the selected PNGs separately and embed the hosted URLs
in the README. Scene renders and diagnostic logs stay under `build/demo/`.
## Live control panels

The [runtime control API](control-api.md) supplies versioned JSON metadata,
atomic material overrides, resolved state, and reset through `hyprctl`.
It is intended for companions such as the Noctalia Hyprliquid panel.
