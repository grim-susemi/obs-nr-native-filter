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
- Proprietary runtime: nvngx_dlssnr.dll is not part of this source lock and is not copied into the
  repository. Since the 0.3.0 Windows release, the owner-selected 310.9.16.0 Model B build
  (SHA-256 cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed) is bundled into the
  full release ZIP by make-release.ps1 after hash verification; the 310.8.0.0 build is not
  redistributed. No runtime binary is referenced as project source by this lock.
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

## Intel XeSS 3.0.2 (XeFG / XeLL) runtime

- [Intel-XeSS-License.txt](Intel-XeSS-License.txt): Intel Simplified Software License (October 2022)
  for the unmodified Intel XeSS SDK 3.0.2 C headers in `deps/xess-3.0.2/`. The release ZIP ships
  this notice but never contains `libxess_fg.dll` or `libxell.dll`.
- `install.ps1 -InstallXeFg` optionally downloads the official
  `XeSS_SDK_3.0.2.zip` (SHA-256
  `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`), verifies Intel
  Authenticode `O=Intel Corporation` and the pinned binary hashes below, then copies the
  unmodified DLLs and `LICENSE.txt` into `bin\64bit\xess-fg\`. The command is never run by the
  release packager, so no Intel binary is bundled or auto-installed.

  | Binary | File version | SHA-256 |
  | --- | --- | --- |
  | `bin/libxess_fg.dll` | 1.3.1.78 | `ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27` |
  | `bin/libxell.dll` | 1.3.2.10 | `d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7` |

  The presenter loads the installed pair from `bin\64bit\xess-fg\` (or the session-provided
  directory) only after the same signature and hash checks. See `deps/xess-3.0.2/README.md`.

## Opt-in user-supplied MFGUnlocker.dll (XeMFG)

- [MFG-Unlocker-MIT.txt](MFG-Unlocker-MIT.txt): MIT notice for the provider ABI declaration
  reproduced from the community MFG-Unlocker project. No implementation code is copied.
- This project and its installers NEVER download, copy, stage, or bundle `MFGUnlocker.dll`.
  XeMFG 3x/4x is a bring-your-own-DLL opt-in:
  1. In the video upscale/frame-generation filter, enable **Opt in to experimental XeMFG (3x/4x)**.
  2. Select the user-supplied `MFGUnlocker.dll` in **VFG.XeMfg.DllPath**.
  3. Keep the verified Intel runtime (`libxess_fg.dll` 1.3.1.78, `libxell.dll` 1.3.2.10) in
     `bin\64bit\xess-fg\` or in an `RTXMFG-XeSS` directory beside `MFGUnlocker.dll`.
- The community DLL is admitted by `XeMfgDllValidator`: canonical path, no reparse point or
  root escape, x64 PE, the `MfgXeMFGProviderGetApi` export, and a SHA-256 log entry. Authenticode
  is intentionally NOT required because valid community builds are unsigned. The Intel
  companion pair still requires the strict Intel signature and pinned hashes above.

See [CREDITS.md](CREDITS.md) for the attribution chain. These notices do not
imply that the ZIP contains any third-party model or addon. The only third-party
runtime in the full 0.3.0 ZIP is the owner-selected, hash-pinned nvngx_dlssnr.dll
described above.
