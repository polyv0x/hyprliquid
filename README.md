# Hyprliquid

Native glass effects for transparent Hyprland windows, written in GLSL ES 3.00.
Blurs and refracts the backdrop while keeping application text sharp, with five
legacy presets, an opt-in Liquid material, and optional layer-surface support.

The companion [Noctalia control panel](https://github.com/polyv0x/hyprliquid-cpanel)
lets you switch presets, adjust materials live, follow the shell theme, and save
custom profiles.

https://github.com/user-attachments/assets/5d078786-1cad-4b21-af75-1c751e7cec3f

[Examples](#examples)

## Installation

Supports **Hyprland 0.56.2 and 0.57.2** from the same source. Build the plugin
against the exact running Hyprland build and library ABI; matching the version
number alone is not enough.
Rebuild after updating Hyprland. Each installation needs its own matching binary.
Other Hyprland versions are not currently tested. See the
[compatibility checks and adapter design](docs/compatibility.md).

Build requirements: Git, a C++23 compiler compatible with your Hyprland build,
GNU Make, Python 3, pkg-config, and development files for Hyprland, pixman,
libdrm, json-c, and Hyprland's dependencies. Install Rust through rustup;
`rust-toolchain.toml` pins the tested stable toolchain (1.97.1). Make invokes
Cargo with the checked-in lockfile and links the Rust core into the same
`hyprliquid.so`. The first build may download the pinned toolchain and crates.
See [the Rust core architecture and checks](docs/rust-core.md).

Unload any other backdrop-effect plugins, including Hyprglass, before loading
Hyprliquid. They can interfere with the same rendering path. Disable their
startup loading too if you are switching to Hyprliquid.

### hyprpm (recommended)

Install `hyprpm` and its build dependencies through your distribution; see the
[Hyprland plugin guide](https://wiki.hypr.land/Plugins/Using-Plugins/).
It fetches the matching Hyprland headers and builds the plugin for you:

```sh
hyprpm add https://github.com/polyv0x/hyprliquid
hyprpm enable hyprliquid
hyprpm reload
```

To load enabled plugins at startup, add this to `~/.config/hypr/hyprland.lua`:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("hyprpm reload")
end)
```

For a legacy `hyprland.conf`, use `exec-once = hyprpm reload` instead.
Keep the guarded plugin configuration from the [example](examples/hyprliquid.lua)
in your config; Hyprliquid reloads it after loading.

Update and rebuild plugins after a Hyprland update, then reload them in the new
Hyprland session:

```sh
hyprpm update
hyprpm reload
```

### Manual build

Install development headers matching your running compositor. Check that
`pkg-config --cflags hyprland pixman-1 libdrm json-c` finds them, then:

```sh
git clone https://github.com/polyv0x/hyprliquid.git
cd hyprliquid
make -j4
hyprctl plugin load "$(pwd)/hyprliquid.so"
```

This creates `hyprliquid.so` with the shaders embedded; no external shader files
are needed. Manual loading lasts for the current session. See the
[loading guide](docs/guide.md#load) for startup loading and unloading.
After updating Hyprland's headers, run `make -j4` again before loading into the
new session. The build detects compiler/header ABI changes and rebuilds the C++
adapter; system-header dependencies are tracked as well.

For distribution, `make release` creates a separate
`build/release/<source-sha256>/` directory containing a stripped `hyprliquid.so`,
its matching `hyprliquid.so.debug`, and a checksum manifest. This requires GNU
`objcopy` (binutils). Distribute the `.so` and retain the debug file for that
exact build; placing them together lets debuggers find the symbols through
`.gnu_debuglink`. Development and hyprpm builds remain unchanged. Packaging
works on a copy and publishes the complete directory atomically, so it can run
while the original library is loaded. Older release directories are retained.

## Try it

Check that the plugin is loaded with `hyprctl plugin list`, then open an
application with a transparent background to see the glass effect.
The default `frosted-glass` preset uses dark tones, strong lens refraction, and
blur across the whole pane. `frosted-glass-clear-edge`, the second preset, keeps
a clear refractive border around a frosted center. Reflection falloff defaults
to `1`, softening detail into the frost. For an even frost without refraction or
rim highlights:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "frosted" })'
```

See the [configuration example](examples/hyprliquid.lua) and [guide](docs/guide.md) for
presets, window rules, and layer surfaces. SDR is supported;
HDR color management is not calibrated.

To select the new Liquid material with its own refraction, key/fill highlights,
and outer shadow:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "frosted-liquid" })'
```

`frosted-liquid` uses blur sigma 19; `frosted-liquid-medium` preserves the original
Liquid settings with sigma 6. See the
[Liquid material contract](docs/guide.md#liquid-material) for controls, theme
mapping, and rendering behavior.

`display_angle_degrees` rotates the edge highlights of both Frosted Glass
presets and the Liquid presets (default `0`). Both Frosted Glass presets use
Liquid's key/fill rim with width and strength set to `1`. The
optional `animate_display_angle = true` setting sweeps 0° → configured angle →
0° during native window size animations, following the window's easing. See the
[Liquid configuration example](docs/guide.md#liquid-material).

## Examples

Browse the [screenshots](screenshots/antialiased/) for all seven built-in
presets, including the two Liquid variants and side-by-side comparisons.

`make demo` renders the ten example scenes in a nested Hyprland session.
Run it from an active Wayland session with PySide6 and `grim` installed.
Captures render at 2× pixel density and resolve to 1920×1200 for smoother
window borders. Use `python3 scripts/capture-demo.py --supersample 1` for
native-resolution captures. To regenerate the gallery:

```sh
make -j4 hyprliquid.so build/demo-scene
python3 scripts/capture-demo.py --output screenshots/antialiased
```

## Tests

From a source checkout:

```sh
make test
```

Tests require Mesa EGL/GLES3 and Lua development files. They check native safety
boundaries and compare the shaders against reference fixtures at three output
scales. Run `python3 scripts/test-nested.py` for compositor integration tests.
Run `make test-release` to verify release code/data, exports, unwind information,
matching debug symbols, and safe publication without modifying the input plugin.
For isolated screenshot comparisons, set `HYPRLIQUID_TEST_OUTPUT` to a new
directory when running `scripts/test-nested.py`. It must not already exist.
`HYPRLIQUID_TEST_PLUGIN` selects a saved comparison library; compiled test
fixtures continue to come from `build/`.

## License

GNU General Public License, version 3 or (at your option) any later version
(`GPL-3.0-or-later`); see [LICENSE](LICENSE). The original BSD 3-Clause notice
for inherited code is preserved in [LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt).
