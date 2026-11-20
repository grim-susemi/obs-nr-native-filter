# Install guide

How to install OBS Native NR 0.2.1 into OBS Studio on Windows.

[한국어 설치 안내는 아래에 있습니다](#한국어-설치-안내)

## Before you start

- **OBS Studio 32.x, 64-bit** (built and tested with 32.2.2) with the **Direct3D 11 renderer**.
- **NVIDIA RTX 20, 30, 40 or 50 series** with a current driver (NVIDIA VFG preview requires RTX 40 or 50).
- **The NR model file** `nvngx_dlssnr.dll` (310.8.0.0 compatibility build, 165,840,496 bytes, SHA-256 `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`). It is not included in the zip; get it from the RenoDX Discord:
  1. Join: https://discord.gg/F6AUTeWJHM
  2. Channel: https://discord.com/channels/1408098019194310818/1543975158937821315
- **Optional DLSS 4.5 SR & NVIDIA VFG runtimes**: `install.bat` automatically offers to download and verify them directly from NVIDIA's official repositories (`github.com/NVIDIA/DLSS` and `pypi.nvidia.com/nvidia-vfx/`).

## One-click install

1. Download `obs-nr-native-filter-0.2.1-windows-x64.zip` and `SHA256SUMS.txt` from the [latest release](https://github.com/grim-susemi/obs-nr-native-filter/releases/latest).
2. Close OBS completely, including the tray icon.
3. Extract the whole zip into a folder and double-click `install.bat`.
   - **Language**: Enter for Korean, `2` for English.
   - **OBS folder**: press Enter to confirm the detected OBS path.
   - **NR model file**: drag `nvngx_dlssnr.dll` into the window and press Enter (or Enter alone to skip).
   - **DLSS 4.5 SR (~56 MB)**: press Enter (Y) to download & signature-verify official `nvngx_dlss.dll` into `bin\64bit\dlss\nvngx_dlss.dll`.
   - **NVIDIA VFG (~415 MB wheel -> 5 DLLs)**: press Enter (Y) to download, verify SHA-256, and extract the 5 signed NVIDIA VFG DLLs into `bin\64bit\nvidia-vfg\` (reused locally if Veyra is already installed).
   - **Admin prompt**: choose **Yes**.
4. Start OBS > right-click a source > **Filters** > **Effect Filters [+]** > **Native NR**.

Logs are saved under `%LOCALAPPDATA%\obs-nr-native-filter`.

## What the installer writes

| File | Folder inside your OBS install |
|---|---|
| `obs-nr-filter.dll` | `obs-plugins\64bit` |
| `obs-nr-fg-presenter.exe` | `bin\64bit` |
| `en-US.ini`, `ko-KR.ini` | `data\obs-plugins\obs-nr-filter\locale` |
| `nr_codec.dxil`, `nr_color.dxil`, `nr_stabilize.dxil`, `nr_cas.dxil`, `nr_guides.cso`, `nr_interop_vertex.cso`, `nr_interop_pixel.cso`, `nr_obs_analysis.cso` | `bin\64bit` |
| `nvngx_dlssnr.dll` (only if supplied) | `bin\64bit` |
| `nvngx_dlss.dll` (optional, downloaded from NVIDIA) | `bin\64bit\dlss` |
| `cudart64_12.dll`, `NVCVImage.dll`, `nvngxruntime.dll`, `NVVideoEffects.dll`, `nvVFXVideoFrameGeneration.dll` (optional, from NVIDIA) | `bin\64bit\nvidia-vfg` |

## Uninstall

Close OBS and double-click `uninstall.bat`. It restores any backed-up files and removes only the plugin files and runtimes that the installer copied.

---

## 한국어 설치 안내

OBS Native NR 0.2.1을 Windows의 OBS Studio에 설치하는 방법입니다.

### 준비

- **OBS Studio 32.x 64비트** (32.2.2에서 시험), 렌더러는 **Direct3D 11** (기본값).
- **NVIDIA RTX 20~50 시리즈** (NVIDIA VFG 2x~4x 미리보기는 RTX 40/50 전용).
- **NR 모델 파일** `nvngx_dlssnr.dll` 310.8.0.0 호환판 (165,840,496바이트, SHA-256 `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`). 릴리즈에 들어 있지 않으며, RenoDX 디스코드(https://discord.gg/F6AUTeWJHM)에서 받을 수 있습니다.
- **DLSS 4.5 SR · NVIDIA VFG 런타임**: 별도로 찾아 받을 필요 없이 `install.bat`이 NVIDIA 공식 서버에서 직접 내려받아 서명·SHA-256을 검증한 뒤 자동 설치해 줍니다.

### 원클릭 설치 순서

1. [최신 릴리즈](https://github.com/grim-susemi/obs-nr-native-filter/releases/latest)에서 `obs-nr-native-filter-0.2.1-windows-x64.zip`을 내려받아 압축을 전부 풉니다.
2. OBS를 완전히 끕니다(트레이 아이콘 포함).
3. 압축 푼 폴더의 `install.bat`을 더블클릭합니다.
   - **언어**: 그냥 Enter를 누르면 한국어입니다.
   - **OBS 폴더**: 찾은 경로가 맞으면 Enter.
   - **NR 모델 파일**: `bin\64bit\nvngx_dlssnr.dll`이 없으면 파일을 창에 끌어다 놓고 Enter (Enter만 누르면 건너뜀).
   - **DLSS 4.5 SR / NVIDIA VFG**: 자동 설치 여부를 묻는 곳에서 그냥 Enter(Y)를 누르면 공식 NVIDIA 파일이 자동 설치됩니다.
   - **관리자 권한 창**: **예**를 누릅니다.
4. OBS를 실행해 소스 우클릭 > **필터** > **효과 필터 [+]** > **Native NR** 을 추가합니다.
   - 기존 구형 `rtx-superresolution` 필터가 켜져 있으면 끄고 Native NR 안의 **DLSS 4.5 업스케일 · CAS 선명도**를 사용하세요.
   - VFG와 NR을 동시에 켜고 GPU 프레임 시간이 빡빡하면 **NR 작업 해상도**를 `75%`로 두거나 **NR 반복 횟수**를 `1`로 두세요.

### 제거

OBS를 끄고 같은 폴더의 `uninstall.bat`을 더블클릭합니다. 설치기가 복사한 파일·런타임만 안전하게 제거하고 기존 파일을 복원합니다.
