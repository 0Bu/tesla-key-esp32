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

test('repository patch text preserves anti-replay early return before dispatch', () => {
  const patch = readFileSync('patches/tesla-ble/0001-reject-replayed-carserver-responses.patch', 'utf8');
  const guard = patch.indexOf('!peer->validate_response_counter(response_counter)');
  const earlyReturn = patch.indexOf('+    return;', guard);
  const firstDispatch = patch.indexOf('if (response.which_response_msg', guard);
  assert.ok(guard >= 0 && earlyReturn > guard && firstDispatch > earlyReturn);
  assert.match(patch, /old actionStatus=OK must not acknowledge a new charging limit/);
});

test('repository key-regeneration patch remains transactional and fail-closed', () => {
  const patch = readFileSync('patches/tesla-ble/0002-report-key-regeneration-result.patch', 'utf8');
  const exportOld = patch.indexOf('client_->get_private_key');
  const createNew = patch.indexOf('client_->create_private_key');
  const persistNew = patch.indexOf('persist_private_key_');
  const restoreOld = patch.indexOf('client_->load_private_key');
  assert.match(patch, /\+  bool regenerate_key\(\);/);
  assert.match(patch, /\+  bool has_private_key\(\) const/);
  assert.ok(exportOld >= 0 && createNew > exportOld && persistNew > createNew && restoreOld > persistNew);
  assert.match(patch, /Failed to export existing private key before regeneration/);
  assert.match(patch, /Failed to restore previous in-memory private key after persistence failure/);
});

function occurrences(text, needle) {
  return text.split(needle).length - 1;
}

function replaceExactlyOnce(text, before, after) {
  assert.equal(occurrences(text, before), 1, `mutation fixture drifted: ${before}`);
  return text.replace(before, after);
}

function validateRxRecoveryLogPatch(patch) {
  const addedLines = patch
    .split('\n')
    .filter((line) => line.startsWith('+') && !line.startsWith('+++'))
    .map((line) => line.slice(1));
  const removedLines = patch
    .split('\n')
    .filter((line) => line.startsWith('-') && !line.startsWith('---'))
    .map((line) => line.slice(1).trim());
  const added = addedLines.join('\n');

  assert.equal(occurrences(added, '#include <cstdint>'), 1);
  assert.equal(
    occurrences(
      added,
      'std::chrono::steady_clock::time_point last_rx_recovery_warning_log_{};',
    ),
    1,
  );
  assert.equal(
    occurrences(
      added,
      'std::chrono::steady_clock::time_point last_rx_recovery_error_log_{};',
    ),
    1,
  );
  assert.equal(occurrences(added, 'uint32_t suppressed_rx_recovery_logs_{0};'), 1);
  assert.equal(
    occurrences(added, 'void log_rx_recovery_(const char *reason, bool severe = false);'),
    1,
  );

  const functionStart = added.indexOf(
    'void Vehicle::log_rx_recovery_(const char *reason, bool severe) {',
  );
  assert.ok(functionStart >= 0);
  const functionEnd = added.indexOf('\n}\n', functionStart);
  assert.ok(functionEnd > functionStart);
  const body = added.slice(functionStart, functionEnd + 2);

  assert.equal(occurrences(body, 'constexpr auto kLogInterval = std::chrono::hours(1);'), 1);
  assert.equal(occurrences(body, 'std::chrono::hours('), 1);
  assert.equal(occurrences(body, 'const auto now = std::chrono::steady_clock::now();'), 1);
  assert.equal(
    occurrences(
      body,
      'auto &last_log = severe ? last_rx_recovery_error_log_ : last_rx_recovery_warning_log_;',
    ),
    1,
  );
  assert.equal(occurrences(added, 'last_rx_recovery_warning_log_'), 2);
  assert.equal(occurrences(added, 'last_rx_recovery_error_log_'), 2);

  const suppressionBlock = `  if (last_log != std::chrono::steady_clock::time_point{} &&
      now - last_log < kLogInterval) {
    if (suppressed_rx_recovery_logs_ != UINT32_MAX) {
      ++suppressed_rx_recovery_logs_;
    }
    return;
  }`;
  assert.equal(occurrences(body, suppressionBlock), 1);
  assert.equal(occurrences(body, 'UINT32_MAX'), 1);
  assert.equal(occurrences(body, '++suppressed_rx_recovery_logs_;'), 1);

  const severityBlock = `  if (severe) {
    LOG_ERROR("RX framing recovery: %s; suppressed %" PRIu32 " similar events", reason,
              suppressed_rx_recovery_logs_);
  } else {
    LOG_WARNING("RX framing recovery: %s; suppressed %" PRIu32 " similar events", reason,
                suppressed_rx_recovery_logs_);
  }`;
  assert.equal(occurrences(body, severityBlock), 1);
  assert.equal(occurrences(body, 'last_log = now;'), 1);
  assert.equal(occurrences(body, 'suppressed_rx_recovery_logs_ = 0;'), 1);
  const severityAt = body.indexOf(severityBlock);
  const clockResetAt = body.indexOf('last_log = now;');
  const suppressionResetAt = body.indexOf('suppressed_rx_recovery_logs_ = 0;');
  assert.ok(severityAt >= 0 && clockResetAt > severityAt && suppressionResetAt > clockResetAt);

  const calls = addedLines
    .filter((line) => /^\s{4,}log_rx_recovery_\(/.test(line))
    .map((line) => line.trim());
  assert.deepEqual(calls, [
    'log_rx_recovery_("invalid message length; attempting recovery");',
    'log_rx_recovery_("severe buffer corruption; clearing buffer", true);',
    'log_rx_recovery_("recovery failed; clearing buffer");',
    'log_rx_recovery_("recovery produced invalid length; clearing buffer");',
    'log_rx_recovery_("message parse failed; attempting recovery");',
    'log_rx_recovery_("recovery failed after parse error; clearing buffer");',
  ]);
  assert.equal(calls.filter((line) => line.endsWith(', true);')).length, 1);
  assert.equal(calls[1], 'log_rx_recovery_("severe buffer corruption; clearing buffer", true);');

  assert.deepEqual(
    removedLines.filter((line) => /^LOG_(?:ERROR|WARNING)\(/.test(line)),
    [
      'LOG_ERROR("Invalid message length %d, attempting buffer recovery", msg_len);',
      'LOG_ERROR("Severe buffer corruption detected (length: %d), clearing buffer", msg_len);',
      'LOG_WARNING("Buffer recovery failed, clearing all data");',
      'LOG_WARNING("Buffer recovery produced invalid length %d, clearing buffer", msg_len);',
      'LOG_ERROR("Failed to parse Universal Message (buffer size: %zu) - attempting buffer recovery", rx_buffer_.size());',
      'LOG_WARNING("Buffer recovery failed after parse error, clearing all data");',
    ],
  );
}

test('repository RX framing recovery patch rate-limits all six callsites fail-closed', () => {
  const patch = readFileSync(
    'patches/tesla-ble/0003-rate-limit-rx-framing-recovery-logs.patch',
    'utf8',
  );
  validateRxRecoveryLogPatch(patch);

  const mutations = [
    replaceExactlyOnce(
      patch,
      'std::chrono::hours(1)',
      'std::chrono::minutes(1)',
    ),
    replaceExactlyOnce(
      patch,
      'severe ? last_rx_recovery_error_log_ : last_rx_recovery_warning_log_',
      'severe ? last_rx_recovery_warning_log_ : last_rx_recovery_warning_log_',
    ),
    replaceExactlyOnce(
      patch,
      'suppressed_rx_recovery_logs_ != UINT32_MAX',
      'suppressed_rx_recovery_logs_ <= UINT32_MAX',
    ),
    (() => {
      const withoutReset = replaceExactlyOnce(
        patch,
        '+  suppressed_rx_recovery_logs_ = 0;\n',
        '',
      );
      return replaceExactlyOnce(
        withoutReset,
        '+  if (severe) {\n',
        '+  suppressed_rx_recovery_logs_ = 0;\n+  if (severe) {\n',
      );
    })(),
    (() => {
      const placeholder = replaceExactlyOnce(
        patch,
        'LOG_ERROR("RX framing recovery:',
        'LOG_TEMP("RX framing recovery:',
      );
      const swappedWarning = replaceExactlyOnce(
        placeholder,
        'LOG_WARNING("RX framing recovery:',
        'LOG_ERROR("RX framing recovery:',
      );
      return replaceExactlyOnce(
        swappedWarning,
        'LOG_TEMP("RX framing recovery:',
        'LOG_WARNING("RX framing recovery:',
      );
    })(),
    replaceExactlyOnce(
      patch,
      '+    log_rx_recovery_("invalid message length; attempting recovery");\n',
      '',
    ),
    replaceExactlyOnce(
      patch,
      'log_rx_recovery_("severe buffer corruption; clearing buffer", true);',
      'log_rx_recovery_("severe buffer corruption; clearing buffer");',
    ),
  ];
  for (const mutation of mutations) {
    assert.throws(() => validateRxRecoveryLogPatch(mutation));
  }
});

test('all target locks pin the reviewed tesla-ble source and exact target set', () => {
  const targets = ['esp32', 'esp32s3', 'esp32c3', 'esp32c6'];
  for (const target of targets) {
    const lock = readFileSync(`dependencies.lock.${target}`, 'utf8');
    assert.match(lock, /version: 54ee51f1c82ae6937b00f6c2347d3fb8a9f06dce/);
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
    ['src/peer.cpp', 'src/vehicle.cpp'],
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

  // vehicle.cpp: must eliminate the force_update_session call site
  assert.match(patch, /-    update_result = peer->force_update_session\(&session_info\);/);
  assert.match(
    patch,
    /\+  if \(peer && peer->update_session\(&session_info\) == TeslaBLE_Status_E_OK\) \{/,
  );
  assert.equal(patch.includes('+    update_result = peer->force_update_session'), false);
}

test('session counter replay patch aligns with signer.go and removes force_update_session dead code', () => {
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
    replaceExactlyOnce(
      patch,
      '-    update_result = peer->force_update_session(&session_info);',
      '+    update_result = peer->force_update_session(&session_info);',
    ),
    replaceExactlyOnce(
      patch,
      '+  if (peer && peer->update_session(&session_info) == TeslaBLE_Status_E_OK) {',
      '+  if (peer && peer->update_session(&session_info) != TeslaBLE_Status_E_OK) {',
    ),
  ];
  for (const mutation of mutations) {
    assert.throws(() => validateSessionCounterReplayPatch(mutation));
  }
});
