#!/usr/bin/env bash
# One-shot installer for the reokto-demo Raspberry Pi side.
#
# Performs:
#   1. apt install of Mosquitto + Flask + paho-mqtt
#   2. NetworkManager hotspot on wlan0 (SSID reokto-net, 10.42.0.1/24, WPA2)
#   3. Drop-in mosquitto config (listener on :1883, anonymous local access)
#   4. Install Flask dashboard under /opt/reokto-dashboard
#   5. Enable + start the reokto-dashboard.service systemd unit
#
# Tested on Raspberry Pi OS Bookworm (NetworkManager-based).

set -euo pipefail

SSID="${REOKTO_SSID:-reokto-net}"
PSK="${REOKTO_PSK:-reokto-pass-change-me}"
WIFI_IFACE="${REOKTO_WIFI_IFACE:-wlan0}"
CONN_NAME="reokto-ap"
INSTALL_DIR="/opt/reokto-dashboard"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script needs root. Re-running with sudo..."
  exec sudo -E bash "$0" "$@"
fi

echo "==> Checking that NetworkManager is the active network stack"
if ! systemctl is-active --quiet NetworkManager; then
  echo "ERROR: NetworkManager is not active. This installer targets RPi OS Bookworm."
  echo "       On older releases (Bullseye/Buster) you would need hostapd + dnsmasq instead."
  exit 1
fi

echo "==> Installing apt packages"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  mosquitto \
  mosquitto-clients \
  python3-flask \
  python3-paho-mqtt

echo "==> Configuring Mosquitto"
install -m 0644 "${SCRIPT_DIR}/mosquitto/reokto.conf" /etc/mosquitto/conf.d/reokto.conf
systemctl enable --now mosquitto
systemctl restart mosquitto

echo "==> Installing dashboard to ${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}"
cp -r "${SCRIPT_DIR}/dashboard/." "${INSTALL_DIR}/"
chown -R root:root "${INSTALL_DIR}"

echo "==> Installing systemd unit"
install -m 0644 "${SCRIPT_DIR}/systemd/reokto-dashboard.service" /etc/systemd/system/reokto-dashboard.service
systemctl daemon-reload
systemctl enable --now reokto-dashboard.service

echo "==> Configuring WiFi access point (${CONN_NAME} on ${WIFI_IFACE})"
if nmcli -t -f NAME con show | grep -Fxq "${CONN_NAME}"; then
  echo "    Connection ${CONN_NAME} already exists; updating."
  nmcli con modify "${CONN_NAME}" \
    802-11-wireless.ssid "${SSID}" \
    802-11-wireless.mode ap \
    802-11-wireless.band bg \
    802-11-wireless.channel 6 \
    ipv4.method shared \
    ipv6.method ignore \
    wifi-sec.key-mgmt wpa-psk \
    wifi-sec.psk "${PSK}" \
    connection.autoconnect yes
else
  nmcli con add type wifi ifname "${WIFI_IFACE}" con-name "${CONN_NAME}" \
    autoconnect yes ssid "${SSID}"
  nmcli con modify "${CONN_NAME}" \
    802-11-wireless.mode ap \
    802-11-wireless.band bg \
    802-11-wireless.channel 6 \
    ipv4.method shared \
    ipv6.method ignore \
    wifi-sec.key-mgmt wpa-psk \
    wifi-sec.psk "${PSK}"
fi
# Channel 6 (2.437 GHz) is allowed in every regulatory domain. Without
# pinning, NetworkManager often picks 12 or 13 which ESP32s on the
# default US regdomain refuse to associate to (silent failure).

# Bring the AP up. `ipv4.method shared` makes NetworkManager spawn an
# internal dnsmasq, so DHCP for clients (10.42.0.0/24) is automatic.
nmcli con up "${CONN_NAME}"

echo
echo "================================================================"
echo " reokto-demo Pi setup complete."
echo
echo "   SSID:         ${SSID}"
echo "   Password:     ${PSK}"
echo "   Pi address:   10.42.0.1"
echo "   Dashboard:    http://10.42.0.1/"
echo "   MQTT broker:  10.42.0.1:1883 (anonymous)"
echo
echo " Flash the ESP32 with the matching SSID/PSK in platformio.ini."
echo "================================================================"
