"""Native partial commits through overlapping panes, compared without repainting."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import time


def verify_regional_damage(ctl, env, log, clients, build, wait, fixtures, plugin, baseline=None):
    def control(op, **values):
        result = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
        assert result['ok'], result
        return result

    def action(window, name, values):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' + json.dumps('address:' + window['address']) + ', ' + values + ' })')

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

    def capture(name, trigger=None):
        path = build / name
        ctl('hyprliquid-test:capture', ('next ' if trigger else '') + str(path))
        if trigger:
            trigger()
        wait(lambda: Path(str(path) + '.json').exists())
        metadata = json.loads(Path(str(path) + '.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        raw = Path(str(path) + '.raw')
        pixels = raw.read_bytes()
        with gzip.open(str(raw) + '.gz', 'wb') as output:
            output.write(pixels)
        raw.unlink()
        return pixels, metadata

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    evidence = {'captures': {}, 'baseline': str(baseline) if baseline else None}
    reference = {}
    current = plugin
    arms = [('baseline', Path(baseline).resolve())] if baseline else []
    arms.append(('candidate', plugin))
    for arm, library in arms:
        if current != library:
            ctl('plugin', 'unload', str(current))
            ctl('plugin', 'load', str(library))
            current = library
        ctl('dismissnotify')
        ctl('eval', 'hl.config({ animations = { enabled = false }, general = { border_size = 0 }, '
            'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false }, '
            'cursor = { no_warps = true, inactive_timeout = 0.1 }, render = { use_fp16 = 2 } })')
        ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
            'scale = 1, transform = 0, bitdepth = 8, cm = "srgb" })')
        processes, paths, windows = [], [], []
        for index, (x, y) in enumerate(((36, 48), (300, 140), (620, 200))):
            path = build / f'{arm}-regional-{index}.json'
            path.write_text('{}')
            process = subprocess.Popen([str(fixtures / 'subsurface-test'), f'regional-{index}', str(path)],
                                       env=env, stdout=log, stderr=log)
            processes.append(process)
            clients.append(process)
            window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == f'regional-{index}'), None))
            action(window, 'float', 'action = "enable"')
            action(window, 'resize', 'x = 580, y = 320, relative = false')
            action(window, 'move', f'x = {x}, y = {y}, relative = false')
            paths.append(path)
            windows.append(window)
        action(windows[-1], 'alter_zorder', 'mode = "top"')

        def check(name, trigger=None):
            # An internal target has undefined pixels outside native damage.
            # Reconstruct presentation from a complete frame and the next real
            # commit; comparing raw undamaged storage would test buffer reuse.
            previous = capture(arm + '-' + name + '-before')[0] if trigger else None
            pixels, metadata = capture(arm + '-' + name, trigger)
            if previous:
                composed = bytearray(previous)
                stride = metadata['width'] * metadata['pixel_bytes']
                for x0, y0, x1, y1 in metadata['native_damage']:
                    for y in range(y0, y1):
                        lo, hi = y * stride + x0 * metadata['pixel_bytes'], y * stride + x1 * metadata['pixel_bytes']
                        composed[lo:hi] = pixels[lo:hi]
                pixels = bytes(composed)
                full, _ = capture(arm + '-' + name + '-full')
                assert pixels == full, f'Partial presentation differs from full repaint: {arm} {name}'
            digest = hashlib.sha256(pixels).hexdigest()
            if arm == 'baseline':
                reference[name] = digest
            else:
                if reference:
                    assert digest == reference[name], f'Baseline pixels differ: {name}'
                control('apply', patch={})
                time.sleep(.15)
                rebuilt, _ = capture(arm + '-' + name + '-rebuilt')
                assert pixels == rebuilt, f'Partial commit differs from full rebuild: {name}'
            evidence['captures'][arm + '-' + name] = {'sha256': digest, 'metadata': metadata}
            (build / 'regional-damage.json').write_text(json.dumps(evidence, indent=2))
            print(f'PASS: {arm} {name} native pixels', flush=True)

        formats = (('8bit', 1, 0, 8, 2), ('10bit', 1, 0, 10, 2), ('fp16', 1, 0, 10, 1),
                   ('scaled', 1.25, 0, 8, 2), ('rotated', 1, 1, 8, 2))
        for format_name, scale, transform, depth, fp16 in formats:
            ctl('eval', f'hl.config({{ render = {{ use_fp16 = {fp16} }} }}); '
                'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
                f'scale = {scale}, transform = {transform}, bitdepth = {depth}, cm = "srgb"' + ' })')
            for preset in ('frosted-glass', 'frosted-liquid-medium', 'frosted-liquid'):
                control('apply', profile={'base_preset': preset, 'theme': 'dark',
                                         'animate_display_angle': False, 'animate_hover_angle': False})
                # Exercise a corner, an edge reflected into the adjacent pane,
                # and the center beneath a transparent overlapping receiver.
                for location, (x, y, w, h) in enumerate(((0, 0, 1, 1), (568, 80, 12, 35), (350, 180, 35, 23))):
                    state = {'patch': 0x3050b0, 'patch_x': x, 'patch_y': y, 'patch_w': w, 'patch_h': h}
                    write(paths[0], {'child': state})
                    time.sleep(.15)
                    for phase, color in enumerate((0xd83070, 0x30c050)):
                        name = f'{format_name}-{preset}-{location}-{phase}'
                        check(name, lambda color=color: write(paths[0], {'child': state | {'patch': color, 'partial': True}}))
                # Clean-neighbor inputs, fades, and geometry/order invalidation.
                if format_name == '8bit':
                    control('apply', patch={'overrides': {'neighbor_contribution': .37, 'glass_opacity': .73}})
                    clean = {'color': 0xa040e0, 'patch': 0x3050b0, 'patch_x': 568,
                             'patch_y': 80, 'patch_w': 12, 'patch_h': 35}
                    write(paths[0], {'child': clean})
                    time.sleep(.2)
                    check(f'{preset}-clean-fade', lambda: write(paths[0], {'child': clean | {'patch': 0xd83070, 'partial': True}}))
                    action(windows[1], 'alter_zorder', 'mode = "top"')
                    time.sleep(.2)
                    check(f'{preset}-restack')
                    action(windows[-1], 'alter_zorder', 'mode = "top"')
        if arm == 'candidate':
            counters = control('buffers')['buffers']['windows']
            assert any(w.get('partial_captures', 0) for w in counters), 'No partial capture exercised'
            assert any(w.get('partial_blurs', 0) for w in counters), 'No partial blur exercised'
            assert any(w.get('partial_materials', 0) for w in counters), 'No partial material draw exercised'
            evidence['windows'] = counters
        for process in processes:
            process.terminate()
            process.wait(timeout=5)
        wait(lambda: not json.loads(ctl('-j', 'clients')))
    (build / 'regional-damage.json').write_text(json.dumps(evidence, indent=2))
    print('PASS: regional client commits, overlapping material order, native formats, scales and transforms', flush=True)
