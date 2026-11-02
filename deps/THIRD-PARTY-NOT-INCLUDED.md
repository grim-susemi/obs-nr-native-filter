# Third-party NVIDIA files not included

This repository does not contain NVIDIA SDK files. They were removed from the whole history before the first push.
The filter needs them to build. Download them from NVIDIA and place them at the paths below; the hashes identify the exact files the N2 candidate was built with.

| Path | SHA-256 | Bytes |
|---|---|---|
| `deps/ngx/libs/nvsdk_ngx_d.lib` | `4b6cecad7f1906571c94010241f650e4a5457e64fad49ddaccb82de79f6c2999` | 4090730 |
| `deps/ngx/nvsdk_ngx_defs_dlssd.h` | `2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716` | 14693 |
| `deps/ngx/nvsdk_ngx_defs.h` | `ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e` | 44313 |
| `deps/ngx/nvsdk_ngx_helpers_cuda.h` | `f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042` | 3593 |
| `deps/ngx/nvsdk_ngx_helpers_d3d.h` | `ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba` | 31976 |
| `deps/ngx/nvsdk_ngx_helpers_dlssd_d3d.h` | `6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293` | 72668 |
| `deps/ngx/nvsdk_ngx_helpers_dlssd.h` | `a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532` | 732 |
| `deps/ngx/nvsdk_ngx_helpers.h` | `5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471` | 5317 |
| `deps/ngx/nvsdk_ngx_loader.h` | `abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75` | 13252 |
| `deps/ngx/nvsdk_ngx_params_dlssd.h` | `d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e` | 1199 |
| `deps/ngx/nvsdk_ngx_params.h` | `943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc` | 7744 |
| `deps/ngx/nvsdk_ngx_standalone_common.h` | `38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53` | 12985 |
| `deps/ngx/nvsdk_ngx_standalone_cuda.h` | `b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499` | 10906 |
| `deps/ngx/nvsdk_ngx.h` | `dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848` | 40289 |
| `deps/nvofa/LicenseAgreement.pdf` | `b8981c69975513d5671661be2f8dc1852d6f5e0715cf5b682e75409065635b5e` | 143682 |
| `deps/nvofa/nvOpticalFlowCommon.h` | `fb96fc6061c01ec2c5970ecebebacac7535c033717ff39810cada1f197c8ec9f` | 33648 |
| `deps/nvofa/nvOpticalFlowD3D12.h` | `d1952d0946a26f1b77e76c320b4dfe9077d946fdc1a6195e01f0460362026305` | 19086 |
| `deps/nvofa/Release_Notes.pdf` | `65d26d91490063e1efa7756327327752168f7b5b2439c62a9884565667f9c1e5` | 169260 |

Sources: NVIDIA DLSS/NGX SDK (`deps/ngx`: headers and `nvsdk_ngx_d.lib`) and NVIDIA Optical Flow SDK (`deps/nvofa`: headers and license PDFs). The pinned SDK versions are recorded in `provenance/sdk-lock.json`.

한국어: NVIDIA SDK 파일은 저장소와 전체 기록에서 제외했습니다. 빌드하려면 위 경로에 공식 배포본을 직접 받아 넣어야 합니다.
