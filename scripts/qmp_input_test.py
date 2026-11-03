#!/usr/bin/env python3
import json
import socket
import sys
import time
from pathlib import Path


def connect_qmp(path: str) -> socket.socket:
    last_error = None
    for _ in range(100):
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(path)
            return connection
        except OSError as error:
            last_error = error
            time.sleep(0.1)
    raise RuntimeError(f"QMP connection failed: {last_error}")


def read_message(connection: socket.socket) -> dict:
    data = b""
    while not data.endswith(b"\n"):
        chunk = connection.recv(4096)
        if not chunk:
            raise RuntimeError("QMP connection closed")
        data += chunk
    return json.loads(data.decode("utf-8"))


def command(connection: socket.socket, name: str, arguments: dict | None = None) -> dict:
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    connection.sendall(json.dumps(request).encode("utf-8") + b"\n")
    while True:
        response = read_message(connection)
        if "return" in response or "error" in response:
            return response


def send_events(connection: socket.socket, events: list[dict], delay: float = 0.3) -> bool:
    for event in events:
        response = command(connection, "input-send-event", {"events": [event]})
        if "return" not in response:
            print(f"QMP input injection failed: {response}", file=sys.stderr)
            return False
        time.sleep(delay)
    return True


def validate_screendump(path: Path) -> bool:
    data = path.read_bytes()
    tokens = []
    index = 0
    while len(tokens) < 4:
        while index < len(data) and chr(data[index]).isspace():
            index += 1
        if index < len(data) and data[index] == ord("#"):
            while index < len(data) and data[index] not in (10, 13):
                index += 1
            continue
        start = index
        while index < len(data) and not chr(data[index]).isspace():
            index += 1
        if start == index:
            return False
        tokens.append(data[start:index])
    if index >= len(data) or not chr(data[index]).isspace():
        return False
    first_separator = data[index]
    index += 1
    if first_separator == 13 and index < len(data) and data[index] == 10:
        index += 1

    magic = tokens[0]
    width = int(tokens[1])
    height = int(tokens[2])
    maximum = int(tokens[3])
    channels = 3 if magic == b"P6" else 1 if magic == b"P5" else 0
    if channels == 0 or width <= 0 or height <= 0 or maximum <= 0:
        return False

    expected = width * height * channels
    if len(data) - index < expected:
        return False

    colors = set()
    dark_pixels = 0
    stride = max(1, width * height // 20000)
    for pixel in range(0, width * height, stride):
        offset = index + pixel * channels
        if channels == 3:
            color = tuple(data[offset:offset + 3])
        else:
            color = (data[offset],) * 3
        colors.add(color)
        if sum(color) < 192:
            dark_pixels += 1
    return len(colors) >= 8 and dark_pixels >= 8


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("Usage: qmp_input_test.py <qmp-socket> [mode]", file=sys.stderr)
        return 2
    mode = sys.argv[2] if len(sys.argv) == 3 else ""

    connection = connect_qmp(sys.argv[1])
    greeting = read_message(connection)
    if "QMP" not in greeting:
        raise RuntimeError("Unexpected QMP greeting")
    command(connection, "qmp_capabilities")

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

    if mode == "tablet":
        pointer_events = [
            {"type": "abs", "data": {"axis": "x", "value": 24000}},
            {"type": "abs", "data": {"axis": "y", "value": 16000}},
        ]
    else:
        pointer_events = [
            {"type": "rel", "data": {"axis": "x", "value": 24}},
            {"type": "rel", "data": {"axis": "y", "value": -12}},
            {"type": "rel", "data": {"axis": "x", "value": 16}},
            {"type": "rel", "data": {"axis": "y", "value": -8}},
        ]
    if not send_events(
        connection,
        pointer_events,
        0.35,
    ):
        return 1
    if not send_events(
        connection,
        [
            {"type": "btn", "data": {"down": True, "button": "left"}},
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ],
        0.9,
    ):
        return 1

    if mode == "hub":
        add_result = command(
            connection,
            "device_add",
            {"driver": "usb-kbd", "id": "hotplug-kbd", "bus": "xhci.0"},
        )
        if "return" not in add_result:
            print(f"USB hotplug add failed: {add_result}", file=sys.stderr)
            return 1
        time.sleep(2)
        if not send_events(
            connection,
            [
                {"type": "key", "data": {
                    "down": True,
                    "key": {"type": "qcode", "data": "a"}}},
                {"type": "key", "data": {
                    "down": False,
                    "key": {"type": "qcode", "data": "a"}}},
            ],
            0.4,
        ):
            return 1
        time.sleep(1)
        del_result = command(
            connection,
            "device_del",
            {"id": "hotplug-kbd"},
        )
        if "return" not in del_result:
            print(f"USB hotplug delete failed: {del_result}", file=sys.stderr)
            return 1
        time.sleep(2)

    screenshot = Path(f"qemu-{mode or 'serial'}-screen.ppm").resolve()
    screen = command(connection, "screendump", {"filename": str(screenshot)})
    if "return" not in screen:
        print(f"QEMU screendump failed: {screen}", file=sys.stderr)
        return 1
    if not screenshot.is_file() or not validate_screendump(screenshot):
        print(f"QEMU framebuffer did not render the welcome UI: {screenshot}",
              file=sys.stderr)
        return 1

    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
