from __future__ import annotations

import os
from typing import Protocol

try:
    import serial as pyserial
except ImportError:
    pyserial = None

if os.name == "posix":
    import termios
else:
    termios = None


class LineWriter(Protocol):
    def write_line(self, text: str) -> None: ...

    def close(self) -> None: ...

    def __enter__(self) -> "LineWriter": ...

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...


class PySerialWriter:
    def __init__(self, port: str, baud_rate: int) -> None:
        if pyserial is None:
            raise RuntimeError("pyserial is not available")

        self._serial = pyserial.Serial(port=port, baudrate=baud_rate, timeout=0)

    def write_line(self, text: str) -> None:
        self._serial.write(f"{text}\n".encode("ascii"))

    def close(self) -> None:
        self._serial.close()

    def __enter__(self) -> "PySerialWriter":
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()


class PosixSerialWriter:
    BAUD_RATES = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
    }

    def __init__(self, port: str, baud_rate: int) -> None:
        if termios is None:
            raise RuntimeError("POSIX serial support is unavailable on this platform")

        if baud_rate not in self.BAUD_RATES:
            supported = ", ".join(str(rate) for rate in sorted(self.BAUD_RATES))
            raise ValueError(f"Unsupported baud rate: {baud_rate}. Supported values: {supported}")

        self._fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_SYNC)
        self._configure(baud_rate)

    def _configure(self, baud_rate: int) -> None:
        attrs = termios.tcgetattr(self._fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = self.BAUD_RATES[baud_rate]
        attrs[5] = self.BAUD_RATES[baud_rate]
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcflush(self._fd, termios.TCIOFLUSH)
        termios.tcsetattr(self._fd, termios.TCSANOW, attrs)

    def write_line(self, text: str) -> None:
        os.write(self._fd, f"{text}\n".encode("ascii"))

    def close(self) -> None:
        os.close(self._fd)

    def __enter__(self) -> "PosixSerialWriter":
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()


def open_serial_writer(port: str, baud_rate: int) -> LineWriter:
    if pyserial is not None:
        return PySerialWriter(port, baud_rate)

    if os.name == "posix":
        return PosixSerialWriter(port, baud_rate)

    raise RuntimeError(
        "No cross-platform serial backend is available. Install pyserial with `python3 -m pip install pyserial`."
    )
