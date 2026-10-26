"""Check Liquid uses native drop shadows in live frames and close snapshots."""
import json
import subprocess
import time


def verify_window_close(ctl, env, log, clients, output, wait, fixtures):
    from PIL import Image

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    ctl('dismissnotify')
    ctl('eval', 'hl.config({ general = { border_size = 0 }, '
        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, '
        'shadow = { enabled = true, range = 28, render_power = 3, offset = "0 12", '
        'color = "rgba(000000b8)", color_inactive = "rgba(000000b8)" } }, '
        'cursor = { no_warps = true, inactive_timeout = 0.1 } })')
    background = subprocess.Popen([
        'kitty', '--class', 'hyprliquid-background', '-o', 'background=#dddddd',
        '-o', 'background_opacity=1', '-o', 'remember_window_size=no',
        'sh', '-c', "printf '\033[?25l'; sleep 120",
    ], env=env, stdout=log, stderr=log)
    clients.append(background)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 1)
    results = []
    bands = [(400, 310, 436, 490), (844, 310, 880, 490)]

    def capture(stem):
        time.sleep(0.15)
        subprocess.run(['grim', str(stem) + '-open.png'], env=env, check=True, capture_output=True, timeout=10)
        assert ctl('hyprliquid-test:snapshot', 'hyprliquid-close', str(stem)) == 'ok'
        metadata = json.loads(stem.with_suffix('.json').read_text())
        assert metadata['gl_error'] == 0 and metadata['gl_type'] == 5121, metadata
        assert metadata['gl_format'] in (6408, 32993), metadata
        raw_mode = 'RGBA' if metadata['gl_format'] == 6408 else 'BGRA'
        snapshot = Image.frombytes('RGBA', (metadata['width'], metadata['height']),
                                   stem.with_suffix('.raw').read_bytes(), 'raw', raw_mode)
        snapshot = snapshot.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        snapshot.save(str(stem) + '-snapshot.png')
        with Image.open(str(stem) + '-open.png') as image:
            live = image.convert('RGB')
        return snapshot, live

    def exterior(image):
        return b''.join(image.crop(box).tobytes() for box in bands)

    for preset in ('frosted-liquid', 'frosted-liquid-medium'):
        for theme in ('dark', 'light'):
            ctl('eval', 'hl.config({ animations = { enabled = false } })')
            reply = json.loads(ctl('hyprliquid:control', json.dumps({
                'op': 'apply', 'profile': {'base_preset': preset, 'theme': theme}})))
            assert reply['ok'], reply
            client = subprocess.Popen([
                'kitty', '--class', 'hyprliquid-close', '-o', 'background_opacity=0',
                '-o', 'remember_window_size=no', '-o', 'cursor_blink_interval=0',
                'sh', '-c', "printf '\033[?25l'; sleep 120",
            ], env=env, stdout=log, stderr=log)
            clients.append(client)
            window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients'))
                                       if w['class'] == 'hyprliquid-close'), None))
            selector = json.dumps('address:' + window['address'])
            for action, fields in [('float', 'action = "enable"'),
                                   ('resize', 'x = 400, y = 300, relative = false'),
                                   ('move', 'x = 440, y = 250, relative = false')]:
                ctl('dispatch', f'hl.dsp.window.{action}({{ window = {selector}, {fields} }})')
            time.sleep(0.3)
            stem = output / f'close-{preset}-{theme}'
            snapshot, live = capture(stem)
            # The same window, native settings and geometry must produce the
            # exact same exterior for Glass and Liquid, including in snapshots.
            ctl('hyprliquid:control', json.dumps({'op': 'apply', 'profile': {'base_preset': 'frosted-glass', 'theme': theme}}))
            reference, reference_live = capture(output / f'reference-{preset}-{theme}')
            assert exterior(snapshot) == exterior(reference), f'{preset}/{theme}: snapshot shadow differs from native Glass'
            assert exterior(live) == exterior(reference_live), f'{preset}/{theme}: live shadow differs from native Glass'
            ctl('hyprliquid:control', json.dumps({'op': 'apply', 'profile': {'base_preset': preset, 'theme': theme}}))
            ctl('eval', 'hl.config({ decoration = { shadow = { enabled = false } } })')
            disabled, disabled_live = capture(output / f'disabled-{preset}-{theme}')
            assert max(disabled.getchannel('A').crop(box).getextrema()[1] for box in bands) == 0, 'Material shadow remains after disabling native shadows'
            assert min(exterior(disabled_live)) >= 220, 'Residual material shadow in the live frame'
            ctl('eval', 'hl.config({ decoration = { shadow = { enabled = true } } })')
            restored, restored_live = capture(output / f'restored-{preset}-{theme}')
            assert exterior(restored) == exterior(snapshot) and exterior(restored_live) == exterior(live), 'Shadow toggle did not restore native pixels'
            alpha = snapshot.getchannel('A')
            # Four pixels clear of the pane, so no rounded-edge antialiasing
            # belongs in this band. The shadow must survive without becoming
            # the opaque rectangle formerly written over transparent black.
            shadow_alpha = max(alpha.crop(box).getextrema()[1] for box in bands)
            results.append({'preset': preset, 'theme': theme, 'exterior_alpha_max': shadow_alpha})
            (output / 'window-close.json').write_text(json.dumps(results, indent=2) + '\n')
            assert 32 < shadow_alpha < 192, f'{preset}/{theme}: exterior snapshot alpha {shadow_alpha}'
            exterior_open_min = min(exterior(live))

            ctl('eval', 'hl.config({ animations = { enabled = true } }); '
                'hl.animation({ leaf = "windowsOut", enabled = true, speed = 1, bezier = "linear", style = "popin 80%" }); '
                'hl.animation({ leaf = "fadeOut", enabled = true, speed = 1, bezier = "linear" })')
            client.terminate()
            for frame in range(4):
                time.sleep(0.1)
                path = f'{stem}-closing-{frame}.png'
                subprocess.run(['grim', path], env=env, check=True, capture_output=True, timeout=10)
                with Image.open(path) as image:
                    exterior_min = min(min(channel[0] for channel in image.convert('RGB').crop(box).getextrema()) for box in bands)
                assert exterior_min >= exterior_open_min - 2, f'{preset}/{theme}: black exterior in closing frame {frame}: {exterior_min}'
            client.wait(timeout=5)
            wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 1)
            time.sleep(1)
    ctl('plugin', 'unload', str(fixtures / 'capture-test.so'))
    print('PASS: both Liquid presets match native Glass shadows in live frames and close snapshots, in dark/light themes', flush=True)
    print('PASS: native shadow toggles leave no duplicate material shadow; closing frames retain translucent exteriors', flush=True)
