#!/usr/bin/env python3
import sys
import time

from qmp_input_test import command, connect_qmp, read_message


def main() -> int:
    if len(sys.argv) != 4:
        print("Usage: qmp_screenshot.py <qmp-socket> <output.ppm> <delay>",
              file=sys.stderr)
        return 2
    time.sleep(float(sys.argv[3]))
    connection = connect_qmp(sys.argv[1])
    greeting = read_message(connection)
    if "QMP" not in greeting:
        return 1
    if "return" not in command(connection, "qmp_capabilities"):
        return 1
    response = command(connection, "screendump", {"filename": sys.argv[2]})
    connection.close()
    return 0 if "return" in response else 1


if __name__ == "__main__":
    raise SystemExit(main())
