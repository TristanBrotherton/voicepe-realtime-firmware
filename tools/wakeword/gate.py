#!/usr/bin/env python3
"""Wake-word quality gate, operating-point selection and tier derivation.

Metrics only: inputs are calibration grids (recall and false accepts per hour
for every probability cutoff and sliding window) and evaluation manifests. No
audio is read or written.

The gate is fixed policy, not configuration. A candidate operating point
passes only if, against the deployed baseline,

    recall >= baseline_recall - 0.01   and
    FA/h   <= baseline_FA/h  + 0.05

evaluated at the exact cutoff AND window that would be packaged. There is no
flag to widen the margins.

Training, calibration (picking the operating point), sensitivity tiers and
deployment are separate decisions; this module only answers the calibration
and gate questions and records evidence for the other two.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

RECALL_TOLERANCE = 0.01
FAPH_TOLERANCE = 0.05
_EPS = 1e-9


@dataclass(frozen=True)
class OperatingPoint:
    cutoff: float
    window: int
    recall: float
    false_accepts_per_hour: float
    ambient_hours: float

    @property
    def cutoff_uint8(self) -> int:
        """micro_wake_word stores cutoffs as round(p * 255)."""
        return int(round(self.cutoff * 255))

    @property
    def false_accept_events(self) -> int:
        return int(round(self.false_accepts_per_hour * self.ambient_hours))

    @classmethod
    def from_dict(cls, d: dict) -> "OperatingPoint":
        return cls(
            cutoff=round(float(d.get("probability_cutoff", d.get("cutoff"))), 4),
            window=int(d.get("sliding_window_size", d.get("window"))),
            recall=float(d["recall"]),
            false_accepts_per_hour=float(d["false_accepts_per_hour"]),
            ambient_hours=float(d.get("ambient_hours", 0.0)),
        )

    def to_dict(self) -> dict:
        return {
            "cutoff": self.cutoff,
            "cutoff_uint8": self.cutoff_uint8,
            "window": self.window,
            "recall": round(self.recall, 6),
            "false_accepts_per_hour": round(self.false_accepts_per_hour, 6),
            "false_accept_events": self.false_accept_events,
            "ambient_hours": round(self.ambient_hours, 6),
        }


@dataclass(frozen=True)
class GateResult:
    passed: bool
    improves: bool
    recall_floor: float
    faph_ceiling: float
    reasons: tuple

    def to_dict(self) -> dict:
        return {
            "recall_tolerance": RECALL_TOLERANCE,
            "faph_tolerance": FAPH_TOLERANCE,
            "recall_floor": round(self.recall_floor, 6),
            "faph_ceiling": round(self.faph_ceiling, 6),
            "result": "pass" if self.passed else "fail",
            "improves_baseline": self.improves,
            "reasons": list(self.reasons),
        }


def evaluate_gate(candidate: OperatingPoint, baseline: OperatingPoint) -> GateResult:
    """Apply the fixed live gate to one exact (cutoff, window) operating point."""
    floor = baseline.recall - RECALL_TOLERANCE
    ceiling = baseline.false_accepts_per_hour + FAPH_TOLERANCE
    reasons = []
    if candidate.recall < floor - _EPS:
        reasons.append(f"recall {candidate.recall:.6f} < floor {floor:.6f}")
    if candidate.false_accepts_per_hour > ceiling + _EPS:
        reasons.append(f"FA/h {candidate.false_accepts_per_hour:.6f} > ceiling {ceiling:.6f}")
    passed = not reasons
    improves = (
        passed
        and candidate.recall >= baseline.recall - _EPS
        and candidate.false_accepts_per_hour <= baseline.false_accepts_per_hour + _EPS
        and (candidate.recall > baseline.recall + _EPS
             or candidate.false_accepts_per_hour < baseline.false_accepts_per_hour - _EPS)
    )
    if passed and not improves:
        reasons.append("passes the gate but does not improve on the baseline")
    return GateResult(passed, improves, floor, ceiling, tuple(reasons))


def load_grid(calibration: dict) -> List[OperatingPoint]:
    points = calibration.get("operating_points") or calibration.get("per_window_best") or []
    return [OperatingPoint.from_dict(p) for p in points]


def select_operating_point(grid: Sequence[OperatingPoint], baseline: OperatingPoint):
    """Evaluate EVERY cutoff/window pair; prefer points that improve the baseline.

    Returns (point, gate_result, evaluated_count) or (None, None, count) when
    no pair passes. Among improving points: highest recall, then lowest FA/h,
    then the higher cutoff (more margin against noise). Non-improving but
    gate-passing points are only returned when nothing improves — callers must
    still treat them as "eligible, not an improvement".
    """
    scored = [(p, evaluate_gate(p, baseline)) for p in grid]
    passing = [(p, g) for p, g in scored if g.passed]
    if not passing:
        return None, None, len(scored)
    improving = [(p, g) for p, g in passing if g.improves]
    pool = improving or passing
    best = min(pool, key=lambda pg: (-pg[0].recall, pg[0].false_accepts_per_hour, -pg[0].cutoff))
    return best[0], best[1], len(scored)


def derive_tiers(grid: Sequence[OperatingPoint], selected: OperatingPoint) -> dict:
    """Reproducible sensitivity tiers on the selected window.

    slight   = the selected (calibrated, gate-validated) point
    moderate = lowest cutoff whose false accepts exceed the selected point's
               by at most ONE event on the ambient corpus
    very     = lowest cutoff whose FA/h is at most 1.5x the selected point's
    The more sensitive tiers are deliberately outside the validated budget;
    the manifest says so.
    """
    same_window = sorted((p for p in grid if p.window == selected.window), key=lambda p: p.cutoff)

    def lowest(predicate) -> OperatingPoint:
        eligible = [p for p in same_window if predicate(p) and p.cutoff <= selected.cutoff + _EPS]
        return min(eligible, key=lambda p: p.cutoff) if eligible else selected

    moderate = lowest(lambda p: p.false_accept_events <= selected.false_accept_events + 1)
    very = lowest(lambda p: p.false_accepts_per_hour <= 1.5 * selected.false_accepts_per_hour + _EPS)
    return {
        "rule": ("slight = calibrated point; moderate = lowest cutoff within +1 false-accept "
                 "event on the ambient corpus; very = lowest cutoff with FA/h <= 1.5x calibrated "
                 "(both outside the validated false-accept budget)"),
        "slight": selected.to_dict(),
        "moderate": moderate.to_dict(),
        "very": very.to_dict(),
        "moderate_delta_uint8": selected.cutoff_uint8 - moderate.cutoff_uint8,
        "very_delta_uint8": selected.cutoff_uint8 - very.cutoff_uint8,
    }


def _point_from_manifest(manifest: dict) -> OperatingPoint:
    op = manifest["operating_point"]
    return OperatingPoint(
        cutoff=float(op["cutoff"]),
        window=int(op["window"]),
        recall=float(op["recall"]),
        false_accepts_per_hour=float(op["false_accepts_per_hour"]),
        ambient_hours=float(op.get("ambient_hours", manifest.get("evaluation", {}).get("ambient_hours", 0))),
    )


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_sel = sub.add_parser("select", help="evaluate every pair of a calibration grid against a baseline manifest")
    p_sel.add_argument("--calibration", type=Path, required=True)
    p_sel.add_argument("--baseline", type=Path, required=True, help="baseline eval manifest")
    p_gate = sub.add_parser("gate", help="gate one candidate manifest against a baseline manifest")
    p_gate.add_argument("--candidate", type=Path, required=True)
    p_gate.add_argument("--baseline", type=Path, required=True)
    args = parser.parse_args(list(argv) if argv is not None else None)

    baseline = _point_from_manifest(json.loads(args.baseline.read_text()))
    if args.cmd == "gate":
        candidate = _point_from_manifest(json.loads(args.candidate.read_text()))
        result = evaluate_gate(candidate, baseline)
        print(json.dumps({"candidate": candidate.to_dict(), "baseline": baseline.to_dict(),
                          "gate": result.to_dict()}, indent=2))
        return 0 if result.passed and result.improves else 1
    grid = load_grid(json.loads(args.calibration.read_text()))
    point, result, count = select_operating_point(grid, baseline)
    out = {"evaluated_pairs": count, "baseline": baseline.to_dict()}
    if point is None:
        out["selected"] = None
        out["decision"] = "no cutoff/window pair passes the gate — do not deploy"
        print(json.dumps(out, indent=2))
        return 1
    out["selected"] = point.to_dict()
    out["gate"] = result.to_dict()
    out["tiers"] = derive_tiers(grid, point)
    out["decision"] = ("eligible and improves the baseline" if result.improves
                       else "eligible but NOT an improvement — do not deploy")
    print(json.dumps(out, indent=2))
    return 0 if result.improves else 1


if __name__ == "__main__":
    sys.exit(main())
