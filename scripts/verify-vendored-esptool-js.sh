#!/usr/bin/env bash
# Verify the immutable npm source contract and the exact locally-served esptool-js bytes.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version=0.6.1
tarball_url="https://registry.npmjs.org/esptool-js/-/esptool-js-${version}.tgz"
expected_sri='sha512-WNgQTfaEIgHyEiT56pI5v7Tq6Pzjc2XaibLxAtWY4v3zE2Ofk5ImkJY5foEUr0JrdkfHWf6rNizAewN4/kSpHw=='
expected_bundle_sha256=ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152
expected_license_sha256=1c25f29242785d63e9adb0be7fdc137551dfbbc0756b622fcd26d9cd7de3a4f3
bundle="$repo_root/docs/vendor/esptool-js-${version}.bundle.js"
license="$repo_root/docs/vendor/esptool-js-${version}.LICENSE"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

for path in "$bundle" "$license"; do
  [[ -f "$path" && ! -L "$path" ]] || { echo "missing/unsafe vendored file: $path" >&2; exit 1; }
done
[[ "$(sha256 "$bundle")" == "$expected_bundle_sha256" ]] || {
  echo "vendored esptool-js bundle hash mismatch" >&2; exit 1;
}
[[ "$(sha256 "$license")" == "$expected_license_sha256" ]] || {
  echo "vendored esptool-js license hash mismatch" >&2; exit 1;
}

case "${1:-}" in
  "") ;;
  --source)
    command -v curl >/dev/null 2>&1 || { echo "curl required for --source" >&2; exit 1; }
    command -v openssl >/dev/null 2>&1 || { echo "openssl required for --source" >&2; exit 1; }
    temp="$(mktemp -d)"
    trap 'rm -rf "$temp"' EXIT
    curl -fsSL "$tarball_url" -o "$temp/package.tgz"
    actual_sri="sha512-$(openssl dgst -sha512 -binary "$temp/package.tgz" | openssl base64 -A)"
    [[ "$actual_sri" == "$expected_sri" ]] || {
      echo "npm tarball SRI mismatch: expected $expected_sri, got $actual_sri" >&2; exit 1;
    }
    tar -xOf "$temp/package.tgz" package/bundle.js > "$temp/bundle.js"
    tar -xOf "$temp/package.tgz" package/LICENSE > "$temp/LICENSE"
    cmp "$temp/bundle.js" "$bundle" || { echo "vendored bundle differs from npm tarball" >&2; exit 1; }
    cmp "$temp/LICENSE" "$license" || { echo "vendored license differs from npm tarball" >&2; exit 1; }
    ;;
  *) echo "usage: verify-vendored-esptool-js.sh [--source]" >&2; exit 2 ;;
esac

echo "vendored esptool-js $version: PASS (native ESM; ESPLoader/Transport named exports)"
