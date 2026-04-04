from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
from pathlib import Path

from .base import SensorCandidate


MACMON_ENV_VAR = "MACMON_BIN"
MACMON_SAMPLE_INTERVAL_MS = 200

SENSOR_DEFINITIONS: dict[str, tuple[str, int]] = {
    "cpu_temp_avg": ("CPU Average", 200),
}


class MacOSTemperatureProvider:
    platform_name = "macos"

    def __init__(self) -> None:
        self._macmon_path = self._resolve_macmon_path()

    def _candidate_macmon_paths(self) -> list[Path]:
        paths: list[Path] = []

        env_path = os.environ.get(MACMON_ENV_VAR)
        if env_path:
            paths.append(Path(env_path).expanduser())

        which_path = shutil.which("macmon")
        if which_path:
            paths.append(Path(which_path))

        project_root = Path(__file__).resolve().parents[3]
        paths.extend([project_root / "macmon", project_root / "tools" / "macmon"])

        unique_paths: list[Path] = []
        seen: set[Path] = set()
        for path in paths:
            if path in seen:
                continue
            seen.add(path)
            unique_paths.append(path)

        return unique_paths

    def _resolve_macmon_path(self) -> Path:
        for candidate in self._candidate_macmon_paths():
            if candidate.exists():
                return candidate

        raise RuntimeError(
            "The macOS Apple Silicon provider requires the `macmon` CLI. Install it with `brew install macmon`, "
            f"or point {MACMON_ENV_VAR} to the binary."
        )

    def _ensure_macos(self) -> None:
        if platform.system() != "Darwin":
            raise RuntimeError("The macOS provider can only run on macOS.")

    def _read_snapshot(self) -> dict[str, object]:
        self._ensure_macos()

        command = [
            str(self._macmon_path),
            "pipe",
            "--samples",
            "1",
            "--interval",
            str(MACMON_SAMPLE_INTERVAL_MS),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip() or f"exit code {result.returncode}"
            raise RuntimeError(f"`macmon` failed to read Apple Silicon metrics: {detail}")

        lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if not lines:
            raise RuntimeError("`macmon` returned no JSON output.")

        # `macmon pipe` prints one JSON object per line, so use the last sample we received.
        try:
            payload = json.loads(lines[-1])
        except json.JSONDecodeError as exc:
            raise RuntimeError("`macmon` returned invalid JSON output.") from exc

        if not isinstance(payload, dict):
            raise RuntimeError("`macmon` returned an unexpected JSON payload.")

        return payload

    def _build_candidates(self) -> list[SensorCandidate]:
        payload = self._read_snapshot()
        temps = payload.get("temp")
        if not isinstance(temps, dict):
            raise RuntimeError("`macmon` did not report any temperature metrics.")

        candidates: list[SensorCandidate] = []
        for identifier, (label, priority) in SENSOR_DEFINITIONS.items():
            value = temps.get(identifier)
            if isinstance(value, (int, float)):
                candidates.append(SensorCandidate(priority, "macmon", identifier, label))

        if not candidates:
            raise RuntimeError("`macmon` did not report a CPU temperature for Apple Silicon.")

        return sorted(candidates, reverse=True)

    def list_candidates(self) -> list[SensorCandidate]:
        return self._build_candidates()

    def choose_sensor(self, explicit_identifier: str | None) -> SensorCandidate:
        candidates = self.list_candidates()
        if explicit_identifier is None:
            return candidates[0]

        lowered = explicit_identifier.lower()
        for candidate in candidates:
            if candidate.identifier == explicit_identifier:
                return candidate
            if candidate.label.lower() == lowered:
                return candidate
            if lowered in candidate.identifier.lower():
                return candidate

        raise RuntimeError(f"macmon sensor not found: {explicit_identifier}")

    def read_celsius(self, sensor: SensorCandidate) -> float:
        payload = self._read_snapshot()
        temps = payload.get("temp")
        if not isinstance(temps, dict):
            raise RuntimeError("`macmon` did not report any temperature metrics.")

        value = temps.get(sensor.identifier)
        if not isinstance(value, (int, float)):
            raise RuntimeError(f"`macmon` did not report a current value for sensor: {sensor.identifier}")

        return float(value)

    def close(self) -> None:
        return None
