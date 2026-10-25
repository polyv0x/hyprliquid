"""Verify that hidden/unmapped decorations are destroyed before plugin dlclose."""
import json
import subprocess


def verify_decoration_lifecycle(ctl, env, log, clients, output, wait, fixtures, plugin):
    fixture = fixtures / 'decoration-lifecycle-test.so'
    assert fixture.exists(), 'Build build/decoration-lifecycle-test.so first'
    ctl('plugin', 'load', str(fixture))
    client = subprocess.Popen([
        'kitty', '--class', 'hyprliquid-lifecycle', '-o', 'remember_window_size=no',
        '-o', 'cursor_blink_interval=0', 'sleep', '120',
    ], env=env, stdout=log, stderr=log)
    clients.append(client)
    wait(lambda: any(w['class'] == 'hyprliquid-lifecycle' for w in json.loads(ctl('-j', 'clients'))))
    results = []
    for mode in ['hidden', 'unmapped', 'hidden', 'unmapped']:
        assert ctl('hyprliquid-test:decorations', mode) == 'ok'
        before = list(map(int, ctl('hyprliquid-test:decorations', 'status').split()))
        assert before[0] == 1 and before[1] == 0, before
        ctl('plugin', 'unload', str(plugin))
        after = list(map(int, ctl('hyprliquid-test:decorations', 'status').split()))
        result = {'mode': mode, 'before': before, 'after': after}
        results.append(result)
        (output / 'decoration-lifecycle.json').write_text(json.dumps(results, indent=2) + '\n')
        assert after[0:2] == [0, 0], f'{mode}: unloaded decoration or deleter retained: {after}'
        assert after[2] == after[3] == before[3], f'{mode}: removed a native decoration'
        assert ctl('hyprliquid-test:decorations', 'restore') == 'ok'
        ctl('plugin', 'load', str(plugin))
        wait(lambda: json.loads(ctl('hyprliquid:control', '{"op":"state"}'))['ok'])
    ctl('plugin', 'unload', str(fixture))
    client.terminate()
    client.wait(timeout=5)
    print('PASS: hidden/unmapped windows survive four unload, remap and reload cycles', flush=True)
