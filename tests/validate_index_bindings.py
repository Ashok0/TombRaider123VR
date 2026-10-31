"""Check shipped actions and Index bindings, optionally against SteamVR's driver profile."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--steamvr', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1] / 'TombRaiderVRBindings'
manifest = json.loads((root / 'actions.json').read_text())
binding = json.loads((root / 'bindings_index.json').read_text())
actions = {a['name']: a['type'] for a in manifest['actions']}
assert len(actions) == len(manifest['actions']) == 14
assert manifest['action_sets'] == [{'name': '/actions/game', 'usage': 'leftright'}]
assert manifest['default_bindings'] == [{'controller_type': 'knuckles', 'binding_url': 'bindings_index.json'}]
assert binding['controller_type'] == 'knuckles'
game = binding['bindings']['/actions/game']
sources = {s['path']: s for s in game['sources']}
assert len(sources) == len(game['sources']) == 10
expected = {
    'thumbstick': ('joystick', {'position': ('stick', 'vector2'), 'click': ('stickclick', 'boolean')}),
    'trigger': ('trigger', {'pull': ('trigger', 'vector1')}),
    'grip': ('force_sensor', {'force': ('grip', 'vector1')}),
    'a': ('button', {'click': ('lower', 'boolean')}),
    'b': ('button', {'click': ('upper', 'boolean')}),
}
seen = set()
for side in ('left', 'right'):
    for control, (mode, inputs) in expected.items():
        source = sources[f'/user/hand/{side}/input/{control}']
        assert source['mode'] == mode
        assert set(source['inputs']) == set(inputs)
        for component, (name, kind) in inputs.items():
            output = source['inputs'][component]['output']
            assert output == f'/actions/game/in/{side}_{name}'
            assert actions[output] == kind
            assert output not in seen
            seen.add(output)
    vibration = {'path': f'/user/hand/{side}/output/haptic', 'output': f'/actions/game/out/{side}_haptic'}
    assert vibration in game['haptics'] and actions[vibration['output']] == 'vibration'
    seen.add(vibration['output'])
assert seen == set(actions)
assert all(name in manifest['localization'][0] for name in actions)
if args.steamvr:
    profile = json.loads((args.steamvr / 'drivers/indexcontroller/resources/input/index_controller_profile.json').read_text())
    assert profile['controller_type'] == 'knuckles'
    components = profile['input_source']
    for name in expected:
        assert f'/input/{name}' in components
    assert components['/input/grip']['force']
    assert components['/input/thumbstick']['click']
    assert components['/input/trigger']['value']
    # Valve's own tutorial demonstrates the force_sensor -> force binding used here.
    tutorial = json.loads((args.steamvr / 'tools/steamvr_tutorial/win64/bindings_knuckles.json').read_text())
    assert any(s.get('mode') == 'force_sensor' and 'force' in s.get('inputs', {})
               for group in tutorial['bindings'].values() for s in group.get('sources', []))
print('PASS: 14 actions bound exactly once; Index buttons, stick clicks, force grips and haptics validated.')
