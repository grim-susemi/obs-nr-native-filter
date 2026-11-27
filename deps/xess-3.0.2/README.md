# Intel XeSS SDK 3.0.2 headers

Unmodified public C headers from the official Intel repository, tag `v3.0.2`
(annotated tag `5ec517a49a1ba8c4ad640361e4eb5227bf0742a5`, commit
`8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0`, release message "XeSS SDK 3.0.2").

Fetched from `https://raw.githubusercontent.com/intel/xess/v3.0.2/` on
2026-11-27 and copied byte for byte. No reformatting, no patching.

| File | SHA-256 |
| --- | --- |
| `inc/xess_fg/xefg_swapchain.h` | `39f20fc1eef0b5d3fd41e25a2c5c04734d1a2cbdcf3f3da1c86cefef181ce976` |
| `inc/xess_fg/xefg_swapchain_d3d12.h` | `1aa9a1c31214aaefb2c4ca4fd3bce29a166c8384148466d69f679ea554cf2dbc` |
| `inc/xess_fg/xefg_swapchain_debug.h` | `3600f2698fc747810948deb6fe568f105a8f40c003c73b6940262ae311f33aeb` |
| `inc/xell/xell.h` | `0a65a677001edf473156e6ed1d6cf97150fa12ff9930811bbaf4613b080c11b2` |
| `inc/xell/xell_d3d12.h` | `e3b07f2f3adc317416a6aaa2343ae9b56c7707a34bb0e56fbefea14463a76bce` |

The SDK license is vendored at
`provenance/source-notices/Intel-XeSS-License.txt`
(SHA-256 `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df`).

No `libxess_fg.dll`, `libxell.dll`, import library, or other binary is stored
in this tree. The presenter loads the user-installed official binaries at
runtime only after Authenticode `O=Intel Corporation` and SHA-256 verification.

Pinned official `XeSS_SDK_3.0.2.zip` (SHA-256
`88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`) binaries:

| Binary | File version | SHA-256 |
| --- | --- | --- |
| `bin/libxess_fg.dll` | 1.3.1.78 | `ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27` |
| `bin/libxell.dll` | 1.3.2.10 | `d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7` |

Those hashes are the constants in `tools/fg-presenter/xefg_runtime.h`.
