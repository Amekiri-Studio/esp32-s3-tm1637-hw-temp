from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol


@dataclass(order=True)
class SensorCandidate:
    priority: int
    source: str
    identifier: str
    label: str


class TemperatureProvider(Protocol):
    platform_name: str

    def list_candidates(self) -> list[SensorCandidate]: ...

    def choose_sensor(self, explicit_identifier: str | None) -> SensorCandidate: ...

    def read_celsius(self, sensor: SensorCandidate) -> float: ...

    def close(self) -> None: ...
