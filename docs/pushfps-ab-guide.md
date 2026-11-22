# PushFPS A/B testing in OBS

This guide describes OBS Native NR builds with the PushFPS A/B controls. A 1x
preview shows original frames. Higher multipliers insert frames in the
preview; the OBS recording and stream are not your A/B output.

## Set up a backend

Open the frame generation effect filter on the test source. Choose a backend
before opening its preview. Close and reopen the preview when changing backends
or settings that are only read at launch.

| Backend | What you need |
| --- | --- |
| NVIDIA VFG (`NativeVfg`) | A supported NVIDIA GPU and the optional NVIDIA VFG runtime. Use the installer’s NVIDIA VFG option or its supported local runtime setup. |
| PushFlow RIFE | ONNX Runtime 1.20.1, DirectML 1.15.4, and a compatible local RIFE `.onnx` model. Start with 2x for comparison; PushFlow supports 2x–4x. |
| ANR FG B15 | Explicit experimental opt-in and the complete local B15 addon. Start with 2x. |

**PushFlow automatic setup:** Accept the installer prompt to download the
PushFlow runtime and RIFE models. It fetches the pinned runtime packages and
the vs-mlrt RIFE ONNX exports separately from the release ZIP and installs the
models with the filenames below. End users do not need Python.

**PushFlow offline setup:** Pass `-PushFlowRuntimeDir` to `scripts\install.ps1`
with the directory containing `onnxruntime.dll` and `DirectML.dll`. Pass
`-PushFlowModelsDir` with the directory that directly contains your compatible
RIFE `.onnx` files. Before installing, name the compatible Fast model
`pushflow-fast.onnx` and the compatible Quality model `pushflow-quality.onnx`.
The installer preserves local filenames; OBS will not find a model by its
original export name alone.

The installer places the DLLs directly in `<OBS folder>/bin/64bit/pushflow`.
Relative to that runtime directory, the installed model paths are:

| PushFlow preset in OBS | Installed model path |
| --- | --- |
| Fast | `models/pushflow-fast.onnx` |
| Quality | `models/pushflow-quality.onnx` |

In OBS, choose a preset in the filter before opening the preview. The preset
selects the installed model; the two directory arguments are installer inputs.
If the runtime is absent, the preview automatically falls back to 1x original
frames. Check the log before counting a run as interpolated.

**ANR FG B15 is experimental and unsigned.** Only opt in if you trust the
addon you obtained. Enable experimental ANR, then select `nvngx_dlssg.dll`
**inside the complete user-supplied B15 addon folder**, keeping its sibling
model and companion files beside it. A lone copied DLL will not work. Missing
siblings block the load before `LoadLibraryExW`; the preview falls back to 1x
original frames and logs the diagnostic reason. The installer does not
download the addon.

## Run a repeatable comparison

1. Use a repeatable clip with steady motion, a static HUD, irregular motion
   cadence and a hard scene cut. Fix the capture settings, presentation size,
   display refresh rate, and preview layout. Record a brief 1x baseline.
2. Run the same segment on each backend with a common fixed multiplier (start
   at 2x), with the A/B controls off. Record the initialized backend and
   effective multiplier. Treat a fallback as a 1x control, not as a backend
   result.
3. Change one live control at a time while keeping the preview open, and run
   the same segment with the control off and on:
   - **Live HUD Lock:** Look for sharper, steadier fixed HUD elements on moving
     backgrounds.
   - **Frame Pacing Repair:** Compare frame intervals for uneven input, such as
     alternating roughly 17 ms and 50 ms gaps at a nominal 30 fps.
   - **Adaptive Multiplier:** Compare effective multipliers and output timing as
     the input rate changes. Record the target FPS and keep it fixed across runs.
4. Test **VfgShotChange** separately. Close the preview, change the toggle,
   then reopen it and replay the hard cut. Compare interpolation artifacts
   around the cut with an off/on pair on each backend. When enabled on
   `NativeVfg`, the presenter disables the internal NVVFX CPU shot-change sync
   before `NvVFX_Load`; the presenter’s GPU cut path handles scene changes.
   Changing the toggle while the preview is open does not change that session.

## Collect the log

Save `%TEMP%\obs-nr-vfg-presenter.log` after each run so the next session does
not replace your evidence. Compare matching clip intervals and log the
driver/GPU, input and display rates, backend/preset, requested multiplier,
live settings and whether the preview was reopened.

| Log fields | What to compare |
| --- | --- |
| `event=session_started`, `event=initialized`, `backend`, `sdk_version`, `gpu_shot_change` | Confirm which backend opened and whether the reopen-applied mode took effect. Record fallback events and any `reason` or `message` separately. |
| `event`, `eff_mult`, `adaptive_fg`, `adaptive_target_fps` | Compare effective multiplier changes and the mix of original and generated presentations. |
| `present_id`, `qpc_ns`, `work_ns`, `b_arrival_ns`, `pacing_shift_ns`, `pacing_repair` | Use consecutive presentation timestamps to report median and p95 frame intervals; report work time and pacing corrections separately. Convert nanoseconds to milliseconds by dividing by 1,000,000. |
| `shot_cut`, `hud_lock` | Match cut and lock flags to the tested scene segment; inspect the preview for ghosts, smearing and HUD blur. |

## 한국어 요약

- 같은 소스와 해상도·주사율에서 먼저 1x 원본을 기록한 뒤 NVIDIA VFG,
  PushFlow RIFE, ANR FG B15를 각각 같은 고정 배수로 비교하세요. 백엔드를
  변경한 뒤에는 미리보기를 다시 여세요.
- **Live HUD Lock**, **Frame Pacing Repair**, **Adaptive Multiplier**는 미리보기를
  닫지 않고 하나씩 전환합니다. **VfgShotChange**는 다음에 미리보기를 열 때
  적용되며, NVIDIA VFG 사용 시 `NvVFX_Load` 전에 내부 CPU 장면 전환 동기화를 끕니다.
- PushFlow는 설치 도우미로 받거나, 오프라인 설치 시 DLL이 들어 있는 폴더를
  `-PushFlowRuntimeDir`로, 호환 `.onnx` 파일이 바로 들어 있는 폴더를
  `-PushFlowModelsDir`로 지정하세요. 오프라인 설치 전 모델을 아래 파일명으로
  맞추세요. 설치기는 원래 파일명을 바꾸지 않습니다.
  설치된 `<OBS 폴더>/bin/64bit/pushflow`에서 OBS의 PushFlow 프리셋 Fast는
  `models/pushflow-fast.onnx`, Quality는 `models/pushflow-quality.onnx`를
  사용합니다. OBS에서는 폴더 대신 프리셋을 고릅니다. 런타임이 없으면 1x 원본으로
  복귀합니다.
- ANR B15는 서명되지 않은 실험 기능입니다. 명시적으로 켠 뒤 모델과 동반
  파일이 모두 있는 B15 폴더 **안의** `nvngx_dlssg.dll`을 지정하세요. 동반
  파일이 없으면 DLL을 로드하지 않고 1x 원본으로 복귀하며 로그에 이유를 남깁니다.
- 실행별로 `%TEMP%\obs-nr-vfg-presenter.log`를 저장하고 백엔드, 폴백,
  `eff_mult`, 프레임 간격의 중앙값·p95, `shot_cut`과 `hud_lock`을 비교하세요.
