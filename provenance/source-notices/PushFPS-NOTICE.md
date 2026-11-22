# OBS Native NR PushFPS notice

PushFPS copyright (C) 2026 DosukaSOL and the PushFPS contributors. PushFPS was
forked on 2026-11-12 from [Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo),
copyright (C) Likely7 and the Veyra contributors. See the
[pinned PushFPS notice](https://github.com/DosukaSOL/PushFPS/blob/4e5c0491d0f5f7c01f1c4718e2b156857573abcd/NOTICE.md)
for the upstream history and notices.

## OBS Native NR modifications (GPL-3.0 section 5(a))

**2026-11-22:** The OBS Native NR PushFPS A/B port adapts selected PushFPS
frame-generation code for the OBS source filter and its separate Windows
presenter. It integrates PushFlow RIFE and opt-in ANR FG B15 alongside native
NVIDIA VFG through a shared presenter and session interface; adapts the HUD
lock, shot-change, cadence repair and adaptive multiplier paths; and adds OBS
settings, diagnostics and optional local runtime setup. This is a selected
port, not a redistribution of PushFPS.exe. The OBS release ZIP does not
include third-party runtimes or models.

## Scope in OBS Native NR

The Section 7 permission below is reproduced verbatim from the pinned PushFPS
notice. It applies only to portions authored for PushFPS by its copyright
holders. It does not change or extend the GPL-3.0-only terms of Veyra or
OptiScaler code, or grant rights to redistribute third-party SDKs, runtimes
or models. The OBS Native NR project remains GPL-3.0-only.

## Additional permission under GNU GPL version 3 section 7

This permission is given by the copyright holders of the parts written for PushFPS (DosukaSOL and the
PushFPS contributors). It applies to those parts only; it does not change the terms of the parts written
by Veyra's authors (see below).

If you modify PushFPS, or any covered work, by linking or combining it with any of the following, containing
parts covered by the terms of their own licenses: the NVIDIA RTX SDKs (DLSS SDK and NGX SDK), the NVIDIA
Video Effects and Video Frame Generation SDKs, the NVIDIA Optical Flow SDK, the NVIDIA CUDA driver API
headers, the Intel XeSS SDK, the AMD FidelityFX SDK, Microsoft DirectML (used by PushFlow through ONNX Runtime),
or the unmodified runtime libraries and models these SDKs provide (together, "the SDKs"), the licensors of PushFPS grant you additional permission to convey the
resulting work. Corresponding Source for a non-source form of such a combination does not need to include
the source code of the parts of the SDKs used, but shall include the source code of PushFPS. The SDKs remain
under their own licenses, and nothing here places them under the GPL.

**Veyra's code.** PushFPS.exe also contains code written by Veyra's authors, which they license under
GPL-3.0. Veyra's author builds and distributes Veyra with the same SDKs, and PushFPS distributes the same
combination. A permission like the one above for Veyra's code can only come from Veyra's copyright holders.
