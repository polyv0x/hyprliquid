# Runtime control API 1

Hyprliquid exposes a local JSON request through the plugin's Hyprland IPC hook:

```sh
hyprctl hyprliquid:control '{"op":"describe"}'
hyprctl hyprliquid:control '{"op":"state"}'
hyprctl hyprliquid:control '{"op":"buffers"}'
hyprctl hyprliquid:control '{"op":"apply","profile":{"base_preset":"soft-glass","theme":"dark","enabled":true,"overrides":{"inner_refraction_amount":-20,"rim_opacity":0.4}}}'
hyprctl hyprliquid:control '{"op":"apply","patch":{"overrides":{"coverage_offset":-0.3}}}'
hyprctl hyprliquid:control '{"op":"apply","patch":{"reset_fields":["rim_opacity"]}}'
hyprctl hyprliquid:control '{"op":"reset"}'
```

Clients should execute `hyprctl`, `hyprliquid:control`, and the encoded JSON as
three argv elements. No shell or embedded Lua is necessary. This API requires
`json-c` development headers/libraries in addition to the existing build dependencies.

Every response is an object with `api: 1`, `ok`, and `state`. A rejected request
also has `code` and a human-readable `error`. Check `ok` even when hyprctl exits
successfully. An unloaded/older plugin returns Hyprland's ordinary unknown-request
text; treat that as unavailable. Requests are limited to 16 KiB and JSON depth 16.

`describe` adds `capabilities`, `parameters`, and `presets`. Each parameter has
`name`, `default`, `min`, and `max`, generated directly from `MaterialParams.inc`.
Each preset has `id`, `inherits`, `valid`, and `model`. The list contains the seven public
built-ins and configured user presets. Compatibility aliases are accepted by
`apply` but omitted from this list; returned preset names use the public name.
The list is in display order: `frosted-glass` first, `frosted-glass-clear-edge`
second, then all remaining entries alphabetically. Clients should preserve this
order. The default preset is `frosted-glass`; the old `clear-edge` and
`frosted-clear-edge` names resolve to `frosted-glass-clear-edge`.
An alias explicitly customized in Lua is listed and retains its own name and
overrides. See the [preset guide](guide.md#configure) for old-name mappings.
Missing parents, cycles, and inheritance chains beyond the renderer's supported
depth make a preset invalid for selection. Invalid configured presets are not
deleted or repaired by the control API.

`state` contains:

| Field | Meaning |
| --- | --- |
| `epoch` | Opaque string, changes on plugin load |
| `generation` | Configuration reload count within this epoch |
| `revision` | Increases on accepted mutations, reloads, and dynamic `config()` writes |
| `enabled`, `base_preset`, `theme` | Effective window defaults; theme is `dark` or `light` |
| `animate_display_angle` | Boolean; sweep Liquid highlights during native window size transitions, default `false` |
| `active` | Whether any panel override exists |
| `panel` | Sparse explicit `enabled`, `animate_display_angle`, `base_preset`, `theme`, and `overrides` |
| `inherited` | Resolved selected base material/theme before scalar panel overrides |
| `material` | Actual resolved/sanitized material after panel overrides |
| `baseline` | Configured enabled/preset/theme and resolved configured material |
| `model` | Resolved material family: `legacy` or `liquid`, inherited through custom presets |
| `applicable_parameters` | Scalar names that affect the effective model |
| `effective_blur_sigma` | Resolved logical blur sigma, including fixed material values |
| `model_data` | Liquid environment, base `parameters`, resolved `effective_parameters`, rendering assumptions, shadow owner and output extent |

`material` includes shader invariants such as ordered refraction thresholds.
`panel.overrides` retains the requested values, so callers can distinguish a
sanitized result from an explicit edit. Numbers are validated and stored with the
renderer's float precision; signed refraction, shadow offset, and coverage remain
signed. Boolean values and numeric strings are not accepted as scalar numbers.

The additive `material_models` capability supplies these model fields in state,
baseline, and window/layer diagnostics. Each described scalar also has a
`models` array. The `material` and `inherited` objects keep the existing scalar
contract; fields absent from `applicable_parameters` are inapplicable to Liquid. Model selection is
through preset inheritance, never a numeric override. `model_data` is read-only
and separate from those scalar objects. Unknown model fields in `apply` reject
the entire request.

```sh
hyprctl hyprliquid:control '{"op":"apply","profile":{"base_preset":"frosted-liquid","theme":"dark","overrides":{"glass_opacity":0.9}}}'
```

Liquid supports 19 of the 34 scalar controls: all blur/refraction controls,
face opacity, highlight width/strength/angle, and the four
overall/neighbor controls. See the [material contract](guide.md#liquid-material)
for their mappings and preset defaults. `neighbor_falloff` defaults to zero.
`blur_sigma` defaults to 19 for `frosted-liquid` and 6 for
`frosted-liquid-medium`; all their other resolved values are identical.
The hidden `liquid-medium` alias resolves to `frosted-liquid`.
Window shadows use Hyprland's native `decoration.shadow` settings;
`model_data.shadow_owner` is `native` for windows and `surface` for layers,
and `output_extent` is zero. Stored material shadow fields remain available
for profile compatibility but are absent from Liquid's applicable controls.
Descriptor defaults remain generic.
`light` selects the `default` material environment; `dark` selects `dark`.

`model_data.parameters` contains the base material variant before preset and scalar overrides.
The additive `model_data.effective_parameters` contains the resolved structured
values uploaded to the renderer, before output/effect scaling. It equals the
base variant for an unedited `frosted-liquid-medium`; `frosted-liquid`
changes only the effective blur radius to 19. Both objects are read-only;
edit scalar overrides rather than sending structured data back in `apply`.
Older material-model responses may omit `effective_parameters`.

`apply` accepts exactly one of `profile` or `patch`. A profile replaces all
runtime ownership; an omitted field inherits configuration. A patch changes only
provided fields. Both accept `enabled`, `animate_display_angle`, `base_preset`, `theme`, `overrides`, and
`reset_fields`. In a patch, JSON null removes an enabled/animation/preset/theme override,
and a null scalar value removes that scalar override. `reset_fields` is an array
of scalar names and is convenient for clients without a JSON-null value.

The additive `display_angle_animation` capability advertises the boolean
`animate_display_angle` and Liquid-only scalar `display_angle_degrees` (-180 to
180, default 0). With the toggle off, the angle stays fixed. With it on, each
native size transition sweeps 0° → configured angle → 0° using the window's
eased resize progress. Idle windows remain at 0°; interrupted resizes continue
from the last drawn angle. The boolean applies to all Liquid windows, including
tagged presets, while their own resolved angle supplies the peak. Layers keep
their static angle. A profile omission or patch null relinquishes ownership of
the boolean; `reset_fields` remains for scalar controls only.

```sh
hyprctl hyprliquid:control '{"op":"apply","patch":{"animate_display_angle":true,"overrides":{"display_angle_degrees":30}}}'
```

`model_data.display_angle_degrees` reports the configured angle. Each window in
`buffers.windows` additionally reports `animate_display_angle` and
`rendered_display_angle_degrees`, the angle used by its last draw. No new frames
are scheduled by querying diagnostics.

Validation finishes before any mutation. Unknown keys, invalid types, unknown or
invalid preset chains, non-finite values, and out-of-range values reject the
entire request. No configuration-defined preset map is modified. The runtime
layer is attached directly to the default window resolution context; it needs
no reserved preset name and therefore cannot collide with a user preset.

Pass both `if_epoch` and `if_revision` with `apply` or `reset` to guard against
concurrent edits. Values come from the latest state response. A mismatch returns
`code: "revision_conflict"` and the current state without modifying anything.
The pair is optional for deliberate command-line edits. Guarded clients should
read and show external changes before accepting another user action.

Runtime material/theme edits affect windows without explicit preset tags.
Explicit preset tags keep their configured material and theme behavior; explicit
theme tags still win. The enable toggle supplies the global window default,
with existing enabled/disabled tags retaining precedence. Layer materials and
native geometry are outside this API's scope. Accepted changes invalidate scene
generations, damage monitors, and schedule frames, including on idle desktops.

`reset` relinquishes panel ownership. Configuration reload clears runtime
ownership and increments both generation and revision. A dynamic
`hl.plugin.hyprliquid.config(...)` call also clears ownership and increments the
revision, allowing terminal edits to take control. Plugin unload destroys the
layer and unregisters the IPC command; a subsequent load gets a new epoch.
Frontend exit does not affect the renderer: the last accepted runtime material
remains until reset, a configuration edit/reload, or unload.

`buffers` is available when `describe.capabilities` contains `"buffers"`. It
adds a `buffers` object without changing state, revisions, damage, allocation,
or cleanup scheduling. This is an observation of plugin-owned storage; native
Hyprland work buffers, client surfaces, scanout, driver caches, and physical
residency are outside its totals.

| Diagnostic field | Meaning |
| --- | --- |
| `buffers` | Live allocations, deduplicated by texture; each has an epoch-local `id`, `role`, `owner`, dimensions, `drm_format`, color description/ID, `nominal_bytes`, allocation/reallocation counters, and `last_used_frame` |
| `retained_bytes` | Nominal bytes in all currently live registered storage |
| `peak_retained_bytes` | High-water nominal storage observed at allocation points since plugin load |
| `last_frame_bytes` | Currently live storage touched in the most recent monitor render; expired buffers and other monitors' earlier frames are excluded |
| `frame`, `last_monitor_id` | Last render serial and monitor; serials are shared across monitors |
| `allocations`, `reallocations`, `allocation_failures` | Cumulative counts since load, including allocations whose buffers have since expired; reallocations are a subset of successful allocations |
| `window_pool` | Pair count, nominal bytes, leased bytes, hits, misses, evictions, idle limits and timeout |
| `cpu_frame_ms` | Up to 256 CPU submission intervals from render begin through post-render; these are not GPU execution or presentation times |
| `windows`, `layers` | Resolved materials, preset/theme, owner and monitor, geometry; windows also report enablement and padding, layers report namespace and cache state |
| `monitors` | Output names/IDs, physical size, scale, transform, refresh, FP16 status and color metadata |

Roles distinguish `window-sharp`, `window-blurred`, `window-clean-blurred`, `scene-background`,
`scene-windows`, `layer-sharp`, `layer-blurred`, `layer-temp`, `blur-pyramid`, and
`blur-temp`. Window samples belong to `pool:<id>`, rather than a particular
window. Owners and color IDs are runtime identifiers, not persistent keys.
Nominal byte counts use the DRM format's stride/block geometry and exclude
driver alignment, metadata and compression.

Liquid adds `window-material-backdrop`, `window-clean-material-backdrop` (only
when protecting underlays at reduced contribution), and `layer-material-backdrop`.
These are retained half-resolution unblurred inputs. Pool bytes, eviction, and
idle cleanup include them. Window diagnostics retain `owns_shadow` for
compatibility; it is false because native decorations own window shadows.
Finished-output-only entries may have no input textures after eviction;
`material_cache` accounts for their retained native pixels separately.
`window_pool.pairs` counts input entries against its limit;
`window_pool.output_only_entries` reports entries that only retain finished pixels.
The `backdrop_cache` admission limit is 160 MiB, including Liquid's half-size
material input. An independent bottom tile reports `material_cache_reuses`
while contained floating clients repaint above it.
`partial_materials` counts material draws restricted by propagated damage,
including auxiliary repairs. These use the existing `partial_captures` and
`partial_blurs` counters for their input work. `auxiliary-repair` is a pooled
native-sized scratch image with the input pool's lifetime; `window-destination`
is a shared temporary Liquid crop for antialiased boundaries,
separate from each pane's retained material input. Finished
Liquid body pixels remain in `material_cache`; boundaries are rendered live.
Layer diagnostics expose `rounded_regions`, the smooth cards recognized from
their protocol region on the last composite. Each entry is `[x, y, width,
height, radius]` in surface-local logical coordinates. An empty array means
the layer uses its existing alpha/protocol mask with rectangular geometry.

The input pool retains at most 32 idle pairs or 256 MiB, with one oversized-pair
exception to avoid allocating on every draw of a large HDR window. Active leases
are excluded from those idle limits. Unused pairs expire after five seconds;
unused scenes retire after two. A one-second cleanup timer can add up to one
second to these delays and never requests redraws. Layer caches remain owned by
their layer surfaces.

Verification:

```sh
make -j4 all test
python3 scripts/test-nested.py
```

The nested test exercises real JSON IPC, invalid atomic patches, signed numbers,
field reset, configured custom presets, revision guards, config/terminal
ownership, idle-window repainting, tagged-window isolation, and load epochs.
The screenshot checks require `kitty`, `grim`, and Python Pillow. Captures remain
under ignored `build/`. The shared library is still linked to a temporary file
and renamed atomically, preserving any library already mapped in a compositor.
