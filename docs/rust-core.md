# Rust core boundary

The plugin uses a Rust core for pure material and numerical geometry
calculations: Liquid defaults/effective values, scalar
sanitization, blur sigma, output extents, sampling padding, dimensions, sample
rectangles, blit coordinates, and nominal buffer byte counts.

`hyprliquid-core` forbids unsafe code and has no dependencies on Hyprland, Lua,
OpenGL, CXX, or other crates. Its build script uses Python 3 to generate scalar
descriptors from `src/MaterialParams.inc` and structured material constants from
`materials/liquid.json`. These are the same inputs used by C++. Base material
values remain doubles; scalar overrides retain their float precision. The
frozen equations under `tests/reference/` are used only by comparison tests.

`hyprliquid-bridge` generates CXX value types and field conversions from those
inputs. `src/RustCore.cpp` adapts the results to the existing C++ structures.
Each exported Rust function catches unwinding panics and returns an explicit
status: success, invalid geometry, or internal failure. No CXX `Result` or C++
exception translation is used. The adapter rejects invalid geometry, reports
internal failures once, and returns disabled or empty results. A panic payload
is deliberately forgotten on that exceptional path because its destructor
could itself panic. Process aborts and allocation failures that abort remain
outside this recovery boundary.

All core operations are stateless and use copied fixed-size values. Calls require
no persistent core instance, per-draw allocation, JSON conversion, or callback
into the host. Results replace caller values only
after a Rust call returns. No Hyprland layout, Lua state, GL object, or borrowed
host container crosses the bridge. The existing C++ protected Lua boundary and
resource teardown order remain responsible for their host lifetimes.

Blit mapping now returns an optional coordinate: zero source spans and results
outside the signed integer range are rejected. Intermediate products use a
wider integer so full-range endpoints do not overflow. Byte counts check both
multiplications. The adapter obtains format metadata from Hyprland and passes
only block dimensions and bytes per block to Rust.

Configuration parsing, preset inheritance/applicability, runtime profiles,
revisions, JSON metadata, and GL ownership remain in C++. Hyprland, Lua, and
OpenGL objects stay on that side of the bridge.

## Build and checks

The existing `make` and hyprpm entry points produce one `hyprliquid.so`. Cargo
uses locked dependencies, Rust 1.97.1, position-independent code, and unwinding.
Make forwards the adapter's compiler and C++ flags to the generated bridge;
GNU unique symbols remain disabled for unload compatibility. JSON and scalar
inputs have both Make prerequisites and Cargo rerun declarations. The embedded
Rust/CXX archive symbols are private to the plugin to avoid interposing on
other in-process Rust plugins. No runtime material files are required.

```sh
cargo fmt --all -- --check
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
make -j4 all test build/security-test.so
python3 scripts/test-rust-build.py
python3 scripts/test-nested.py
```

`make test` retains the existing render/security checks and adds exact
comparisons against the frozen C++ implementations: 40,000 material
combinations, individual controls and nonfinite inputs, plus 100,000 geometry
cases. A separate test archive enables an intentional panic entry point and
exercises its status through CXX; that entry point is absent from the normal
plugin. Rust tests cover precision, disabled rays, repeated blur stops,
sanitization, fractional geometry, full-range integer mapping, byte overflow,
and panic recovery without a partial result.

`test-rust-build.py` performs a clean build in a temporary source copy, then
changes the scalar table, material JSON, and bridge declarations and checks
that regenerated values reach the C++ caller. `--quick` skips the initial full
plugin/render build. The nested suite loads the plugin only in its disposable
compositor and covers API compatibility, rendering, allocation diagnostics,
and unload/reload.
