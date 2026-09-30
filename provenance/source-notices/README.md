# Source notices index (task-1 provenance subunit)

This directory preserves the license and attribution notices for the source files selected in
provenance/source-lock.json. It is a notice index, not a source or model copy.

## Files

- GPL-3.0-NOTICE.txt: GPL-3.0 identification and per-program notice, hash-bound to S/LICENSE.
- RenoDX-MIT.txt: copy of the RenoDX MIT attribution/licence notice (colour composition derivation).
- CREDITS.md: copy of the upstream/fork attribution chain (S/docs/CREDITS.md), treated as data.

## license_ids used in source-lock.json

- GPL-3.0-only: project source licensed under GNU GPL version 3 (S/LICENSE,
  sha256 230184f60bae2feaf244f10a8bac053c8ff33a183bcc365b4d8b876d2b7f4809). No per-file
  or-later grant was found, so the identifier is -only.
- MIT-RenoDX: MIT portions derived from RenoDX, Copyright (c) 2025 Carlos Lopez Jr.; applies to
  S/OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl (colour composition). Full MIT text is in
  RenoDX-MIT.txt.

## Exclusions recorded by the lock (no copy selected)

- MFG unlock: MIT (ImDreamt / Michael Robles dashdogy), excluded by default and separate from NR;
  it is not in the reuse set.
- Proprietary runtime: nvngx_dlssnr.dll is owner/self-supplied and NOT redistributable or bundled;
  no runtime binary is copied, hashed or referenced as project source by this lock.
- AGNYA material: comparison-only; its terms remain UNKNOWN/unresolved and NO AGNYA content is
  selected or copied. This is a scope statement, not a claim about absent files.
- Generated shader artifacts (.cso, generated *_Shader.h, .spv) are build outputs, not authored
  source, and are not listed in files[]; only the .hlsl/.hlsli sources are locked.

## Stale control

Every files[] entry carries the frozen preflight sha256 alongside the current sha256 and a
preflight_match flag. A changed input is kept as a finding, never silently erased.

## Copy normalization

CREDITS.md and RenoDX-MIT.txt are LF-normalized copies of S/docs/CREDITS.md and S/Licenses/RenoDX_ATTRIBUTION.txt. Content is identical modulo CRLF to LF (the native editor writes LF only); the source sha256 values are recorded in the notice entries of source-lock.json.
