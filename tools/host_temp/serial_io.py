from __future__ import annotations

from dataclasses import dataclass
from glob import glob
import os
from typing import Protocol

try:
    import serial as pyserial
except ImportError:
    pyserial = None

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

if os.name == "posix":
    import termios
else:
    termios = None


AUTO_DETECT_KEYWORDS = (
    "esp32",
    "esp32-s3",
    "usb jtag/serial",
    "cp210",
    "ch340",
    "ch910",
    "ftdi",
    "wch",
    "silicon labs",
    "uart",
    "ttyacm",
    "ttyusb",
    "usbmodem",
    "usbserial",
    "acm",
)

POSIX_PORT_GLOBS = (
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
    "/dev/tty.usbmodem*",
    "/dev/tty.usbserial*",
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
)


class LineWriter(Protocol):
    def write_line(self, text: str) -> None: ...

    def close(self) -> None: ...

    def __enter__(self) -> "LineWriter": ...

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...


@dataclass(frozen=True)
class SerialPortCandidate:
    device: str
    description: str
    manufacturer: str
    hwid: str
    score: int
    detection_method: str


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


def _score_port_text(*parts: str) -> int:
    text = " ".join(part.lower() for part in parts if part)
    score = 0

    for keyword in AUTO_DETECT_KEYWORDS:
        if keyword in text:
            score += 10

    if "bluetooth" in text:
        score -= 25
    if "debug" in text and "usb jtag/serial" not in text:
        score -= 5

    return score


def _list_ports_with_pyserial() -> list[SerialPortCandidate]:
    if list_ports is None:
        return []

    candidates: list[SerialPortCandidate] = []
    for port in list_ports.comports():
        description = getattr(port, "description", "") or ""
        manufacturer = getattr(port, "manufacturer", "") or ""
        product = getattr(port, "product", "") or ""
        interface = getattr(port, "interface", "") or ""
        hwid = getattr(port, "hwid", "") or ""
        device = getattr(port, "device", "") or ""
        score = _score_port_text(device, description, manufacturer, product, interface, hwid)
        candidates.append(
            SerialPortCandidate(
                device=device,
                description=description or product or "Unknown serial device",
                manufacturer=manufacturer,
                hwid=hwid,
                score=score,
                detection_method="pyserial",
            )
        )

    return sorted(candidates, key=lambda candidate: (-candidate.score, candidate.device))


def _list_ports_with_posix_glob() -> list[SerialPortCandidate]:
    devices = sorted({device for pattern in POSIX_PORT_GLOBS for device in glob(pattern)})
    return [
        SerialPortCandidate(
            device=device,
            description="POSIX serial device",
            manufacturer="",
            hwid="",
            score=_score_port_text(device),
            detection_method="glob",
        )
        for device in devices
    ]


def list_serial_ports() -> list[SerialPortCandidate]:
    candidates = _list_ports_with_pyserial()
    if candidates:
        return candidates

    if os.name == "posix":
        return _list_ports_with_posix_glob()

    return []


def auto_detect_serial_port() -> str:
    candidates = list_serial_ports()
    if not candidates:
        if os.name == "nt":
            raise RuntimeError(
                "No serial ports were found. Install `pyserial` for Windows auto-detection or pass `--port COMx`."
            )

        raise RuntimeError("No serial ports were found. Connect the ESP32-S3 or pass `--port` explicitly.")

    top_candidate = candidates[0]
    if top_candidate.score > 0 or len(candidates) == 1:
        return top_candidate.device

    devices = ", ".join(candidate.device for candidate in candidates[:5])
    raise RuntimeError(
        "Found serial ports but could not confidently choose the ESP32-S3. "
        f"Use `--port` explicitly or inspect them with `--list-ports`: {devices}"
    )


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
