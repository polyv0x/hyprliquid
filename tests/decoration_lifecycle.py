"""Verify that hidden/unmapped decorations are destroyed before plugin dlclose."""
import json
import os
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
    upgrade_guard = os.environ.get('HYPRLIQUID_TEST_PREPARE_UPGRADE') == '1'
    for mode in ['hidden', 'unmapped', 'reused', 'hidden', 'unmapped']:
        prepared = ctl('hyprliquid-test:decorations', mode)
        assert prepared == 'ok', f'{mode}: {prepared}'
        before = list(map(int, ctl('hyprliquid-test:decorations', 'status').split()))
        assert before[0] == 1 and before[1] == 0, before
        if upgrade_guard:
            ctl('plugin', 'load', str(fixtures / 'reload-guard.so'))
        ctl('plugin', 'unload', str(plugin))
        if upgrade_guard:
            ctl('plugin', 'unload', str(fixtures / 'reload-guard.so'))
        after = list(map(int, ctl('hyprliquid-test:decorations', 'status').split()))
        result = {'mode': mode, 'before': before, 'after': after}
        results.append(result)
        (output / 'decoration-lifecycle.json').write_text(json.dumps(results, indent=2) + '\n')
        assert after[0:2] == [0, 0], f'{mode}: unloaded decoration or deleter retained: {after}'
        assert after[2] == after[3] == before[3], f'{mode}: removed a native decoration'
        assert after[4] == 0, f'{mode}: queued a native decoration for removal: {after}'
        assert ctl('hyprliquid-test:decorations', 'restore') == 'ok'
        ctl('plugin', 'load', str(plugin))
        wait(lambda: json.loads(ctl('hyprliquid:control', '{"op":"state"}'))['ok'])
    client.terminate()
    client.wait(timeout=5)
    wait(lambda: not any(w['class'] == 'hyprliquid-lifecycle' for w in json.loads(ctl('-j', 'clients'))))
    if not upgrade_guard:
        def clean_registry():
            registered, live = map(int, ctl('hyprliquid-test:decorations', 'registry').split())
            return registered == live
        wait(clean_registry)
    ctl('plugin', 'unload', str(fixture))
    print('PASS: native decorations survive five unload/remap cycles, including a reused registration', flush=True)
    if not upgrade_guard:
        print('PASS: closed windows retire their decoration registrations', flush=True)
