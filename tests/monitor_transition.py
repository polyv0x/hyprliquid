"""Move a floating pane across differently scaled outputs with 10-bit rendering."""
import gzip
import json
from pathlib import Path
import subprocess
import time


def verify_monitor_transition(ctl, env, log, clients, build, wait, fixtures):
    def control(op, **fields):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))
        assert reply['ok'], reply
        return reply

    def action(window, name, fields):
        ctl('dispatch', 'hl.dsp.window.' + name + '({window=' + json.dumps('address:' + window['address']) + ',' + fields + '})')

    def capture(name):
        path = build / name
        assert ctl('hyprliquid-test:capture', 'monitor HYPRLIQUID-TEST ' + str(path)) == 'ok'
        wait(lambda: path.with_suffix('.json').exists())
        metadata = json.loads(path.with_suffix('.json').read_text())
        assert metadata['width'] == 3840 and metadata['height'] == 2160 and metadata['gl_error'] == 0, metadata
        pixels = path.with_suffix('.raw').read_bytes()
        with gzip.open(path.with_suffix('.raw.gz'), 'wb') as output:
            output.write(pixels)
        path.with_suffix('.raw').unlink()
        return pixels, metadata

    def spawn(name, state):
        path = build / (name + '.json')
        path.write_text(json.dumps(state))
        process = subprocess.Popen([str(fixtures / 'subsurface-test'), name, str(path)], env=env, stdout=log, stderr=log)
        clients.append(process)
        window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == name), None))
        return process, window

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    ctl('dismissnotify')
    ctl('eval', 'hl.config({animations={enabled=false}, general={border_size=0,gaps_in=8,gaps_out=12}, '
        'decoration={active_opacity=1,inactive_opacity=1,dim_inactive=false,shadow={enabled=false}}, '
        'cursor={no_warps=true,inactive_timeout=0.1},render={use_fp16=0}}); '
        'hl.monitor({output="HYPRLIQUID-TEST",mode="3840x2160@160",position="0x0",scale=1.25,bitdepth=10,cm="srgb"}); '
        'hl.monitor({output="HYPRLIQUID-UP",mode="2560x1440@120",position="0x-1440",scale=1,bitdepth=10,cm="hdr"})')
    assert ctl('output', 'create', 'headless', 'HYPRLIQUID-UP') == 'ok'
    monitors = wait(lambda: (m if len(m := json.loads(ctl('-j', 'monitors'))) == 2 else None))
    upper = next(m for m in monitors if m['name'] == 'HYPRLIQUID-UP')
    lower = next(m for m in monitors if m['name'] == 'HYPRLIQUID-TEST')
    ctl('dispatch', 'hl.dsp.focus({monitor="HYPRLIQUID-TEST"})')
    control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark'})
    _, tile = spawn('transition-tile', {'root': {'alpha': 70, 'color': 0x183048}, 'child': {'mapped': False}})
    time.sleep(.3)
    control('apply', patch={})
    reference, _ = capture('reference-alone')
    process, floating = spawn('transition-float', {'root': {'alpha': 210, 'color': 0xa04020}, 'child': {'mapped': False}})
    action(floating, 'float', 'action="enable"')
    action(floating, 'resize', 'x=1512,y=923,relative=false')
    results = []
    for cycle in range(3):
        action(floating, 'move', 'workspace=' + str(lower['activeWorkspace']['id']) + ',follow=false')
        for y in (180, 0, -180, -600, -947):
            action(floating, 'move', f'x=213,y={y},relative=false')
            time.sleep(.06)
        action(floating, 'move', 'workspace=' + str(upper['activeWorkspace']['id']) + ',follow=false')
        action(floating, 'move', 'x=213,y=-947,relative=false')
        wait(lambda: next(w for w in json.loads(ctl('-j', 'clients')) if w['address'] == floating['address'])['monitor'] == upper['id'])
        time.sleep(.2)
        # A screenshot repaints the target but must not rebuild a stale material
        # cache. The original fault survives screenshots and ordinary commits.
        pixels, _ = capture(f'moved-{cycle}')
        mismatches = sum(a != b for a, b in zip(reference, pixels))
        results.append({'cycle': cycle, 'mismatched_bytes': mismatches})
        (build / 'monitor-transition.json').write_text(json.dumps(results, indent=2))
        print(f'{"PASS" if not mismatches else "FAIL"}: monitor move {cycle}: {mismatches} differing bytes', flush=True)
    # Headless outputs may fall back from requested HDR to SDR; preserve the
    # actual framebuffer descriptions alongside the movement comparisons.
    (build / 'final-buffers.json').write_text(json.dumps(control('buffers'), indent=2))
    (build / 'final-clients.json').write_text(ctl('-j', 'clients'))
    assert not any(r['mismatched_bytes'] for r in results), 'Moving the float left a partial material fill'
    process.terminate()
    process.wait(timeout=5)
    ctl('plugin', 'unload', str(fixtures / 'capture-test.so'))
