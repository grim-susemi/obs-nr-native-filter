# Credits

This fork builds on [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) and [OptiScaler](https://github.com/optiscaler/OptiScaler). OptiScaler began with [PotatoOfDoom's CyberFSR2](https://github.com/PotatoOfDoom/CyberFSR2).

Colour processing derives from [clshortfuse's RenoDX](https://github.com/clshortfuse/renodx); see [attribution/licence](../Licenses/RenoDX_ATTRIBUTION.txt).

Integrations include hhkbble's multipass/composition, [y4my4my4m's Vulkan work](NR-VULKAN.md) and [cmh1448's motion metadata](NR-MOTION-METADATA.md). Linked notes identify source commits and test limits.

This variant restores the built-in [Ada MFG work and attribution](RTX40-MFG.md), derived from y4my4my4m and the earlier fork under GPL-3.0.

## RTX 40 MFG unlock

The built-in RTX 40 multi frame generation unlock is adapted from [y4my4my4m's fork](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG) (GPL-3.0). The provider discovery, the Streamline plugin frame-ceiling patch, the software frame pacing option and the PTX temporal fix are adapted from [KleberMotta's fork](https://github.com/KleberMotta/OptiScaler-DLSS5-MFG-RTX40) (MIT), a port of the MFG Unlock ReShade addon by [Dreamt](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx) and [mavismmg](https://github.com/mavismmg/MFGAdaUnlock-RenoDx). The technique originates from [dashdogy's RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock). See [RTX40-MFG.md](RTX40-MFG.md) and the [licences](../Licenses/MFGUnlock_LICENSE.txt).

## OptiScaler contributors

- @PotatoOfDoom for CyberFSR2.
- @Artur for DLSS Enabler and help with the NVNGX API.
- @LukeFZ and @Nukem for their mods and shared knowledge.
- @FakeMichau for support, testing and features.
- @QM for testing and access to games.
- @TheRazerMD for testing and support.
- @Cryio, @krispy, @krisshietala, @Lordubuntu, @scz and @Veeqo for the earlier compatibility matrix.
- The DLSS2FSR community for its support.

This project uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype), licensed under the [FTL](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT). Other notices are in [Licenses](../Licenses).

## Upstream sponsorship

Upstream credits [SignPath.io](https://signpath.io/) for Windows code signing and the [SignPath Foundation](https://signpath.org/) for its certificate.

Support upstream: [cdozdil](https://github.com/sponsors/cdozdil?frequency=one-time) and [nitec](https://buymeacoffee.com/nitec).

## OBS Native NR 0.2.0 additions

- Anti-flicker (`nr_stabilize.hlsl`), refined NVOF optical flow (`nr_guides.hlsl`), NVIDIA VFG low-latency presenter architecture, and NGX DLSS Super Resolution integration adapted from [Likely7/Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo) (GPL-3.0-only).
- Contrast Adaptive Sharpening (`nr_cas.hlsl`) derived from [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS). See the [MIT notice](AMD-FidelityFX-CAS-MIT.txt).

## OBS Native NR PushFPS A/B additions

- [DosukaSOL/PushFPS](https://github.com/DosukaSOL/PushFPS/tree/4e5c0491d0f5f7c01f1c4718e2b156857573abcd), forked from Likely7/Veyra-NRVideo: selected frame generation and comparison code. See the [modification and scoped Section 7 notice](PushFPS-NOTICE.md).
- [hzwer/ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE) (Megvii Inc.): RIFE interpolation research and implementation; see the [MIT notice](RIFE-MIT.txt). [AmusementClub/vs-mlrt](https://github.com/AmusementClub/vs-mlrt) supplies the optional external RIFE ONNX exports.
- [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) 1.20.1 supplies the vendored API headers; see the [MIT notice](ONNXRuntime-MIT.txt). Its optional runtime and [Microsoft DirectML 1.15.4](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) run PushFlow. The ZIP includes neither runtime, RIFE models, NVIDIA runtimes, nor ANR addon files.
