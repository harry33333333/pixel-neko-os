#!/usr/bin/env python3
import sys
import time
from pathlib import Path

from qmp_input_test import (
    command,
    connect_qmp,
    read_message,
    send_events,
    validate_screendump,
)


SCREEN_WIDTH = 1024
SCREEN_HEIGHT = 768


def click(connection, x: int, y: int) -> None:
    for event in (
        {"type": "abs", "data": {
            "axis": "x", "value": int(x * 32767 / (SCREEN_WIDTH - 1))}},
        {"type": "abs", "data": {
            "axis": "y", "value": int(y * 32767 / (SCREEN_HEIGHT - 1))}},
    ):
        response = command(connection, "input-send-event", {"events": [event]})
        if "return" not in response:
            raise RuntimeError(f"QMP pointer move failed: {response}")
        time.sleep(0.2)
    for down in (True, False):
        response = command(
            connection,
            "input-send-event",
            {"events": [{"type": "btn", "data": {
                "down": down, "button": "left"}}]},
        )
        if "return" not in response:
            raise RuntimeError(f"QMP click failed: {response}")
        time.sleep(0.45)


def screenshot(connection, path: Path) -> None:
    response = command(connection, "screendump", {"filename": str(path.resolve())})
    if "return" not in response or not path.is_file() or not validate_screendump(path):
        raise RuntimeError(f"Installer screenshot validation failed: {path}")


def wait_for(path: Path, needle: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: qmp_p6_install.py <qmp-socket> <serial-log>", file=sys.stderr)
        return 2
    socket_path = sys.argv[1]
    serial_log = Path(sys.argv[2])

    connection = connect_qmp(socket_path)
    greeting = read_message(connection)
    if "QMP" not in greeting:
        print("Unexpected QMP greeting", file=sys.stderr)
        return 1
    if "return" not in command(connection, "qmp_capabilities"):
        print("Could not enable QMP capabilities", file=sys.stderr)
        return 1

    if not send_events(
        connection,
        [
            {"type": "key", "data": {
                "down": True, "key": {"type": "qcode", "data": "p"}}},
            {"type": "key", "data": {
                "down": False, "key": {"type": "qcode", "data": "p"}}},
        ],
        0.4,
    ):
        return 1

    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-select.ppm"))

    next_x = 16 + (SCREEN_WIDTH - 32) - 92 + 39
    next_y = 30 + (SCREEN_HEIGHT - 50) - 32 + 11
    click(connection, next_x, next_y)  # Select partition -> filesystem.
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-filesystem.ppm"))

    click(connection, next_x, next_y)  # FAT32 -> confirmation.
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-confirm.ppm"))

    click(connection, next_x + 5, next_y)  # Install Now.
    if not wait_for(serial_log, "[P6 ] Installation complete on", 22.0):
        print("Installer did not report a completed offline deployment", file=sys.stderr)
        return 1
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-screen.ppm"))
    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
