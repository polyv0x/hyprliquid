#!/usr/bin/env python3
"""Build the reviewed renderer with only the new read-only accounting.

The clean baseline remains a separate comparison arm. This reference retains
per-window samples and the original scene lifetime, making allocation/timing
comparisons use identical accounting overhead. Never builds in the live tree.
"""
import argparse
import io
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent
BASE = '46ffbf2f76f80fe1d89eba0d30d1c68e06bbc189'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive = subprocess.check_output(['git', 'archive', BASE], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(output, filter='data')

    # These changes only observe allocation and render events on the baseline.
    for name in ('RenderBuffers.hpp', 'RenderBuffers.cpp', 'Globals.hpp',
                 'RuntimeControl.cpp', 'GlassRenderer.cpp', 'GlassLayerSurface.cpp', 'GlassLayerSurface.hpp', 'main.cpp'):
        shutil.copy2(ROOT / 'src' / name, output / 'src' / name)
    header = output / 'src/GlassDecoration.hpp'
    header.write_text(header.read_text().replace('#pragma once', '#pragma once\nstruct json_object;').replace(
        '    void                    onFullscreenStateChanged();',
        '    void                    onFullscreenStateChanged();\n    json_object* diagnostics() const;'))
    cpp = output / 'src/GlassDecoration.cpp'
    source = cpp.read_text().replace('#include <algorithm>', '#include <algorithm>\n#include <json-c/json.h>')
    marker = 'json_object* CGlassDecoration::diagnostics() const {'
    current = (ROOT / 'src/GlassDecoration.cpp').read_text()
    start = current.index(marker)
    # The diagnostic method ends before the next method definition.
    end = current.index('\n}\n', start) + 3
    source += '\n' + current[start:end]
    source = source.replace('    float monitorScale  = monitor->m_scale;', '''    const auto owner = std::format("window:{:p}", static_cast<void*>(window.get()));
    g_pGlobalState->renderBuffers.identify(m_sampleFramebuffer, "window-sharp", owner);
    g_pGlobalState->renderBuffers.identify(m_blurredFramebuffer, "window-blurred", owner);
    float monitorScale  = monitor->m_scale;''')
    cpp.write_text(source)
    header = output / 'src/WindowScene.hpp'
    header.write_text(header.read_text().replace('void destroy();', '''void destroy();
void collectUnused();
bool hasRetired();
void retireMonitor(MONITORID);
void finishFrame(MONITORID, uint64_t);'''))
    cpp = output / 'src/WindowScene.cpp'
    source = cpp.read_text()
    old = '''        for (auto& fb : {m_scene->background, m_scene->image}) {
            if (fb->m_size != source->m_size || fb->m_drmFormat != source->m_drmFormat)
                if (!fb->alloc(source->m_size.x, source->m_size.y, source->m_drmFormat))
                    return {};'''
    new = '''        for (auto* framebuffer : {&m_scene->background, &m_scene->image}) {
            auto& fb = *framebuffer;
            if (!g_pGlobalState->renderBuffers.prepare(fb, source->m_size.x, source->m_size.y, source->m_drmFormat,
                    fb == m_scene->background ? "scene-background" : "scene-windows", std::format("monitor:{}", monitor->m_id)))
                return {};'''
    assert old in source
    source = source.replace(old, new)
    source += '''\nnamespace WindowScene {
void collectUnused() {}
bool hasRetired() { return false; }
void retireMonitor(MONITORID) {}
void finishFrame(MONITORID, uint64_t) {}
}\n'''
    cpp.write_text(source)
    (output / 'REFERENCE.txt').write_text(f'Accounting-only reference based on {BASE}\n')
    subprocess.run(['make', '-j4', 'all'], cwd=output, check=True)


if __name__ == '__main__':
    main()
