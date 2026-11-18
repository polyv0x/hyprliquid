# Hyprland compatibility

The same source supports Hyprland 0.56.2 and 0.57.2. Build a separate plugin for
each compositor's exact build and dependency ABI. The load-time ABI check stays
enabled; header fallbacks do not make a compiled `.so` portable across versions.

## Adapter boundaries

`src/HyprlandCompat.hpp` selects moved Window and Workspace headers and wraps
window state/presentation access. Header probes use `src/...` through the
Hyprland include directory supplied by pkg-config. Probing `hyprland/src/...`
could accidentally find newer system headers when hyprpm selected an older
header tree. This matters when upgrading packages before restarting Hyprland.
It also observes client damage before 0.57 consumes GPU-backed buffer damage,
restoring both pending regions for the native renderer. Window and layer caches
then use the same commit snapshot, including updates from DMABUF clients.

`src/HyprlandRenderCompat.hpp` adapts render contexts and virtual callbacks.
Context bindings are scoped and restored across nested draws. Queued window
passes retain their presentation, including workspace offsets and fade alpha.
Crop, damage, scissor and copy coordinates follow the active target's
`transformDamage` flag: 0.56 normally renders in physical output orientation,
whereas 0.57 rotates the monitor-oriented scene during its final output transfer.
Renderer hooks use the matching signatures, including the scene-mode overload
of `renderWorkspaceWindows` on 0.57. Lookup requires the complete mangled
signature, so an unfamiliar overload cannot be called with the wrong arguments.
Capture/workspace scenes retain the native
visibility rules. Isolated workspace captures use native shell rendering so
desktop layer caches cannot expose excluded windows. Ordinary monitor screenshots
still include layer glass. Layer injection also honors the compositor's content checks.

`src/HyprlandIPCCompat.hpp` adapts both generations of command registration.
Both match the full command by prefix; handlers validate the command's grammar.

Finished-input and regional cache reuse are available on both versions.
`src/NativeDecorationState.cpp` reads 0.57's public resolved gradient state,
including fade progress and animated angles. It discovers each native gradient
through a scoped `updateState()`/`setTarget()` hook that leaves the target and
animation unchanged, and ties its lifetime to a weak decoration reference.
Native geometry, eligibility rules and appearance settings also enter the cache
key on both versions. The 0.56 path reads resolved gradients directly from its
public window state. Native settings and rules also invalidate that older path,
including dynamic shadow shape changes that otherwise leave stale neighbor inputs.

Only the exact native border, shadow and glow classes are inspected. If the
matching hook or expected native contract is unavailable, dependent reuse stays
disabled for that scene; wallpaper and base-glass caching remain available.

Header layout, render-context support, and IPC support are detected separately.
Future versions retaining these interfaces can use the adapters, but new internal
API or ABI changes still require validation before declaring support.

## Rebuilding after an upgrade

Run `make -j4` with pkg-config selecting headers for the intended compositor.
The build stamp fingerprints compiler arguments and preprocessor definitions
from those headers, including the Hyprland/dependency version definitions. It
changes only when that fingerprint changes. Dependency files also track system
headers, so ordinary header edits invalidate affected objects. A no-op build
preserves the stamp and compiled objects.

`hyprpm update` builds its managed copy. It does not replace a binary in another
checkout named by `hl.plugin.load(...)`. Use either the hyprpm startup reload or
the manual checkout's load path consistently.

## Verify both supported versions

Use separate runtime roots with each Hyprland binary, development headers, and
matching libraries. Extracting distribution packages into a directory works;
installing or downgrading the host compositor is unnecessary. The matrix runner
rebases private copies of pkg-config metadata and creates fresh build trees.

```sh
python3 scripts/test-hyprland-compat.py \
    --target 0.56.2=/path/to/hyprland-056-root \
    --target 0.57.2=/ \
    --output /tmp/hyprliquid-compat-results
```

The output directory must be new. Each target runs the existing unit/shader
tests, verifies a no-op rebuild, and starts disposable compositors with private
configuration, runtime directories, and IPC sockets. It checks both window and
layer material families, theme round trips, fullscreen/resize, runtime controls,
and unloading/reloading with live, hidden, and unmapped windows. Logs and
screenshots are retained under the output directory. It never loads the plugin
into the user's running desktop.

For a focused check using an already configured build environment:

```sh
make -j4 all test build/layer-test build/decoration-lifecycle-test.so
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-render-check \
    python3 scripts/test-nested.py --compatibility-only
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-lifecycle-check \
    python3 scripts/test-nested.py --decoration-lifecycle-only
```

The optional capture/security fixtures also use the adapter interfaces. Presented
frame captures retain damage metadata while a render context is live and read
the saved framebuffer after presentation, when 0.57 no longer supplies a context.

For GPU allocation/GL-state checks and native presented-buffer comparisons:

```sh
make -j4 build/security-test.so build/capture-test.so build/layer-test build/subsurface-test
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-gpu-security python3 scripts/test-nested.py --security-only
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-window-close python3 scripts/test-nested.py --window-close-only
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-tiny-shadows python3 scripts/test-nested.py --tiny-shadows-only
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-presented-damage python3 scripts/test-nested.py --damage-flicker-only
```

The damage replay compares partial redraws with full rebuilds byte for byte,
including reflections of changing subsurfaces in neighboring panes. Optical
damage propagation remains necessary even when finished-input caching is disabled.
On both versions, also run `--surface-cache-only` and `--regional-damage-only`
to check native 8-bit, 10-bit and FP16 captures, rotation, fractional scaling,
partial repairs and cache reuse. The foreground-only checks require zero new
captures, blurs or material draws after warming the cache.

`--native-style-only` freezes native animation ticks and records the actual
decoration values used for drawing. It compares partial presented frames,
full repaints and forced cache rebuilds at identical phases, covering focus
fades, angle loops, instant targets, native settings/rules and plugin reloads.
For angle changes, the exact cache oracle compares the full repaint with the
forced rebuild. First-update differences are recorded separately: the variation
also occurs on 0.57.2 with Hyprliquid fully unloaded. Older-host controls and
dithering checks did not establish its precise cause. Native rendering, the
shared driver and the capture observer remain possible contributors. The
ordinary damage suites retain their exact partial-frame assertions.
The animation hooks exist only in `capture-test.so` and are installed only when
that test requests them.

Hyprland 0.57 additionally exposes native workspace capture. This check uses
the actual Wayland workspace source in `everything` mode, including shell layers:

```sh
make build/workspace-capture-test build/capture-test.so build/layer-test build/subsurface-test
HYPRLIQUID_TEST_OUTPUT=/tmp/hlq-workspace-capture python3 scripts/test-nested.py --workspace-capture-only
```

It warms window and layer caches, changes a protected window beneath both,
checks that workspace pixels remain unchanged, and compares against rebuilt
window glass and native layer rendering. Visible window/layer markers and desktop restoration prevent an empty capture from
passing. The desktop reference comes from the internal framebuffer fixture:
ordinary output screenshots also mask protected windows. The temporary compositor
grants screencopy access only to the workspace fixture's
exact executable path. The client needs `wayland-scanner`, Wayland client headers,
and the installed `wayland-protocols`/`hyprland-protocols` XMLs. Hyprland 0.56 does
not implement this workspace-capture protocol.
