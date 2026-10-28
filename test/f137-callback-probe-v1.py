"""Generate a private host probe from complete actual BLE callback/reset bodies.

Generation only: no compiler, SDK, device, key or test process is invoked here.
SDK/RTOS registration and semaphore effects are passive facades, not target proof.
"""
from pathlib import Path
import hashlib
import json
import sys

source = Path(sys.argv[1])
output = Path(sys.argv[2])
text = (source / "main/ble_client.cpp").read_text()

def region(begin, end):
    assert text.count(begin) == 1 and text.count(end) == 1
    value = text.split(begin, 1)[1].split(end, 1)[0]
    assert value.rstrip().endswith("}")
    return begin + value

bodies = [
    region("static void on_sync_cb() {", "\n\nstatic void on_reset_cb"),
    region("static void on_reset_cb(int reason) {", "\n\nstatic int gap_event_cb"),
    region("static void scan_timeout_cb(void* arg) {", "\n\nvoid BleClient::report_callback_fault"),
    region("void BleClient::report_callback_fault(bool timer_context, const char* site) noexcept {", "\n\nBleClient::BleClient"),
    region("void BleClient::on_reset() {", "\n\nbool BleClient::start_scan_locked_"),
]
template = Path(__file__).with_name("f137-callback-probe-template-v1.cpp").read_text()
assert template.count("// ACTUAL_COMPLETE_BODIES") == 1
generated = template.replace("// ACTUAL_COMPLETE_BODIES", "\n\n".join(bodies))
with output.open("x") as f:
    f.write(generated)
print(json.dumps({"generatedOnly": True, "completeFunctions": 5,
                  "sourceSha256": hashlib.sha256(text.encode()).hexdigest(),
                  "generatedSha256": hashlib.sha256(generated.encode()).hexdigest(),
                  "scope": "Actual wrappers/reset/fault reporter; passive SDK/task registration, no target ABI or live proof."}))
