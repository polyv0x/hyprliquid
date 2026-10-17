# Hyprliquid

Native glass effects for transparent Hyprland windows, written in GLSL ES 3.00.
Blurs and refracts the backdrop while keeping application text sharp, with seven
presets and optional layer-surface support.

## Installation

Targets **Hyprland 0.56.2**. The plugin must be built against the exact running
Hyprland build and library ABI; matching the version number alone is not enough.
Rebuild after updating Hyprland. Other Hyprland versions are not currently
supported.

Build requirements: Git, a C++23 compiler compatible with your Hyprland build,
GNU Make, Python 3, pkg-config, and development files for Hyprland, pixman,
libdrm, json-c, and Hyprland's dependencies.

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
After updating Hyprland's headers, use `make -B -j4` to rebuild all objects before
loading into the new session.

## Try it

Check that the plugin is loaded with `hyprctl plugin list`, then open an
application with a transparent background to see the glass effect.
The default `measured` preset gives a soft frosted effect. For stronger refraction:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "platform" })'
```

See the [configuration example](examples/hyprliquid.lua) and [guide](docs/guide.md) for
presets, window rules, layer surfaces, and integration tests. SDR is validated;
HDR color management is not calibrated.

## Tests

From a source checkout:

```sh
make test
```

Tests require Mesa EGL/GLES3 and compare the native shaders against reference
fixtures at three output scales.

## License

BSD 3-Clause; see [LICENSE](LICENSE).
