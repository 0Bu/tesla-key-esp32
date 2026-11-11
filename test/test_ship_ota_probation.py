#!/usr/bin/env python3
"""Run the ship verification block against an offline device and clock fixture."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / ".agents/skills/ship/SKILL.md"
FIXTURE = r"""
scenario=$1
reboot_marker=$2
DEVICE_IP=fixture.invalid
DELIVERY=ota
VERSION=1.6.6
FAMILY=ESP32
SECONDS=0

sleep() {
  if (( SECONDS < 5 )); then
    SECONDS=5
  else
    SECONDS=105
  fi
}

curl() {
  local uptime arg revalidate=0 boot=0123456789abcdef
  for arg in "$@"; do
    if [ "$arg" = 'Cache-Control: no-cache, no-store' ]; then
      revalidate=1
    fi
  done
  case "${!#}" in
    http://fixture.invalid/ota/status)
      if [ "$scenario" = missing-boot-id ]; then
        printf '%s\n' '{"state":"idle","message":""}'
        return
      fi
      if [ "$scenario" = malformed-boot-id ]; then
        printf '%s\n' '{"state":"idle","message":"","boot_id":"0123456789abcdef\n"}'
        return
      fi
      if [ -f "$reboot_marker" ] || { [ "$scenario" = hidden-reboot ] && (( SECONDS >= 5 )); }; then
        boot=fedcba9876543210
      fi
      if [ "$scenario" = cached-reboot ] && (( SECONDS >= 5 && revalidate )); then
        boot=fedcba9876543210
      fi
      printf '{"state":"idle","message":"","boot_id":"%s"}\n' "$boot"
      ;;
    http://fixture.invalid/status)
      uptime=$((SECONDS + 1))
      if [[ "$scenario" == hidden-reboot || "$scenario" == cached-reboot ]] && (( SECONDS >= 5 )); then
        uptime=$((SECONDS - 3))
      fi
      printf '{"version":"1.6.6","sys":{"uptime_s":%s}}\n' "$uptime"
      ;;
    http://fixture.invalid/api/proxy/1/version)
      if [ "$scenario" = baseline-reboot ]; then
        if [ -f "$reboot_marker.ready" ]; then
          : > "$reboot_marker"
        else
          : > "$reboot_marker.ready"
        fi
      fi
      if [ "$scenario" = sample-reboot ] && (( SECONDS >= 5 )); then
        : > "$reboot_marker"
      fi
      printf '%s\n' '{"version":"1.6.6-esp32","platform":"ESP32"}'
      ;;
    'http://fixture.invalid/diag?redact=1')
      if [ "$scenario" = diagnostic-reboot ]; then
        : > "$reboot_marker"
      fi
      printf '%s\n' 'OTA image healthy after 90 s, marked valid (rollback cancelled)'
      ;;
    *)
      printf 'unexpected fixture URL: %s\n' "${!#}" >&2
      return 90
      ;;
  esac
}
"""


class ShipOtaProbationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        section = SKILL.read_text().split("\n## 5. Verify", 1)[1]
        cls.verification = section.split("\n```bash\n", 1)[1].split("\n```", 1)[0]

    def run_scenario(self, scenario):
        with tempfile.TemporaryDirectory(prefix="ship-ota-probation-") as temp:
            return subprocess.run(
                ["bash", "-c", FIXTURE + self.verification, "ship-fixture", scenario,
                 str(Path(temp) / "rebooted")],
                capture_output=True, text=True, timeout=10, check=False,
            )

    def test_same_boot_is_accepted(self):
        result = self.run_scenario("stable")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_reboots_are_rejected(self):
        for scenario in (
            "baseline-reboot", "hidden-reboot", "cached-reboot", "sample-reboot", "diagnostic-reboot",
        ):
            with self.subTest(scenario=scenario):
                result = self.run_scenario(scenario)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("OTA boot identity changed", result.stderr)

    def test_invalid_boot_ids_are_rejected(self):
        for scenario in ("missing-boot-id", "malformed-boot-id"):
            with self.subTest(scenario=scenario):
                result = self.run_scenario(scenario)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("invalid OTA boot identity", result.stderr)


if __name__ == "__main__":
    unittest.main()
