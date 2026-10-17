"""Behavioral checks against the real plugin IPC in a disposable compositor."""
import json
import math


def verify_control(ctl, config_path):
    def request(op, **fields):
        return json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))

    description = request('describe')
    assert description['ok'] and description['api'] == 1, description
    assert len(description['parameters']) == 34
    neighbor = next(p for p in description['parameters'] if p['name'] == 'neighbor_contribution')
    assert (neighbor['min'], neighbor['max'], neighbor['default']) == (0, 1, 1)
    falloff = next(p for p in description['parameters'] if p['name'] == 'neighbor_falloff')
    assert (falloff['min'], falloff['max'], falloff['default']) == (0, 1, 0)
    assert 'superness' not in {p['name'] for p in description['parameters']}
    assert 'test' in {p['id'] for p in description['presets']}
    if 'buffers' in description['capabilities']:
        diagnostic = request('buffers')
        assert diagnostic['ok'] and diagnostic['state'] == description['state']
        assert diagnostic['buffers']['window_pool']['leased_bytes'] == 0
        assert diagnostic['buffers']['allocation_failures'] == 0
        assert diagnostic['buffers']['monitors']
    baseline = description['state']
    assert baseline['base_preset'] == 'test' and not baseline['active'], baseline
    assert math.isclose(baseline['material']['inner_refraction_amount'], -11)

    # Every public preset must resolve the previewed tones without scalar
    # overrides. A theme switch changes tone, not blur/refraction geometry.
    for preset, rim in [('frosted', 0), ('soft-glass', 0.92), ('clear-glass', 1.2),
                        ('frosted-glass', 1.2), ('clear-edge', 1.2)]:
        materials = {}
        for theme in ('dark', 'light'):
            themed = request('apply', profile={'base_preset': preset, 'theme': theme})
            assert themed['ok'], themed
            state = themed['state']
            assert state['theme'] == theme and state['panel']['overrides'] == {}
            material = materials[theme] = state['material']
            assert material['use_measured_face'] == 1
            assert math.isclose(material['face_slope'], 0.72 if theme == 'dark' else 0.68, abs_tol=1e-6)
            assert math.isclose(material['face_intercept'], 0.06 if theme == 'dark' else 0.30, abs_tol=1e-6)
            assert math.isclose(material['rim_opacity'], rim * (0.65 if theme == 'dark' else 0.85), abs_tol=1e-6)
        changed = {key for key in materials['dark'] if materials['dark'][key] != materials['light'][key]}
        assert changed == {'face_slope', 'face_intercept'} | ({'rim_opacity'} if rim else set()), (preset, changed)
    assert request('reset')['ok']

    # An explicitly customized old name remains selectable under its own id.
    assert 'frosted-full' in {p['id'] for p in description['presets']}
    customized = request('apply', profile={'base_preset': 'frosted-full'})
    assert customized['ok'] and customized['state']['base_preset'] == 'frosted-full'
    assert customized['state']['material']['blur_sigma'] == 9
    assert request('reset')['ok']

    applied = request('apply', profile={'base_preset': 'platform', 'theme': 'light', 'enabled': False,
                                      'overrides': {'inner_refraction_amount': -37, 'coverage_offset': -0.75, 'rim_opacity': 0.12,
                                                    'neighbor_contribution': 0.375, 'neighbor_falloff': 0.5}})
    assert applied['ok'], applied
    state = applied['state']
    assert not state['enabled'] and state['theme'] == 'light'
    assert state['material']['inner_refraction_amount'] == -37
    assert state['material']['coverage_offset'] == -0.75
    assert state['material']['neighbor_contribution'] == 0.375
    assert state['material']['neighbor_falloff'] == 0.5
    assert math.isclose(state['material']['rim_opacity'], 0.12, abs_tol=1e-6)
    assert state['material']['rim_opacity'] != state['inherited']['rim_opacity']
    assert state['baseline'] == baseline['baseline']

    for patch in [
        {'overrides': {'blur_sigma': 4, 'unknown': 1}},
        {'overrides': {'blur_sigma': 4, 'rim_opacity': 50}},
        {'overrides': {'rim_opacity': float('nan')}},
        {'overrides': {'rim_opacity': float('inf')}},
        {'overrides': {'rim_opacity': True}},
        {'overrides': {'rim_opacity': '0.2'}},
        {'overrides': {'neighbor_contribution': -0.01}},
        {'overrides': {'neighbor_contribution': 1.01}},
        {'overrides': {'neighbor_falloff': -0.01}},
        {'overrides': {'neighbor_falloff': 1.01}},
        {'base_preset': 'absent'}, {'base_preset': 'cycle'},
        {'theme': 'auto'}, {'enabled': 1}, {'surprise': 1},
        {'reset_fields': ['rim_opacity', 'unknown']},
    ]:
        failed = request('apply', patch=patch)
        assert not failed['ok'] and failed['state'] == state, failed
    for payload in ['{}', 'null', '[]', '{broken', '{"op":"reset"} trailing']:
        failed = json.loads(ctl('hyprliquid:control', payload))
        assert not failed['ok'] and failed['state'] == state, failed
    conflict = request('reset', if_epoch=state['epoch'], if_revision=state['revision'] - 1)
    assert not conflict['ok'] and conflict['code'] == 'revision_conflict' and conflict['state'] == state
    conflict = request('reset', if_epoch='old-instance', if_revision=state['revision'])
    assert not conflict['ok'] and conflict['code'] == 'revision_conflict'

    reset_field = request('apply', patch={'reset_fields': ['rim_opacity']})
    assert reset_field['ok']
    assert 'rim_opacity' not in reset_field['state']['panel']['overrides']
    assert reset_field['state']['material']['rim_opacity'] == reset_field['state']['inherited']['rim_opacity']
    # Minimums arrive as decimal input; they must survive conversion to float.
    assert request('apply', patch={'overrides': {'effect_scale': 0.1, 'rim_width': 0.01}})['ok']
    reset = request('reset')['state']
    assert not reset['active'] and reset['material'] == baseline['material']
    assert {p['id'] for p in request('describe')['presets']} == {p['id'] for p in description['presets']}

    assert request('apply', patch={'overrides': {'blur_sigma': 1}})['ok']
    ctl('reload')
    reloaded = request('state')['state']
    assert reloaded['generation'] > baseline['generation'] and not reloaded['active']
    assert reloaded['epoch'] == baseline['epoch'] and reloaded['material'] == baseline['material']
    assert request('apply', patch={'overrides': {'blur_sigma': 1}})['ok']
    ctl('eval', 'hl.plugin.hyprliquid.config({ default_preset = "test" })')
    terminal = request('state')['state']
    assert not terminal['active'] and terminal['generation'] == reloaded['generation']
    print('PASS: control describe/state, atomic validation, signed values, field/full reset, revision guards, reload and terminal ownership', flush=True)

    # Theme defaults must not hide an existing user's shared Lua customization.
    original_config = config_path.read_text()
    try:
        config_path.write_text(original_config + '''
if hl.plugin.hyprliquid then
    hl.plugin.hyprliquid.preset("soft-glass", {
        face_intercept = 0.18, dark = { face_intercept = 0.09 },
    })
end
''')
        ctl('reload')
        for preset in ('soft-glass', 'clear-edge'):
            for theme, intercept in [('dark', 0.09), ('light', 0.18)]:
                customized = request('apply', profile={'base_preset': preset, 'theme': theme})
                assert customized['ok'], customized
                assert math.isclose(customized['state']['material']['face_intercept'], intercept, abs_tol=1e-6)
    finally:
        config_path.write_text(original_config)
        ctl('reload')
    print('PASS: all preset theme variants, inherited tones and shared/theme-specific Lua customizations', flush=True)
    return request
