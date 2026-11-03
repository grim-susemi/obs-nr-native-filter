# Install guide

How to install OBS Native NR 0.1.0 into OBS Studio on Windows. It takes about five minutes.

[한국어 설치 안내는 아래에 있습니다](#한국어-설치-안내)

## Before you start

- **OBS Studio 32.x, 64-bit** (tested with 32.2.2). In OBS: Help > About.
- **Direct3D 11 renderer**, which is the OBS default. In OBS: Settings > Advanced > Video > Renderer.
- **NVIDIA RTX 20, 30, 40 or 50 series** with a current driver (tested: RTX 4090, driver 617.14).
- **The NR model file** `nvngx_dlssnr.dll`, version 310.8.0.0, 165,840,496 bytes, SHA-256 `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`. It is not included. It circulates in the RenoDX community (ShortFuse's DLSS thread on the RenoDX Discord). To check a file you have, run this in PowerShell; the result must start with `E67DEE209320`:

  ```powershell
  (Get-FileHash 'C:\path\to\nvngx_dlssnr.dll' -Algorithm SHA256).Hash
  ```

  NVIDIA's signed builds (310.8.0.0 `E16BCF15...`, 310.9.16.0) only support RTX 50 and are refused by this version.

## Install with the installer

1. Download `obs-nr-native-filter-0.1.0-windows-x64.zip` and `SHA256SUMS.txt` from the [latest release](https://github.com/grim-susemi/obs-nr-native-filter/releases/latest). Optional check: `(Get-FileHash .\obs-nr-native-filter-0.1.0-windows-x64.zip).Hash` must match the line in `SHA256SUMS.txt`.
2. Close OBS completely, including the tray icon.
3. Extract the whole zip. You get a folder named `obs-nr-native-filter-0.1.0-windows-x64`.
4. Double-click `install.bat` in that folder.
   - **Language**: press `2` for English. Enter alone picks Korean.
   - **OBS folder**: the installer finds it. Press Enter to accept, or pick a number or type the path if it asks.
   - **NR model file**: if `nvngx_dlssnr.dll` is not already in `obs-studio\bin\64bit`, drag your file into the window and press Enter. The installer checks the hash and copies it. Enter alone skips this step.
   - **Admin prompt**: choose **Yes**. OBS lives under Program Files, so copying needs admin rights. The rest continues in the admin window.
5. Wait for **Install complete**. A log is kept in `%LOCALAPPDATA%\obs-nr-native-filter`.

## Add the filter

1. Start OBS.
2. Right-click your source (capture card, game capture, window, media) > **Filters**.
3. Under **Effect Filters**, click **+** > **Native NR**.
4. The filter properties show a status line. **Status: NR is being applied** means it works. **Status: warming up (original shown)** is normal for the first second or two.

Start with the default settings. The [settings table](../README.md#settings) explains each value.

### If OBS is not in C:\Program Files\obs-studio

The filter looks for the model file at `C:\Program Files\obs-studio\bin\64bit\nvngx_dlssnr.dll`. If your OBS is somewhere else (Steam, portable, another drive), the installer prints the path to use. Paste it into **NR runtime file** in the filter properties, then remove the filter and add it again. A changed runtime path only applies to a newly added filter.

## What the installer writes

| File | Folder inside your OBS install |
|---|---|
| `obs-nr-filter.dll` | `obs-plugins\64bit` |
| `en-US.ini`, `ko-KR.ini` | `data\obs-plugins\obs-nr-filter\locale` |
| `nr_codec.dxil`, `nr_color.dxil`, `nr_guides.cso`, `nr_interop_vertex.cso`, `nr_interop_pixel.cso`, `nr_obs_analysis.cso` | `bin\64bit` |
| `nvngx_dlssnr.dll`, only if you gave it one | `bin\64bit` |

Any file it replaces is first backed up to `%LOCALAPPDATA%\obs-nr-native-filter\backup-<date>`.

## Manual install

Close OBS. Copy everything inside the `files` folder of the release into your OBS folder, keeping the sub folders. Put `nvngx_dlssnr.dll` in `bin\64bit`. Writing to Program Files needs admin rights.

## Uninstall

Close OBS and double-click `uninstall.bat` in the same release folder. It removes the files listed above, puts back any file the installer replaced, and removes `nvngx_dlssnr.dll` only if the installer copied it. Files you changed after installing are left alone.

## Troubleshooting

| What you see | What to do |
|---|---|
| **Native NR** is not in the filter list | The plugin is not in this OBS. Run `install.bat` again and check the OBS folder it shows. OBS must be 32.x, 64-bit. |
| Reason: the NR runtime file does not exist | Put `nvngx_dlssnr.dll` in `obs-studio\bin\64bit`, or set **NR runtime file** to its full path, then remove and re-add the filter. |
| Reason: ... (hash differs) | Wrong build of the model file. You need 310.8.0.0, SHA-256 `E67DEE209320...`. |
| Reason: D3D11 renderer required | OBS Settings > Advanced > Video > Renderer: Direct3D 11, then restart OBS. |
| Reason: the graphics driver reset the GPU | Restart OBS. If it keeps happening, lower the source resolution or frame rate. |
| Picture looks brighter or greenish | Set **Local tone** to 0, the default. |
| The original shows for a moment after changing a value | Normal. NR restarts with the new values. |
| The installer says OBS is running | Close OBS fully (tray icon too) and press Enter. |

The OBS log (Help > Log Files > View Current Log) has lines starting with `[obs-nr-filter]`. A line with `native=1 failed=0` means NR started. Include those lines and the installer log when you [report a problem](https://github.com/grim-susemi/obs-nr-native-filter/issues).

---

## 한국어 설치 안내

OBS Native NR 0.1.0을 Windows의 OBS Studio에 설치하는 방법입니다. 5분 정도 걸립니다.

### 준비

- **OBS Studio 32.x 64비트** (32.2.2에서 시험). OBS 메뉴 도움말 > 정보에서 확인.
- **Direct3D 11 렌더러** (OBS 기본값). 설정 > 고급 > 비디오 > 렌더러.
- **NVIDIA RTX 20~50 시리즈**와 최신 드라이버 (시험: RTX 4090, 드라이버 617.14).
- **NR 모델 파일** `nvngx_dlssnr.dll` 310.8.0.0, 165,840,496바이트, SHA-256 `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`. 릴리즈에 들어 있지 않으니 RenoDX 커뮤니티(ShortFuse DLSS 스레드)에서 구하세요. 가진 파일이 맞는지는 PowerShell에서 아래 명령으로 확인하고, 결과가 `E67DEE209320`으로 시작해야 합니다.

  ```powershell
  (Get-FileHash 'C:\경로\nvngx_dlssnr.dll' -Algorithm SHA256).Hash
  ```

  NVIDIA 정품 서명판(310.8.0.0 `E16BCF15...`, 310.9.16.0)은 RTX 50 전용이라 이 버전에서는 받지 않습니다.

### 설치기로 설치

1. [최신 릴리즈](https://github.com/grim-susemi/obs-nr-native-filter/releases/latest)에서 `obs-nr-native-filter-0.1.0-windows-x64.zip`과 `SHA256SUMS.txt`를 받습니다.
2. OBS를 완전히 끕니다. 작업 표시줄 오른쪽 트레이 아이콘도 닫으세요.
3. zip을 전부 풉니다. `obs-nr-native-filter-0.1.0-windows-x64` 폴더가 생깁니다.
4. 그 폴더의 `install.bat`을 더블클릭합니다.
   - **언어**: 그냥 Enter를 누르면 한국어입니다.
   - **OBS 폴더**: 설치기가 찾아 보여 줍니다. 맞으면 Enter. 여러 개면 번호를 고르고, 못 찾으면 경로를 입력합니다.
   - **NR 모델 파일**: OBS의 `bin\64bit`에 `nvngx_dlssnr.dll`이 없으면 가진 파일을 창에 끌어다 놓고 Enter. 해시를 확인한 뒤 복사합니다. 그냥 Enter를 누르면 건너뜁니다.
   - **관리자 권한 창**: **예**를 누릅니다. OBS가 Program Files에 있어 복사에 관리자 권한이 필요합니다. 나머지는 관리자 창에서 이어집니다.
5. **설치 완료**가 뜨면 끝입니다. 기록은 `%LOCALAPPDATA%\obs-nr-native-filter`에 남습니다.

### 필터 추가

1. OBS를 켭니다.
2. 소스(캡처보드, 게임 캡처, 창 캡처, 미디어)를 우클릭 > **필터**.
3. **효과 필터** 아래 **+** > **Native NR**.
4. 필터 속성에 **상태: NR 적용 중**이 보이면 성공입니다. 처음 1~2초 동안 **상태: 준비 중 (원본 표시)**는 정상입니다.

처음에는 기본값 그대로 쓰세요. 값마다 뜻은 [설정 표](../README.md#settings)에 있습니다. **로컬 톤**은 기본 0입니다. NVIDIA 기본값 1은 어두운 곳을 밝히고 초록빛을 살짝 더해 화면이 허옇게 보일 수 있습니다.

### OBS가 C:\Program Files\obs-studio가 아닌 곳에 있을 때

필터는 기본으로 `C:\Program Files\obs-studio\bin\64bit\nvngx_dlssnr.dll`을 찾습니다. Steam판, 포터블, 다른 드라이브처럼 다른 곳에 OBS가 있으면 설치기가 넣을 경로를 알려 줍니다. 그 경로를 필터 속성의 **NR 런타임 파일** 칸에 넣고, 필터를 지웠다가 다시 추가하세요.

### 제거

OBS를 끄고 같은 폴더의 `uninstall.bat`을 더블클릭합니다. 설치한 파일을 지우고, 설치 때 바꾼 파일은 되돌리고, `nvngx_dlssnr.dll`은 설치기가 넣었을 때만 지웁니다.

### 문제 해결

| 보이는 것 | 할 일 |
|---|---|
| 필터 목록에 **Native NR**이 없음 | 이 OBS에 플러그인이 없습니다. `install.bat`을 다시 실행해 보여 주는 OBS 폴더를 확인하세요. OBS 32.x 64비트여야 합니다. |
| 이유: NR 런타임 파일이 없음 | `nvngx_dlssnr.dll`을 OBS의 `bin\64bit`에 넣거나 **NR 런타임 파일**에 전체 경로를 넣고 필터를 지웠다 다시 추가하세요. |
| 이유: … (해시 다름) | 다른 판의 모델 파일입니다. 310.8.0.0 `E67DEE209320...`이 필요합니다. |
| 이유: D3D11 렌더러가 필요함 | 설정 > 고급 > 비디오 > 렌더러를 Direct3D 11로 바꾸고 OBS를 다시 켜세요. |
| 이유: 그래픽 드라이버가 GPU를 재설정해 NR을 멈춤 | OBS를 다시 켜세요. 반복되면 소스 해상도나 프레임을 낮추세요. |
| 화면이 밝아지거나 초록빛이 돎 | **로컬 톤**을 0으로 두세요. |
| 값을 바꾼 직후 잠깐 원본이 보임 | 정상입니다. 새 값으로 NR을 다시 시작하는 중입니다. |

OBS 로그(도움말 > 로그 파일 > 현재 로그 보기)에 `[obs-nr-filter]`로 시작하는 줄이 있습니다. `native=1 failed=0`이 보이면 NR이 시작된 것입니다. 문제를 [알려 주실 때](https://github.com/grim-susemi/obs-nr-native-filter/issues) 그 줄들과 설치기 기록을 함께 붙여 주세요.
