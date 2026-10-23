#!/usr/bin/env bash
# Verify the immutable npm source contract and the exact locally-served esptool-js bytes.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version=0.6.1
tarball_url="https://registry.npmjs.org/esptool-js/-/esptool-js-${version}.tgz"
expected_sri='sha512-WNgQTfaEIgHyEiT56pI5v7Tq6Pzjc2XaibLxAtWY4v3zE2Ofk5ImkJY5foEUr0JrdkfHWf6rNizAewN4/kSpHw=='
expected_bundle_sha256=ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152
expected_license_sha256=1c25f29242785d63e9adb0be7fdc137551dfbbc0756b622fcd26d9cd7de3a4f3
expected_pako_license_sha256=a04665b3b2de56c66730c1f720f528175739e4104f79073614aa611da1e85539
expected_pako_zlib_sha256=d8b499598e43d755ea8918448128259ff01820c50d94d5a48c8883d0a594ddb1
bundle="$repo_root/docs/vendor/esptool-js-${version}.bundle.js"
license="$repo_root/docs/vendor/esptool-js-${version}.LICENSE"
pako_license="$repo_root/docs/vendor/pako-2.1.0.LICENSE"
pako_zlib="$repo_root/docs/vendor/pako-2.1.0.zlib.README"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

for path in "$bundle" "$license" "$pako_license" "$pako_zlib"; do
  [[ -f "$path" && ! -L "$path" ]] || { echo "missing/unsafe vendored file: $path" >&2; exit 1; }
done
[[ "$(sha256 "$bundle")" == "$expected_bundle_sha256" ]] || {
  echo "vendored esptool-js bundle hash mismatch" >&2; exit 1;
}
[[ "$(sha256 "$license")" == "$expected_license_sha256" ]] || {
  echo "vendored esptool-js license hash mismatch" >&2; exit 1;
}
[[ "$(sha256 "$pako_license")" == "$expected_pako_license_sha256" ]] || {
  echo "vendored pako license hash mismatch" >&2; exit 1;
}
[[ "$(sha256 "$pako_zlib")" == "$expected_pako_zlib_sha256" ]] || {
  echo "vendored pako zlib notice hash mismatch" >&2; exit 1;
}

case "${1:-}" in
  "") ;;
  --source)
    command -v curl >/dev/null 2>&1 || { echo "curl required for --source" >&2; exit 1; }
    command -v openssl >/dev/null 2>&1 || { echo "openssl required for --source" >&2; exit 1; }
    temp="$(mktemp -d)"
    trap 'rm -rf "$temp"' EXIT
    curl -fsSL --connect-timeout 10 --max-time 45 "$tarball_url" -o "$temp/package.tgz"
    actual_sri="sha512-$(openssl dgst -sha512 -binary "$temp/package.tgz" | openssl base64 -A)"
    [[ "$actual_sri" == "$expected_sri" ]] || {
      echo "npm tarball SRI mismatch: expected $expected_sri, got $actual_sri" >&2; exit 1;
    }
    tar -xOf "$temp/package.tgz" package/bundle.js > "$temp/bundle.js"
    tar -xOf "$temp/package.tgz" package/LICENSE > "$temp/LICENSE"
    cmp "$temp/bundle.js" "$bundle" || { echo "vendored bundle differs from npm tarball" >&2; exit 1; }
    cmp "$temp/LICENSE" "$license" || { echo "vendored license differs from npm tarball" >&2; exit 1; }
    curl -fsSL --connect-timeout 10 --max-time 20 https://raw.githubusercontent.com/nodeca/pako/2.1.0/LICENSE -o "$temp/pako.LICENSE"
    curl -fsSL --connect-timeout 10 --max-time 20 https://raw.githubusercontent.com/nodeca/pako/2.1.0/lib/zlib/README -o "$temp/pako.zlib.README"
    cmp "$temp/pako.LICENSE" "$pako_license" || { echo "vendored pako license differs from upstream" >&2; exit 1; }
    cmp "$temp/pako.zlib.README" "$pako_zlib" || { echo "vendored pako zlib notice differs from upstream" >&2; exit 1; }
    ;;
  *) echo "usage: verify-vendored-esptool-js.sh [--source]" >&2; exit 2 ;;
esac

echo "vendored esptool-js $version: PASS (native ESM; ESPLoader/Transport named exports)"
