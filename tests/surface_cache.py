"""Real subsurface commits, cache invalidation and native-format pixel parity."""
import gzip
import hashlib
import json
import subprocess
import time
from pathlib import Path


def verify_surface_cache(ctl, env, log, clients, build, wait, fixtures, plugin, baseline=None):
    def control(op, **values):
        result = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
        assert result['ok'], result
        return result

    def counters():
        return {w['owner']: w for w in control('buffers')['buffers']['windows'] if w['enabled']}

    def write(path, value):
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps(value))
        temporary.replace(path)
        def acknowledged():
            try:
                return json.loads(Path(str(path) + '.ack').read_text()) == value
            except (OSError, ValueError):
                return False
        wait(acknowledged)

    def action(window, name, values):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' + json.dumps('address:' + window['address']) + ', ' + values + ' })')

    def raw(name, trigger=None):
        path = build / name
        ctl('hyprliquid-test:capture', ('next ' if trigger else '') + str(path))
        if trigger:
            trigger()
        wait(lambda: Path(str(path) + '.json').exists())
        metadata = json.loads(Path(str(path) + '.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        raw_path = Path(str(path) + '.raw')
        pixels = raw_path.read_bytes()
        assert any(pixels), 'Empty capture'
        with gzip.open(str(raw_path) + '.gz', 'wb') as compressed:
            compressed.write(pixels)
        raw_path.unlink()
        return pixels, metadata

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    current = plugin
    evidence = {'captures': {}, 'repaints': {}, 'baseline': str(baseline) if baseline else None}
    reference = {}
    arms = [('baseline', Path(baseline).resolve())] if baseline else []
    arms.append(('candidate', plugin))
    for arm, library in arms:
        if current != library:
            ctl('plugin', 'unload', str(current))
            ctl('plugin', 'load', str(library))
            current = library
        ctl('dismissnotify')
        ctl('eval', 'hl.config({ animations = { enabled = false }, general = { border_size = 0 }, '
            'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, shadow = { enabled = true } }, '
            'cursor = { no_warps = true, inactive_timeout = 0.1 }, render = { use_fp16 = 2 } })')
        ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
            'scale = 1, transform = 0, bitdepth = 8, cm = "srgb", supports_hdr = 0, supports_wide_color = 0 })')
        control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark',
                                 'animate_display_angle': False, 'animate_hover_angle': False})
        processes = []
        paths = []
        windows = []

        def spawn(index):
            path = build / f'{arm}-surface-{index}.json'
            path.write_text('{}')
            process = subprocess.Popen([str(fixtures / 'subsurface-test'), f'surface-cache-{index}', str(path)],
                                       env=env, stdout=log, stderr=log)
            processes.append(process)
            clients.append(process)
            window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == f'surface-cache-{index}'), None))
            action(window, 'float', 'action = "enable"')
            action(window, 'resize', 'x = 580, y = 320, relative = false')
            action(window, 'move', f'x = {36 + index * 600}, y = 48, relative = false')
            paths.append(path)
            windows.append(window)
            time.sleep(.3)

        def check(name):
            time.sleep(.15)
            pixels, metadata = raw(arm + '-' + name)
            digest = hashlib.sha256(pixels).hexdigest()
            if arm == 'baseline':
                reference[name] = digest
            else:
                if reference:
                    assert digest == reference[name], f'Baseline pixels differ: {name}'
                control('apply', patch={})
                time.sleep(.15)
                rebuilt, _ = raw(arm + '-' + name + '-rebuilt')
                assert rebuilt == pixels, f'Cached pixels differ from rebuild: {name}'
            evidence['captures'][arm + '-' + name] = {'sha256': digest, 'metadata': metadata}
            print(f'PASS: {arm} {name} native pixels', flush=True)

        def incremental(name, color):
            partial, metadata = raw(arm + '-' + name + '-partial', lambda: write(paths[0], {
                'child': {'patch': color, 'partial': True}}))
            rebuilt, _ = raw(arm + '-' + name + '-full')
            assert partial == rebuilt, f'Incremental output differs from a full repaint: {name}'
            evidence['repaints'][arm + '-' + name] = metadata
            check(name)

        spawn(0)
        check('single-full-child')
        before = counters()
        for i in range(6):
            write(paths[0], {'child': {'color': 0xa02050 if i % 2 else 0x20a050}})
            time.sleep(.04)
        after = counters()
        owner = 'window:' + windows[0]['address']
        if arm == 'candidate':
            assert after[owner]['captures'] == before[owner]['captures'], (before, after)
            assert after[owner]['window_backdrop_reuses'] > before[owner]['window_backdrop_reuses'], (before, after)
            assert after[owner]['material_draws'] == before[owner]['material_draws'], (before, after)
            assert after[owner]['material_cache_reuses'] > before[owner]['material_cache_reuses'], (before, after)
        evidence['repaints'][arm + '-single'] = {'before': before, 'after': after}
        check('single-child-damage')
        # Capture the next real partial commit without damaging the monitor.
        # A normal screenshot request forces a complete repaint and would hide
        # stale pixels caused by narrowing the cached pass's damage footprint.
        incremental('single-incremental', 0x3876a2)
        for bottom in (False, True):
            layer = subprocess.Popen([str(fixtures / 'layer-test'), '--animate', *(['--bottom'] if bottom else [])],
                                     env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
            clients.append(layer)
            wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
            time.sleep(.4)
            before = counters()
            time.sleep(.4)
            after = counters()
            if arm == 'candidate':
                assert (after[owner]['captures'] > before[owner]['captures']) == bottom, (bottom, before, after)
            layer.terminate()
            layer.wait(timeout=5)
            wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
        check('single-background-restored')
        for theme in ('light', 'dark'):
            control('apply', patch={'theme': theme, 'overrides': {'glass_opacity': .73}})
            check('single-' + theme + '-translucent')
        control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark'})
        ctl('eval', 'hl.config({ decoration = { shadow = { range = 40, render_power = 2, offset = "12 8" } } })')
        check('single-shadow-settings')
        ctl('eval', 'hl.config({ decoration = { shadow = { range = 4, render_power = 3, offset = "0 0" } } })')
        check('single-shadow-restored')
        spawn(1)
        write(paths[0], {})
        check('two-full-children')
        before = counters()
        for i in range(6):
            write(paths[0], {'child': {'color': 0xa02050 if i % 2 else 0x20a050}})
            time.sleep(.04)
        after = counters()
        if arm == 'candidate':
            assert after[owner]['captures'] == before[owner]['captures'], (before, after)
            adjacent = 'window:' + windows[1]['address']
            assert after[adjacent]['partial_blurs'] > before[adjacent]['partial_blurs'], (before, after)
            assert after[owner]['scene_base_cache_reuses'] > before[owner]['scene_base_cache_reuses'], (before, after)
        evidence['repaints'][arm + '-two'] = {'before': before, 'after': after}
        check('two-child-damage')
        incremental('two-incremental', 0xd97824)
        for preset in ('soft-glass', 'clear-glass', 'frosted-glass-clear-edge', 'frosted-glass'):
            control('apply', profile={'base_preset': preset, 'theme': 'dark'})
            check('material-' + preset)

        cases = [
            ('partial-initial', {}),
            ('partial-child-damage', {'child': {'patch': 0xe08020, 'partial': True}}),
            ('child-inset', {'child': {'x': 40, 'y': 30, 'width': 500, 'height': 240}}),
            ('child-moved-without-damage', {'child': {'x': 60, 'y': 60, 'width': 500, 'height': 240}}),
            ('nested-child', {'child': {'x': 40, 'y': 30, 'width': 500, 'height': 240},
                              'grandchild': {'mapped': True, 'x': 420, 'y': 100, 'color': 0xe0a010}}),
            ('nested-damage', {'child': {'x': 40, 'y': 30, 'width': 500, 'height': 240},
                               'grandchild': {'mapped': True, 'x': 420, 'y': 100, 'color': 0x10a0e0}}),
            ('nested-moved-without-damage', {'child': {'x': 40, 'y': 30, 'width': 500, 'height': 240},
                                            'grandchild': {'mapped': True, 'x': 400, 'y': 120, 'color': 0x10a0e0}}),
            ('child-full-again', {}),
            ('child-below', {'child': {'below': True}}),
            ('child-above', {}),
            ('child-unmapped', {'child': {'mapped': False}}),
            ('child-remapped', {'child': {'color': 0x40c050}}),
            ('outside-left', {'child': {'x': -30}}),
            ('outside-right', {'child': {'x': 30, 'color': 0xb05010}}),
            ('resume-contained', {'child': {'color': 0x20d0e0}}),
            ('child-scale-2', {'child': {'scale': 2}}),
            ('child-rotated', {'child': {'transform': 1}}),
            ('child-restored', {}),
        ]
        for name, state in cases:
            write(paths[0], state)
            check(name)
        for scale, transform in ((1.25, 0), (1.6, 0), (1, 1), (1.25, 3)):
            ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", '
                f'scale = {scale}, transform = {transform}' + ' })')
            time.sleep(.4)
            check(f'scale-{scale}-transform-{transform}')
            incremental(f'scale-{scale}-transform-{transform}-incremental', 0x40b078 + int(scale * 16) + transform)
        for name, options in [('sdr-10bit', 'bitdepth = 10, cm = "srgb"'),
                              ('hdr-fp16', 'bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1')]:
            ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", scale = 1, transform = 0, ' + options + ' })')
            time.sleep(.4)
            check(name)
            metadata = evidence['captures'][arm + '-' + name]['metadata']
            assert metadata['pixel_bytes'] >= (8 if name == 'hdr-fp16' else 4), metadata
            incremental(name + '-incremental', 0x6850b8 if name == 'hdr-fp16' else 0xb85068)
        for process in processes:
            process.terminate()
            process.wait(timeout=5)
        wait(lambda: not json.loads(ctl('-j', 'clients')))
        (build / 'surface-cache.json').write_text(json.dumps(evidence, indent=2))
    print('PASS: single-window and bounded subsurface caches, damage, structural changes, scales and native formats', flush=True)
