import assert from 'node:assert/strict';
import {
  createHash,
  createHmac,
  createPrivateKey,
  createPublicKey,
  createCipheriv,
  createDecipheriv,
  diffieHellman,
} from 'node:crypto';
import { readFileSync } from 'node:fs';
import test from 'node:test';

// Hardware-free protocol-contract vectors from Tesla's canonical vehicle-command
// protocol documentation. These are deliberately public test keys; never replace
// them with a device or vehicle key. The gate pins byte order, metadata encoding,
// KDF labels and AES-GCM nonce/AAD/tag layout independently of a live vehicle.
// Source: https://github.com/teslamotors/vehicle-command/blob/main/pkg/protocol/protocol.md
const VEHICLE_PRIVATE_KEY = `-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIDRO5bRmp88e6xK29QMx2y5exYNO9fS+/P2MvlXCUo1woAoGCCqGSM49
AwEHoUQDQgAEx6H0cThIaqRymXFJSHjTOxok45Vx90im4WxZVbPYd9OmqqDpVRZk
dK9dMsQQ9DmiI0E3rRuwhf1OiBPJWPEdlw==
-----END EC PRIVATE KEY-----`;

const CLIENT_PRIVATE_KEY = `-----BEGIN EC PRIVATE KEY-----
MHcCAQEEICU4zcKal8GcHpmmN9bPT4yXDBGLVu3h5jI+bRYsSzDboAoGCCqGSM49
AwEHoUQDQgAEsra8aMLaBmXOZWgVWUmWxiOU7di+qQX+eBp1T+aoRacUMwkC8iXp
Jp1GbgWzSZgf2p2FzCPG+0RKpztikQXcbg==
-----END EC PRIVATE KEY-----`;

const EXPECTED_SHARED_KEY = '1b2fce19967b79db696f909cff89ea9a';
const EXPECTED_SESSION_INFO_KEY =
  'fceb679ee7bca756fcd441bf238bf2f338629b41d9eb9c67be1b32c9672ce300';

function u32be(value) {
  const out = Buffer.alloc(4);
  out.writeUInt32BE(value);
  return out;
}

function metadata(entries) {
  const encoded = [...entries]
    .sort((a, b) => a.tag - b.tag)
    .map(({ tag, value }) => {
      const bytes = Buffer.isBuffer(value) ? value : Buffer.from(value);
      assert.ok(bytes.length <= 255, 'Tesla TLV values use a one-byte length');
      return Buffer.concat([Buffer.from([tag, bytes.length]), bytes]);
    });
  return Buffer.concat([...encoded, Buffer.from([0xff])]);
}

test('official VIN advertisement vector pins SHA-1 byte selection', () => {
  const digest = createHash('sha1').update('5YJS0000000000000', 'ascii').digest('hex');
  assert.equal(`S${digest.slice(0, 16)}C`, 'S1a87a5a75f3df858C');
});

test('official P-256 ECDH vector pins X-coordinate byte order and SHA1[:16]', () => {
  const sharedSecret = diffieHellman({
    privateKey: createPrivateKey(CLIENT_PRIVATE_KEY),
    publicKey: createPublicKey(createPrivateKey(VEHICLE_PRIVATE_KEY)),
  });
  assert.equal(sharedSecret.length, 32);
  const key = createHash('sha1').update(sharedSecret).digest().subarray(0, 16);
  assert.equal(key.toString('hex'), EXPECTED_SHARED_KEY);
});

test('official session-info KDF vector pins literal label and HMAC-SHA256', () => {
  const key = Buffer.from(EXPECTED_SHARED_KEY, 'hex');
  const derived = createHmac('sha256', key).update('session info', 'ascii').digest('hex');
  assert.equal(derived, EXPECTED_SESSION_INFO_KEY);

  const wrongLabel = createHmac('sha256', key).update('session-info', 'ascii').digest('hex');
  assert.notEqual(wrongLabel, EXPECTED_SESSION_INFO_KEY);
});

test('official AES-GCM command vector pins sorted TLV, AAD, nonce and tag layout', () => {
  const encoded = metadata([
    { tag: 0x07, value: u32be(2) },
    { tag: 0x05, value: u32be(7) },
    { tag: 0x04, value: u32be(2655) },
    { tag: 0x03, value: Buffer.from('4c463f9cc0d3d26906e982ed224adde6', 'hex') },
    { tag: 0x02, value: Buffer.from('5YJ30123456789ABC', 'ascii') },
    { tag: 0x01, value: Buffer.from([0x03]) },
    { tag: 0x00, value: Buffer.from([0x05]) },
  ]);
  assert.equal(
    encoded.toString('hex'),
    '000105010103021135594a333031323334353637383941424303104c463f9cc0d3d26906e982ed224adde6040400000a5f050400000007070400000002ff',
  );

  const key = Buffer.from(EXPECTED_SHARED_KEY, 'hex');
  const nonce = Buffer.from('dbf79447fa156674dae1caed', 'hex');
  const plaintext = Buffer.from('120452020801', 'hex');
  const aad = createHash('sha256').update(encoded).digest();
  const cipher = createCipheriv('aes-128-gcm', key, nonce, { authTagLength: 16 });
  cipher.setAAD(aad);
  const ciphertext = Buffer.concat([cipher.update(plaintext), cipher.final()]);
  const tag = cipher.getAuthTag();
  assert.equal(ciphertext.toString('hex'), '38038e8c0f2e');
  assert.equal(tag.toString('hex'), 'c228e0ff64991481db3a7bbc133696c5');

  const decipher = createDecipheriv('aes-128-gcm', key, nonce, { authTagLength: 16 });
  decipher.setAAD(aad);
  decipher.setAuthTag(tag);
  assert.deepEqual(Buffer.concat([decipher.update(ciphertext), decipher.final()]), plaintext);

  const tampered = Buffer.from(tag);
  tampered[0] ^= 0x01;
  const rejected = createDecipheriv('aes-128-gcm', key, nonce, { authTagLength: 16 });
  rejected.setAAD(aad);
  rejected.setAuthTag(tampered);
  rejected.update(ciphertext);
  assert.throws(() => rejected.final());
});

function occurrences(text, needle) {
  return text.split(needle).length - 1;
}

function replaceExactlyOnce(text, before, after) {
  assert.equal(occurrences(text, before), 1, `mutation fixture drifted: ${before}`);
  return text.replace(before, after);
}

test('all target locks pin the reviewed tesla-ble source and exact target set', () => {
  const targets = ['esp32', 'esp32s3', 'esp32c3', 'esp32c6'];
  for (const target of targets) {
    const lock = readFileSync(`dependencies.lock.${target}`, 'utf8');
    assert.match(lock, /version: 07a4ef503a52f736009fdeba953f185aecc863f3/);
    assert.match(lock, new RegExp(`^target: ${target}$`, 'm'));
    const listed = [...lock.matchAll(/^    - (esp32(?:s3|c3|c6)?)$/gm)].map((match) => match[1]);
    assert.deepEqual(listed, targets);
  }
});

// Patch 0004 is the one size-motivated patch in the series: it deletes VehicleAction oneof arms
// so --gc-sections can reach their nanopb descriptors. Deleting an arm the firmware DOES send
// would still compile and still shrink the image — it would fail only against a real vehicle.
// So assert the two properties that make it safe: it removes nothing but Parental Controls, and
// what it removes is disjoint from the tags vehicle_commands.cpp actually builds.
test('parental-controls trim patch removes only actions this firmware never sends', () => {
  const patch = readFileSync(
    'patches/tesla-ble/0004-drop-unused-parental-controls-actions.patch',
    'utf8',
  );
  const removed = patch
    .split('\n')
    .filter((line) => line.startsWith('-') && !line.startsWith('---'))
    .map((line) => line.slice(1));

  // A pure-deletion patch: rewriting a descriptor is not in this patch's remit.
  const added = patch
    .split('\n')
    .filter((line) => line.startsWith('+') && !line.startsWith('+++'));
  assert.deepEqual(added, []);

  // Only the two generated protobuf files, never hand-written upstream source.
  const files = [...patch.matchAll(/^diff --git a\/(\S+) b\/(\S+)$/gm)];
  assert.deepEqual(
    files.map((match) => match[1]),
    ['generated/include/car_server.pb.h', 'generated/src/car_server.pb.c'],
  );
  for (const [, before, after] of files) {
    assert.equal(before, after);
  }

  const arms = [
    ...removed.join('\n').matchAll(/\(vehicle_action_msg, ([A-Za-z]+),/g),
  ].map((match) => match[1]);
  const unique = [...new Set(arms)].sort();
  assert.deepEqual(unique, [
    'parentalControlsAction',
    'parentalControlsClearPinAction',
    'parentalControlsClearPinAdminAction',
    'parentalControlsEnableSettingsAction',
    'parentalControlsSetSpeedLimitAction',
  ]);

  // Each removed arm loses its MSGTYPE alias and its descriptor binding, and nothing else does.
  const msgtypes = removed.filter((line) => line.startsWith('#define'));
  const binds = removed.filter((line) => line.startsWith('PB_BIND('));
  assert.equal(msgtypes.length, unique.length);
  assert.equal(binds.length, unique.length);
  for (const line of [...msgtypes, ...binds]) {
    assert.match(line, /parentalControls/i);
  }

  // The safety property: none of these is a tag the firmware builds a command from.
  const commands = readFileSync('main/vehicle_commands.cpp', 'utf8');
  assert.match(commands, /CarServer_VehicleAction_chargingStartStopAction_tag/);
  for (const arm of unique) {
    assert.equal(
      commands.includes(`CarServer_VehicleAction_${arm}_tag`),
      false,
      `patch 0004 removes ${arm}, which vehicle_commands.cpp still sends`,
    );
  }
});

function validateSessionCounterReplayPatch(patch) {
  const files = [...patch.matchAll(/^diff --git a\/(\S+) b\/(\S+)$/gm)];
  assert.deepEqual(
    files.map((match) => match[1]),
    ['src/peer.cpp'],
  );
  for (const [, before, after] of files) {
    assert.equal(before, after);
  }

  // peer.cpp: must remove the hard rejection return TeslaBLE_Status_E_ERROR_COUNTER_REPLAY
  assert.match(patch, /-    return TeslaBLE_Status_E_ERROR_COUNTER_REPLAY;/);
  assert.match(
    patch,
    /\+    LOG_WARNING\("Session counter replay detected \(vehicle=%" PRIu32 ", local=%" PRIu32 "\) - keeping higher local counter"/,
  );
}

test('session counter replay patch aligns with signer.go', () => {
  const patch = readFileSync(
    'patches/tesla-ble/0005-align-session-counter-replay-with-signer-go.patch',
    'utf8',
  );
  validateSessionCounterReplayPatch(patch);

  const mutations = [
    replaceExactlyOnce(
      patch,
      '-    return TeslaBLE_Status_E_ERROR_COUNTER_REPLAY;',
      '+    return TeslaBLE_Status_E_ERROR_COUNTER_REPLAY;',
    ),
  ];
  for (const mutation of mutations) {
    assert.throws(() => validateSessionCounterReplayPatch(mutation));
  }
});

function functionBody(source, signature, nextSignature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, `${signature} must be defined`);
  const end = source.indexOf(nextSignature, start + signature.length);
  assert.ok(end > start, `${signature} scope bounded by ${nextSignature}`);
  return source.slice(start, end);
}

test('B1: TX path builds through the harness-tested helper and refuses malformed frames', () => {
  // The behaviour (builder output written unchanged, double prefix refused) is exercised against
  // the real tesla-ble by test/test_tesla_ble_harness.cpp and in test_logic.cpp. This pins the
  // IDF call site to those helpers so the harness keeps covering the production path.
  const telemetry = readFileSync('main/vehicle_telemetry.cpp', 'utf8');
  const body = functionBody(telemetry, 'void VehicleController::drive_command_runner_()',
                            'void VehicleController::loop_task_fn_(');
  const build = body.indexOf('tk::build_ble_tx_frame(');
  const guard = body.indexOf('tk::is_well_formed_ble_frame(tx_buffer_)');
  const write = body.indexOf('ble_->write(tx_buffer_)');
  assert.ok(build >= 0, 'drive_command_runner_ must build via tk::build_ble_tx_frame');
  assert.ok(guard > build, 'the structural guard must run after building');
  assert.ok(write > guard, 'the frame must be written only after the guard');
  assert.equal(occurrences(body, 'ble_->write('), 1, 'exactly one BLE write site');
  for (const forbidden of ['tx_buffer_.insert(', 'tx_buffer_[0]', 'tx_buffer_[1]', 'framed[0]']) {
    assert.equal(body.includes(forbidden), false, `must not touch the frame header (${forbidden})`);
  }

  const framing = readFileSync('main/logic/rx_framing.hpp', 'utf8');
  assert.match(framing, /int build_ble_tx_frame\(std::vector<uint8_t>& wire, size_t capacity, BuildFn&& build\)/);
  assert.match(framing, /inline bool is_well_formed_ble_frame\(const uint8_t\* wire, size_t size\) noexcept/);
});

test('B2: key regeneration runs the harness-tested transaction with a PEM-sized buffer', () => {
  const pairing = readFileSync('main/vehicle_pairing.cpp', 'utf8');
  const body = functionBody(pairing, 'bool VehicleController::regenerate_key_native_()',
                            'bool VehicleController::finish_key_rotation_cleanup_()');
  assert.ok(body.includes('tk::regenerate_private_key('),
            'regenerate_key_native_ must delegate to tk::regenerate_private_key');
  for (const direct of ['get_private_key(', 'create_private_key(', 'load_private_key(']) {
    assert.equal(body.includes(direct), false, `the transaction must not be re-implemented (${direct})`);
  }
  assert.match(body, /storage_->save\(tk::nvs_contract::kPrivateKey, new_key\)/);

  const rotation = readFileSync('main/logic/key_rotation.hpp', 'utf8');
  const capacity = rotation.match(/inline constexpr size_t kPrivateKeyPemCapacity = (\d+);/);
  assert.ok(capacity && Number(capacity[1]) >= 2048, 'PEM export capacity must stay >= 2048 B');
  assert.match(rotation, /KeyRegenerationResult regenerate_private_key\(Client& client, Persist&& persist,/);
});

test('H1: CarServer response is routed by UUID before delivering vehicleData telemetry callbacks', () => {
  const telemetry = readFileSync('main/vehicle_telemetry.cpp', 'utf8');
  const fnStart = telemetry.indexOf('void VehicleController::handle_carserver_frame_(');
  assert.ok(fnStart >= 0);
  const fnEnd = telemetry.indexOf('void VehicleController::process_rx_frame_(');
  assert.ok(fnEnd > fnStart);
  const fnBody = telemetry.slice(fnStart, fnEnd);

  const routeCall = fnBody.indexOf('command_runner_.handle_response(');
  const dropCheck = fnBody.indexOf('if (!outcome.routed) {');
  const callbackDelivery = fnBody.indexOf('if (response.which_response_msg == CarServer_Response_vehicleData_tag) {');

  assert.ok(routeCall >= 0, 'must route response via command_runner_.handle_response');
  assert.ok(dropCheck > routeCall, 'must check outcome.routed immediately after handle_response');
  assert.ok(callbackDelivery > dropCheck, 'must deliver vehicleData callbacks strictly AFTER successful routing');
});

test('H2: RxFramer is strictly single-owner on vehicle_loop', () => {
  const pairing = readFileSync('main/vehicle_pairing.cpp', 'utf8');
  assert.match(
    pairing,
    /command_runner_\.reset\(\/\*reset_framer=\*\/false\);/,
    'clear_session_and_cache_ must not reset RxFramer directly from external task',
  );
  assert.match(
    pairing,
    /request_framer_reset_\(\);/,
    'clear_session_and_cache_ must request framer reset via atomic flag',
  );

  const commands = readFileSync('main/vehicle_commands.cpp', 'utf8');
  assert.equal(
    commands.includes('command_runner_.rx_framer().reset()'),
    false,
    'vehicle_commands.cpp must not reset RxFramer directly',
  );
  assert.match(
    commands,
    /request_framer_reset_\(\);/,
    'vehicle_commands.cpp must request framer reset via atomic flag',
  );
});
