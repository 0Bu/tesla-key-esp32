# Installer third-party notices

The locally served `esptool-js` 0.7.0 bundle includes these components:

| Component | License and copyright notices | Upstream source |
| --- | --- | --- |
| esptool-js 0.7.0 | [Apache-2.0](vendor/esptool-js-0.7.0.LICENSE) | [Release package](https://www.npmjs.com/package/esptool-js/v/0.7.0) |
| pako 2.1.0 | [MIT](vendor/pako-2.1.0.LICENSE) and [zlib](vendor/pako-2.1.0.zlib.README) | [Release source](https://github.com/nodeca/pako/tree/2.1.0) |

These notices accompany the unchanged JavaScript bundle in the assembled installer site.
`scripts/verify-vendored-esptool-js.sh` pins their exact bytes; `--source` also compares them
with their upstream distribution. CI runs this bounded network check in the unprivileged host
job; a mismatch or unavailable source fails that gate. Offline repository lint does not fetch it.
