"""Use paced writes for the board's CH343 USB serial adapter on macOS."""

from pathlib import Path
import subprocess
import sys

Import("env")

if sys.platform == "darwin":
    venv_dir = Path(env.subst("$PROJECT_BUILD_DIR")) / "upload-venv"
    python = venv_dir / "bin" / "python"

    def prepare_upload(source, target, env):
        if not python.exists():
            subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)
        probe = subprocess.run(
            [str(python), "-c", "import esptool; assert esptool.__version__ == '5.4.0'"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if probe.returncode:
            subprocess.run([str(python), "-m", "pip", "install", "esptool==5.4.0"], check=True)

    env.AddPreAction("upload", prepare_upload)
    env.Replace(
        UPLOADCMD=(
            f'"{python}" "$PROJECT_DIR/scripts/paced_esptool.py" '
            "$UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE"
        )
    )
