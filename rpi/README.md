# reokto-demo Raspberry Pi side

Turns a Raspberry Pi 4/5 (RPi OS Bookworm) into:

- a standalone WiFi access point (`reokto-net`, `10.42.0.1/24`),
- a Mosquitto MQTT broker on `:1883`,
- a Flask dashboard on `:80` to control an ESP32-WROOM-32D LED.

No upstream router is required. The Pi is the network.

## Architecture

```
            +---------------------------------------+
            | Raspberry Pi 4/5 (Bookworm)           |
            |                                       |
 phone ---->| wlan0 AP "reokto-net" 10.42.0.1/24    |
 laptop --->|   |                                    |
            |   +---> Flask dashboard  :80          |
            |   +---> Mosquitto broker :1883        |
            +---------------------------------------+
                           ^
                           | WiFi STA + MQTT
                           |
                       ESP32-WROOM-32D
                       LED on GPIO 2
```

## First-boot install

On a freshly imaged RPi OS Bookworm, with the repo copied to the Pi
(e.g. `~/reokto-demo`):

```bash
cd ~/reokto-demo/rpi
sudo ./setup.sh
```

The script is idempotent — re-running it just refreshes the AP
profile, mosquitto config, and dashboard files.

Override defaults via env vars:

```bash
sudo REOKTO_SSID=my-ssid REOKTO_PSK='something-better' ./setup.sh
```

Once it finishes:

| What             | Where                         |
| ---------------- | ----------------------------- |
| WiFi SSID        | `reokto-net`                  |
| WiFi password    | `reokto-pass-change-me`       |
| Pi address       | `10.42.0.1`                   |
| Dashboard        | <http://10.42.0.1/>           |
| MQTT broker      | `10.42.0.1:1883` (anonymous)  |

## ESP32 wiring + flashing

LED on GPIO 2:

```
GPIO 2 ---[220Ω]---|>|--- GND
                    LED
```

From the repo root:

```bash
pio run -t upload          # flash firmware
pio device monitor         # see serial logs
```

If your SSID/PSK differ from the defaults in `platformio.ini`, either
edit the `build_flags` there or create a gitignored `secrets.ini`:

```ini
[env:esp32dev]
build_flags =
    -DWIFI_SSID=\"my-ssid\"
    -DWIFI_PSK=\"my-psk\"
    -DMQTT_HOST=\"10.42.0.1\"
```

…and uncomment `extra_configs = secrets.ini` in `platformio.ini`.

## MQTT topics

| Topic             | Direction        | Retained | Payload                  |
| ----------------- | ---------------- | -------- | ------------------------ |
| `led/freq/set`    | dashboard → ESP  | yes      | integer Hz (1..50)       |
| `led/freq/state`  | ESP → dashboard  | yes      | integer Hz currently used|
| `led/status`      | ESP → dashboard  | yes (LWT)| `online` / `offline`     |

Quick smoke test from any AP-connected machine:

```bash
mosquitto_pub -h 10.42.0.1 -t led/freq/set -r -m 5    # set 5 Hz
mosquitto_sub -h 10.42.0.1 -t 'led/#' -v               # watch traffic
```

## Operations

```bash
sudo systemctl status reokto-dashboard      # dashboard service
sudo journalctl -u reokto-dashboard -f      # tail dashboard logs
sudo systemctl status mosquitto             # broker
nmcli con show reokto-ap                    # AP profile
nmcli con down reokto-ap && nmcli con up reokto-ap   # bounce the AP
```

## Gotchas

- **Power.** Pi 4/5 wants the official 3 A / 5 A supply. Under-voltage
  manifests as the AP randomly dropping clients — debug this first.
- **Single radio.** `wlan0` is in AP mode; the Pi has no internet.
  Plug in Ethernet (or a USB WiFi dongle as `wlan1`) if you need
  uplink for `apt`.
- **Security.** WPA2 SSID + anonymous MQTT is fine for a closed demo.
  Before exposing this anywhere real, add a Mosquitto password file
  (`mosquitto_passwd`) and require auth in the dashboard env vars.
