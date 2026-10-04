# Wake-word models

| File | What it is |
|---|---|
| `hey_leonard.json` / `.tflite` | The packaged "Hey Leonard" microWakeWord model. Its `probability_cutoff` and `sliding_window_size` are the calibrated operating point. |
| `hey_leonard.eval.json` | Metrics-only evaluation manifest: data counts, evaluation sets, the operating point, the gate result against the previous model, derived sensitivity tiers, and provenance. No audio, transcripts, enrollment data or voice prints. |
| `previous/` | The model one release back, with its manifest — the one-step rollback. |

The default model was trained with real voices from the maintainer's household
(two speakers, one room). That is disclosed in its manifest; only aggregate
metrics are published.

## The gate (fixed policy)

A model ships only if one exact packaged operating point — cutoff **and**
window — meets, against the deployed model:

- recall ≥ baseline recall − 0.01, and
- false accepts/hour ≤ baseline FA/h + 0.05,

and actually improves on it. "Passes but does not improve" means keep the
incumbent. `tools/wakeword/gate.py select` evaluates every cutoff/window pair
of a calibration grid and says which case applies; there is no option to
widen the margins.

## Sensitivity tiers

The firmware's "Wake word sensitivity" select uses the model's own calibrated
cutoff as **Slightly sensitive** (the default) and derives the other two tiers
from the manifest (`tiers`): **Moderately** = the lowest cutoff within one more
false accept on the ambient corpus, **Very** = the lowest cutoff with at most
1.5× the calibrated FA/h. Both are outside the validated false-accept budget;
choosing them is a deliberate trade, separate from training and calibration.

## Releasing a model

1. Stage a candidate (outside this repo) and run
   `python tools/wakeword/gate.py select --calibration <grid.json> --baseline models/hey_leonard.eval.json`.
   Ship only on "eligible and improves the baseline".
2. Move the current `hey_leonard.*` and its manifest into `previous/`.
3. Add the new model files and write its manifest (same schema; `baseline` =
   the model now in `previous/`).
4. Set `wake_model_sha256` (and the tier deltas, if they changed) in
   `home-assistant-voice.realtime.yaml`, commit, then update the
   `wake_word_model` pin to that commit in a follow-up commit.
5. `python tools/wakeword/check_release.py --git` must pass (CI runs it).
6. Promotion is staged: shadow (`packages/wake-word-shadow.yaml`, ≥ 7 days),
   one-device canary (≥ 7 days), then all devices —
   `tools/wakeword/promotion.py` records each decision and the automatic
   rollback trigger (false-wake flags/hour above the incumbent baseline + 0.05).
   Rollback = restore `previous/` and its manifest, re-pin, flash.

Nothing in this repository trains or flashes automatically.
