import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.bundle.js";
import { attachWebInstaller } from "./web-installer.mjs";

export function bootstrapInstaller({
  documentImpl = globalThis.document,
  locationImpl = globalThis.location,
  navigatorImpl = globalThis.navigator,
  SerialPortCtor = globalThis.SerialPort
} = {}) {
  if (!documentImpl || !locationImpl) return null;
  const root = documentImpl.getElementById("web-installer");
  if (!root) return null;
  const controller = attachWebInstaller({
    root,
    serial: navigatorImpl && navigatorImpl.serial,
    SerialPortCtor,
    ESPLoaderCtor: ESPLoader,
    TransportCtor: Transport,
    locationImpl,
    // ?trace adds esptool-js's packet trace to the log.
    trace: new URLSearchParams(locationImpl.search).has("trace")
  });
  // A single port this page is already paired with reconnects without the chooser.
  controller.ready.then(() => controller.reconnectPaired());
  return controller;
}

bootstrapInstaller();
