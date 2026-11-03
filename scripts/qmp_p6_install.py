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


def move_relative(connection, dx: int, dy: int) -> None:
    while dx or dy:
        step_x = max(-80, min(80, dx))
        step_y = max(-80, min(80, dy))
        events = [
            {"type": "rel", "data": {"axis": "x", "value": step_x}},
            {"type": "rel", "data": {"axis": "y", "value": step_y}},
        ]
        response = command(connection, "input-send-event", {"events": events})
        if "return" not in response:
            raise RuntimeError(f"QMP pointer move failed: {response}")
        dx -= step_x
        dy -= step_y
        time.sleep(0.12)


def click(connection) -> None:
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


def screenshot(connection, path: Path, allow_sparse: bool = False) -> None:
    response = command(connection, "screendump", {"filename": str(path.resolve())})
    if "return" not in response:
        raise RuntimeError(f"Installer screenshot validation failed: {path}")
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if path.is_file() and validate_screendump(path):
            return
        if allow_sparse and path.is_file():
            data = path.read_bytes()
            if data.startswith(b"P6\n"):
                header_end = 0
                lines = 0
                while lines < 3 and header_end < len(data):
                    header_end = data.find(b"\n", header_end) + 1
                    if header_end <= 0:
                        break
                    lines += 1
                if lines == 3 and len(data) - header_end >= 1024 * 768 * 3:
                    return
        time.sleep(0.1)
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
    # The cursor starts at screen center. Move to the bottom-right Next button
    # once, then reuse the same position for each wizard step.
    move_relative(
        connection,
        next_x - SCREEN_WIDTH // 2,
        -(next_y - SCREEN_HEIGHT // 2),
    )
    click(connection)  # Select partition -> filesystem.
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-filesystem.ppm"))

    click(connection)  # FAT32 -> confirmation.
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-confirm.ppm"))

    click(connection)  # Install Now.
    if not wait_for(serial_log, "[P6 ] Installation complete on", 45.0):
        print("Installer did not report a completed offline deployment", file=sys.stderr)
        return 1
    time.sleep(0.8)
    screenshot(connection, Path("qemu-p6-install-screen.ppm"), allow_sparse=True)
    try:
        connection.sendall(b'{"execute":"quit"}\n')
    except OSError:
        pass
    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
