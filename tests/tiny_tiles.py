"""Check rounded auxiliary panes after both opaque and translucent drawing."""
import json
import subprocess
import time


def verify_tiny_shadows(ctl, env, log, clients, build, wait, fixtures):
    """Oversized native radii must match a valid radius, including reflections."""
    from pathlib import Path
    from PIL import Image

    # Loading a fixture reloads Hyprland's config; set runtime test values after it.
    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    ctl('eval', 'hl.config({animations={enabled=false},general={border_size=1},'
        'decoration={rounding=24,rounding_power=6,active_opacity=1,inactive_opacity=1,'
        'shadow={enabled=true,range=28,render_power=3,offset="0 0",'
        'color="rgba(000000b8)",color_inactive="rgba(000000b8)"}},'
        'cursor={no_warps=true,inactive_timeout=0.1}}); '
        'hl.window_rule({name="tiny-reflection-radius",match={class="^tiny-reflection$"},rounding=20}); '
        'hl.window_rule({name="tiny-background-radius",match={class="^hyprliquid-background$"},rounding=0})')
    background = subprocess.Popen([
        'kitty', '--class', 'hyprliquid-background', '-o', 'background=#dddddd',
        '-o', 'background_opacity=1', '-o', 'remember_window_size=no',
        'sh', '-c', "printf '\\033[?25l'; sleep 120",
    ], env=env, stdout=log, stderr=log)
    clients.append(background)
    wait(lambda: any(w['class'] == 'hyprliquid-background' for w in json.loads(ctl('-j', 'clients'))))
    windows = []
    for name, x, y, width, height in (('tiny-shadow', 300, 280, 79, 36), ('tiny-reflection', 200, 400, 400, 240)):
        path = build / (name + '.json')
        path.write_text(json.dumps({'root': {'alpha': 0}, 'child': {'mapped': False}}))
        process = subprocess.Popen([str(fixtures / 'subsurface-test'), name, str(path)], env=env, stdout=log, stderr=log)
        clients.append(process)
        window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == name), None))
        address = json.dumps('address:' + window['address'])
        ctl('dispatch', 'hl.dsp.window.float({window=' + address + ',action="enable"})')
        ctl('dispatch', 'hl.dsp.window.resize({window=' + address + f',x={width},y={height},relative=false' + '})')
        ctl('dispatch', 'hl.dsp.window.move({window=' + address + f',x={x},y={y},relative=false' + '})')
        windows.append(address)
    ctl('dismissnotify')

    def capture(name):
        path = build / name
        Path(str(path) + '.scene.json').write_text(json.dumps({
            'clients': json.loads(ctl('-j', 'clients')),
            'rounding': json.loads(ctl('-j', 'getoption', 'decoration:rounding')),
            'power': json.loads(ctl('-j', 'getoption', 'decoration:rounding_power')),
        }, indent=2))
        ctl('hyprliquid-test:capture', str(path))
        wait(lambda: Path(str(path) + '.json').exists())
        metadata = json.loads(Path(str(path) + '.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        raw = Path(str(path) + '.raw')
        pixels = raw.read_bytes()
        if metadata['gl_type'] == 5121 and metadata['gl_format'] in (6408, 32993):
            mode = 'RGBA' if metadata['gl_format'] == 6408 else 'BGRA'
            Image.frombytes('RGBA', (metadata['width'], metadata['height']), pixels, 'raw', mode).save(str(path) + '.png')
        raw.unlink()
        return pixels

    results = []
    for name, scale, transform, depth, fp16 in (('8bit', 1, 0, 8, 2), ('10bit', 1.25, 0, 10, 2),
                                               ('fp16', 1, 0, 10, 1), ('rotated', 1.25, 1, 10, 2)):
        ctl('eval', 'hl.config({render={use_fp16=' + str(fp16) + '}}); '
            'hl.monitor({output="HYPRLIQUID-TEST",mode="1280x800@60",position="auto",'
            f'scale={scale},transform={transform},bitdepth={depth},cm="srgb"' + '})')
        for preset in ('frosted-glass', 'frosted-liquid-medium', 'frosted-liquid'):
            reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'profile': {'base_preset': preset, 'theme': 'dark'}})))
            assert reply['ok'], reply
            for width, height in ((79, 36), (36, 79), (36, 36), (365, 193)):
                ctl('dispatch', 'hl.dsp.window.resize({window=' + windows[0] + f',x={width},y={height},relative=false' + '})')
                # Keep the receiver close to the bottom of every tested pane.
                ctl('dispatch', 'hl.dsp.window.move({window=' + windows[1] + f',x=200,y={300 + height},relative=false' + '})')
                ctl('eval', 'hl.config({decoration={rounding=24}})')
                time.sleep(.1)
                stem = f'{name}-{preset}-{width}x{height}'
                actual = capture(stem + '-oversized')
                # rounding_power=6 multiplies configured rounding by three.
                reference = min(24, min(width, height) // 6)
                ctl('eval', f'hl.config({{decoration={{rounding={reference}}}}})')
                time.sleep(.1)
                expected = capture(stem + '-bounded')
                equal = actual == expected
                results.append({'format': name, 'preset': preset, 'size': [width, height], 'equal': equal})
                (build / 'tiny-shadows.json').write_text(json.dumps(results, indent=2))
                assert equal, f'{stem}: oversized native rounding changes the pane, shadow or reflected neighbor'
                print(f'PASS: {stem} shadow and reflection match a bounded native radius', flush=True)
    ctl('plugin', 'unload', str(fixtures / 'capture-test.so'))


def verify_tiny_tiles(ctl, env, log, clients, build, wait, fixtures, security_plugin):
    ctl('plugin', 'load', str(security_plugin))
    ctl('eval', 'hl.config({animations={enabled=false},general={border_size=0},'
        'decoration={rounding=24,rounding_power=6,shadow={enabled=false}},'
        'cursor={no_warps=true,inactive_timeout=0.1}})')
    path = build / 'tiny-window.json'
    path.write_text(json.dumps({'root': {'alpha': 0}, 'child': {'mapped': False}}))
    process = subprocess.Popen([str(fixtures / 'subsurface-test'), 'tiny-corner-test', str(path)], env=env, stdout=log, stderr=log)
    clients.append(process)
    window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == 'tiny-corner-test'), None))
    address = json.dumps('address:' + window['address'])
    ctl('dispatch', 'hl.dsp.window.float({window=' + address + ',action="enable"})')
    ctl('dispatch', 'hl.dsp.window.move({window=' + address + ',x=160,y=160,relative=false})')
    results = []
    for name, scale, transform, depth, fp16 in (('8bit', 1, 0, 8, 2), ('10bit', 1.25, 0, 10, 2),
                                               ('fp16', 1, 0, 10, 1), ('rotated', 1.25, 1, 10, 2)):
        ctl('eval', 'hl.config({render={use_fp16=' + str(fp16) + '}}); '
            'hl.monitor({output="HYPRLIQUID-TEST",mode="1280x800@60",position="auto",'
            f'scale={scale},transform={transform},bitdepth={depth},cm="srgb"' + '})')
        for preset in ('frosted-glass', 'frosted-liquid-medium', 'frosted-liquid'):
            reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'profile': {'base_preset': preset, 'theme': 'dark'}})))
            assert reply['ok'], reply
            for width, height in ((79, 36), (36, 79), (36, 36), (365, 193)):
                ctl('dispatch', 'hl.dsp.window.resize({window=' + address + f',x={width},y={height},relative=false' + '})')
                time.sleep(.05)
                ctl('hyprliquid-test:security', 'tiny')
                result = wait(lambda: (s if (s := ctl('hyprliquid-test:security', 'status')) != 'pending' else None))
                results.append({'format': name, 'preset': preset, 'size': [width, height], 'result': result})
                (build / 'tiny-corners.json').write_text(json.dumps(results, indent=2))
                assert result == 'pass', f'{name} {preset} {width}x{height}: {result}'
                print(f'PASS: {name} {preset} {width}x{height} auxiliary corners preserve the backdrop', flush=True)
    ctl('plugin', 'unload', str(security_plugin))
