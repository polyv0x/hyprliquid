"""Real Noctalia rounded regions in a disposable compositor and private D-Bus."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from PIL import Image, ImageChops


def verify_noctalia_layers(ctl, env, log, clients, build, wait, nested_config):
    with tempfile.TemporaryDirectory(prefix='hlq-layers-', dir='/tmp') as temporary:
        base = Path(temporary)
        shell_env = env.copy()
        # Noctalia embeds this value in its IPC filename; its Wayland socket is
        # already beneath the nested compositor's private XDG_RUNTIME_DIR.
        shell_env['WAYLAND_DISPLAY'] = Path(env['WAYLAND_DISPLAY']).name
        for kind in ('CONFIG', 'STATE', 'CACHE', 'DATA'):
            directory = str(base / kind.lower())
            shell_env[f'XDG_{kind}_HOME'] = directory
            shell_env[f'NOCTALIA_{kind}_HOME'] = directory
        config = base / 'config/noctalia/config.toml'
        config.parent.mkdir(parents=True)
        config.write_text('''[shell]
offline_mode = true
clipboard_enabled = false
polkit_agent = false
setup_wizard_enabled = false
[shell.animation]
enabled = false
[shell.panel]
transparency_mode = "glass"
[theme]
mode = "dark"
source = "builtin"
[theme.templates]
enable_builtin_templates = false
enable_community_templates = false
[wallpaper]
enabled = false
[plugins]
enabled = []
auto_update = "none"
[[plugins.source]]
name = "official"
kind = "git"
location = "https://github.com/noctalia-dev/official-plugins"
enabled = false
[[plugins.source]]
name = "community"
kind = "git"
location = "https://github.com/noctalia-dev/community-plugins"
enabled = false
[bar.default]
enabled = false
[notification]
background_opacity = 0.15
''')
        # No activation directories or connection to the desktop/system bus.
        bus_config = base / 'bus.conf'
        bus_config.write_text(f'''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:tmpdir={base}</listen>
<policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
        bus = subprocess.Popen(['dbus-daemon', '--config-file=' + str(bus_config), '--nofork', '--print-address=1'],
                               stdout=subprocess.PIPE, stderr=log, text=True)
        clients.append(bus)
        shell_env['DBUS_SESSION_BUS_ADDRESS'] = bus.stdout.readline().strip()
        shell_env['DBUS_SYSTEM_BUS_ADDRESS'] = 'unix:path=' + str(base / 'no-system-bus')
        shell = None

        def run(*args, check=True):
            return subprocess.run(args, env=shell_env, capture_output=True, text=True, timeout=10, check=check)

        def msg(*args, check=True):
            return run('noctalia', 'msg', *args, check=check)

        def layers():
            return json.loads(ctl('hyprliquid:control', '{"op":"buffers"}'))['buffers']['layers']

        with nested_config.open('a') as file:
            file.write('''
hl.plugin.hyprliquid.config({layers={enabled=true, manage_blur=true, mask_mode="auto"}})
hl.plugin.hyprliquid.layer("noctalia-panel", {preset="frosted-liquid-medium"})
hl.plugin.hyprliquid.layer("noctalia-notification", {preset="frosted-liquid-medium"})
''')

        def capture(name):
            time.sleep(.35)
            path = build / f'{name}.png'
            run('grim', str(path))
            return Image.open(path).convert('RGBA')

        def closed():
            return not any(item['namespace'] == 'noctalia-panel' for item in layers())

        results = []
        try:
            run('noctalia', 'config', 'validate', str(config))
            shell = subprocess.Popen(['noctalia'], env=shell_env, stdout=log, stderr=log, start_new_session=True)
            def ready():
                status = msg('status', check=False)
                (build / 'noctalia-ipc-status.txt').write_text(status.stdout + status.stderr)
                if shell.poll() is not None:
                    raise RuntimeError(f'Noctalia exited: {shell.returncode}')
                return status.returncode == 0
            wait(ready)
            ctl('reload')
            ctl('dismissnotify')
            monitor = json.loads(ctl('-j', 'monitors'))[0]['name']
            for scale, transform in ((1, 0), (1.25, 0), (1.6, 0), (1.25, 1)):
                ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
                    f', mode = "1600x1000@60", position = "auto", scale = {scale}, transform = {transform}' + ' })')
                ctl('eval', 'hl.config({decoration={blur={enabled=true}}})')
                time.sleep(.5)
                assert json.loads(ctl('-j', 'monitors'))[0]['scale'] == scale
                msg('panel-open', 'polkit')
                wait(lambda: any(item['namespace'] == 'noctalia-panel' and item.get('rounded_regions') for item in layers()))
                panel = next(item for item in layers() if item['namespace'] == 'noctalia-panel')
                assert len(panel['rounded_regions']) == 1, panel
                assert panel['rounded_regions'][0][4] > 0, panel
                capture(f'noctalia-polkit-{scale}-{transform}')
                msg('panel-close', 'polkit')
                wait(closed)
                ids = [run('notify-send', '--print-id', '--expire-time=0', '--app-name=Glass test',
                           f'Rounded card {i + 1}', 'Separate glass edges and clear text.').stdout.strip() for i in range(2)]
                wait(lambda: any(item['namespace'] == 'noctalia-notification' and len(item.get('rounded_regions', [])) == 2 for item in layers()))
                toast = next(item for item in layers() if item['namespace'] == 'noctalia-notification')
                assert all(s[4] > 0 for s in toast['rounded_regions']), toast
                native_on = capture(f'noctalia-stack-{scale}-{transform}')
                ctl('eval', 'hl.config({decoration={blur={enabled=false}}})')
                native_off = capture(f'noctalia-no-native-blur-{scale}-{transform}')
                assert not ImageChops.difference(native_on, native_off).convert('RGB').getbbox(), 'Native blur changed managed Noctalia output'
                results.append({'scale': scale, 'transform': transform, 'panel': panel, 'notifications': toast,
                                'native_blur_on_off_identical': True})
                for ident in ids:
                    run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                        '--object-path', '/org/freedesktop/Notifications',
                        '--method', 'org.freedesktop.Notifications.CloseNotification', ident)
                wait(lambda: not any(item['namespace'] == 'noctalia-notification' and item.get('rounded_regions') for item in layers()))
                print(f'PASS: Noctalia polkit and two rounded toasts, scale {scale}, transform {transform}; native blur on/off identical', flush=True)
            (build / 'noctalia-layer-shapes.json').write_text(json.dumps(results, indent=2))
        finally:
            (build / 'noctalia-final-layers.json').write_text(json.dumps(layers(), indent=2))
            if shell is not None and shell.poll() is None:
                os.killpg(shell.pid, signal.SIGTERM)
                shell.wait(timeout=10)
            if bus.poll() is None:
                bus.terminate()
                bus.wait(timeout=10)
