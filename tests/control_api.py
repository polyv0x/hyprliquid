"""Behavioral checks against the real plugin IPC in a disposable compositor."""
import json
import math
from pathlib import Path


def verify_control(ctl, config_path):
    def request(op, **fields):
        return json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))

    description = request('describe')
    assert description['ok'] and description['api'] == 1, description
    assert len(description['parameters']) == 34
    angle = next(p for p in description['parameters'] if p['name'] == 'display_angle_degrees')
    assert (angle['min'], angle['max'], angle['default'], angle['models']) == (-180, 180, 0, ['liquid'])
    assert 'display_angle_animation' in description['capabilities']
    assert 'hover_angle_animation' in description['capabilities']
    neighbor = next(p for p in description['parameters'] if p['name'] == 'neighbor_contribution')
    assert (neighbor['min'], neighbor['max'], neighbor['default']) == (0, 1, 1)
    falloff = next(p for p in description['parameters'] if p['name'] == 'neighbor_falloff')
    assert (falloff['min'], falloff['max'], falloff['default']) == (0, 1, 1)
    assert not {'superness', 'use_measured_face'} & {p['name'] for p in description['parameters']}
    assert 'test' in {p['id'] for p in description['presets']}
    preset_ids = [p['id'] for p in description['presets']]
    assert preset_ids[:2] == ['frosted-glass', 'frosted-glass-clear-edge'], preset_ids
    assert preset_ids[2:] == sorted(preset_ids[2:]), preset_ids
    assert 'clear-edge' not in preset_ids and 'frosted-clear-edge' not in preset_ids
    if 'buffers' in description['capabilities']:
        diagnostic = request('buffers')
        assert diagnostic['ok'] and diagnostic['state'] == description['state']
        assert diagnostic['buffers']['window_pool']['leased_bytes'] == 0
        assert diagnostic['buffers']['allocation_failures'] == 0
        assert diagnostic['buffers']['monitors']
    baseline = description['state']
    assert baseline['animate_display_angle'] is False
    assert baseline['animate_hover_angle'] is False
    assert baseline['base_preset'] == 'test' and not baseline['active'], baseline
    for material in (baseline['material'], baseline['inherited'], baseline['baseline']['material']):
        assert 'use_measured_face' not in material
    assert math.isclose(baseline['material']['inner_refraction_amount'], -11)

    # Every public preset must resolve the previewed tones without scalar
    # overrides. A theme switch changes tone, not blur/refraction geometry.
    for preset, rim in [('frosted', 0), ('soft-glass', 0.92), ('clear-glass', 1.2),
                        ('frosted-glass', 1.2), ('frosted-glass-clear-edge', 1.2)]:
        materials = {}
        for theme in ('dark', 'light'):
            themed = request('apply', profile={'base_preset': preset, 'theme': theme})
            assert themed['ok'], themed
            state = themed['state']
            assert state['theme'] == theme and state['panel']['overrides'] == {}
            material = materials[theme] = state['material']
            if preset == 'frosted-glass-clear-edge':
                for alias in ('clear-edge', 'frosted-clear-edge'):
                    renamed = request('apply', profile={'base_preset': alias, 'theme': theme})
                    assert renamed['ok'] and renamed['state']['base_preset'] == preset
                    assert renamed['state']['material'] == material
            assert 'use_measured_face' not in material
            assert math.isclose(material['face_slope'], 0.72 if theme == 'dark' else 0.68, abs_tol=1e-6)
            assert math.isclose(material['face_intercept'], 0.06 if theme == 'dark' else 0.30, abs_tol=1e-6)
            assert math.isclose(material['rim_opacity'], rim * (0.65 if theme == 'dark' else 0.85), abs_tol=1e-6)
        changed = {key for key in materials['dark'] if materials['dark'][key] != materials['light'][key]}
        assert changed == {'face_slope', 'face_intercept'} | ({'rim_opacity'} if rim else set()), (preset, changed)
    assert request('reset')['ok']

    # Structured material data stays outside the compatible scalar contract.
    fixture = json.loads((Path(__file__).resolve().parent.parent / 'materials/liquid.json').read_text())
    shared = {'glass_opacity', 'effect_scale', 'neighbor_contribution', 'neighbor_falloff',
              'blur_sigma', 'blur_alpha0', 'blur_alpha3', 'blur_dist3',
              'inner_refraction_amount', 'inner_refraction_inv_height', 'outer_refraction_amount', 'outer_refraction_inv_height',
              'refraction_threshold0', 'refraction_threshold1', 'refraction_opacity',
              'face_opacity', 'rim_width', 'rim_opacity', 'display_angle_degrees'}
    assert 'material_models' in description['capabilities']
    for parameter in description['parameters']:
        assert ('liquid' in parameter['models']) == (parameter['name'] in shared)
    presets = {p['id']: p for p in description['presets']}
    assert 'liquid-medium' not in presets
    for preset in ('frosted-liquid', 'frosted-liquid-medium'):
        assert presets[preset]['valid'] and presets[preset]['model'] == 'liquid'
    for theme, environment in [('light', 'default'), ('dark', 'dark')]:
        response = request('apply', profile={'base_preset': 'frosted-liquid-medium', 'theme': theme})
        assert response['ok'], response
        state = response['state']
        assert state['model'] == 'liquid' and state['effective_blur_sigma'] == 6
        assert set(state['applicable_parameters']) == shared
        assert state['model_data']['environment'] == environment
        assert set(state['model_data']) == {'environment', 'parameters', 'effective_parameters',
                                           'display_angle_degrees', 'shadow_owner', 'output_extent', 'assumptions'}
        assert state['model_data']['parameters'] == fixture['variants'][environment]
        assert state['model_data']['effective_parameters'] == fixture['variants'][environment]
        assert state['model_data']['shadow_owner'] == 'native'
        assert state['model_data']['output_extent'] == 0
        assert state['material']['neighbor_falloff'] == 0
        # The renamed preset differs only in blur; the medium variant preserves
        # the complete original material in both themes.
        for preset in ('frosted-liquid', 'liquid-medium'):
            frosted = request('apply', profile={'base_preset': preset, 'theme': theme})['state']
            assert frosted['base_preset'] == 'frosted-liquid'
            assert frosted['effective_blur_sigma'] == 19
            assert frosted['material'] == state['material'] | {'blur_sigma': 19}
            expected = json.loads(json.dumps(state['model_data']))
            expected['effective_parameters']['blur']['radius'] = 19
            assert frosted['model_data'] == expected
        state = request('apply', profile={'base_preset': 'frosted-liquid-medium', 'theme': theme})['state']
        for invalid in [{'model': 1}, {'overrides': {'family': 1}},
                        {'overrides': {'effect_scale': float('nan')}},
                        {'overrides': {'glass_opacity': 0.2, 'effect_scale': 4}}]:
            rejected = request('apply', patch=invalid)
            assert not rejected['ok'] and rejected['state'] == state
        edited = request('apply', patch={'overrides': {'effect_scale': 1.5, 'glass_opacity': 0.75}})
        assert edited['ok'] and edited['state']['model_data'] == state['model_data']
        restored = request('apply', patch={'reset_fields': ['effect_scale', 'glass_opacity']})
        assert restored['state']['material'] == state['material']
        overrides = {'blur_sigma': 12, 'blur_alpha0': 0.4, 'blur_alpha3': 0.25, 'blur_dist3': 166,
                     'inner_refraction_amount': -30, 'inner_refraction_inv_height': 0.1,
                     'outer_refraction_amount': 40, 'outer_refraction_inv_height': 0.2,
                     'refraction_opacity': 0.5, 'refraction_threshold0': 2, 'refraction_threshold1': 8,
                     'face_opacity': 0.5, 'rim_width': 3, 'rim_opacity': 0.5, 'shadow_amount': 0.4, 'shadow_offset': -12}
        edited = request('apply', patch={'overrides': overrides})['state']
        effective = edited['model_data']['effective_parameters']
        assert edited['effective_blur_sigma'] == 12
        assert effective['blur']['radius'] == 12 and effective['blur']['distances'] == [-166, -2, 0, 0, 0]
        assert all(math.isclose(a, b, abs_tol=1e-6) for a, b in zip(effective['blur']['opacities'], [0.25, 0.2, 0.2, 0.4, 0.4]))
        assert effective['refraction']['innerAmount'] == -15 and effective['refraction']['outerAmount'] == 20
        assert math.isclose(effective['refraction']['innerHeight'], 10, abs_tol=1e-5)
        assert math.isclose(effective['refraction']['outerHeight'], 5, abs_tol=1e-5)
        assert effective['refraction']['outerDistances'] == [-8, -2] and effective['face']['opacity'] == 0.5
        assert effective['highlight']['keyHeight'] == 3 and effective['highlight']['keyColor'] == [0.5] * 4
        assert effective['shadow']['offset'] == [0, -12] and math.isclose(effective['shadow']['opacity'], 0.4, abs_tol=1e-6)
        assert edited['model_data']['parameters'] == fixture['variants'][environment]
        restored = request('apply', patch={'reset_fields': list(overrides)})['state']
        assert restored['material'] == state['material'] and restored['model_data'] == state['model_data']
        disabled = request('apply', patch={'overrides': {'blur_sigma': 0, 'refraction_opacity': 0}})['state']
        assert disabled['effective_blur_sigma'] == 0
        assert disabled['model_data']['effective_parameters']['refraction']['innerAmount'] == 0
    assert request('reset')['ok']
    assert request('state')['state']['model'] == 'legacy'
    angle_state = request('apply', profile={'base_preset': 'frosted-liquid', 'animate_display_angle': True,
                                          'overrides': {'display_angle_degrees': -45}})['state']
    assert angle_state['animate_display_angle'] is True
    assert angle_state['panel']['animate_display_angle'] is True
    assert angle_state['model_data']['display_angle_degrees'] == -45
    for invalid in ({'animate_display_angle': 1}, {'animate_display_angle': 'true'},
                    {'overrides': {'display_angle_degrees': True}},
                    {'overrides': {'display_angle_degrees': 181}},
                    {'overrides': {'display_angle_degrees': -181}},
                    {'animate_display_angle': False, 'overrides': {'display_angle_degrees': float('nan')}}):
        rejected = request('apply', patch=invalid)
        assert not rejected['ok'] and rejected['state'] == angle_state
    reset_angle = request('apply', patch={'animate_display_angle': None, 'reset_fields': ['display_angle_degrees']})['state']
    assert reset_angle['animate_display_angle'] is False and reset_angle['material']['display_angle_degrees'] == 0
    assert 'animate_display_angle' not in reset_angle['panel']
    hover = request('apply', profile={'animate_hover_angle': True})['state']
    assert hover['animate_hover_angle'] is True and hover['active']
    assert hover['panel']['animate_hover_angle'] is True and hover['animate_display_angle'] is False
    for invalid in ({'animate_hover_angle': 1}, {'animate_hover_angle': 'true'},
                    {'overrides': {'animate_hover_angle': True}}, {'reset_fields': ['animate_hover_angle']}):
        rejected = request('apply', patch=invalid)
        assert not rejected['ok'] and rejected['state'] == hover
    hover = request('apply', patch={'animate_hover_angle': False})['state']
    assert hover['animate_hover_angle'] is False and hover['panel']['animate_hover_angle'] is False and hover['active']
    hover = request('apply', patch={'animate_hover_angle': None})['state']
    assert hover['animate_hover_angle'] is False and 'animate_hover_angle' not in hover['panel'] and not hover['active']
    request('apply', patch={'animate_hover_angle': True})
    assert request('apply', profile={})['state']['animate_hover_angle'] is False
    reset_state = request('reset')['state']
    assert reset_state == baseline | {'revision': reset_state['revision']}
    print('PASS: Liquid base/effective variants, 19 applicable controls, angle animation toggle, exact reset and atomic rejection', flush=True)

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

    # Retired controls must reject the whole request, including otherwise valid
    # edits. A saved profile needs migration before it can be applied again.
    for field in ('patch', 'profile'):
        for value in (0, 0.5, 1, None):
            failed = request('apply', **{field: {'overrides': {'blur_sigma': 4, 'use_measured_face': value}}})
            assert not failed['ok'] and failed['state'] == state, failed
            assert failed['error'] == 'unknown material parameter: use_measured_face', failed

    for patch in [
        {'overrides': {'blur_sigma': 4, 'unknown': 1}},
        {'overrides': {'blur_sigma': 4, 'rim_opacity': 50}},
        {'overrides': {'rim_opacity': float('nan')}},
        {'overrides': {'rim_opacity': float('inf')}},
        {'overrides': {'rim_opacity': 1e300}},
        {'overrides': {'rim_opacity': -1e300}},
        {'overrides': {'rim_opacity': True}},
        {'overrides': {'rim_opacity': '0.2'}},
        {'overrides': {'neighbor_contribution': -0.01}},
        {'overrides': {'neighbor_contribution': 1.01}},
        {'overrides': {'neighbor_falloff': -0.01}},
        {'overrides': {'neighbor_falloff': 1.01}},
        {'base_preset': 'absent'}, {'base_preset': 'cycle'},
        {'theme': 'auto'}, {'enabled': 1}, {'surprise': 1},
        {'reset_fields': ['rim_opacity', 'unknown']},
        {'reset_fields': ['rim_opacity', 'use_measured_face']},
    ]:
        failed = request('apply', patch=patch)
        assert not failed['ok'] and failed['state'] == state, failed
    for payload in ['{}', 'null', '[]', '123', 'true', '"reset"', '{broken', '{"op":"reset"} trailing',
                    '{"op":"state","padding":"' + 'x' * 16384 + '"}',
                    '{"op":"state","nested":' + '[' * 32 + '0' + ']' * 32 + '}']:
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
        config_path.write_text(original_config.replace('default_preset = "test", ', '', 1))
        ctl('reload')
        default_state = request('state')['state']
        assert default_state['base_preset'] == 'frosted-glass' and not default_state['active']
        assert default_state['baseline']['base_preset'] == 'frosted-glass'
        config_path.write_text(original_config + '''
if hl.plugin.hyprliquid then
    hl.plugin.hyprliquid.config({ animate_display_angle = true, animate_hover_angle = true })
    hl.plugin.hyprliquid.preset("liquid-custom", { inherits = "frosted-liquid-medium", glass_opacity = 0.8, blur_sigma = 9, inner_refraction_amount = -25, display_angle_degrees = -30 })
    hl.plugin.hyprliquid.preset("liquid-child", { inherits = "liquid-custom", effect_scale = 1.25 })
    hl.plugin.hyprliquid.preset("soft-glass", {
        face_intercept = 0.18, dark = { face_intercept = 0.09 },
    })
end
''')
        ctl('reload')
        inherited = request('apply', profile={'base_preset': 'liquid-child', 'theme': 'dark'})
        assert inherited['ok'] and inherited['state']['model'] == 'liquid'
        assert math.isclose(inherited['state']['material']['glass_opacity'], 0.8, abs_tol=1e-6)
        assert inherited['state']['material']['effect_scale'] == 1.25
        assert inherited['state']['model_data']['parameters'] == fixture['variants']['dark']
        assert inherited['state']['effective_blur_sigma'] == 9
        assert inherited['state']['material']['display_angle_degrees'] == -30
        assert inherited['state']['animate_display_angle'] is True
        assert request('apply', patch={'animate_display_angle': False})['state']['animate_display_angle'] is False
        assert request('apply', patch={'animate_display_angle': None})['state']['animate_display_angle'] is True
        assert inherited['state']['animate_hover_angle'] is True
        assert request('apply', patch={'animate_hover_angle': False})['state']['animate_hover_angle'] is False
        assert request('apply', patch={'animate_hover_angle': None})['state']['animate_hover_angle'] is True
        assert inherited['state']['model_data']['effective_parameters']['refraction']['innerAmount'] == -25
        for preset in ('soft-glass', 'frosted-glass-clear-edge'):
            for theme, intercept in [('dark', 0.09), ('light', 0.18)]:
                customized = request('apply', profile={'base_preset': preset, 'theme': theme})
                assert customized['ok'], customized
                assert math.isclose(customized['state']['material']['face_intercept'], intercept, abs_tol=1e-6)
    finally:
        config_path.write_text(original_config)
        ctl('reload')
    print('PASS: all preset theme variants, inherited tones and shared/theme-specific Lua customizations', flush=True)
    ctl('eval', '''
local glass = hl.plugin.hyprliquid
for i = 1, 20 do
    local values = {[1] = 0.2, [17] = 0.5, blur_sigma = 8,
        dark = {[1] = 0.1, rim_opacity = 0.2}, light = {[2] = 0.3, rim_opacity = 0.4}}
    assert(pcall(glass.preset, "numeric-keys", values))
    assert(values[1] == 0.2 and values.dark[1] == 0.1)
    local ok, message = pcall(glass.preset, "name:test, unknown:1")
    assert(not ok and message:find("invalid setting"))
    ok, message = pcall(glass.layer, "test-errors", setmetatable({}, {
        __index = function() error("layer metamethod error") end,
    }))
    assert(not ok and message:find("layer metamethod error"))
end
for _, value in ipairs({1e300, -1e300, 0/0, math.huge}) do
    assert(not pcall(glass.layer, "invalid-threshold", {mask_threshold = value}))
end
''')
    ctl('reload')
    assert request('state')['state']['material'] == baseline['material']
    print('PASS: numeric Lua keys, malformed presets, throwing metamethods and extreme layer values', flush=True)
    return request
