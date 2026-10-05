"""Offline contract for isolated, stick-only grab bindings."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GrabBindingTests(unittest.TestCase):
    def test_grab_set_claims_only_each_hands_stick(self):
        manifest = json.loads((ROOT / 'assets/actions.json').read_text())
        self.assertIn({'name': '/actions/grab', 'usage': 'single'}, manifest['action_sets'])
        actions = {a['name']: a['type'] for a in manifest['actions']}
        for filename in ('bindings_frame_controller.json', 'bindings_knuckles.json'):
            bindings = json.loads((ROOT / 'assets' / filename).read_text())['bindings']
            grab = bindings['/actions/grab']
            self.assertEqual(set(grab), {'sources'})
            self.assertEqual({s['path'] for s in grab['sources']}, {
                '/user/hand/left/input/thumbstick', '/user/hand/right/input/thumbstick'})
            self.assertEqual(len(grab['sources']), 2)
            for source in grab['sources']:
                self.assertEqual(source['mode'], 'joystick')
                self.assertEqual(set(source['inputs']), {'position'})
                output = source['inputs']['position']['output']
                self.assertTrue(output.startswith('/actions/grab/in/'))
                self.assertEqual(actions[output], 'vector2')
            for name, binding in bindings.items():
                if name == '/actions/grab':
                    continue
                for source in binding.get('sources', []):
                    self.assertNotIn('position', source['inputs'])


if __name__ == '__main__':
    unittest.main()
