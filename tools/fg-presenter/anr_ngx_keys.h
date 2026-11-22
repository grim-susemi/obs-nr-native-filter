// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cfloat>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

namespace nr::fg::anr::keys {
inline constexpr auto feature = NVSDK_NGX_Feature_FrameGeneration;
inline constexpr auto creationNodeMask = NVSDK_NGX_Parameter_CreationNodeMask;
inline constexpr auto visibilityNodeMask = NVSDK_NGX_Parameter_VisibilityNodeMask;
inline constexpr auto width = NVSDK_NGX_Parameter_Width;
inline constexpr auto height = NVSDK_NGX_Parameter_Height;
inline constexpr char backbufferFormat[] = "DLSSG.BackbufferFormat";
inline constexpr char internalWidth[] = "DLSSG.InternalWidth";
inline constexpr char internalHeight[] = "DLSSG.InternalHeight";
inline constexpr char dynamicResolution[] = "DLSSG.DynamicResolution";
inline constexpr char resourceAlwaysProvidedFlags[] = "DLSSG.ResourceAlwaysProvidedFlags";
inline constexpr char resourceNeverProvidedFlags[] = "DLSSG.ResourceNeverProvidedFlags";
inline constexpr unsigned alwaysProvided = 0x07;
inline constexpr unsigned neverProvided = 0xB8;
inline constexpr char multiFrameCountMax[] = "DLSSG.MultiFrameCountMax";
inline constexpr char multiFrameCount[] = "DLSSG.MultiFrameCount";
inline constexpr char multiFrameIndex[] = "DLSSG.MultiFrameIndex";
inline constexpr char backbuffer[] = "DLSSG.Backbuffer";
inline constexpr char outputInterpolated[] = "DLSSG.OutputInterpolated";
inline constexpr char outputDisableInterpolation[] = "DLSSG.OutputDisableInterpolation";
inline constexpr char mvecs[] = "DLSSG.MVecs";
inline constexpr char depth[] = "DLSSG.Depth";
inline constexpr char reset[] = "DLSSG.Reset";
inline constexpr char mvecScaleX[] = "DLSSG.MvecScaleX";
inline constexpr char mvecScaleY[] = "DLSSG.MvecScaleY";
inline constexpr char backbufferFrameId[] = "DLSSG.BackbufferFrameID";
inline constexpr char cameraViewToClip[] = "DLSSG.CameraViewToClip";
inline constexpr char clipToCameraView[] = "DLSSG.ClipToCameraView";
inline constexpr char clipToLensClip[] = "DLSSG.ClipToLensClip";
inline constexpr char clipToPrevClip[] = "DLSSG.ClipToPrevClip";
inline constexpr char prevClipToClip[] = "DLSSG.PrevClipToClip";
inline constexpr const char* cameraPosition[] = {"DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ"};
inline constexpr const char* cameraUp[] = {"DLSSG.CameraUpX", "DLSSG.CameraUpY", "DLSSG.CameraUpZ"};
inline constexpr const char* cameraRight[] = {"DLSSG.CameraRightX", "DLSSG.CameraRightY", "DLSSG.CameraRightZ"};
inline constexpr const char* cameraForward[] = {"DLSSG.CameraFwdX", "DLSSG.CameraFwdY", "DLSSG.CameraFwdZ"};
inline constexpr char cameraNear[] = "DLSSG.CameraNear";
inline constexpr char cameraFar[] = "DLSSG.CameraFar";
inline constexpr char cameraFov[] = "DLSSG.CameraFOV";
inline constexpr char cameraAspectRatio[] = "DLSSG.CameraAspectRatio";
inline constexpr char cameraMotionIncluded[] = "DLSSG.CameraMotionIncluded";
inline constexpr char depthInverted[] = "DLSSG.DepthInverted";
inline constexpr char mvecJittered[] = "DLSSG.MvecJittered";
inline constexpr char mvecInvalidValue[] = "DLSSG.MvecInvalidValue";
inline constexpr float invalidMotionVector = FLT_MAX;
} // namespace nr::fg::anr::keys
