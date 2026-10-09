"""Enter the ESP WiFi key locally. No ESP-IDF activation or extra packages needed."""
import getpass
import json
import os
from pathlib import Path
import re


def main():
    password = getpass.getpass("Clave de WIFI_COMPARTIDO (no se mostrará): ")
    if not 8 <= len(password.encode("utf-8")) <= 63:
        raise SystemExit("La clave PSK debe ocupar entre 8 y 63 bytes; no se guardó nada")
    config = Path(__file__).resolve().parent / "sdkconfig"
    text = config.read_text()
    values = {
        "CONFIG_TIC3_WIFI_ENABLE": "y", "CONFIG_TIC3_NETWORK_SEND": "y",
        "CONFIG_TIC3_WIFI_3_ENABLE": "y", "CONFIG_TIC3_WIFI_3_PASSWORD": json.dumps(password),
        "CONFIG_TIC3_WIFI_3_SERVER": '"192.0.2.10"',
        "CONFIG_TIC3_WIFI_3_PORT": "5001", "CONFIG_TIC3_TRANSPORT_TCP": "y",
    }
    for key, value in values.items():
        pattern = rf"^(?:{re.escape(key)}=.*|# {re.escape(key)} is not set)$"
        replacement = key + "=" + value
        if re.search(pattern, text, flags=re.M):
            text = re.sub(pattern, lambda match, line=replacement: line, text, flags=re.M)
        else:
            text += "\n" + replacement + "\n"
    config.write_text(text)
    os.chmod(config, 0o600)
    print("Clave guardada localmente en sdkconfig. Ahora puedo recompilar y grabar la ESP.")


if __name__ == "__main__":
    main()
