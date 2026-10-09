"""Configure TIC3 hotspot with NetworkManager. Run locally on Raspberry with sudo.

Default only prepares profile; --activate switches wlan0 and may disconnect SSH.
Credentials are read from a private JSON file, never printed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def nmcli(*arguments):
    result = subprocess.run(["nmcli", *arguments], text=True, capture_output=True)
    if result.returncode:
        # Never print the command or captured error that could echo its PSK.
        raise RuntimeError("NetworkManager rechazó la configuración; revisar el perfil localmente")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--credentials", default=str(Path.home() / ".ap_credentials.json"))
    parser.add_argument("--activate", action="store_true")
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("Ejecutar con sudo desde una terminal de la Raspberry")
    credentials = json.loads(Path(args.credentials).read_text())
    ssid, password, address = credentials["ssid"], credentials["password"], credentials["address"]
    if not 1 <= len(ssid.encode()) <= 32 or not 8 <= len(password) <= 63 or address != "192.168.50.1/24":
        raise SystemExit("Archivo privado de configuración inválido")
    names = nmcli("-t", "-f", "NAME", "connection", "show").splitlines()
    if "TIC3-Raccoon" in names:
        mode = nmcli("-g", "802-11-wireless.mode", "connection", "show", "TIC3-Raccoon")
        if mode != "ap":
            raise SystemExit("Ya existe otro perfil con ese nombre; no se modifica")
    else:
        nmcli("connection", "add", "type", "wifi", "ifname", "wlan0", "con-name",
              "TIC3-Raccoon", "autoconnect", "no", "ssid", ssid)
    nmcli("connection", "modify", "TIC3-Raccoon", "802-11-wireless.mode", "ap",
          "802-11-wireless.band", "bg", "802-11-wireless.channel", "1",
          "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", password,
          "wifi-sec.proto", "rsn", "wifi-sec.pairwise", "ccmp", "wifi-sec.group", "ccmp",
          "ipv4.method", "shared", "ipv4.addresses", address,
          "ipv6.method", "disabled", "connection.autoconnect", "no")
    print("Perfil TIC3-Raccoon preparado: AP WPA2/AES 2,4 GHz; Raspberry 192.168.50.1; DHCP para clientes.")
    if args.activate:
        print("Activando AP en wlan0: la conexión WiFi/SSH actual puede cortarse.", flush=True)
        nmcli("connection", "up", "TIC3-Raccoon")
        print("AP activo. Conectar PC al SSID y usar SSH a 192.168.50.1.")
    else:
        print("No se cambió la conexión actual. Añadir --activate cuando haya acceso local/Ethernet.")


if __name__ == "__main__":
    main()
