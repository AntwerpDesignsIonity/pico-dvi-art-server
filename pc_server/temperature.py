"""Server-side temperature source.

Provides the "server" temperature shown on the HUD. The second HUD reading -
the RP2040 on-chip sensor - is reported by the Pico itself over the stream
socket and handled in `server.py`.

Sources:
  weather : Open-Meteo current temperature (no API key required)
  cpu     : host CPU package sensor via psutil (optional dependency)
  static  : a fixed number from the config
  none    : disabled
"""

from __future__ import annotations

import json
import threading
import time
import urllib.parse
import urllib.request

OPEN_METEO = "https://api.open-meteo.com/v1/forecast"


class TemperatureProvider:
    """Background-refreshed temperature reading in degrees Celsius."""

    def __init__(
        self,
        source: str = "weather",
        latitude: float = 51.2194,
        longitude: float = 4.4025,
        static_c: float = 21.0,
        refresh_s: float = 600.0,
        timeout_s: float = 8.0,
    ) -> None:
        self.source = source
        self.latitude = latitude
        self.longitude = longitude
        self.static_c = static_c
        self.refresh_s = max(30.0, float(refresh_s))
        self.timeout_s = timeout_s

        self._lock = threading.Lock()
        self._value: float | None = static_c if source == "static" else None
        self._updated: float = 0.0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    # -- lifecycle -------------------------------------------------------
    def start(self) -> "TemperatureProvider":
        if self.source in ("none", "static"):
            return self
        self._thread = threading.Thread(
            target=self._loop, name="temperature", daemon=True
        )
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                value = self._read()
            except Exception as exc:  # network hiccups must never kill the stream
                print(f"[temp] read failed: {exc}")
                value = None
            if value is not None:
                with self._lock:
                    self._value = value
                    self._updated = time.time()
            self._stop.wait(self.refresh_s)

    # -- readings --------------------------------------------------------
    def _read(self) -> float | None:
        if self.source == "weather":
            return self._read_weather()
        if self.source == "cpu":
            return self._read_cpu()
        if self.source == "static":
            return self.static_c
        return None

    def _read_weather(self) -> float | None:
        query = urllib.parse.urlencode(
            {
                "latitude": self.latitude,
                "longitude": self.longitude,
                "current": "temperature_2m",
                "timezone": "auto",
            }
        )
        request = urllib.request.Request(
            f"{OPEN_METEO}?{query}", headers={"User-Agent": "pico-dvi-art-server/1.0"}
        )
        with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
            payload = json.loads(response.read().decode("utf-8"))
        return float(payload["current"]["temperature_2m"])

    @staticmethod
    def _read_cpu() -> float | None:
        try:
            import psutil  # optional dependency
        except ImportError:
            print("[temp] psutil not installed - CPU temperature unavailable")
            return None
        sensors = psutil.sensors_temperatures() if hasattr(psutil, "sensors_temperatures") else {}
        for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz"):
            if sensors.get(key):
                return float(sensors[key][0].current)
        for entries in sensors.values():
            if entries:
                return float(entries[0].current)
        return None

    # -- accessors -------------------------------------------------------
    @property
    def value(self) -> float | None:
        with self._lock:
            return self._value

    @property
    def age_s(self) -> float | None:
        with self._lock:
            if not self._updated:
                return None
            return time.time() - self._updated
