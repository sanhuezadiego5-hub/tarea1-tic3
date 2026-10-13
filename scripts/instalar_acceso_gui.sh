#!/usr/bin/env bash
# Ejecutar una vez en el escritorio de Raspberry Pi; no requiere sudo.
set -euo pipefail
proyecto=$(cd "$(dirname "$0")/.." && pwd)
python3 -c 'import tkinter' || {
  echo 'Falta Tkinter. Instalar una vez: sudo apt install python3-tk'
  exit 1
}
mkdir -p "$HOME/.local/share/applications"
python3 - "$proyecto" <<'PY'
import sys
from pathlib import Path
root=Path(sys.argv[1]);target=Path.home()/'.local/share/applications/tic3.desktop'
# Desktop Entry escaping (including paths with spaces).
script=str(root/'raspi/launcher.py').replace('\\','\\\\').replace('"','\\"').replace('`','\\`').replace('$','\\$').replace('%','%%')
target.write_text('[Desktop Entry]\nType=Application\nName=TIC3 - Captura BMI270\nComment=Servidor y control gráfico de la ESP\nExec=python3 "'+script+'"\nIcon=applications-science\nTerminal=false\nCategories=Education;Science;\n')
print('Instalado acceso «TIC3 - Captura BMI270» en el menú de aplicaciones.')
PY
