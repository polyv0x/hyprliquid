"""Behavioral checks against the real plugin IPC in a disposable compositor."""
import json
import math


def verify_control(ctl):
    def request(op, **fields):
        return json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))

    description = request('describe')
    assert description['ok'] and description['api'] == 1, description
    assert len(description['parameters']) == 32
    assert 'superness' not in {p['name'] for p in description['parameters']}
    assert 'test' in {p['id'] for p in description['presets']}
    baseline = description['state']
    assert baseline['base_preset'] == 'test' and not baseline['active'], baseline
    assert math.isclose(baseline['material']['inner_refraction_amount'], -11)

    applied = request('apply', profile={'base_preset': 'platform', 'theme': 'light', 'enabled': False,
                                      'overrides': {'inner_refraction_amount': -37, 'coverage_offset': -0.75, 'rim_opacity': 0.12}})
    assert applied['ok'], applied
    state = applied['state']
    assert not state['enabled'] and state['theme'] == 'light'
    assert state['material']['inner_refraction_amount'] == -37
    assert state['material']['coverage_offset'] == -0.75
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
    return request
