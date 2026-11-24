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

The original CREDITS.md and RenoDX-MIT.txt entries came from S/docs/CREDITS.md and
S/Licenses/RenoDX_ATTRIBUTION.txt, with CRLF normalized to LF. The original source
hashes remain in source-lock.json. CREDITS.md now also records OBS additions.

## OBS Native NR PushFPS A/B additions

- [PushFPS-NOTICE.md](PushFPS-NOTICE.md): selected source from [DosukaSOL/PushFPS](https://github.com/DosukaSOL/PushFPS/tree/4e5c0491d0f5f7c01f1c4718e2b156857573abcd), its Veyra lineage, the dated OBS modifications, and the upstream Section 7 permission limited to PushFPS-authored portions.
- [ONNXRuntime-MIT.txt](ONNXRuntime-MIT.txt): MIT text from the vendored [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) 1.20.1 header distribution. The optional runtime uses [Microsoft DirectML 1.15.4](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4), which has its own package terms.
- [RIFE-MIT.txt](RIFE-MIT.txt): MIT text from [hzwer/ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE/blob/19b903effb008cefbffaea2c4a0b877beaece6b1/LICENSE). [AmusementClub/vs-mlrt](https://github.com/AmusementClub/vs-mlrt) distributes the optional RIFE ONNX exports separately.

See [CREDITS.md](CREDITS.md) for the attribution chain. These notices do not
imply that the ZIP contains any third-party runtime, model, or addon.
