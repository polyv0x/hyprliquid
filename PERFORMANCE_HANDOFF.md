# Glass performance handoff — 2026-10-25

## Goal and constraints

Bring `frosted-glass`, `frosted-liquid-medium`, and `frosted-liquid` GPU usage close to native Hyprland blur, especially with many transparent windows and video on DP-1. **Production optimizations must preserve byte-identical output.**

Regional repair is now implemented: stable overlapping stacks update only the portions that can receive changed neighboring colors. The original Gaussian filters, texture dimensions, formats, projection, and blending order are preserved. Native-blur parity remains a measurement target, not an established result.

## Current state

- `hyprliquid.so` contains the regional implementation, unload-lifecycle and closing-snapshot fixes, plus native drop shadows for both Liquid presets. The latest build is installed in the current session; the compositor process and windows survived the reload. The experimental Kawase preview remains off.
- The working tree contains pre-existing user changes and this implementation. Preserve everything; do not reset it. The starting binary and source for regional work are `build/regional-damage-20261025/before.so` and `before-source.tar.gz`. Earlier snapshots remain in `build/moving-glass-20261025/`.
- Implemented optimizations include skipping inactive optical calculations, caching immutable backgrounds and the auxiliary tiled-glass base, caching an independent bottom pane’s output, reusing unchanged auxiliary scenes, and increasing retained-buffer budgets to prevent allocation churn.
- Historical many-window DP-1 measurements, before regional work: native blur median **34–35%**, glass **50%**, liquid-medium **58%**, liquid **59%** total GPU. Layout/workload changed during that investigation. These are not matched before/after results for the regional implementation.
- Kawase center A/B looked very similar to both user and assistant. Captured optical edge band had zero changed pixels; center differed. Filtering alone was ~11% faster. The live prototype computed both filters, so it established no end-to-end saving. This visual experiment was the only authorized exception to byte identity.

## Implemented regional damage through glass

Relevant code:

- `src/main.cpp`: the existing `hkDamageSurface` records client/subsurface damage.
- `src/DamageRegion.hpp`: bounded integer regions, conservative pyramid support, and forward/backward filter dependency mapping. Pixman merges spans; more than 32 rectangles become a bounding box, and 75% coverage becomes a full region.
- `src/WindowScene.cpp`: propagates changes through receivers in native painting order, expands native repaint damage before composition, and repairs a retained auxiliary scene. Repairs use a native-sized scratch image seeded from the immutable background/base, then copy only the affected core. Later clients cannot contaminate an earlier pane's backdrop.
- `src/GlassRenderer.cpp`: repairs capture, half-resolution Liquid material input, and the existing Gaussian pyramid. Clean-neighbor and Liquid filtering support partial updates. Backward dependency expansion reconstructs every required shared-scratch sample even after another pane overwrites scratch storage.
- `src/GlassDecoration.cpp`: retains logical material inputs and finished native pixels, intersects final draws with propagated damage, and preserves live Liquid antialiased boundaries. Liquid's live destination crop is separate from its retained filter input. Only the fully covered body reuses finished output. Native Hyprland decorations now own all window drop shadows.
- `src/RenderBuffers.cpp`: finished-image admission respects the existing 64 MiB budget instead of cycling allocations when a large stack cannot fit. Input-only and output-only pool entries are reported separately.

Cache validity and fallback:

1. Geometry, material, opacity, surface-tree, paint order, background changes, or missed scene serials rebuild retained inputs. Popup, group, unknown decoration, fadeout, and unsupported scene cases retain their existing fallback.
2. Native framebuffer pixels outside the renderer's damage may be undefined. Partial capture intersects native validity; a newly allocated input is reusable only after its complete owner was reconstructed.
3. The existing independent-bottom-pane optimization remains in use. Regional repair handles the remaining eligible overlapping receivers.
4. At 65% receiver-body coverage, use the original full update to avoid the extra destination crop and finished-image transfers. Full video behind a transparent center still requires updating that center.
5. Before retaining per-pane inputs, estimate their full/half-size textures, clean variants, live destinations, and auxiliary workspace against the 256 MiB / 32-entry input budget. Oversized stacks retain regional auxiliary repair and use shared full inputs. Live destination entries release unused filter textures left by earlier material draws.
6. `partial_captures`, `partial_blurs`, and new `partial_materials` diagnostics expose regional work. `auxiliary-repair` and `window-destination` identify temporary storage; normal idle retirement still applies.

The earlier failed per-tile cache was not reinstated. Retained results now come from the correctly reconstructed native destination after blending; Liquid's boundary is always rendered live.

## Validation and artifacts for this implementation

All paths below are under `build/regional-damage-20261025/` unless noted. The measured regional binary is saved as `candidate-v3.so`, with its source in `candidate-source.tar.gz`. The unload/closing correction is `candidate-v4.so`, with source in `candidate-v4-source.tar.gz`. The latest installed native-shadow correction is under `build/native-shadow-20261025/`, described below.

| Check | Result / artifact |
| --- | --- |
| `make -j4 test` | Passed; `unit-complete.log`, including 540 exact Glass and 288 exact Liquid shader comparisons |
| Partial Gaussian/clean/Liquid filters | 315 exact comparisons across three formats, five sigmas, and seven damage patterns; shared scratch deliberately overwritten; included in `legacy-complete.log` |
| Real partial client commits | `regional-complete.log`: 192 baseline/candidate captures across all three frosted presets, 8/10-bit/FP16, fractional scale, rotation, one-pixel/edge/center changes, partial clean-neighbor updates with reduced opacity, and restack transitions |
| Overlapping video stacks before budget refinement | `validation-final.log`: six cases, 192 exact full-frame comparisons; `verified-*/result.json` contains raw capture metadata |
| Broader regression | `full-complete.log`: 74 exact comparisons, three resize/disable/reopen cycles, workspace transitions, and monitor removal |
| Oversized 4K working set | `verified-16-liquid.log`: 80 exact comparisons across sixteen panes; the budget fallback runs without allocation churn |
| Glass and Liquid GPU/cache lifecycle | `legacy-complete.log`, `cache-complete.log`: allocation failure, lock/unlock, layers, mixed outputs, idle retirement, and settled-frame checks passed |

`tests/capture.cpp` reports native framebuffer damage. The regional suite reconstructs the presented frame from the prior full image and the actual changed rectangles, then compares it with a full repaint. It also compares against the saved starting binary, so undefined pixels outside the current framebuffer damage cannot hide stale output.

Reproduce real partial-update checks:

```sh
HYPRLIQUID_TEST_BASELINE=build/regional-damage-20261025/before.so \
HYPRLIQUID_TEST_OUTPUT=build/regional-damage-20261025/regional-repeat \
python3 scripts/test-nested.py --regional-damage-only
```

`scripts/benchmark-vram.py` now supports `--steady-video-edge` for a small video beside a tall receiving pane and `--steady-transform` for rotation/reflection. `--verify-steady-pixels --only-arm candidate` also compares retained frames against full rebuilds without reference arms. Supply the usual library/output arguments.

## Matched performance measurements

Disposable headless compositor on RX 9070 XT (`0000:03:00.0`), 3840×2160 at 60 Hz, six panes, client updates at 40 Hz, eight-second warmup, twelve-second sample. Figures are **compositor graphics-engine milliseconds per second**, read from DRM accounting, not total desktop GPU utilization. The reference is the prior validated `scene-final.so`; the baseline is the starting `before.so`.

| Workload | Before | Reference | Regional | Change from before |
| --- | ---: | ---: | ---: | ---: |
| Glass, video beside pane | 50.32 | 50.69 | 36.29 | −27.9% |
| Liquid-medium, video beside pane | 75.40 | 76.16 | 42.75 | −43.3% |
| Liquid, video beside pane | 66.35 | 77.44 | 53.75 | −19.0% |
| Liquid, large video behind center | 89.46 | 94.82 | 90.35 | +1.0%, within before/reference spread |
| Liquid, video beside pane, sixteen panes | 106.52 | 107.23 | 90.11 | −15.4% |

The three edge workloads also passed 120 exact full-frame comparisons at 4K before the budget refinement (`4k-edge-{glass,medium,liquid}.log`). Those separate timing runs showed 30%, 37%, and 26% reductions respectively. Treat the timings as workload-specific observations; the reference arms show some run-to-run variation.

The final build allocated no buffers during any measured interval. Its input pool retained about 119 MiB for Glass, 198 MiB for Liquid-medium, and 221 MiB for Liquid in the six-pane edge scene; the center and sixteen-pane fallbacks used about 172–173 MiB. Edge-workload CPU median render time increased by roughly 0.06–0.18 ms; this optimization trades CPU region bookkeeping and retained storage for less GPU work.

Native Hyprland blur (size 8, three passes, optimizations enabled, plugin unloaded) measured **2.02 ms/s** on the same edge layout. Its blur can reuse much more of that scene and does not reproduce the plugin's neighboring-color optics. **Native-blur parity is not achieved.** This headless result cannot be substituted for the historical live DP-1 utilization figures.

Raw final results are `final-*/result.json` and `verified-16-liquid/result.json`; `performance-summary.json` collects GPU, CPU, allocation, and retained-memory figures. `measure-final.py` reproduces the sequential warmed six-pane measurements. No live plugin was reloaded for these measurements.

## Live upgrade and closing-snapshot corrections

The first requested live upgrade crashed compositor PID 691663 after the old
plugin was unloaded. The core dereferences a missing `DECORATION_BORDER` in
Hyprland's animation damage path. Hyprland keeps raw plugin-decoration
registrations after windows die, and its unload sweep removes decorations by
pointer. A recycled allocation can therefore alias a native border. The
deterministic reused-registration regression reproduces that native removal on
`candidate-v3.so`.

`CGlassDecoration` now retires its registration in its destructor, and
`PLUGIN_EXIT` clears the registry before its existing ownership-based teardown.
The optional `build/reload-guard.so` compatibility bridge clears the already
loaded old version's registrations before unloading it. The actual live guard
retired 29 registrations with six windows open. The corrected plugin needs no
bridge for subsequent reloads.

Both Liquid presets also wrote fully opaque exterior pixels into Hyprland's
transparent closing snapshot, producing the reported black frame. The shader
now composes snapshot alpha from the destination, shadow, pane coverage and
highlight. Its normal monitor path is unchanged. In the regression's exterior
bands, alpha changed from 255 to 6 in dark mode and 19 in light mode; the shadow
remains present.

Correction evidence (under `live-install/` unless specified):

- `hyprland-crash.txt`, `hyprland.core`, and `gdb-decoration.txt`: original crash evidence.
- `unload-repro4.log`: old binary queues a native border for deletion; `unload-fixed2.log`: five hidden/unmapped/reused-registration cycles and closed-window registration cleanup pass.
- `guard-verified.log`: the exact compatibility bridge passes five cycles against the old binary.
- `close-repro1.log`: old Liquid snapshot fails at exterior alpha 255; `close-fixed1.log`: both presets pass in dark/light themes. `../close-fixed1/actual-closing-frames.json` verifies all sixteen captured closing frames retain the bright exterior backdrop.
- `build-close-fix.log`: `make -j4 test` passes, including 540 exact Glass and 288 exact Liquid comparisons.
- `fixes-regional.log`: all 192 normal-frame comparisons against `candidate-v3.so` pass across presets, native formats, fractional scale and rotation.
- `fixed-install.json`, `fixed-mapping.txt`, and `fixed-state.json`: corrected binary loaded in PID 2371538, preserving the current dark `frosted-liquid-medium` profile and all six windows, with no configuration errors. The on-disk rollback remains `pre-fix-disk.so`.

The v4 SHA-256 was `e767245161f2b1a3f9f439b858bc5f64afab8b751037008b2c807908fd18d406`.
The performance table above measures v3; v4's normal pixels were revalidated,
but its timing was not separately remeasured.

Reproduce the new regressions using a disposable compositor:

```sh
make build/capture-test.so build/decoration-lifecycle-test.so build/reload-guard.so
python3 scripts/test-nested.py --window-close-only
python3 scripts/test-nested.py --decoration-lifecycle-only
```

## Native drop-shadow consistency

The user requested visible native drop shadows for both Liquid presets, matching
`frosted-glass`. Liquid previously suppressed native shadows and substituted a
material shadow with a peak opacity of only 2.5% in dark mode. Window decorations
now leave native shadow properties alone and pass `renderShadow=false` to the
material renderer. The material output and its retained/damaged bounds stay
inside the window; native decorations handle the exterior. Sampling padding
retains its prior dimensions and Gaussian pipeline.

Liquid's scalar `shadow_amount` and `shadow_offset` fields remain stored for
compatibility but are no longer advertised as applicable controls. There are
19 applicable controls; diagnostics report `shadow_owner: native`,
`output_extent: 0`, and `owns_shadow: false`. Native `decoration.shadow`
settings control the visible shadow. Layer surfaces keep their surface shadows.

Evidence in `build/native-shadow-20261025/`:

- `before.so` is the previous v4 binary. `before-close.log` reproduces its
  mismatch against the native Glass shadow.
- `close-fixed.log` and `close-fixed/` compare both presets in dark/light themes
  against native Glass exterior pixels, in live frames and closing snapshots.
  All match exactly. Disabling native shadows leaves zero exterior snapshot
  alpha and no residual material shadow; reenabling restores the exact pixels.
  Sixteen captured closing frames pass the black-frame regression.
- `build-test.log`: unit/render tests pass, including 540 exact Glass and 288
  exact Liquid shader comparisons.
- `liquid-regression.log`: control API, scales/rotation, neighboring panes,
  opaque underlays, native shadow toggles, movement/resize/close cleanup,
  fullscreen/offscreen behavior and cache retirement pass.
- `regional.log`: 96 actual partial redraws match full repaints/rebuilds across
  three presets, 8/10-bit/FP16, fractional scale and rotation.
- `lifecycle.log`: five hidden/unmapped/reused-registration cycles and closed
  window registration cleanup pass.
- `candidate.so`, `candidate-source.tar.gz`, and `live-install.json` identify
  the installed correction. The compositor remained PID 2371538 and retained
  the current dark `frosted-glass` profile and all windows. `live-rollback.so`
  retains the previous binary. No new timing measurements were taken.

Installed SHA-256: `80891fb1b8c57802cfb11e713076c82396c2facc14ddb71e4d646af1fb976c06`.

## Intermittent partial-redraw corruption

The reported setup was a browser beside Kitty/btop in a 50/50 vertical split,
with brief flicker on hover, damaged edges and the Noctalia bar. The replay now
uses 4K at scale 1.25, a real layer-shell bar, small surface commits and
subsurface moves. References rebuild the complete scene; replay captures the
next genuine frame without scheduling a screenshot repaint between updates.
The capture fixture also leaves native pass simplification enabled.

Two concrete failures were reproduced:

- Six of 24 small-update frames left a one-pixel filtering fringe outside the
  rounded native damage rectangle. The scene's already guarded surface damage
  now also reaches the native framebuffer repaint.
- Moving a small subsurface invalidated the input key without repainting the
  complete material crop. A fresh cache then retained undefined native pixels:
  the replay first differed in 175,000 bytes, then roughly 12 million bytes on
  subsequent updates. An uncached pass now requests its full sampling crop
  before drawing. Partial-update inputs remain pinned through auxiliary
  rendering, and incomplete captures cannot become reusable material outputs.

Discarded valid cached passes now advance their scene serial too, so a
bar-only frame or opaque occlusion does not cause an unnecessary cache miss
on the next small client commit. A single-window/bar replay covers that path.
Small ordinary commits still use partial redraws; this is asserted in the test.
The isolated replay confirms these corruption mechanisms, but has not reproduced
every aspect of the reported transient Noctalia bar flicker itself.

Evidence in `build/flicker-20261026/`:

- `split-before-all.log`: six filtering-fringe failures before correction.
- `split-metadata.log` and its captured images: large cache corruption after
  subsurface movement, with only the initial fringe correction present.
- `split-rebuild.log`: 27 exact replay comparisons with native bar blur enabled.
- `flicker-final.log` and `flicker-final/`: 27 split-window plus four single-window
  comparisons, with the live desktop's bar settings; zero differing bytes.
- `regional.log`: all 96 partial/full comparisons pass across Glass and both
  Liquid presets, 8/10-bit/FP16, fractional scaling and rotation.
- `lifecycle.log`: five unload/remap/reused-registration cycles and closed-window
  registration cleanup pass. `git diff --check` also passes.
- `candidate.so`, `candidate-source.tar.gz`, `live-install.json` and
  `live-mapping.txt`: installed in the existing compositor PID 2371538, preserving
  all three open windows and the current dark `frosted-glass` runtime profile.
  No configuration errors. `live-rollback.so` retains the native-shadow build.
- `live-mitigation.json`: temporary diagnostic full redraws were removed after
  installation; `debug:damage_tracking` is restored to **2**. No persistent
  desktop configuration was edited. Performance timings were not remeasured.

Installed SHA-256: `c19d5b741f2d3b19aaacf1ca243358076dcd5db62d6b4b73698cf5feb4124c30`.

Reproduce with `python3 scripts/test-nested.py --damage-flicker-only` after
building the plugin, `build/capture-test.so`, `build/layer-test` and
`build/subsurface-test`. The fixture must only run in a disposable compositor.

## Partial fill after moving a float to another output

The user observed a persistent partial fill on DP-1 after dragging a floating
browser upward to DP-2. A live capture showed glass only in an upper-left
rectangle, even after a screenshot repaint. Its extent matched the terminal's
2174-by-1314 half-resolution blur image. DP-1 was using 4K, scale 1.25 and
10-bit SDR; DP-2 used scale 1 and HDR/FP16.

Our regional capture/blur paths called raw `glScissor`, while Hyprland caches
its last scissor rectangle. If the next native or material draw requested the
same rectangle used before that offscreen work, Hyprland skipped the GL call
and retained the scratch buffer's smaller clip. A material cache could then
retain the partial fill indefinitely. The auxiliary scene's raw scissor
restoration could also leave this cache inconsistent.

All production scissor changes now use Hyprland's `scissor` helper. Offscreen
and saved GL rectangles pass `transform=false`, preserving their buffer-local
coordinates while keeping the native cache synchronized. The security fixture
now primes a native rectangle, runs partial capture/reduction/blur, and checks
that requesting the same native rectangle restores the correct GL clip.

Evidence in `build/partial-fill-20261026/`:

- `before-*.json` and `dp-1.png`: live diagnostics and the partial fill.
- `scissor-before.log`: the previous build fails the deterministic regression
  with `partial blur left native scissor cache stale: 90,68,31,33`.
- `scissor-fixed.log`: state preservation and 315 partial/full filter comparisons
  pass across ordinary, clean-neighbor and Liquid inputs, five blur sigmas,
  seven damage patterns, and 8-bit/10-bit/FP16 attachments.
- `move-final.log`: three moves to an upper, differently scaled output leave
  DP-1 byte-identical to its original complete pane. The headless output falls
  back to SDR despite the requested HDR mode; actual descriptions are saved.
  Movement alone also passed on the old build, so the deterministic scissor
  test supplies the regression proof for the state-dependent fault.
- `regional.log` and `flicker.log`: all 96 existing regional comparisons and
  31 split/single-window replay steps pass, including output rotation.
- `candidate.so`, `candidate-source.tar.gz`, `live-install.json`, and
  `live-mapping.txt`: installed in PID 2371538, preserving the current profile,
  open windows and normal `debug:damage_tracking=2`. `live-rollback.so` retains
  the preceding build. No desktop configuration files were changed.
- `dp-1-after.png`: visual verification that the live terminal is completely
  filled again. `git diff --check` passes; timings were not remeasured.

Installed SHA-256: `be1242a07491d610ce3e4ed15735c1c1e4352d89a6837f882bf61c815e754da2`.

The isolated `--monitor-transition-only` suite uses monitor-selected raw
captures, which remain test-only. For a staged plugin, build its matching
`build/security-test.so` with the same Makefile `TARGET` and set
`HYPRLIQUID_TEST_SECURITY_PLUGIN=build/security-test.so` alongside
`HYPRLIQUID_TEST_PLUGIN` when running `--security-only`.

## Tiny tiles and rectangular shadows (2026-10-27)

DP-1 had 79-by-36 logical-pixel Kitty panes at scale 1.25. With configured
rounding 24 and rounding power 6, Hyprland's native radius was 72 logical
pixels. This inverted native shadow corner bounds and the interior cutout,
leaving hard rectangular patches around the tiny panes and their neighbors.
The patches persisted with glass disabled and disappeared with native shadows
disabled or a smaller configured radius. All temporary live settings were
restored.

The plugin now bounds `CWindow::rounding()` by half the smaller current window
dimension for enabled glass windows. This matches the existing material SDF
limit across native surfaces, borders, shadows and snapshots. Ordinary radii
return immediately; disabled/untreated windows retain their native radius.
The hook uses the existing exact-build ABI check and is removed before
decoration teardown.

Investigation also found a separate compositing bug: auxiliary glass rendered
after an opaque pass could overwrite transparent corners when blending was
disabled. Every material draw now enables blending and restores the caller's
state. This fix alone did not change the reported live artifact; the bounded
native radius is the verified correction for the user's screenshot.

Evidence in `build/tiny-tiles-20261026/`:

- `shadows-baseline-final.log` reproduces the failure on the blend-fixed,
  unbounded build; its screenshot has the same hard rectangular shadow.
- `shadows-final/tiny-shadows.json`: all 48 complete native-frame comparisons
  match an explicitly bounded reference. Covers horizontal, vertical and square
  tiny panes, an ordinary pane, a reflected neighbor, all three material
  families, 8-bit/10-bit/FP16, fractional scaling and rotation.
- `corners-before-2.log` independently reproduces the inherited-blending bug;
  `rounded-corners/tiny-corners.json` passes all 48 blend-state comparisons.
- `rounded-lifecycle.log`: five unload/remap cycles preserve native decorations.
  `rounded-closing.log`: native shadows, toggles and closing snapshots pass.
- `unit-final.log`: `make test` passes, including 540 exact Glass and 288 exact
  Liquid shader comparisons. `git diff --check` passes.
- `live-rounded/`: before/after screenshots confirm the rectangular patches
  are gone on DP-1. Installation kept compositor PID 2371538, all 13 clients
  and their geometry, dark `frosted-glass`, and normal damage tracking 2.
  No configuration errors; `live-rollback.so` retains the blend-only build.

Installed SHA-256: `78074ff5911c87338a448abbb67e1faacf99323e63a75dfa34721e03d674c0b4`.

Reproduce with `--tiny-shadows-only` and `--tiny-tiles-only` in
`scripts/test-nested.py`. Build the matching security fixture for the chosen
plugin target and pass `HYPRLIQUID_TEST_SECURITY_PLUGIN` as described above.
Load fixtures before setting transient test config: plugin loading reloads
Hyprland's configuration. Instrumentation remains disposable-compositor only.

## Zen hover reflections and output-buffer history — 2026-11-14

Hovering Zen's side tabs caused a thin reflection strip on adjacent windows to
flicker, while continuous scrolling looked smooth. Temporarily disabling damage
tracking removed it. The fix is installed in compositor PID 6926 with normal
`debug:damage_tracking=2`; all six windows, geometry and material settings were
preserved. The user confirmed **"Stable now"** after the final installation.

Two omissions combined:

1. Disjoint cached Glass panes did not declare their complete optical output
   damage. Native blur can render a larger region than it finally transfers to
   the output, so correct reflection pixels could remain undisplayed.
2. Expanding only the current frame still omitted those changes from Hyprland's
   buffer-age damage history, which rotates before workspace rendering. An older
   swapchain buffer could restore the stale strip on a subsequent bar-only frame.
   The fractional surface-filter guard had the same history omission, including
   the separate single-window fast path.

`WindowScene.cpp` now propagates neighboring optical output into render and final
transfer damage, and records newly introduced optical and guarded source regions
in the monitor's pending damage history. It does not schedule extra frames or
feed accumulated historical render damage back into the ring. Regional material
propagation records its newly generated damage too. Rendering remains partial.

The old replay oracle inspected internal render damage, then reconstructed a
frame from the previously presented image. That hid both final-transfer omissions
and stale swapchain-buffer reversion. The capture fixture now reports
`final_damage` and supports `next presented monitor NAME /absolute/path`: retain
`outFB` during rendering and read its actual pixels at `RENDER_POST`. The hover
regression uses complete output-buffer pixels by default. `framebuffer` metadata
demonstrates rotation; benchmark comparisons exclude these telemetry fields.
All capture instrumentation remains restricted to disposable compositors.

Evidence under `build/hover-refresh/`:

- `before.so`: original live build. `before-replay/`: 15/27 side-update frames
  fail the corrected final-transfer reconstruction at 10-bit/scale 1.25.
- `candidate.so`: current-frame-only correction. `swapchain-before/`: actual
  output readback exposes 9/27 failing frames, including bar-only reversion.
- `zen-browser/`: independent real Zen sidebar reproduction. Before history
  correction, 8/15 consecutive frames contain 6,794 incorrect neighboring-strip
  pixels, alternating with three output buffers. After correction, two hovers
  yield 28/28 byte-identical full-output frames with no recording gaps.
- `final-output/` and `independent-final-output/`: final build passes 31/31
  actual-output comparisons each at 10-bit and 8-bit, including single-window
  filter fringes. Ordinary small commits still exercise partial repaint.
- `regional-output/`: 96 actual-output comparisons pass across overlapping
  Glass/Liquid materials, 8-bit/10-bit/FP16 rendering, fractional scale and rotation.
- `hyprliquid-hover-unit.log`: `make -j4 test` passes. Lifecycle logs cover five
  unload/remap cycles and closed-window registration cleanup.
- `final-live/`: settings/geometry preservation and live installation evidence;
  `before.so` there retains the immediately preceding plugin for rollback.

Final installed SHA-256:
`abcdd6205c2503bbfd1de469c1b8c186a134f7035296d5c6e3e32fdc7ebc9480`.

Reproduce with `python3 scripts/test-nested.py --damage-flicker-only` after
building the capture/client fixtures. Set `HYPRLIQUID_TEST_BITDEPTH=10` and
`HYPRLIQUID_TEST_PATCH_Y=300` for the straight-side regression;
`HYPRLIQUID_TEST_WINDOWS=5` runs the larger split-layout sequence.
`HYPRLIQUID_TEST_PRESENTED=0` selects the intermediate-image diagnostic oracle.
For regional output-buffer checks, set `HYPRLIQUID_TEST_PRESENTED=1` with
`--regional-damage-only`. Performance timings were not remeasured.

## Historical artifacts

- `make -j4 test`: passed, including 540 exact Glass and 288 exact Liquid shader comparisons across 8-bit, 10-bit, and FP16.
- `build/moving-glass-20261025/scene-stack-validation.log`: 108 exact full-frame comparisons for the final production changes.
- `build/moving-glass-20261025/final-cache-regression.log`: nested GPU/cache regression suite passed.
- `tests/security-render.cpp::verifyBlurRegions` exercises partial blur, including overwritten shared scratch buffers.
- `scripts/benchmark-vram.py`: use `--steady-video-stack` and `--verify-steady-pixels` for overlapping-window comparisons.
- `build/moving-glass-20261025/live-comparison.json`: live measurements; `scene-final.so`: validated production binary.
- `build/kawase-center-ab/`: matched-frame images, `result.json`, isolated preview source, and `live-preview.py` (`load`, `gaussian`, `kawase`, `restore`). Do not reactivate unless requested.

Use codebase-memory-mcp for discovery; project: `home-polyvox-hyprliquid`. Live GPU is RX 9070 XT (`0000:03:00.0`); standalone EGL otherwise selects the iGPU, so use `DRI_PRIME=pci-0000_03_00_0`. Rediscover the live Hyprland instance: the shell’s default instance signature was stale.
