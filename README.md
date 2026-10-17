# Hyprliquid

Native glass effects for transparent Hyprland windows, written in GLSL ES 3.00.
Blurs and refracts the backdrop while keeping application text sharp, with seven
presets and optional layer-surface support.

Targets **Hyprland 0.56.2** and requires matching development headers, a C++23
compiler, Make, Python 3, and pkg-config. Rebuild after updating Hyprland.

## Build

```sh
make -j4
make test
```

Tests require Mesa EGL/GLES3 and compare the native shaders against reference
fixtures at three output scales. Shaders are embedded in `hyprliquid.so`.

## Try it

Unload any other backdrop-effect plugins before loading:

```sh
hyprctl plugin load /absolute/path/to/hyprliquid/hyprliquid.so
```

The default `measured` preset gives a soft frosted effect. For stronger refraction:

```sh
hyprctl eval 'hl.plugin.hyprliquid.config({ default_preset = "platform" })'
```

Manual loading applies to the current session. See the
[configuration example](examples/hyprliquid.lua) and [guide](docs/guide.md) for
presets, window rules, layer surfaces, and integration tests. SDR is validated;
HDR color management is not calibrated.

## License

BSD 3-Clause; see [LICENSE](LICENSE).
