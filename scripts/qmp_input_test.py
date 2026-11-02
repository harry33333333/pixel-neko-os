#!/usr/bin/env python3
import json
import socket
import sys
import time


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

    hmp_commands = [
        "sendkey p",
        "mouse_move 24 -12",
        "mouse_button 1",
    ]
    for hmp in hmp_commands:
        response = command(
            connection,
            "human-monitor-command",
            {"command-line": hmp},
        )
        if "return" not in response:
            print(f"HMP injection failed: {hmp}: {response}", file=sys.stderr)

    key_events = [
        {"type": "key", "down": True,
         "key": {"type": "qcode", "data": "p"}},
        {"type": "key", "down": False,
         "key": {"type": "qcode", "data": "p"}},
    ]
    response = command(
        connection,
        "input-send-event",
        {"device": "usb-kbd", "events": key_events},
    )
    if "return" not in response:
        print(f"QMP keyboard injection failed: {response}", file=sys.stderr)

    events = [
        {"type": "rel", "axis": "x", "value": 16},
        {"type": "rel", "axis": "y", "value": -8},
        {"type": "btn", "down": True, "button": "left"},
        {"type": "btn", "down": False, "button": "left"},
    ]
    response = command(
        connection,
        "input-send-event",
        {"device": "usb-mouse", "events": events},
    )
    if "return" not in response:
        print(f"QMP input injection failed: {response}", file=sys.stderr)

    if mode == "hub":
        add_result = command(
            connection,
            "device_add",
            {"driver": "usb-kbd", "id": "hotplug-kbd", "bus": "xhci.0"},
        )
        if "return" not in add_result:
            print(f"USB hotplug add failed: {add_result}", file=sys.stderr)
        time.sleep(2)
        command(
            connection,
            "input-send-event",
            {
                "device": "hotplug-kbd",
                "events": [
                    {"type": "key", "down": True,
                     "key": {"type": "qcode", "data": "a"}},
                    {"type": "key", "down": False,
                     "key": {"type": "qcode", "data": "a"}},
                ],
            },
        )
        time.sleep(1)
        del_result = command(
            connection,
            "device_del",
            {"id": "hotplug-kbd"},
        )
        if "return" not in del_result:
            print(f"USB hotplug delete failed: {del_result}", file=sys.stderr)
        time.sleep(2)

    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
