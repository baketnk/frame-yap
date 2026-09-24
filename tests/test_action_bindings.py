"""Offline contract for the default Frame controller shortcut (no OpenVR)."""
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ActionBindingTests(unittest.TestCase):
    def test_review_controls_match_config_defaults(self):
        manifest = json.loads((ROOT / "assets/actions.json").read_text())
        binding = json.loads((ROOT / "assets/bindings_frame_controller.json").read_text())
        config = json.loads((ROOT / "assets/config.example.json").read_text())
        sources = binding["bindings"]["/actions/frameyap"]["sources"]
        self.assertEqual(len({s["path"] for s in sources}), len(sources))
        for action, button in {"ptt": "x", "cancel": "b", "insert": "a", "enter": "y"}.items():
            name = f"/actions/frameyap/in/{action}"
            path = f"/user/hand/right/input/{button}"
            self.assertIn({"name": name, "type": "boolean"}, manifest["actions"])
            self.assertEqual(config["buttons"][action], path)
            self.assertEqual([s["path"] for s in sources if s["inputs"]["click"]["output"] == name], [path])

    def test_right_x_is_hold_to_talk(self):
        manifest = json.loads((ROOT / "assets/actions.json").read_text())
        binding = json.loads((ROOT / "assets/bindings_frame_controller.json").read_text())
        self.assertIn(
            {"controller_type": "frame_controller", "binding_url": "bindings_frame_controller.json"},
            manifest["default_bindings"],
        )
        self.assertIn(
            {"name": "/actions/frameyap/in/ptt", "type": "boolean"},
            manifest["actions"],
        )
        sources = binding["bindings"]["/actions/frameyap"]["sources"]
        right_x = [source for source in sources if source["path"] == "/user/hand/right/input/x"]
        self.assertEqual(right_x, [{
            "path": "/user/hand/right/input/x",
            "mode": "button",
            "inputs": {"click": {"output": "/actions/frameyap/in/ptt"}},
        }])
        # Do not map a second PTT source or replace the dashboard's pointer trigger.
        self.assertEqual(sum(
            source["inputs"].get("click", {}).get("output") == "/actions/frameyap/in/ptt"
            for source in sources
        ), 1)
        self.assertFalse(any(source["path"].endswith("/input/trigger") for source in sources))


if __name__ == "__main__":
    unittest.main()
