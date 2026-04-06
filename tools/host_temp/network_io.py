from __future__ import annotations

import socket
from typing import Protocol


class TemperatureSender(Protocol):
    def send_temperature(self, text: str) -> None: ...

    def close(self) -> None: ...

    def __enter__(self) -> "TemperatureSender": ...

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...


class UdpTemperatureSender:
    def __init__(self, host: str, port: int) -> None:
        self._target = (host, port)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_temperature(self, text: str) -> None:
        self._socket.sendto(text.encode("ascii"), self._target)

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "UdpTemperatureSender":
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()


def open_udp_sender(host: str, port: int) -> TemperatureSender:
    return UdpTemperatureSender(host, port)
