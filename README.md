# esp32-tesla-key

Ein ESP32-S3 Mikrocontroller als BLE-Schlüssel für Tesla-Fahrzeuge mit HTTP-API.  
Das Gerät verbindet sich per Bluetooth Low Energy (BLE) mit dem Tesla und stellt eine REST-API bereit — kompatibel mit [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy) und [evcc](https://evcc.io).

```
[evcc / Home Assistant]
        │  HTTP (Port 80)
        ▼
  [ESP32-S3] ─── BLE ───► [Tesla Model 3/Y/S/X]
  (im Heimnetz)
```

## Hardware-Voraussetzungen

### Unterstützte Boards

| Board | Flash | BLE | PSRAM | Empfehlung |
|-------|-------|-----|-------|------------|
| [ESP32-S3-DevKitC-1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1.html) | 8 MB | ✓ | optional | ⭐ Einfachster Einstieg |
| [Seeed XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) | 8 MB | ✓ | — | Klein, preiswert |
| [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) | 16 MB | ✓ | 8 MB | Mit Display |
| [Waveshare ESP32-S3](https://www.waveshare.com/wiki/ESP32-S3-DEV-KIT-N8R8) | 8 MB | ✓ | 8 MB | Günstig |

**Mindestanforderungen:**
- Chip: **ESP32-S3** (hat integrierten Bluetooth 5.0 LE)
- Flash: **≥ 4 MB** (8 MB empfohlen — Firmware ~2,5 MB, Rest für OTA)
- RAM: **512 KB SRAM** (kein PSRAM nötig)
- Anschluss: USB-C oder USB-Micro (für erstmaliges Flashen)
- Stromversorgung: 3,3 V oder 5 V via USB

> **Nicht geeignet:** ESP32 (original), ESP32-S2, ESP32-C3 — entweder kein BLE oder falscher Chip-Typ

### Kabel & Zubehör
- USB-Kabel (Daten, kein reines Ladekabel)
- Optional: Permanente 5 V Stromversorgung über USB-Netzteil am Verbauort nahe dem Tesla-Stellplatz

---

## Voraussetzungen (einmalig)

### 1. ESP-IDF installieren

```bash
# macOS / Linux (Homebrew)
brew install cmake ninja dfu-util

mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.3.2   # oder aktuelles Stable-Release
./install.sh esp32s3

# Shell-Integration (in ~/.zshrc oder ~/.bashrc eintragen)
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

### 2. ESP-IDF aktivieren

```bash
get_idf   # muss in jeder neuen Shell-Session aufgerufen werden
```

Prüfen:
```bash
idf.py --version   # sollte "ESP-IDF v5.x.x" ausgeben
```

### 3. Repo klonen

```bash
cd ~/esp   # oder beliebiger Pfad
git clone https://github.com/DEIN-USERNAME/esp32-tesla-key.git
cd esp32-tesla-key
```

---

## Build & Flash

### Schritt 1 — Target setzen

```bash
idf.py set-target esp32s3
```

### Schritt 2 — WiFi und VIN konfigurieren

```bash
idf.py menuconfig
```

Im Menü navigieren: **Tesla Key Configuration**

| Einstellung | Beschreibung |
|-------------|--------------|
| `WiFi SSID` | Name deines Heimnetzwerks |
| `WiFi Password` | WLAN-Passwort |
| `Tesla Vehicle VIN` | 17-stellige Fahrzeug-Identifizierungsnummer |
| `Tesla BLE MAC` | Optional — leer lassen, wird automatisch gefunden |

> Die VIN findet sich in der Tesla-App unter **Fahrzeug → Informationen** oder im Fahrzeugschein.

Mit `Q` beenden, Änderungen speichern.

### Schritt 3 — Kompilieren

```bash
idf.py build
```

Beim ersten Build wird `yoziru/tesla-ble` automatisch von GitHub heruntergeladen (erfordert Internetverbindung). Das dauert beim ersten Mal 2–4 Minuten.

### Schritt 4 — ESP32 verbinden und flashen

USB-Kabel einstecken, dann:

```bash
# Serielle Ports anzeigen
ls /dev/tty.usb*       # macOS
ls /dev/ttyUSB* /dev/ttyACM*   # Linux

# Flashen (Port anpassen)
idf.py -p /dev/tty.usbserial-0001 flash
```

Falls der ESP32 sich nicht automatisch in den Flash-Modus versetzt:
1. `BOOT`-Taste gedrückt halten
2. `RESET`-Taste kurz drücken
3. `BOOT`-Taste loslassen
4. Flash-Befehl ausführen

### Schritt 5 — Serielle Ausgabe überwachen

```bash
idf.py -p /dev/tty.usbserial-0001 monitor
```

Erwartete Ausgabe beim ersten Start:
```
I (500) main: VIN: 5YJ3E1EA1JF000001  BLE MAC: (scan)
I (600) wifi: WiFi connected to 'MyNetwork'
I (650) main: IP: 192.168.1.42
I (700) http_server: HTTP server started on :80
I (700) main: esp32-tesla-key running. API on port 80.
```

**Ctrl+]** beendet den Monitor.

---

## Alternative: Provisioning ohne Neukompilieren

WiFi und VIN können auch nachträglich auf ein bereits geflashtes Gerät geschrieben werden:

```bash
pip install esptool nvs_partition_gen

python provision.py \
    --port /dev/tty.usbserial-0001 \
    --ssid "MeinNetz" \
    --password "geheim" \
    --vin "5YJ3E1EA1JF000001"
```

Dies schreibt nur die NVS-Konfigurationspartition neu — die Firmware bleibt unverändert.

---

## Tesla Key aktivieren

Das ist der einmalige Schritt, bei dem der ESP32 als vertrauenswürdiger BLE-Schlüssel beim Fahrzeug registriert wird.

### Voraussetzung
- ESP32 läuft, ist im WLAN und hat eine IP-Adresse (aus Serial Monitor ablesen)
- Tesla-Fahrzeug steht in BLE-Reichweite (ca. 10 Meter)
- Du hast physischen Zugang zum Fahrzeug
- Eine Tesla NFC-Karte (kommt mit dem Fahrzeug)

### Schritt 1 — ECDSA-Schlüsselpaar generieren

```bash
curl -X POST http://192.168.1.42/gen_keys
```

Antwort:
```json
{"result": true, "reason": "key generated — use /send_key to pair with vehicle"}
```

Der private Schlüssel wird im NVS-Flash des ESP32 gespeichert. Er verlässt das Gerät nie.

### Schritt 2 — Schlüssel an das Fahrzeug senden

```bash
# Empfohlen: Charging Manager Rolle (nur Laden + Aufwecken)
curl -X POST http://192.168.1.42/send_key

# Oder: Owner-Rolle (voller Zugriff: Türen, Klima, alles)
curl -X POST "http://192.168.1.42/send_key?role=owner"
```

Antwort:
```json
{
  "result": true,
  "role": "charging_manager",
  "reason": "key sent — tap NFC card on Tesla center console to confirm"
}
```

### Schritt 3 — Bestätigung am Fahrzeug

> ⚠️ Dieser Schritt muss **innerhalb von ~30 Sekunden** nach dem `/send_key`-Aufruf erfolgen.

1. **Fahrzeugtür öffnen** (Fahrzeug muss aufgesperrt und wach sein)
2. Auf dem **Touchscreen**: erscheint eine Meldung "Neuer Schlüssel hinzufügen?"
3. **NFC-Karte** (die mitgelieferte Tesla-Karte) auf das **Kartenlesegerät auf der Mittelkonsole** legen
4. Touchscreen bestätigt "Schlüssel hinzugefügt"

Der ESP32 ist jetzt ein vertrauenswürdiger Schlüssel und kann BLE-Befehle senden.

### Schritt 4 — Verbindung testen

```bash
# Fahrzeug aufwecken
curl -X POST http://192.168.1.42/api/1/vehicles/5YJ3E1EA1JF000001/command/wake_up

# Ladezustand abfragen
curl http://192.168.1.42/api/1/vehicles/5YJ3E1EA1JF000001/vehicle_data
```

Beim ersten Befehl nach längerem Idle dauert es 3–8 Sekunden (BLE-Scan + Verbindungsaufbau + Session-Handshake). Folgebefehle nutzen die bestehende Verbindung.

### Rollen-Übersicht

| Rolle | Befehle |
|-------|---------|
| `charging_manager` (Standard) | Aufladen steuern, Aufwecken, Ladestatus lesen |
| `owner` | Zusätzlich: Türen sperren/öffnen, Klima, Lichter, Hupe, Sentry |

> ℹ️ Tesla erlaubt **maximal 3 gleichzeitig aktive BLE-Schlüssel** pro Fahrzeug.  
> Einen alten Schlüssel entfernen: Tesla-App → Schlösser → Schlüssel verwalten.

---

## API-Referenz

Basis-URL: `http://<ESP32-IP>`

### Befehle

```
POST /api/1/vehicles/{VIN}/command/{befehl}
Content-Type: application/json
```

| Befehl | Body | Beschreibung |
|--------|------|--------------|
| `wake_up` | — | Fahrzeug aufwecken |
| `charge_start` | — | Laden starten |
| `charge_stop` | — | Laden stoppen |
| `set_charging_amps` | `{"charging_amps": 11}` | Ladestrom in Ampere (0–32) |
| `set_charge_limit` | `{"percent": 80}` | Ladelimit in % (50–100) |
| `charge_port_door_open` | — | Ladeklappe öffnen |
| `charge_port_door_close` | — | Ladeklappe schließen |
| `door_lock` | — | Türen sperren |
| `door_unlock` | — | Türen öffnen |
| `flash_lights` | — | Lichter blinken |
| `honk_horn` | — | Hupen |
| `set_sentry_mode` | `{"on": true}` | Sentry-Modus an/aus |
| `auto_conditioning_start` | — | Klimaanlage starten |
| `auto_conditioning_stop` | — | Klimaanlage stoppen |

**Beispielantwort:**
```json
{
  "response": {
    "result": true,
    "command": "charge_start",
    "vin": "5YJ3E1EA1JF000001",
    "reason": "command executed successfully"
  }
}
```

### Fahrzeugdaten

```
GET /api/1/vehicles/{VIN}/vehicle_data
```

```json
{
  "response": {
    "result": true,
    "data": {
      "charge_state": {
        "charging_state": "Charging",
        "battery_level": 72,
        "charge_limit_soc": 80,
        "charger_power": 11,
        "charge_rate": 58.3,
        "charging_amps": 16,
        "battery_range": 280.5
      }
    }
  }
}
```

### Fahrzeugstatus (ohne Aufwecken)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```

```json
{
  "response": {
    "result": true,
    "data": {
      "vehicle_lock_state": "LOCKED",
      "vehicle_sleep_status": "ASLEEP",
      "user_presence": "NOT_PRESENT"
    }
  }
}
```

### Schlüsselverwaltung

```
POST /gen_keys               → Schlüsselpaar generieren
POST /send_key               → Schlüssel senden (charging_manager)
POST /send_key?role=owner    → Schlüssel senden (owner)
GET  /api/proxy/1/version    → Firmware-Version
```

---

## evcc Integration

In `evcc.yaml`:

```yaml
vehicles:
  - name: Tesla
    type: tesla-ble-http
    url: http://192.168.1.42   # IP des ESP32
    vin: 5YJ3E1EA1JF000001
```

evcc ruft dann automatisch folgende Endpunkte auf:
- `GET /api/1/vehicles/{VIN}/vehicle_data` — für Ladestatus (SOC, Limit)
- `POST /api/1/vehicles/{VIN}/command/charge_start` — Laden starten
- `POST /api/1/vehicles/{VIN}/command/charge_stop` — Laden stoppen
- `POST /api/1/vehicles/{VIN}/command/set_charging_amps` — Ladestrom setzen

---

## Fehlersuche

### ESP32 verbindet sich nicht mit WiFi
- SSID/Passwort in `menuconfig` prüfen (Groß-/Kleinschreibung beachten)
- WPA2-Netz vorausgesetzt; Enterprise-WLAN wird nicht unterstützt
- Nach Änderung: Neuflashen erforderlich (oder `provision.py`)

### BLE-Scan findet Fahrzeug nicht
- Fahrzeug muss in Reichweite sein (≤10m, kein Metallwand dazwischen)
- BLE-Scan startet nach WiFi-Verbindung — beide müssen aktiv sein
- Im Serial Monitor nach `"Tesla BLE found"` oder `"scanning for Tesla BLE..."` schauen
- Fahrzeug mit der Tesla-App aufwecken, dann erneut versuchen

### Befehl scheitert mit Timeout
```
W vehicle_ctrl: 'charge_start' timed out
```
- Fahrzeug ist tief im Schlaf und antwortet nicht auf BLE
- Zuerst `wake_up` aufrufen, 5 Sekunden warten, dann Befehl wiederholen
- Session-Daten können ungültig geworden sein: NVS-Sessions löschen:
  ```bash
  idf.py erase-flash && idf.py flash
  ```

### "key sent" aber Fahrzeug bestätigt nicht
- Schritt 3 muss **innerhalb 30 Sekunden** abgeschlossen sein
- Fahrzeug muss aufgesperrt und aktiv sein (kein Deep Sleep)
- NFC-Karte direkt auf die Mittelkonsole legen (nicht auf das Touchscreen-Glas)
- Erneut versuchen: `/gen_keys` → `/send_key` → NFC legen

### Schlüssel wird vom Fahrzeug abgelehnt
Alten Schlüssel entfernen und neu registrieren:
1. Tesla-App → Sicherheit → Schlüssel → alten ESP32-Schlüssel löschen
2. Im Serial Monitor: `idf.py erase-flash && idf.py flash`
3. Kompletten Key-Aktivierungs-Prozess neu durchführen

### Serielle Port-Berechtigung (Linux)
```bash
sudo usermod -aG dialout $USER   # Gruppe hinzufügen
# Neu anmelden oder:
newgrp dialout
```

---

## Sicherheitshinweise

- Der **private BLE-Schlüssel** liegt unverschlüsselt im Flash des ESP32
- Physischer Zugang zum ESP32 = potenzieller Fahrzeugzugang (mit `owner`-Rolle)
- **Empfehlung**: `charging_manager`-Rolle verwenden (kein Türöffnen möglich)
- Das Gerät **nicht im öffentlichen Netz** exponieren; nur im lokalen Heimnetz betreiben
- Keine HTTPs/Auth eingebaut — bei Bedarf hinter nginx-Proxy mit BasicAuth betreiben

---

## Technische Details

- **BLE-Protokoll**: Tesla VCSEC + Infotainment Protocol (Protobuf über GATT)
- **Service UUID**: `00000211-b2d1-43f0-9b88-960cebf8b91e`
- **Verschlüsselung**: ECDH-Schlüsselaustausch + AES-GCM (via mbedTLS)
- **Signierung**: ECDSA P-256 (Schlüssel im ESP32 NVS)
- **Bibliothek**: [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) v5.1.1
- **BLE-Stack**: NimBLE (leichter als Bluedroid, ~200 KB weniger Flash)
- **Fragmentierung**: 20-Byte-Chunks pro BLE-Write (kompatibel mit allen MTU-Konfigurationen)

## Lizenz

MIT License — [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) unterliegt einer eigenen MIT-Lizenz.
