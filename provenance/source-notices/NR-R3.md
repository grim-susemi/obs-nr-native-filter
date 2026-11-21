# NR R3 design references

The R3 implementation in `src/nr_prewarm.h`, `src/nr_model.cpp` and
`src/shaders/nr_stabilize.hlsl` is original project code, GPL-3.0-only.
No upstream code, shaders, binaries, model data, or third-party runtime
integration was imported for this change. These links record the design references:

- Likely7/Veyra-NRVideo, `fe8df5328a62849eb2d79053564b9574ab68aecd`,
  [`PreviewGpuSession.cpp`](https://github.com/Likely7/Veyra-NRVideo/blob/fe8df5328a62849eb2d79053564b9574ab68aecd/src/engine/PreviewGpuSession.cpp):
  prepare before demand, adopt only at exact identity. See upstream GPLv3 LICENSE.
- 2600th/dlss5-video-player, `c6bfdb3b2ecf8003b1d33b51b0da517459fafed0`,
  [`NvofResolveShader.h`](https://github.com/2600th/dlss5-video-player/blob/c6bfdb3b2ecf8003b1d33b51b0da517459fafed0/src/NvofResolveShader.h):
  compare a static-pixel hypothesis to the candidate vector. See upstream MIT LICENSE.
- Likely7/Veyra-NRVideo at the same commit,
  [`NrTemporal.hlsl`](https://github.com/Likely7/Veyra-NRVideo/blob/fe8df5328a62849eb2d79053564b9574ab68aecd/shaders/NrTemporal.hlsl):
  motion-compensated correction history. Veyra credits SAOG0721/Magpie,
  `3841698348bfb246623d4acf791984c8b68a577b`, `src/Magpie.Core/DLSSNRTemporalShader.h`,
  under GPLv3. The current shader uses this project's own color/hold/ownership contract.
