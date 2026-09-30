# Ghosting differential (Task 25) - finite, read-only

> Status: bounded historical attribution only. This does not measure or fix ghosting, and it does not
> approve implementation, installation, or any quality claim.

## What this is

The owner reported that ordinary operation ghosts even with the NR effect OFF, and that an apparent
Opti failure reduced ghosting regardless of the NR toggle. Those are subjective observations: no
matched same-settings A/B footage and no measured pixels exist. This ledger separates the plausible
candidate mechanisms, binds each to the actual source/config/log evidence that was read once, and marks
what is confirmed source versus unconfirmed incident versus unknown.

A loaded module or an existing code path proves neither the effective incident stage, nor SR/FG/TRAA
evaluation, nor the current source image, nor resize/evaluate state. Missing incident history is
recorded as UNKNOWN, which closes finite historical attribution and does not block the next native tasks.

## Candidate dispositions

| # | Candidate | Source evidence | Incident state | A/B validity |
| --- | --- | --- | --- | --- |
| C1 | ordinary SR | SR-then-neural-pass design; ordinary DLSS create/evaluate helpers; DLSS5_Feed enabled | UNKNOWN | INVALID - which SR backend ran is unobservable; no matched footage |
| C2 | frame generation (FG) | FGInput=Upscaler/FGOutput=XeFG; bridge selects provider and multiplier | UNKNOWN | INVALID - stored config postdates the session; no FG-on/off pair |
| C3 | TRAA temporal blend | TRAA enabled in chain with HISTORY_BLEND=0.9; reproject + history lerp | UNKNOWN | INVALID - TRAA-only toggle is a future control, never executed |
| C4 | stale/duplicate bridge/cadence | producer-prediction applies a previous guide ticket to current color; presentation order | UNKNOWN | INVALID - static alignment risk, no key/ticket trace |
| C5 | raw capture/display blur | capture at 1080p60/NV12; differing source settings across sessions | UNKNOWN | INVALID - sessions differ; belongs to the future oracle |
| C6 | real NR (Feature 18) | DlssNr Enabled/FinishedPicture stored true; NR-off is disabled-mode, not stack bypass; log stale | UNKNOWN | INVALID - never a measured same-scene NR-on/off comparison |
| C7 | invalid error/frozen/resize | resize failure 887A000A + render-target errors; earlier crash in the module's Present stack | UNKNOWN | INVALID - error/freeze/resize states cannot enter a healthy A/B or a causal conclusion |

## Key retained source facts

- The NR toggle is **not** a whole-stack bypass: with NR disabled the bridge still runs interop, copy and
  provider presentation, so "NR off" is not a clean baseline by itself.
- A separate enabled temporal operation (the ReShade TRAA history blend at 0.9) exists independently of the
  NR and FG switches.
- The producer-prediction route can apply a **previous** guide ticket to the **current** color. This is a
  confirmed static alignment risk, not a runtime-proven cause.
- Mapped module presence does not prove evaluation, and which upscaler ran is not observable locally.

## Why the cause stays UNKNOWN

No matched same-settings footage, no guide/color alignment trace, no current evaluate/apply counts. Stored
settings were rewritten after the reported session and the legacy module log is stale and effectively
unwritable. Therefore:

- The apparent Opti-failure ghost reduction is **not** treated as causal proof of improvement.
- Error, blank, freeze, resize or changed-scene states are **excluded** from any healthy comparison.
- The historical incident cause is closed as **UNKNOWN**; this does not block native task progression.

Evidence ledger: `E/task-25/differential.json` (each row carries path, date and sha256 identity).
