"""Loads the generated instrumentation.json produced from instrumentation.yaml.

The converter never re-declares event ids or argument names; everything comes
from this single source of truth.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

DEFAULT_SCHEMA = (
    Path(__file__).resolve().parents[2] / "instrumentation" / "generated" / "instrumentation.json"
)


@dataclass
class EventArg:
    name: str
    type: str
    unit: str | None
    enum: str | None
    description: str | None = None

    @property
    def is_reserved(self) -> bool:
        return self.name.startswith("reserved")


@dataclass
class EventDef:
    name: str
    id: int
    track: str
    description: str
    args: list[EventArg]

    def decode(self, info: tuple[int, int, int, int]) -> dict[str, int]:
        out: dict[str, int] = {}
        for arg, raw in zip(self.args, info):
            if arg.is_reserved:
                continue
            value = raw
            if arg.type == "i32" and value >= 0x80000000:
                value -= 0x100000000
            out[arg.name] = value
        return out


class Schema:
    def __init__(self, model: dict) -> None:
        self._model = model
        self.version: int = model["schema_version"]
        self.hash: int = model["schema_hash"]
        self.correlation_key: str = model.get("correlation_key", "frame_id")
        self.trace: dict = model.get("trace", {})
        self.enums: dict = model.get("enums", {})
        self.metrics: dict = model.get("metrics", {})
        self.flows: dict = model.get("flows", {})
        self.slices: dict = model.get("slices", {})

        self.events_by_name: dict[str, EventDef] = {}
        for name, ev in model["events"].items():
            self.events_by_name[name] = EventDef(
                name=name,
                id=ev["id"],
                track=ev.get("track", "Application"),
                description=ev.get("description", ""),
                args=[
                    EventArg(
                        a["name"],
                        a.get("type", "u32"),
                        a.get("unit"),
                        a.get("enum"),
                        a.get("description"),
                    )
                    for a in ev["args"]
                ],
            )
        self.events_by_id: dict[int, EventDef] = {e.id: e for e in self.events_by_name.values()}

    @property
    def default_cpu_hz(self) -> int:
        return int(self.trace.get("default_cpu_hz", 800_000_000))

    def enum_label(self, enum_name: str | None, value: int) -> str | None:
        if not enum_name:
            return None
        return self.enums.get(enum_name, {}).get(str(value))

    @classmethod
    def load(cls, path: Path | None = None) -> "Schema":
        path = Path(path) if path else DEFAULT_SCHEMA
        if not path.exists():
            raise FileNotFoundError(
                f"{path} is missing. Run "
                "Tools/instrumentation/gen_instrumentation.py to generate it."
            )
        with path.open("r", encoding="utf-8") as fh:
            return cls(json.load(fh))
