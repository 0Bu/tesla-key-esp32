#!/usr/bin/env python3
"""Execute the repository's evcc example with synthetic cached vehicle responses."""

import copy
import json
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "docs/examples/evcc-custom-vehicle.yaml"
STATES = {
    "Charging": "C",
    "Starting": "B",
    "Stopped": "B",
    "NoPower": "B",
    "Complete": "B",
    "Disconnected": "A",
    "Unknown": "A",
    "": "A",
}


def load_example(text: str) -> dict:
    result = subprocess.run(
        ["ruby", "-ryaml", "-rjson", "-e",
         "puts JSON.generate(YAML.safe_load(STDIN.read, aliases: false))"],
        input=text, text=True, capture_output=True, check=True, timeout=10,
    )
    return json.loads(result.stdout)


def status(config: dict, state: str | None) -> str:
    charge_state = {} if state is None else {"charging_state": state}
    response = {"response": {"response": {"charge_state": charge_state}}}
    result = subprocess.run(
        ["jq", "-er", config["status"]["jq"]], input=json.dumps(response),
        text=True, capture_output=True, check=True, timeout=5,
    )
    return result.stdout.strip()


def require_command_timeouts(config: dict) -> None:
    for name in ("chargeEnable", "maxcurrent", "wakeup"):
        command = config[name]
        assert command["method"] == "POST", name
        assert command.get("timeout") == "30s", f"{name} needs its explicit 30s timeout"


class EvccExampleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        for tool in ("ruby", "jq"):
            if not shutil.which(tool):
                raise RuntimeError(f"evcc example tests require {tool}")
        cls.source = EXAMPLE.read_text()
        cls.config = load_example(cls.source)

    def test_actual_status_expression(self) -> None:
        for vehicle_state, expected in STATES.items():
            with self.subTest(vehicle_state=vehicle_state):
                self.assertEqual(status(self.config, vehicle_state), expected)
        self.assertEqual(status(self.config, None), "A")

    def test_command_timeouts(self) -> None:
        require_command_timeouts(self.config)

    def test_starting_omission_is_detected(self) -> None:
        branch = '    elif (.response.response.charge_state.charging_state == "Starting") then "B"\n'
        self.assertEqual(self.source.count(branch), 1)
        old = load_example(self.source.replace(branch, "", 1))
        self.assertEqual(status(old, "Starting"), "A")
        self.assertNotEqual(status(old, "Starting"), STATES["Starting"])
        self.assertEqual(status(self.config, "Starting"), "B")

    def test_missing_timeout_is_detected(self) -> None:
        for name in ("chargeEnable", "maxcurrent", "wakeup"):
            with self.subTest(command=name):
                old = copy.deepcopy(self.config)
                del old[name]["timeout"]
                with self.assertRaisesRegex(AssertionError, name):
                    require_command_timeouts(old)


if __name__ == "__main__":
    unittest.main()
