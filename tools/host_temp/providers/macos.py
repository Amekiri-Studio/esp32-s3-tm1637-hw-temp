from __future__ import annotations

from .base import SensorCandidate


class MacOSTemperatureProvider:
    platform_name = "macos"

    def list_candidates(self) -> list[SensorCandidate]:
        raise NotImplementedError(
            "The macOS provider is only a skeleton right now. Wire it to an SMC-based backend or another sensor source."
        )

    def choose_sensor(self, explicit_identifier: str | None) -> SensorCandidate:
        raise NotImplementedError(
            "The macOS provider is only a skeleton right now. Wire it to an SMC-based backend or another sensor source."
        )

    def read_celsius(self, sensor: SensorCandidate) -> float:
        raise NotImplementedError(
            "The macOS provider is only a skeleton right now. Wire it to an SMC-based backend or another sensor source."
        )

    def close(self) -> None:
        return None
