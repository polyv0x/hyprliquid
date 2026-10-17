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
Each preset has `id`, `inherits`, and `valid`. The list contains the five public
built-ins and configured user presets. Compatibility aliases are accepted by
`apply` but omitted from this list; returned preset names use the public name.
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
| `active` | Whether any panel override exists |
| `panel` | Sparse explicit `enabled`, `base_preset`, `theme`, and `overrides` |
| `inherited` | Resolved selected base material/theme before scalar panel overrides |
| `material` | Actual resolved/sanitized material after panel overrides |
| `baseline` | Configured enabled/preset/theme and resolved configured material |

`material` includes shader invariants such as ordered refraction thresholds.
`panel.overrides` retains the requested values, so callers can distinguish a
sanitized result from an explicit edit. Numbers are validated and stored with the
renderer's float precision; signed refraction, shadow offset, and coverage remain
signed. Boolean values and numeric strings are not accepted as scalar numbers.

`apply` accepts exactly one of `profile` or `patch`. A profile replaces all
runtime ownership; an omitted field inherits configuration. A patch changes only
provided fields. Both accept `enabled`, `base_preset`, `theme`, `overrides`, and
`reset_fields`. In a patch, JSON null removes an enabled/preset/theme override,
and a null scalar value removes that scalar override. `reset_fields` is an array
of scalar names and is convenient for clients without a JSON-null value.

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

Roles distinguish `window-sharp`, `window-blurred`, `scene-background`,
`scene-windows`, `layer-sharp`, `layer-blurred`, `layer-temp`, `blur-pyramid`, and
`blur-temp`. Window samples belong to `pool:<id>`, rather than a particular
window. Owners and color IDs are runtime identifiers, not persistent keys.
Nominal byte counts use the DRM format's stride/block geometry and exclude
driver alignment, metadata and compression.

The pool retains at most eight idle pairs or 128 MiB, with one oversized-pair
exception to avoid allocating on every draw of a large HDR window. Active leases
are excluded from those idle limits. Unused pairs expire after five seconds;
unused scenes retire after two. A one-second cleanup timer can add up to one
second to these delays and never requests redraws. Layer caches remain owned by
their layer surfaces. See [the allocation and validation report](vram-optimization.md).

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
