#!/usr/bin/env python3
import sys
import time
from pathlib import Path

from qmp_input_test import command, connect_qmp, read_message


def wait_for_serial(path: Path, needle: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.1)
    return False


def change_medium(connection, device: str, filename: str) -> bool:
    response = command(
        connection,
        "blockdev-change-medium",
        {
            "device": device,
            "filename": filename,
            "format": "raw",
            "force": True,
        },
    )
    if "return" not in response:
        print(f"QMP medium change failed: {response}", file=sys.stderr)
        return False
    return True


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "Usage: qmp_floppy_test.py <qmp-socket> <serial-log> "
            "<replacement-image>",
            file=sys.stderr,
        )
        return 2

    connection = connect_qmp(sys.argv[1])
    greeting = read_message(connection)
    if "QMP" not in greeting:
        print("Unexpected QMP greeting", file=sys.stderr)
        return 1
    if "return" not in command(connection, "qmp_capabilities"):
        print("Could not enable QMP capabilities", file=sys.stderr)
        return 1

    serial_log = Path(sys.argv[2])
    replacement = str(Path(sys.argv[3]).resolve())
    if not wait_for_serial(serial_log, "[P6 ] Full desktop", 35.0):
        print("Desktop did not start before floppy media test", file=sys.stderr)
        return 1

    if not change_medium(connection, "fda", replacement):
        return 1
    if not wait_for_serial(
        serial_log, "[VFS ] A: floppy changed (720 KiB)", 12.0
    ):
        print("Floppy capacity change was not detected", file=sys.stderr)
        return 1

    response = command(
        connection, "eject", {"device": "fda", "force": True}
    )
    if "return" not in response:
        print(f"QMP floppy eject failed: {response}", file=sys.stderr)
        return 1
    if not wait_for_serial(
        serial_log, "[VFS ] A: floppy media removed", 12.0
    ):
        print("Floppy removal was not detected", file=sys.stderr)
        return 1

    if not change_medium(connection, "fda", replacement):
        return 1
    if not wait_for_serial(
        serial_log, "[VFS ] A: floppy inserted (720 KiB)", 12.0
    ):
        print("Floppy reinsertion was not detected", file=sys.stderr)
        return 1

    connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
