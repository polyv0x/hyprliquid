#!/usr/bin/env python3
"""Compare renderer builds in one disposable compositor, with exact raw pixels.

make -j4 all build/capture-test.so build/layer-test
python3 scripts/benchmark-vram.py --baseline /path/to/clean/hyprliquid.so \
    --reference /path/to/accounting-reference/hyprliquid.so --output build/vram-results
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('nested', ROOT / 'scripts/test-nested.py')
nested = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nested)


def drm(pid):
    """Only this compositor, deduplicated by PCI device and DRM client ID."""
    clients = {}
    denied = 0
    for path in (Path('/proc') / str(pid) / 'fdinfo').iterdir():
        try:
            fields = dict(line.split(':', 1) for line in path.read_text().splitlines() if line.startswith('drm-'))
        except OSError:
            denied += 1
            continue
        fields = {k: v.strip() for k, v in fields.items()}
        if 'drm-driver' in fields:
            clients[(fields.get('drm-pdev'), fields.get('drm-client-id'))] = fields
    return {'pid': pid, 'clients': list(clients.values()), 'inaccessible_fds': denied}


def distribution(samples):
    ordered = sorted(samples)
    return {'count': len(samples), 'median_ms': statistics.median(ordered),
            'p95_ms': ordered[min(len(ordered) - 1, int(len(ordered) * .95))],
            'max_ms': max(ordered)} if ordered else {'count': 0}


def device_stats(snapshot):
    """System-wide context only; process GPU work comes from deduplicated fdinfo."""
    result = {}
    for client in snapshot['clients']:
        device = client.get('drm-pdev')
        if not device or device in result:
            continue
        root = Path('/sys/bus/pci/devices') / device
        values = {}
        for name in ('gpu_busy_percent', 'mem_info_vram_used', 'mem_info_gtt_used', 'pp_dpm_sclk', 'pp_dpm_mclk'):
            try: values[name] = (root / name).read_text().strip()
            except OSError: pass
        for hwmon in (root / 'hwmon').glob('hwmon*'):
            for name in ('power1_average', 'temp1_input'):
                try: values[name] = (hwmon / name).read_text().strip()
                except OSError: pass
        result[device] = values
    return result


def engine_deltas(before, after, elapsed_ns):
    first = {(c.get('drm-pdev'), c.get('drm-client-id')): c for c in before['clients']}
    result = []
    for client in after['clients']:
        key = (client.get('drm-pdev'), client.get('drm-client-id'))
        previous = first.get(key, {})
        for name, value in client.items():
            if not name.startswith('drm-engine-') or name not in previous:
                continue
            # DRM counters are cumulative ns. A reset/disappearing client is
            # unavailable, never negative work or another engine's percentage.
            delta = int(value.split()[0]) - int(previous[name].split()[0])
            if delta >= 0:
                result.append({'device': key[0], 'client_id': key[1], 'engine': name,
                               'elapsed_ns': delta, 'ms_per_second': delta / elapsed_ns * 1000})
    return result


def set_pattern(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data))
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, default=ROOT / 'hyprliquid.so')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quick', action='store_true', help='Skip individual parameter-extreme cases')
    parser.add_argument('--steady-only', action='store_true', help='Measure the fixed warm scene without the visual/lifecycle matrix')
    parser.add_argument('--steady-preset', default='lens', help='Material for --steady-only (default: lens)')
    parser.add_argument('--steady-mode', default='1280x800@60', help='Output mode for --steady-only (default: 1280x800@60)')
    parser.add_argument('--steady-bitdepth', type=int, choices=(8, 10), default=8, help='Output bit depth for --steady-only')
    parser.add_argument('--steady-panes', type=int, default=2, help='Pane count for --steady-only (default: 2)')
    parser.add_argument('--steady-overlap', action='store_true', help='Stack two floating panes and animate the background pane')
    parser.add_argument('--steady-video-stack', action='store_true', help='Transparent tiles below an opaque animated float and a transparent receiving float (at least three panes)')
    parser.add_argument('--steady-video-edge', action='store_true', help='With --steady-video-stack, place a smaller video beside a tall receiving pane')
    parser.add_argument('--steady-scale', type=float, default=1, help='Output scale for the steady workload')
    parser.add_argument('--steady-fp16', action='store_true', help='Use an FP16 internal framebuffer for steady pixel comparisons')
    parser.add_argument('--steady-transform', type=int, choices=range(8), default=0, help='Output transform for steady pixel comparisons')
    parser.add_argument('--verify-steady-pixels', action='store_true', help='Compare backdrop phases and cache rebuilds, and compare arms unless --only-arm is set')
    parser.add_argument('--client-update-hz', type=float, default=40, help='Changing client repaint rate (default: 40 Hz)')
    parser.add_argument('--animate-top-layer', action='store_true', help='Repaint a top layer independently at 60 Hz during steady timing')
    parser.add_argument('--opaque-clients', action='store_true', help='Use fully opaque client surfaces to exercise occlusion')
    parser.add_argument('--x11-clients', action='store_true', help='Use the disposable compositor’s Xwayland server for pattern clients')
    parser.add_argument('--warmup-seconds', type=float, default=1, help='Foreground repaint warmup before sampling')
    parser.add_argument('--sample-seconds', type=float, default=6, help='Duration of foreground repaint sampling')
    parser.add_argument('--native-blur', action='store_true', help='With --steady-only, disable Liquid and enable native blur (size 8, three passes)')
    parser.add_argument('--native-unloaded', action='store_true', help='With --native-blur, also unload the plugin for a native-only reference')
    parser.add_argument('--only-arm', choices=('baseline', 'reference', 'candidate'),
                        help='With --steady-only, measure one build in a fresh compositor for comparable DRM residency')
    args = parser.parse_args()
    if args.only_arm and not args.steady_only:
        parser.error('--only-arm requires --steady-only')
    if args.native_blur and not args.steady_only:
        parser.error('--native-blur requires --steady-only')
    if args.native_unloaded and not args.native_blur:
        parser.error('--native-unloaded requires --native-blur')
    if args.steady_overlap and (not args.steady_only or args.steady_panes != 2):
        parser.error('--steady-overlap requires --steady-only and two panes')
    if args.steady_video_stack and (not args.steady_only or args.steady_panes < 3 or args.steady_overlap):
        parser.error('--steady-video-stack requires --steady-only and at least three panes without --steady-overlap')
    if args.steady_video_edge and not args.steady_video_stack:
        parser.error('--steady-video-edge requires --steady-video-stack')
    if not 0.5 <= args.steady_scale <= 3 or ((args.steady_scale != 1 or args.steady_fp16) and not args.steady_only):
        parser.error('--steady-scale must be between 0.5 and 3; scale/FP16 overrides require --steady-only')
    if args.verify_steady_pixels and (not args.steady_only or args.native_blur):
        parser.error('--verify-steady-pixels requires --steady-only without --native-blur')
    if args.steady_transform and not args.steady_only:
        parser.error('--steady-transform requires --steady-only')
    if args.steady_panes < 1:
        parser.error('--steady-panes must be positive')
    if not 1 <= args.client_update_hz <= 240:
        parser.error('--client-update-hz must be between 1 and 240')
    update_interval = 1 / args.client_update_hz
    if args.warmup_seconds < 0 or args.sample_seconds < 3:
        parser.error('--warmup-seconds must be nonnegative and --sample-seconds at least 3')
    if not args.steady_only and (args.steady_preset != 'lens' or args.steady_panes != 2 or args.steady_mode != '1280x800@60' or args.steady_bitdepth != 8):
        parser.error('--steady-preset, --steady-panes, --steady-mode and --steady-bitdepth require --steady-only')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    arms = {key: getattr(args, key).resolve() for key in ('baseline', 'reference', 'candidate')}
    if args.only_arm: arms = {args.only_arm: arms[args.only_arm]}
    results = {'versions': subprocess.check_output(['Hyprland', '--version'], text=True),
               'workload': {'preset': args.steady_preset, 'panes': args.steady_panes, 'bitdepth': args.steady_bitdepth,
                            'warmup_seconds': args.warmup_seconds, 'sample_seconds': args.sample_seconds,
                            'native_blur': args.native_blur,
                            'native_unloaded': args.native_unloaded,
                            'client_update_interval_seconds': update_interval,
                            'animate_top_layer': args.animate_top_layer, 'opaque_clients': args.opaque_clients,
                            'overlap': args.steady_overlap, 'transform': args.steady_transform, 'video_edge': args.steady_video_edge,
                            'video_stack': args.steady_video_stack, 'scale': args.steady_scale, 'fp16': args.steady_fp16,
                            'x11_clients': args.x11_clients},
               'builds': {key: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                          for key, path in arms.items()}, 'arms': {}, 'comparisons': []}
    (output / 'result.json').write_text(json.dumps(results, indent=2))

    with tempfile.TemporaryDirectory(prefix='hlq-', dir='/tmp') as temporary:
        runtime = Path(temporary)
        env = os.environ.copy()
        display = Path(env.get('WAYLAND_DISPLAY', 'wayland-1'))
        if not display.is_absolute():
            display = Path(env.get('XDG_RUNTIME_DIR', '/run/user/1000')) / display
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(runtime / 'config'),
                   XDG_CACHE_HOME=str(runtime / 'cache'), XDG_STATE_HOME=str(runtime / 'state'),
                   WAYLAND_DISPLAY=str(display), HYPRLAND_NO_SD_VARS='1', HYPRLAND_NO_SD_NOTIFY='1',
                   LIBSEAT_BACKEND='seatd', SEATD_SOCK=str(runtime / 'no-seatd.sock'))
        env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        clients = []
        with (output / 'nested.log').open('w') as log:
            compositor = subprocess.Popen(['Hyprland', '--config', str(ROOT / 'tests/nested.lua')],
                                          env=env, stdout=log, stderr=log)
            try:
                wait = lambda predicate: nested.wait_for(predicate, compositor)
                socket = wait(lambda: next(runtime.glob('hypr/*/.socket.sock'), None))
                env['HYPRLAND_INSTANCE_SIGNATURE'] = socket.parent.name
                env['WAYLAND_DISPLAY'] = str(wait(lambda: next((p for p in runtime.glob('wayland-*') if p.is_socket()), None)))

                def ctl(*words):
                    result = subprocess.run(['hyprctl', '-i', socket.parent.name, *words], env=env,
                                            text=True, capture_output=True, timeout=15)
                    if result.returncode:
                        raise RuntimeError(f'{words}: {result.stdout}{result.stderr}')
                    if words[0] in ('eval', 'dispatch', 'plugin') and result.stdout.strip() != 'ok':
                        raise RuntimeError(f'{words}: {result.stdout}')
                    return result.stdout.strip()

                def control(op, **values):
                    response = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
                    assert response['ok'], response
                    return response

                def action(window, name, fields):
                    ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' +
                        json.dumps('address:' + window['address']) + ', ' + fields + ' })')

                parents = wait(lambda: json.loads(ctl('-j', 'monitors')))
                assert ctl('output', 'create', 'headless', 'HYPRLIQUID-TEST') == 'ok'
                for monitor in parents:
                    assert ctl('output', 'remove', monitor['name']) == 'ok'
                wait(lambda: len(json.loads(ctl('-j', 'monitors'))) == 1)
                ctl('plugin', 'load', str(ROOT / 'build/capture-test.so'))

                client_env = env.copy()
                if args.x11_clients:
                    def x11_environment():
                        children = Path(f'/proc/{compositor.pid}/task/{compositor.pid}/children').read_text().split()
                        for child in children:
                            try: argv = Path(f'/proc/{child}/cmdline').read_bytes().decode().strip('\0').split('\0')
                            except OSError: continue
                            if not argv or Path(argv[0]).name != 'Xwayland': continue
                            display = next((word for word in argv[1:] if word.startswith(':')), None)
                            if display:
                                result = {'DISPLAY': display}
                                if '-auth' in argv: result['XAUTHORITY'] = argv[argv.index('-auth') + 1]
                                return result
                    client_env.pop('XAUTHORITY', None)
                    client_env.update(wait(x11_environment))

                controls = []
                pattern = r'''
import json, shutil, sys, time
from pathlib import Path
path = Path(sys.argv[1])
previous = None
while True:
    size, data = shutil.get_terminal_size(), json.loads(path.read_text())
    state = (size, data)
    if state != previous:
        previous = state
        out = '\x1b[?25l\x1b[0m\x1b[2J'
        for y in range(size.lines):
            out += '\x1b[%d;1H' % (y + 1)
            if (y // 3) % 2 == data['phase']:
                out += '\x1b[48;2;%d;%d;%dm' % tuple(data['color']) + ' ' * (size.columns - 1) + '\x1b[0m'
        print(out, end='', flush=True)
    time.sleep(.01)
'''
                colors = tuple(([220, 40, 20], [20, 200, 70])[i % 2] for i in range(args.steady_panes))
                def spawn_panes():
                    for side, color in enumerate(colors):
                        path = runtime / f'color-{side}.json'
                        set_pattern(path, {'color': color, 'phase': side % 2})
                        if len(controls) <= side: controls.append(path)
                        clients.append(subprocess.Popen(['kitty', '--start-as=normal', '--class', f'vram-pane-{side}',
                            '-o', 'background_opacity=' + ('1' if args.opaque_clients or (args.steady_video_stack and side == args.steady_panes - 2) else '0'), '-o', 'remember_window_size=no',
                            '-o', 'cursor_blink_interval=0', '-o', 'font_size=16',
                            '-o', 'linux_display_server=' + ('x11' if args.x11_clients else 'wayland'),
                            'python3', '-c', pattern, str(path)], env=client_env, stdout=log, stderr=log))
                        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == side + 1)

                def capture(directory, name):
                    path = directory / name
                    assert ctl('hyprliquid-test:capture', str(path)) == 'ok'
                    metadata_path = Path(str(path) + '.json')
                    raw_path = Path(str(path) + '.raw')
                    wait(lambda: metadata_path.exists())
                    metadata = json.loads(metadata_path.read_text())
                    assert metadata['gl_error'] == 0, metadata
                    raw = raw_path.read_bytes()
                    pixel_bytes = metadata['pixel_bytes']
                    assert any(raw[i:i + pixel_bytes] != raw[:pixel_bytes] for i in range(pixel_bytes, len(raw), pixel_bytes)), f'Capture is blank: {path}'
                    # Store lossless artifacts without retaining hundreds of MiB
                    # of redundant uncompressed backgrounds.
                    with gzip.open(Path(str(path) + '.raw.gz'), 'wb') as compressed:
                        compressed.write(raw)
                    raw_path.unlink()
                    arm_result['settings'][name] = {'state': control('state')['state'],
                        'clients': json.loads(ctl('-j', 'clients')), 'monitors': json.loads(ctl('-j', 'monitors'))}
                    if has_buffers:
                        diagnostic = control('buffers')['buffers']
                        diagnostic.pop('cpu_frame_ms')
                        arm_result['settings'][name]['buffers'] = diagnostic
                    return {'metadata': metadata, 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}

                previous = None
                reference_geometry = None
                for arm, plugin in arms.items():
                    directory = output / arm
                    directory.mkdir()
                    for client in clients:
                        if client.poll() is None:
                            client.terminate()
                            client.wait(timeout=5)
                    wait(lambda: not json.loads(ctl('-j', 'clients')))
                    if previous: ctl('plugin', 'unload', str(previous))
                    ctl('plugin', 'load', str(plugin))
                    previous = plugin
                    ctl('dismissnotify')
                    ctl('eval', 'hl.config({ general = { border_size = 0, gaps_in = 8, gaps_out = 20 }, '
                        'dwindle = { force_split = 2 }, '
                        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, '
                        'shadow = { enabled = true } }, cursor = { no_warps = true, inactive_timeout = 0.1 } })')
                    mode = args.steady_mode if args.steady_only else '1280x800@60'
                    ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = ' + json.dumps(mode) +
                        ', position = "auto", scale = ' + str(args.steady_scale) + ', transform = ' + str(args.steady_transform) + ', bitdepth = ' + str(args.steady_bitdepth) + ' })')
                    if args.steady_fp16:
                        ctl('eval', 'hl.config({ render = { use_fp16 = 1 } })')
                    control('apply', profile={'base_preset': args.steady_preset, 'theme': 'dark'})
                    if args.native_blur:
                        control('apply', patch={'enabled': False})
                        ctl('eval', 'hl.config({ decoration = { blur = { enabled = true, size = 8, passes = 3, new_optimizations = true } } })')
                        if args.native_unloaded:
                            ctl('plugin', 'unload', str(plugin))
                            previous = None
                    spawn_panes()
                    if args.steady_overlap or args.steady_video_stack:
                        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: int(w['class'].rsplit('-', 1)[1]))
                        output_monitor = json.loads(ctl('-j', 'monitors'))[0]
                        width, height = output_monitor['width'] / args.steady_scale, output_monitor['height'] / args.steady_scale
                        if args.steady_transform % 2:
                            width, height = height, width
                        positions = ((.51, .3, .22, .22), (.13, .15, .37, .7)) if args.steady_video_edge else (
                            (.19, .18, .62, .66), (.27, .27, .5, .48))
                        for window, (x, y, w, h) in zip(windows[-2:], positions):
                            action(window, 'float', 'action = "enable"')
                            action(window, 'resize', f'x = {int(width*w)}, y = {int(height*h)}, relative = false')
                            action(window, 'move', f'x = {int(width*x)}, y = {int(height*y)}, relative = false')
                        action(windows[-1], 'alter_zorder', 'mode = "top"')
                    time.sleep(.6)
                    arm_result = {'captures': {}, 'settings': {}, 'steady_drm': [], 'timing': {}}
                    results['arms'][arm] = arm_result
                    has_buffers = not args.native_unloaded and 'buffers' in control('describe')['capabilities']
                    arm_result['geometry'] = json.loads(ctl('-j', 'clients'))
                    assert all(w['xwayland'] == args.x11_clients for w in arm_result['geometry']), 'Client backend differs from requested workload'
                    geometry = sorted((w['class'], w['at'], w['size']) for w in arm_result['geometry'])
                    if args.steady_panes == 2 and mode == '1280x800@60' and not args.steady_overlap and args.steady_scale == 1 and args.steady_transform == 0:
                        assert geometry == [('vram-pane-0', [20, 20], [612, 760]),
                                            ('vram-pane-1', [648, 20], [612, 760])], geometry
                    if reference_geometry is None:
                        reference_geometry = geometry
                    assert geometry == reference_geometry, 'Workload geometry changed between builds'
                    top_layer = None
                    if args.animate_top_layer:
                        top_layer = subprocess.Popen([str(ROOT / 'build/layer-test'), '--animate'],
                            env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
                        clients.append(top_layer)
                        wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
                    # Real foreground damage with fixed geometry/materials.
                    changing = len(controls) - 2 if args.steady_video_stack else 0 if args.steady_overlap else len(controls) - 1
                    warm_until = time.monotonic() + args.warmup_seconds
                    iteration = 0
                    while time.monotonic() < warm_until:
                        set_pattern(controls[changing], {'color': colors[changing], 'phase': iteration % 2})
                        iteration += 1
                        time.sleep(update_interval)
                    if has_buffers: arm_result['warm'] = control('buffers')['buffers']
                    if args.opaque_clients and not args.native_blur and has_buffers:
                        panes = arm_result['warm']['windows']
                        if any('occluded_draws' in pane for pane in panes):
                            assert any(pane.get('occluded_draws', 0) for pane in panes), 'Opaque-client workload did not exercise occlusion'
                    arm_result['warm_drm'] = drm(compositor.pid)
                    gpu_started = time.monotonic_ns()
                    ctl('hyprliquid-test:timing')
                    arm_result['gpu_samples'] = []
                    sample_until = time.monotonic() + args.sample_seconds
                    next_sample = time.monotonic()
                    while time.monotonic() < sample_until:
                        set_pattern(controls[changing], {'color': colors[changing], 'phase': iteration % 2})
                        iteration += 1
                        if time.monotonic() >= next_sample:
                            snapshot = drm(compositor.pid)
                            arm_result['gpu_samples'].append({'monotonic_ns': time.monotonic_ns(), 'unix_ns': time.time_ns(),
                                                             'drm': snapshot, 'device': device_stats(snapshot)})
                            next_sample = time.monotonic() + 1
                        time.sleep(update_interval)
                    gpu_finished = time.monotonic_ns()
                    arm_result['gpu_end_drm'] = drm(compositor.pid)
                    arm_result['gpu_interval_ns'] = gpu_finished - gpu_started
                    arm_result['gpu_engines'] = engine_deltas(arm_result['warm_drm'], arm_result['gpu_end_drm'],
                                                             arm_result['gpu_interval_ns'])
                    arm_result['timing'] = json.loads(ctl('hyprliquid-test:timing'))
                    arm_result['timing']['distribution'] = distribution(arm_result['timing']['cpu_frame_ms'])
                    assert len(json.loads(ctl('-j', 'clients'))) == args.steady_panes, 'A pattern client exited'
                    assert arm_result['timing']['distribution']['count'] >= 80, 'Insufficient rendered timing samples'
                    if has_buffers: arm_result['steady'] = control('buffers')['buffers']
                    for _ in range(3):
                        arm_result['steady_drm'].append(drm(compositor.pid))
                        time.sleep(.1)
                    for side in range(len(colors)): set_pattern(controls[side], {'color': colors[side], 'phase': side % 2})
                    if top_layer:
                        top_layer.terminate()
                        top_layer.wait(timeout=5)
                        wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
                    if args.steady_only:
                        if args.verify_steady_pixels:
                            for phase in (0, 1):
                                set_pattern(controls[changing], {'color': colors[changing], 'phase': phase})
                                time.sleep(.3)
                                for state in ('cached', 'rebuilt'):
                                    if state == 'rebuilt':
                                        control('apply', patch={})
                                        time.sleep(.3)
                                    name = f'steady-phase-{phase}-{state}'
                                    arm_result['captures'][name] = capture(directory, name)
                                assert arm_result['captures'][f'steady-phase-{phase}-cached']['sha256'] == arm_result['captures'][f'steady-phase-{phase}-rebuilt']['sha256'], 'Cached backdrop differs from rebuild'
                            if args.steady_video_stack:
                                def stack_capture(name):
                                    time.sleep(.3)
                                    arm_result['captures'][name] = capture(directory, name)
                                for opacity in (.73, 1):
                                    ctl('eval', 'hl.config({ decoration = { active_opacity = ' + str(opacity) + ', inactive_opacity = ' + str(opacity) + ' } })')
                                    stack_capture('stack-opacity-' + str(opacity))
                                for contribution in (0, 1):
                                    control('apply', patch={'overrides': {'neighbor_contribution': contribution}})
                                    stack_capture('stack-contribution-' + str(contribution))
                                bottom = subprocess.Popen([str(ROOT / 'build/layer-test'), '--bottom'],
                                    env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
                                clients.append(bottom)
                                wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
                                stack_capture('stack-wallpaper-changed')
                                bottom.terminate(); bottom.wait(timeout=5)
                                wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
                                stack_capture('stack-wallpaper-restored')
                                # Crossing the tile boundary creates a real
                                # neighbor dependency, even with fixed content.
                                action(windows[-2], 'move', 'x = -40, y = 100, relative = false')
                                for phase in (0, 1):
                                    set_pattern(controls[changing], {'color': colors[changing], 'phase': phase})
                                    stack_capture('stack-crossing-' + str(phase))
                                for tile in range(len(controls) - 2):
                                    set_pattern(controls[tile], {'color': colors[tile], 'phase': 1 - tile % 2})
                                    name = 'stack-tile-' + str(tile)
                                    stack_capture(name + '-cached')
                                    control('apply', patch={})
                                    stack_capture(name + '-rebuilt')
                                    assert arm_result['captures'][name + '-cached']['sha256'] == arm_result['captures'][name + '-rebuilt']['sha256'], 'Tiled backdrop differs from rebuild'
                        (output / 'result.json').write_text(json.dumps(results, indent=2))
                        print(f'Measured {arm}: CPU {arm_result["timing"]["distribution"]}', flush=True)
                        continue

                    # Legacy base ids also work with the older reference binaries.
                    materials = {
                        'frosted': {'base_preset': 'measured'},
                        'soft-glass': {'base_preset': 'platform'},
                        'clear-glass': {'base_preset': 'lens'},
                        'frosted-glass': {'base_preset': 'lens', 'overrides': {'blur_alpha0': 0.96, 'blur_alpha3': 1}},
                        'frosted-glass-clear-edge': {'base_preset': 'lens', 'overrides': {'blur_alpha0': 0.02, 'blur_alpha3': 1, 'blur_dist3': 38.4}},
                        # Local default isolation deliberately changes neighbor
                        # pixels. Outward optics keep the baseline scene path.
                        'liquid-outward': {'base_preset': 'frosted-liquid', 'overrides': {'inner_refraction_amount': 60}},
                    }
                    cases = [(f'{preset}-{theme}', profile | {'theme': theme})
                             for preset, profile in materials.items()
                             for theme in ('dark', 'light')]
                    if not args.quick:
                        for parameter in control('describe')['parameters']:
                            for bound in ('min', 'max'):
                                cases.append((parameter['name'] + '-' + bound,
                                    {'base_preset': 'lens', 'overrides': {parameter['name']: parameter[bound]}}))
                    cases.append(('combined-optics', {'base_preset': 'lens', 'overrides': {
                        'blur_sigma': 60, 'outer_refraction_amount': -120, 'inner_refraction_amount': 120,
                        'aberration_amount': 26, 'edge_bleed_amount': 70, 'edge_bleed_opacity': 1}}))
                    for index, (name, profile) in enumerate(cases):
                        control('apply', profile=profile)
                        time.sleep(.12)
                        if args.x11_clients:
                            set_pattern(controls[-1], {'color': colors[-1], 'phase': index % 2})
                            time.sleep(.15)
                        arm_result['captures'][name] = capture(directory, name)
                    for opacity in (.92, 1):
                        ctl('eval', 'hl.config({ decoration = { active_opacity = ' + str(opacity) + ', inactive_opacity = ' + str(opacity) + ' } })')
                        time.sleep(.2)
                        name = 'native-opacity-' + str(opacity)
                        arm_result['captures'][name] = capture(directory, name)
                    for scale, transform in ((1.25, 0), (1.6, 0), (1, 1), (1.25, 3)):
                        name = f'scale-{scale}-transform-{transform}'
                        control('apply', profile={'base_preset': 'lens'})
                        ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", '
                            f'position = "auto", scale = {scale}, transform = {transform}' + ' })')
                        time.sleep(.6)
                        if args.x11_clients:
                            set_pattern(controls[-1], {'color': colors[-1], 'phase': transform % 2})
                            time.sleep(.15)
                        arm_result['captures'][name] = capture(directory, name)

                    ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
                        'scale = 1, transform = 0, bitdepth = 10, cm = "srgb" })')
                    time.sleep(.4)
                    arm_result['captures']['sdr-10bit'] = capture(directory, 'sdr-10bit')
                    assert arm_result['captures']['sdr-10bit']['metadata']['drm_format'] == int.from_bytes(b'XR30', 'little')
                    ctl('eval', 'hl.config({ render = { use_fp16 = 1 } })')
                    time.sleep(.4)
                    arm_result['captures']['sdr-fp16'] = capture(directory, 'sdr-fp16')
                    assert arm_result['captures']['sdr-fp16']['metadata']['pixel_bytes'] >= 8
                    ctl('eval', 'hl.config({ render = { use_fp16 = 2 } })')

                    # Real HDR FP16 internal targets and native-precision readback.
                    # The virtual output tests rendering/metadata; it cannot
                    # substitute for inspection of a physical HDR display.
                    ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
                        'scale = 1, transform = 0, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1 })')
                    time.sleep(.6)
                    for preset in ('platform', 'lens'):
                        control('apply', profile={'base_preset': preset})
                        time.sleep(.15)
                        name = 'hdr-' + preset
                        arm_result['captures'][name] = capture(directory, name)
                        assert arm_result['captures'][name]['metadata']['pixel_bytes'] >= 8, 'HDR capture lost precision'
                        assert 'TF:SRGB' not in arm_result['captures'][name]['metadata']['color_description'], 'HDR configuration was ignored'
                    if has_buffers: arm_result['hdr'] = control('buffers')['buffers']
                    ctl('eval', 'hl.monitor({ output = "HYPRLIQUID-TEST", mode = "1280x800@60", position = "auto", '
                        'scale = 1, transform = 0, bitdepth = 8, cm = "srgb", supports_hdr = 0, supports_wide_color = 0 })')
                    ctl('eval', 'hl.config({ render = { use_fp16 = 2 } })')
                    time.sleep(.4)

                    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: w['class'])

                    def focus(window):
                        ctl('dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + window['address']) + ' })')

                    for side in (0, 1):
                        focus(windows[side])
                        time.sleep(.15)
                        arm_result['captures'][f'tile-focus-{side}'] = capture(directory, f'tile-focus-{side}')
                    for window in windows:
                        action(window, 'float', 'action = "enable"')
                        action(window, 'resize', 'x = 480, y = 300, relative = false')
                    action(windows[1], 'move', 'x = 420, y = 280, relative = false')
                    for name, x, y in (('overlap', 300, 200), ('top-left', -50, -30),
                                       ('top-right', 950, -30), ('bottom-left', -50, 650), ('bottom-right', 950, 650)):
                        action(windows[0], 'move', f'x = {x}, y = {y}, relative = false')
                        focus(windows[0])
                        time.sleep(.4)
                        arm_result['captures'][name] = capture(directory, name)
                        if name == 'overlap':
                            for theme in ('dark', 'light'):
                                for sigma in (0, 19):
                                    control('apply', profile={'base_preset': 'frosted-liquid', 'theme': theme,
                                                            'overrides': {'blur_sigma': sigma}})
                                    time.sleep(.15)
                                    case = f'liquid-underlay-{theme}-{sigma}'
                                    arm_result['captures'][case] = capture(directory, case)
                            control('apply', profile={'base_preset': 'lens'})
                            time.sleep(.15)
                    action(windows[0], 'fullscreen', 'mode = "fullscreen", action = "set"')
                    time.sleep(.3)
                    arm_result['captures']['fullscreen'] = capture(directory, 'fullscreen')
                    action(windows[0], 'fullscreen', 'mode = "fullscreen", action = "unset"')
                    for window in windows: action(window, 'float', 'action = "disable"')
                    time.sleep(.4)

                    # The GTK fixture supplies translucent content, not a
                    # background-effect region. Exercise its alpha mask just
                    # as the nested layer-cache/lock suite does.
                    ctl('eval', 'hl.plugin.hyprliquid.config({ layers = { enabled = true, mask_mode = "alpha" } })')
                    layer_env = env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}
                    layer = subprocess.Popen([str(ROOT / 'build/layer-test')], env=layer_env, stdout=log, stderr=log)
                    clients.append(layer)
                    wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
                    time.sleep(.4)
                    arm_result['captures']['layer-glass'] = capture(directory, 'layer-glass')
                    if has_buffers:
                        arm_result['layer'] = control('buffers')['buffers']
                        assert len(arm_result['layer']['layers']) == 1
                        assert {b['role'] for b in arm_result['layer']['buffers']} >= {'layer-sharp', 'layer-blurred', 'layer-temp'}
                    # A cached layer redraw must not take ownership of any
                    # pooled window storage. Capture the same idle scene twice.
                    time.sleep(.2)
                    arm_result['captures']['layer-cached'] = capture(directory, 'layer-cached')
                    layer.terminate()
                    layer.wait(timeout=5)
                    wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
                    if has_buffers:
                        assert not control('buffers')['buffers']['layers']
                    ctl('eval', 'hl.plugin.hyprliquid.config({ layers = { enabled = false } })')

                    (output / 'result.json').write_text(json.dumps(results, indent=2))
                    print(f'Captured {arm}: {len(arm_result["captures"])} cases; '
                          f'CPU {arm_result["timing"]["distribution"]}', flush=True)

                if args.steady_only and (not args.verify_steady_pixels or args.only_arm):
                    if args.verify_steady_pixels:
                        print('PASS: steady cached/rebuilt native framebuffer comparisons', flush=True)
                    ctl('dispatch', 'hl.dsp.exit()')
                    compositor.wait(timeout=10)
                    return

                baseline = results['arms']['baseline']['captures']
                for name, expected in baseline.items():
                    for arm in ('reference', 'candidate'):
                        actual = results['arms'][arm]['captures'][name]
                        # Damage is telemetry, not image data: cache hits are
                        # expected to reduce it while preserving every pixel.
                        image = lambda capture: capture | {'metadata': {k: v for k, v in capture['metadata'].items() if k not in ('damage', 'native_damage')}}
                        identical = image(expected) == image(actual)
                        results['comparisons'].append({'case': name, 'arm': arm, 'identical': identical})
                (output / 'result.json').write_text(json.dumps(results, indent=2))
                differences = [row for row in results['comparisons'] if not row['identical']]
                assert not differences, differences
                print(f'PASS: {len(results["comparisons"])} byte-identical internal framebuffer comparisons', flush=True)

                if args.steady_only:
                    ctl('dispatch', 'hl.dsp.exit()')
                    compositor.wait(timeout=10)
                    return

                # Lifecycle tests deliberately run after the comparison arms so
                # resize history cannot contaminate their steady-state totals.
                lifecycle = []
                for cycle in range(3):
                    control('apply', profile={'base_preset': 'lens'})
                    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: w['class'])
                    action(windows[0], 'float', 'action = "enable"')
                    for step in range(24):
                        action(windows[0], 'resize', f'x = {310 + step * 7}, y = {210 + step * 3}, relative = false')
                        time.sleep(.025)
                        diagnostic = control('buffers')['buffers']
                        pool = diagnostic['window_pool']
                        assert pool['pairs'] <= pool['retained_limit_pairs'], pool
                        assert pool['bytes'] <= pool['retained_limit_bytes'] or pool['pairs'] == 1, pool
                        assert pool['leased_bytes'] == 0 and diagnostic['allocation_failures'] == 0
                        if 'scratch_pool' in diagnostic:
                            scratch = diagnostic['scratch_pool']
                            assert scratch['sets'] <= scratch['retained_limit_sets']
                            assert scratch['bytes'] <= scratch['retained_limit_bytes']
                            assert scratch['leased_bytes'] == 0
                        if 'backdrop_cache' in diagnostic:
                            cache = diagnostic['backdrop_cache']
                            assert cache['pairs'] <= cache['retained_limit_pairs']
                            assert cache['bytes'] <= cache['retained_limit_bytes']
                            assert cache['leased_bytes'] == 0
                    lifecycle.append({'cycle': cycle, 'resized': diagnostic, 'drm': drm(compositor.pid)})
                    action(windows[0], 'float', 'action = "disable"')
                    control('apply', patch={'enabled': False})
                    time.sleep(6.2)
                    disabled = control('buffers')['buffers']
                    assert not any(b['role'].startswith('scene-') for b in disabled['buffers']), disabled
                    assert disabled['window_pool']['bytes'] == 0, disabled
                    if 'scratch_pool' in disabled: assert disabled['scratch_pool']['bytes'] == 0, disabled
                    if 'backdrop_cache' in disabled: assert disabled['backdrop_cache']['bytes'] == 0, disabled
                    lifecycle[-1]['disabled'] = disabled
                    control('apply', patch={'enabled': True})
                    time.sleep(.3)
                    for client in clients:
                        if client.poll() is None:
                            client.terminate()
                            client.wait(timeout=5)
                    wait(lambda: not json.loads(ctl('-j', 'clients')))
                    spawn_panes()
                    time.sleep(.4)

                windows = json.loads(ctl('-j', 'clients'))
                workspace = str(windows[0]['workspace']['id'])
                ctl('eval', 'hl.config({ animations = { enabled = true } })')
                ctl('dispatch', 'hl.dsp.focus({ workspace = "99" })')
                time.sleep(3.2)
                away = control('buffers')['buffers']
                assert not any(b['role'].startswith('scene-') for b in away['buffers']), away
                ctl('dispatch', 'hl.dsp.focus({ workspace = ' + json.dumps(workspace) + ' })')
                time.sleep(1)
                returned = control('buffers')['buffers']
                assert len([b for b in returned['buffers'] if b['role'] == 'scene-background']) == 1
                assert len([b for b in returned['buffers'] if b['role'] == 'scene-windows']) == 1
                assert len([b for b in returned['buffers'] if b['role'] == 'scene-base']) <= 1

                assert ctl('output', 'create', 'headless', 'HYPRLIQUID-REMOVE') == 'ok'
                ctl('dispatch', 'hl.dsp.workspace.move({ workspace = ' + json.dumps(workspace) + ', monitor = "HYPRLIQUID-REMOVE" })')
                time.sleep(1)
                extra = next(m for m in json.loads(ctl('-j', 'monitors')) if m['name'] == 'HYPRLIQUID-REMOVE')
                before_remove = control('buffers')['buffers']
                assert any(b['owner'] == f'monitor:{extra["id"]}' for b in before_remove['buffers'])
                assert ctl('output', 'remove', 'HYPRLIQUID-REMOVE') == 'ok'
                time.sleep(3.2)
                removed = control('buffers')['buffers']
                assert not any(b['owner'] == f'monitor:{extra["id"]}' for b in removed['buffers'])
                results['lifecycle'] = {'cycles': lifecycle, 'workspace_away': away, 'workspace_returned': returned,
                                        'before_monitor_removal': before_remove, 'after_monitor_removal': removed}
                (output / 'result.json').write_text(json.dumps(results, indent=2))
                print('PASS: three resize/disable/reopen cycles, animated workspace moves, monitor removal', flush=True)
                ctl('dispatch', 'hl.dsp.exit()')
                compositor.wait(timeout=10)
            finally:
                (output / 'result.json').write_text(json.dumps(results, indent=2))
                logs = list(runtime.glob('hypr/*/hyprland.log'))
                if logs: (output / 'compositor.log').write_text(logs[-1].read_text())
                for report in (runtime / 'cache/hyprland').glob('hyprlandCrashReport*.txt'):
                    (output / report.name).write_bytes(report.read_bytes())
                for client in clients:
                    if client.poll() is None:
                        client.terminate()
                        client.wait(timeout=5)
                if compositor.poll() is None:
                    compositor.terminate()
                    try: compositor.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        compositor.kill()
                        compositor.wait()


if __name__ == '__main__':
    main()
