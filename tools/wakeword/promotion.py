#!/usr/bin/env python3
"""Shadow -> canary -> fleet promotion decisions for a wake-word candidate.

Inputs are metrics only: the candidate's eval manifest, the baseline manifest,
and the add-on's metadata-only weekly reports (``python -m app.wake_events
report``), which count wakes, false-wake flags and shadow detections per
device. Output is a decision record. Nothing here flashes a device:
promotion is always a deliberate, reviewed release.

Stages and criteria (the margins are the live gate's FA/h margin, 0.05/h):

1. offline   — the exact packaged operating point passes the gate AND
               improves the baseline (eligible-but-not-better never ships).
2. shadow    — the candidate runs log-only next to the incumbent on one
               device for >= 7 days. Its extra detections per hour over the
               incumbent's wakes must stay within the margin, so it cannot
               be firing on noise the incumbent ignores.
3. canary    — the candidate is the live model on one device for >= 7 days.
               Its false-wake flags per hour must stay within the margin of
               the incumbent's 7-day baseline on that device.
4. fleet     — every device, same rollback trigger.

Automatic rollback trigger (canary and fleet): flags per hour above the
incumbent baseline + margin. Restoring models/previous/ is the rollback.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate import FAPH_TOLERANCE, OperatingPoint, evaluate_gate  # noqa: E402

MIN_DAYS = 7


@dataclass
class Decision:
    stage: str
    action: str
    reasons: List[str] = field(default_factory=list)
    metrics: dict = field(default_factory=dict)

    def to_dict(self) -> dict:
        return {"stage": self.stage, "action": self.action, "reasons": self.reasons, "metrics": self.metrics}


def _point(manifest: dict) -> OperatingPoint:
    op = manifest["operating_point"]
    return OperatingPoint(float(op["cutoff"]), int(op["window"]), float(op["recall"]),
                          float(op["false_accepts_per_hour"]), float(op["ambient_hours"]))


def offline_decision(candidate: dict, baseline: dict) -> Decision:
    result = evaluate_gate(_point(candidate), _point(baseline))
    if not result.passed:
        return Decision("offline", "reject", list(result.reasons), result.to_dict())
    if not result.improves:
        return Decision("offline", "hold", ["eligible but not an improvement; keep the incumbent"],
                        result.to_dict())
    return Decision("offline", "start_shadow", [], result.to_dict())


def _device(report: dict, device_id: str) -> dict:
    return (report.get("devices") or {}).get(device_id) or {}


def shadow_decision(report: dict, device_id: str, candidate_model: str) -> Decision:
    days = float(report.get("period_days") or 0)
    stats = _device(report, device_id)
    hours = days * 24.0
    shadow = int((stats.get("shadow_detections") or {}).get(candidate_model, 0))
    wakes = int(stats.get("wakes", 0))
    extra_per_hour = (shadow - wakes) / hours if hours else float("inf")
    metrics = {"days": days, "shadow_detections": shadow, "incumbent_wakes": wakes,
               "extra_detections_per_hour": round(extra_per_hour, 4), "margin_per_hour": FAPH_TOLERANCE}
    if days < MIN_DAYS:
        return Decision("shadow", "continue", [f"only {days:g} of {MIN_DAYS} days observed"], metrics)
    if extra_per_hour > FAPH_TOLERANCE:
        return Decision("shadow", "reject",
                        ["candidate fires more often than the incumbent beyond the FA/h margin"], metrics)
    return Decision("shadow", "start_canary", [], metrics)


def canary_decision(report: dict, baseline_report: dict, device_id: str, stage: str = "canary") -> Decision:
    days = float(report.get("period_days") or 0)
    base_days = float(baseline_report.get("period_days") or 0)
    flags = int(_device(report, device_id).get("false_wake_flags", 0))
    base_flags = int(_device(baseline_report, device_id).get("false_wake_flags", 0))
    rate = flags / (days * 24.0) if days else float("inf")
    base_rate = base_flags / (base_days * 24.0) if base_days else 0.0
    metrics = {"days": days, "flags": flags, "flags_per_hour": round(rate, 4),
               "baseline_flags_per_hour": round(base_rate, 4), "margin_per_hour": FAPH_TOLERANCE}
    if rate > base_rate + FAPH_TOLERANCE:
        return Decision(stage, "rollback",
                        ["false-wake flags exceed the incumbent baseline by more than the margin; "
                         "restore models/previous/"], metrics)
    if days < MIN_DAYS:
        return Decision(stage, "continue", [f"only {days:g} of {MIN_DAYS} days observed"], metrics)
    return Decision(stage, "promote_fleet" if stage == "canary" else "keep", [], metrics)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_off = sub.add_parser("offline")
    p_off.add_argument("--candidate", type=Path, required=True)
    p_off.add_argument("--baseline", type=Path, required=True)
    p_sh = sub.add_parser("shadow")
    p_sh.add_argument("--report", type=Path, required=True)
    p_sh.add_argument("--device", required=True)
    p_sh.add_argument("--candidate-model", required=True)
    for name in ("canary", "fleet"):
        p = sub.add_parser(name)
        p.add_argument("--report", type=Path, required=True)
        p.add_argument("--baseline-report", type=Path, required=True)
        p.add_argument("--device", required=True)
    args = parser.parse_args(argv)

    def load(path):
        return json.loads(Path(path).read_text())

    if args.cmd == "offline":
        decision = offline_decision(load(args.candidate), load(args.baseline))
    elif args.cmd == "shadow":
        decision = shadow_decision(load(args.report), args.device, args.candidate_model)
    else:
        decision = canary_decision(load(args.report), load(args.baseline_report), args.device, stage=args.cmd)
    print(json.dumps(decision.to_dict(), indent=2))
    return 0 if decision.action in ("start_shadow", "start_canary", "promote_fleet", "keep", "continue") else 1


if __name__ == "__main__":
    sys.exit(main())
