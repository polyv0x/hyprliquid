"""Compare retained glass with a rebuild at identical native animation values."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import time


def verify_native_style(ctl, env, log, clients, output, wait, fixtures, plugin, config_path):
    evidence = {}
    saved_config = config_path.read_text()
    native_config = []

    def configure(code):
        native_config.append(code)
        ctl('eval', code)

    def control(op, **values):
        result = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
        assert result['ok'], result
        return result

    def animation(action='status'):
        result = json.loads(ctl('hyprliquid-test:animation', action))
        assert 'error' not in result, result
        return result

    def action(window, name, arguments=''):
        dispatcher = 'hl.dsp.focus' if name == 'focus' else 'hl.dsp.window.' + name
        ctl('dispatch', dispatcher + '({ window = ' +
            json.dumps('address:' + window['address']) + (', ' + arguments if arguments else '') + ' })')

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

    def raw(name, trigger=None):
        path = output / ('style-' + name)
        ctl('hyprliquid-test:capture', ('next presented ' if trigger else 'presented ') + str(path))
        if trigger:
            trigger()
        wait(lambda: path.with_suffix('.json').exists())
        metadata = json.loads(path.with_suffix('.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        pixels = path.with_suffix('.raw').read_bytes()
        assert any(pixels), name
        with gzip.open(str(path) + '.raw.gz', 'wb') as compressed:
            compressed.write(pixels)
        path.with_suffix('.raw').unlink()
        state = animation()
        assert state['frozen'] and len(state['gradients']) >= 6, state
        return pixels, metadata, state

    def phase(state):
        return state['tick_serial'], state['gradients']

    def check(name, trigger=None, transition=None, compare_update=True):
        pixels, metadata, before = raw(name + '-cached', trigger)
        repaint, _, repainted = raw(name + '-repaint')
        control('apply', patch={})
        rebuilt, _, after = raw(name + '-rebuilt')
        # Retain all three frames even when an equality check fails.
        assert phase(repainted) == phase(before), (name, 'native phase moved during repaint', before, repainted)
        assert phase(after) == phase(before), (name, 'native phase moved during rebuild', before, after)
        if compare_update:
            assert repaint == pixels, name + ': partial presented pixels differ from repaint'
        assert rebuilt == repaint, name + ': cached repaint pixels differ from rebuild'
        states = list(before['gradients'].values())
        if transition is True:
            assert any(s['transitioning'] and 0 < s['progress'] < 1 for s in states), (name, before)
        elif transition is False:
            assert not any(s['transitioning'] for s in states), (name, before)
        evidence[name] = {'sha256': hashlib.sha256(repaint).hexdigest(), 'metadata': metadata, 'state': before,
                          'partial_equality_required': compare_update,
                          'first_update_differing_bytes': sum(a != b for a, b in zip(pixels, repaint))}
        comparisons = 'partial/repaint/rebuild' if compare_update else 'repaint/rebuild'
        print('PASS: native style ' + name + ' frozen-state ' + comparisons + ' pixels', flush=True)
        return repaint, before

    def configure_animation(enabled, angles=False):
        configure('hl.config({ animations = { enabled = ' + ('true' if enabled else 'false') + ' } })')
        configure('hl.animation({ leaf = "global", enabled = false })')
        for leaf in ('border', 'fadeShadow', 'fadeGlow'):
            configure('hl.animation({ leaf = ' + json.dumps(leaf) +
                ', enabled = true, speed = 20, bezier = "linear" })')
        for leaf in ('borderangle', 'shadowangle', 'glowangle'):
            configure('hl.animation({ leaf = ' + json.dumps(leaf) + ', enabled = ' +
                ('true, speed = 10, bezier = "linear", style = "loop"' if angles else 'false') + ' })')

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    processes, paths, windows = [], [], []
    try:
        configure('hl.config({ animations = { enabled = false }, general = { border_size = 8, '
            '["col.active_border"] = { colors = { "rgba(ff3040ff)", "rgba(40a0ffff)" }, angle = 30 }, '
            '["col.inactive_border"] = { colors = { "rgba(20c060ff)", "rgba(d08020ff)" }, angle = 110 } }, '
            'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, '
            'shadow = { enabled = true, range = 48, color = { colors = { "rgba(ff2040cc)", "rgba(4080ffcc)" }, angle = 40 }, '
            'color_inactive = { colors = { "rgba(20e060cc)", "rgba(d08020cc)" }, angle = 120 } }, '
            'glow = { enabled = true, range = 20, color = { colors = { "rgba(2040ffcc)", "rgba(e0a020cc)" }, angle = 60 }, '
            'color_inactive = { colors = { "rgba(c04020cc)", "rgba(20e0c0cc)" }, angle = 160 } } }, '
            'cursor = { no_warps = true, inactive_timeout = 0.1 }, render = { use_fp16 = 2 } })')
        control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark',
                                 'animate_display_angle': False, 'animate_hover_angle': False})
        for index in range(2):
            path = output / f'native-style-{index}.json'
            path.write_text('{}')
            process = subprocess.Popen([str(fixtures / 'subsurface-test'), f'native-style-{index}', str(path)],
                                       env=env, stdout=log, stderr=log)
            clients.append(process)
            processes.append(process)
            window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == f'native-style-{index}'), None))
            action(window, 'float', 'action = "enable"')
            action(window, 'resize', 'x = 560, y = 340, relative = false')
            action(window, 'move', f'x = {48 + index * 572}, y = 120, relative = false')
            paths.append(path)
            windows.append(window)
        ctl('dismissnotify')
        time.sleep(.4)
        animation('freeze')
        static, _ = check('static', transition=False)
        configure_animation(True)
        for index in (0, 1):
            check(f'focus-{index}-initial', lambda: action(windows[index], 'focus'))
            time.sleep(.2)
            intermediate, intermediate_state = check(f'focus-{index}-intermediate', lambda: animation('advance'), transition=True)
            if index == 0:
                # Plugin reload rereads compositor configuration. Persist the
                # native targets/settings so it cannot replace our test style.
                config_path.write_text(saved_config + '\n' + '\n'.join(native_config) + '\n')
                ctl('plugin', 'unload', str(plugin))
                ctl('plugin', 'load', str(plugin))
                control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark',
                                         'animate_display_angle': False, 'animate_hover_angle': False})
                ctl('dismissnotify')
                reloaded, reloaded_state = check('reload-during-fade', transition=True)
                assert phase(reloaded_state) == phase(intermediate_state), 'Native gradient discovery changed the fade'
                assert reloaded == intermediate, 'Reload changed frozen native appearance'
            time.sleep(2.1)
            final, _ = check(f'focus-{index}-final', lambda: animation('advance'), transition=False)
            assert intermediate != final, 'Native focus animation did not change visible pixels'
        configure_animation(True, angles=True)
        # Focus restarts native angle loops; observation checks actual values.
        action(windows[0], 'focus')
        time.sleep(.25)
        # Native-only controls also show first-angle-update differences: 0.57
        # with the plugin fully unloaded, and 0.56 with glass disabled. Disabling
        # dithering did not eliminate the old-host case; its cause is unresolved.
        # Record that native first-frame difference separately; the retained
        # repaint and forced rebuild still must match every byte at this phase.
        angle, angle_state = check('angle-intermediate', lambda: animation('advance'), compare_update=False)
        time.sleep(1.1)
        wrapped, wrapped_state = check('angle-wrap', lambda: animation('advance'), compare_update=False)
        assert angle_state['gradients'] != wrapped_state['gradients'] and angle != wrapped, 'Angle loop was not visible'
        configure_animation(False)
        disabled, _ = check('animations-disabled', lambda: animation('advance'), transition=False, compare_update=False)
        # Global animation disable takes effect on the next native tick. Set
        # the target first, then capture that tick's immediate final value.
        action(windows[1], 'focus')
        instant, _ = check('instant-focus', lambda: animation('advance'), transition=False)
        assert instant != disabled, 'Instant target replacement did not change the native appearance'
        for name, config in (
            ('shadow-shape', 'shadow = { range = 64, offset = "12 8", scale = 0.9, render_power = 2, sharp = true }'),
            ('glow-shape', 'glow = { range = 36, render_power = 2 }')):
            check(name, lambda config=config: ctl('eval', 'hl.config({ decoration = { ' + config + ' } })'), transition=False)
        rules = ['no_shadow', 'decorate']
        if animation()['resolved_gradients']:
            rules.insert(1, 'no_glow')
        for rule in rules:
            appearances = []
            for value in ('false', 'true'):
                pixels, _ = check(rule + '-' + value, lambda rule=rule, value=value: ctl('eval',
                    'hl.window_rule({ name = "native-style-' + rule + '", match = { class = "^native-style-0$" }, ' + rule + ' = ' + value + ' })'),
                    transition=False)
                appearances.append(pixels)
            assert appearances[0] != appearances[1], rule + ': native rule had no visible effect'
        ctl('eval', 'hl.config({ decoration = { '
            'shadow = { color = "rgba(00000000)", color_inactive = "rgba(00000000)" }, '
            'glow = { color = "rgba(00000000)", color_inactive = "rgba(00000000)" } } })')
        check('transparent-effects', lambda: animation('advance'), transition=False)

        # Return to a visible ordinary native style before testing reuse.
        ctl('eval', 'hl.config({ decoration = { shadow = { color = "rgba(202040aa)", color_inactive = "rgba(202040aa)" } } })')
        check('reuse-warm', lambda: animation('advance'), transition=False)
        def counters():
            return {item['owner']: item for item in control('buffers')['buffers']['windows'] if item['enabled']}
        owner = 'window:' + windows[0]['address']
        before = counters()[owner]
        for index in range(6):
            write(paths[0], {'child': {'color': 0xa02050 if index % 2 else 0x20a050}})
            time.sleep(.05)
        after = counters()[owner]
        for key in ('captures', 'blurs', 'material_draws'):
            assert after[key] == before[key], (key, before, after)
        for key in ('window_backdrop_reuses', 'material_cache_reuses'):
            assert after[key] > before[key], (key, before, after)
        evidence['foreground-reuse'] = {'before': before, 'after': after}
        print('PASS: unchanged native style preserves foreground cache reuse', flush=True)
        animation('thaw')
        ctl('plugin', 'unload', str(fixtures / 'capture-test.so'))
        ctl('plugin', 'unload', str(plugin))
        ctl('plugin', 'load', str(plugin))
        action(windows[1], 'focus')
        control('buffers')
        print('PASS: animation and native-gradient hooks unload/reload cleanly', flush=True)
    finally:
        config_path.write_text(saved_config)
        (output / 'native-style.json').write_text(json.dumps(evidence, indent=2))
        for process in processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
