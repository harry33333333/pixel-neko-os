#!/usr/bin/env python3
import sys
import time

from qmp_input_test import command, connect_qmp, read_message, send_events


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: qmp_boot_menu.py <qmp-socket>", file=sys.stderr)
        return 2

    connection = connect_qmp(sys.argv[1])
    greeting = read_message(connection)
    if "QMP" not in greeting:
        print("Unexpected QMP greeting", file=sys.stderr)
        return 1
    if "return" not in command(connection, "qmp_capabilities"):
        print("Could not enable QMP capabilities", file=sys.stderr)
        return 1

    # Limine waits five seconds. Wait until OVMF has reached the boot menu,
    # then explicitly reset the selection before choosing Graphical Installer.
    time.sleep(3.0)
    if not send_events(
        connection,
        [
            {"type": "key", "data": {
                "down": True, "key": {"type": "qcode", "data": "home"}}},
            {"type": "key", "data": {
                "down": False, "key": {"type": "qcode", "data": "home"}}},
            {"type": "key", "data": {
                "down": True, "key": {"type": "qcode", "data": "down"}}},
            {"type": "key", "data": {
                "down": False, "key": {"type": "qcode", "data": "down"}}},
            {"type": "key", "data": {
                "down": True, "key": {"type": "qcode", "data": "ret"}}},
            {"type": "key", "data": {
                "down": False, "key": {"type": "qcode", "data": "ret"}}},
        ],
        0.35,
    ):
        return 1
    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
