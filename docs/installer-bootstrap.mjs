import { ESPLoader, Transport } from "./vendor/esptool-js-0.6.1.bundle.js";
import { attachSerialPortRelease } from "./serial-port-release.mjs";
import { attachWebInstaller } from "./web-installer.mjs";

function configurePreview(documentImpl, locationImpl) {
  const match = locationImpl.pathname.match(/\/PR\/(\d+)\//);
  if (!match) return;

  const number = match[1].replace(/\D/g, "");
  const channel = documentImpl.getElementById("build-channel");
  const note = documentImpl.getElementById("preview-note");
  const text = documentImpl.getElementById("preview-note-text");
  if (!number || !channel || !note || !text) return;

  const link = documentImpl.createElement("a");
  const strong = documentImpl.createElement("strong");
  channel.textContent = `Preview build · PR #${number}`;
  channel.dataset.kind = "preview";
  strong.textContent = `Pre-merge firmware for PR #${number}. `;
  link.id = "preview-source-link";
  link.href = `https://github.com/0Bu/tesla-key-esp32/pull/${number}`;
  link.textContent = "View pull request while source metadata loads";
  text.append(
    strong,
    "This page flashes that PR's signed build; the device still checks OTA against main. ",
    link,
    "."
  );
  note.hidden = false;
}

export function bootstrapInstaller({
  documentImpl = globalThis.document,
  locationImpl = globalThis.location,
  navigatorImpl = globalThis.navigator,
  SerialPortCtor = globalThis.SerialPort
} = {}) {
  if (!documentImpl || !locationImpl) return null;
  configurePreview(documentImpl, locationImpl);

  const root = documentImpl.getElementById("web-installer");
  if (!root) return null;
  const serial = navigatorImpl && navigatorImpl.serial;
  const controller = attachWebInstaller({
    root,
    serial,
    ESPLoaderCtor: ESPLoader,
    TransportCtor: Transport
  });

  // Visibility is based on granted ports rather than the current installer connection, so a stale
  // browser grant remains removable even before the user reconnects the device in this tab.
  void attachSerialPortRelease({
    serial,
    SerialPortCtor,
    container: documentImpl.getElementById("serial-release"),
    button: documentImpl.getElementById("release-serial-port"),
    status: documentImpl.getElementById("serial-release-status"),
    onBeforeRelease: controller.disconnectPortForRelease
  });
  return controller;
}

bootstrapInstaller();
