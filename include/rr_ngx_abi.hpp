// SPDX-License-Identifier: MIT
// Minimal clean-room NGX D3D12 C ABI definitions for standalone DLSS-RR reference evaluation.
// Parameter string keys and enum values verified against public NVIDIA/DLSS v310.9.1 SDK headers:
//   - https://github.com/NVIDIA/DLSS/blob/v310.9.1/include/nvsdk_ngx_defs.h
//   - https://github.com/NVIDIA/DLSS/blob/v310.9.1/include/nvsdk_ngx_defs_dlssd.h
//   - https://github.com/NVIDIA/DLSS/blob/v310.9.1/include/nvsdk_ngx_helpers_dlssd_d3d.h
// Alternatively, compile with -DRR_USE_OFFICIAL_NGX_SDK=1 and point -I / -L to a local NVIDIA/DLSS v310.9.1 checkout.
#pragma once

#include "rr_ngx_slots.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter;

typedef enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = NVSDK_NGX_Result_Fail | 1,
    NVSDK_NGX_Result_FAIL_PlatformError = NVSDK_NGX_Result_Fail | 2,
    NVSDK_NGX_Result_FAIL_FeatureAlreadyExists = NVSDK_NGX_Result_Fail | 3,
    NVSDK_NGX_Result_FAIL_FeatureNotFound = NVSDK_NGX_Result_Fail | 4,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall = NVSDK_NGX_Result_Fail | 6,
    NVSDK_NGX_Result_FAIL_NotInitialized = NVSDK_NGX_Result_Fail | 7,
    NVSDK_NGX_Result_FAIL_UnsupportedInputFormat = NVSDK_NGX_Result_Fail | 8,
    NVSDK_NGX_Result_FAIL_RWFlagMissing = NVSDK_NGX_Result_Fail | 9,
    NVSDK_NGX_Result_FAIL_MissingInput = NVSDK_NGX_Result_Fail | 10,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = NVSDK_NGX_Result_Fail | 11,
    NVSDK_NGX_Result_FAIL_OutOfDate = NVSDK_NGX_Result_Fail | 12,
    NVSDK_NGX_Result_FAIL_OutOfGPUMemory = NVSDK_NGX_Result_Fail | 13,
    NVSDK_NGX_Result_FAIL_UnsupportedFormat = NVSDK_NGX_Result_Fail | 14,
    NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath = NVSDK_NGX_Result_Fail | 15,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = NVSDK_NGX_Result_Fail | 16,
    NVSDK_NGX_Result_FAIL_Denied = NVSDK_NGX_Result_Fail | 17,
    NVSDK_NGX_Result_FAIL_NotImplemented = NVSDK_NGX_Result_Fail | 18,
} NVSDK_NGX_Result;

#define NVSDK_NGX_SUCCEED(value) (((value) & 0xFFF00000u) != static_cast<uint32_t>(NVSDK_NGX_Result_Fail))
#define NVSDK_NGX_FAILED(value)  (((value) & 0xFFF00000u) == static_cast<uint32_t>(NVSDK_NGX_Result_Fail))

typedef enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_RayReconstruction = 13,
} NVSDK_NGX_Feature;

typedef enum NVSDK_NGX_Version
{
    NVSDK_NGX_Version_API = 0x0000015,
} NVSDK_NGX_Version;

typedef enum NVSDK_NGX_EngineType
{
    NVSDK_NGX_ENGINE_TYPE_CUSTOM = 0,
} NVSDK_NGX_EngineType;

typedef enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced = 1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality = 2,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance = 3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality = 4,
    NVSDK_NGX_PerfQuality_Value_DLAA = 5,
} NVSDK_NGX_PerfQuality_Value;

typedef enum NVSDK_NGX_DLSS_Feature_Flags
{
    NVSDK_NGX_DLSS_Feature_Flags_None = 0,
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR = 1 << 0,
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes = 1 << 1,
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered = 1 << 2,
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = 1 << 3,
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5,
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6,
    NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling = 1 << 7,
} NVSDK_NGX_DLSS_Feature_Flags;

typedef enum NVSDK_NGX_DLSS_Denoise_Mode
{
    NVSDK_NGX_DLSS_Denoise_Mode_Off = 0,
    NVSDK_NGX_DLSS_Denoise_Mode_DLUnified = 1,
} NVSDK_NGX_DLSS_Denoise_Mode;

typedef enum NVSDK_NGX_DLSS_Roughness_Mode
{
    NVSDK_NGX_DLSS_Roughness_Mode_Unpacked = 0,
    NVSDK_NGX_DLSS_Roughness_Mode_Packed = 1,
} NVSDK_NGX_DLSS_Roughness_Mode;

typedef enum NVSDK_NGX_DLSS_Depth_Type
{
    NVSDK_NGX_DLSS_Depth_Type_Linear = 0,
    NVSDK_NGX_DLSS_Depth_Type_HW = 1,
} NVSDK_NGX_DLSS_Depth_Type;

typedef enum NVSDK_NGX_RayReconstruction_Hint_Render_Preset
{
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default = 0,
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D = 4,
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E = 5, // Required by SDK header note when DoF guide is used
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F = 6, // Default model RR2 in v310.9.1
} NVSDK_NGX_RayReconstruction_Hint_Render_Preset;

typedef struct NVSDK_NGX_PathListInfo
{
    wchar_t const* const* Path;
    unsigned int Length;
} NVSDK_NGX_PathListInfo;

typedef struct NVSDK_NGX_LoggingInfo
{
    void* LoggingCallback;
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
} NVSDK_NGX_LoggingInfo;

typedef struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
} NVSDK_NGX_FeatureCommonInfo;

typedef struct NVSDK_NGX_Handle
{
    unsigned int Id;
} NVSDK_NGX_Handle;

struct NVSDK_NGX_Parameter;

struct NVSDK_NGX_ParameterVtbl
{
    void (RR_NGX_MSABI* SetVoidPtr)(NVSDK_NGX_Parameter*, const char*, void*);
    void (RR_NGX_MSABI* SetD3D12Resource)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    void (RR_NGX_MSABI* SetD3D11Resource)(NVSDK_NGX_Parameter*, const char*, ID3D11Resource*);
    void (RR_NGX_MSABI* SetInt)(NVSDK_NGX_Parameter*, const char*, int);
    void (RR_NGX_MSABI* SetUInt)(NVSDK_NGX_Parameter*, const char*, unsigned int);
    void (RR_NGX_MSABI* SetDouble)(NVSDK_NGX_Parameter*, const char*, double);
    void (RR_NGX_MSABI* SetFloat)(NVSDK_NGX_Parameter*, const char*, float);
    void (RR_NGX_MSABI* SetULL)(NVSDK_NGX_Parameter*, const char*, unsigned long long);

    NVSDK_NGX_Result (RR_NGX_MSABI* GetVoidPtr)(const NVSDK_NGX_Parameter*, const char*, void**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetD3D12Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D12Resource**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetD3D11Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D11Resource**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetInt)(const NVSDK_NGX_Parameter*, const char*, int*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetUInt)(const NVSDK_NGX_Parameter*, const char*, unsigned int*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetDouble)(const NVSDK_NGX_Parameter*, const char*, double*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetFloat)(const NVSDK_NGX_Parameter*, const char*, float*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetULL)(const NVSDK_NGX_Parameter*, const char*, unsigned long long*);

    void (RR_NGX_MSABI* Reset)(NVSDK_NGX_Parameter*);
};

struct NVSDK_NGX_Parameter
{
    const NVSDK_NGX_ParameterVtbl* vtbl;

    // Overloaded Set/Get matching public NVIDIA/DLSS v310.9.1 nvsdk_ngx_params.h
    void Set(const char* n, unsigned long long v) { vtbl->SetULL(this, n, v); }
    void Set(const char* n, float v)              { vtbl->SetFloat(this, n, v); }
    void Set(const char* n, double v)             { vtbl->SetDouble(this, n, v); }
    void Set(const char* n, unsigned int v)       { vtbl->SetUInt(this, n, v); }
    void Set(const char* n, int v)                { vtbl->SetInt(this, n, v); }
    void Set(const char* n, ID3D11Resource* v)    { vtbl->SetD3D11Resource(this, n, v); }
    void Set(const char* n, ID3D12Resource* v)    { vtbl->SetD3D12Resource(this, n, v); }
    void Set(const char* n, void* v)              { vtbl->SetVoidPtr(this, n, v); }

    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const { return vtbl->GetULL(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const              { return vtbl->GetFloat(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const             { return vtbl->GetDouble(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const       { return vtbl->GetUInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const                { return vtbl->GetInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const    { return vtbl->GetD3D11Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const    { return vtbl->GetD3D12Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, void** v) const              { return vtbl->GetVoidPtr(this, n, v); }

    void Reset() { vtbl->Reset(this); }
};

static_assert(sizeof(NVSDK_NGX_Parameter) == sizeof(void*), "NVSDK_NGX_Parameter must be a single vtable pointer");
static_assert(sizeof(NVSDK_NGX_ParameterVtbl) == 17 * sizeof(void*), "NVSDK_NGX_ParameterVtbl must have 17 slots");

// Official NGX v310.9.1 Create & Evaluate parameter keys (matching nvsdk_ngx_helpers_dlssd_d3d.h)
#define NVSDK_NGX_Parameter_CreationNodeMask       "CreationNodeMask"
#define NVSDK_NGX_Parameter_VisibilityNodeMask     "VisibilityNodeMask"
#define NVSDK_NGX_Parameter_Width                  "Width"
#define NVSDK_NGX_Parameter_Height                 "Height"
#define NVSDK_NGX_Parameter_OutWidth               "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight              "OutHeight"
#define NVSDK_NGX_Parameter_PerfQualityValue       "PerfQualityValue"
#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects "DLSS.Enable.Output.Subrects"
#define NVSDK_NGX_Parameter_DLSS_Denoise_Mode      "DLSS.Denoise.Mode"
#define NVSDK_NGX_Parameter_DLSS_Roughness_Mode    "DLSS.Roughness.Mode"
#define NVSDK_NGX_Parameter_Use_HW_Depth           "DLSS.Use.HW.Depth"

#define NVSDK_NGX_Parameter_Color                  "Color"
#define NVSDK_NGX_Parameter_Output                 "Output"
#define NVSDK_NGX_Parameter_Depth                  "Depth"
#define NVSDK_NGX_Parameter_MotionVectors          "MotionVectors"
#define NVSDK_NGX_Parameter_Jitter_Offset_X        "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y        "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_MV_Scale_X             "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y             "MV.Scale.Y"
#define NVSDK_NGX_Parameter_Reset                  "Reset"
#define NVSDK_NGX_Parameter_ExposureTexture        "ExposureTexture"
#define NVSDK_NGX_Parameter_DLSS_Pre_Exposure      "DLSS.Pre.Exposure"
#define NVSDK_NGX_Parameter_DLSS_Exposure_Scale    "DLSS.Exposure.Scale"

#define NVSDK_NGX_Parameter_DiffuseAlbedo          "DLSS.Input.DiffuseAlbedo"
#define NVSDK_NGX_Parameter_SpecularAlbedo         "DLSS.Input.SpecularAlbedo"
#define NVSDK_NGX_Parameter_GBuffer_Normals        "GBuffer.Normals"
#define NVSDK_NGX_Parameter_GBuffer_Roughness      "GBuffer.Roughness"
#define NVSDK_NGX_Parameter_GBuffer_SpecularMvec   "GBuffer.SpecularMvec"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance "DLSSD.SpecularHitDistance"

#define NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX "WorldToViewMatrix"
#define NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX  "ViewToClipMatrix"

#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width  "DLSS.Render.Subrect.Dimensions.Width"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height "DLSS.Render.Subrect.Dimensions.Height"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X       "DLSS.Input.Color.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y       "DLSS.Input.Color.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X       "DLSS.Input.Depth.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y       "DLSS.Input.Depth.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X           "DLSS.Input.MV.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y           "DLSS.Input.MV.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X "DLSS.Input.DiffuseAlbedo.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X "DLSS.Input.SpecularAlbedo.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y "DLSS.Input.SpecularAlbedo.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X     "DLSS.Input.Normals.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y     "DLSS.Input.Normals.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X   "DLSS.Input.Roughness.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y   "DLSS.Input.Roughness.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X            "DLSS.Output.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y            "DLSS.Output.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X "DLSSD.SpecularHitDistance.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y "DLSSD.SpecularHitDistance.Subrect.Base.Y"

// Optional guides
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayer          "DLSS.TransparencyLayer"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X "DLSS.TransparencyLayer.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y "DLSS.TransparencyLayer.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity   "DLSS.TransparencyLayerOpacity"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X "DLSS.TransparencyLayerOpacity.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y "DLSS.TransparencyLayerOpacity.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs     "DLSS.TransparencyLayerMvecs"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X "DLSS.TransparencyLayerMvecs.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y "DLSS.TransparencyLayerMvecs.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_DisocclusionMask           "DLSS.DisocclusionMask"
#define NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X "DLSS.DisocclusionMask.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y "DLSS.DisocclusionMask.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles      "DLSSD.ColorBeforeParticles"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency   "DLSSD.ColorBeforeTransparency"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_X "DLSSD.ColorBeforeTransparency.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_Y "DLSSD.ColorBeforeTransparency.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog            "DLSSD.ColorBeforeFog"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog_Subrect_Base_X "DLSSD.ColorBeforeFog.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog_Subrect_Base_Y "DLSSD.ColorBeforeFog.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide "DLSSD.ScreenSpaceSubsurfaceScatteringGuide"
#define NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_X "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_Y "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide         "DLSSD.DepthOfFieldGuide"
#define NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_X "DLSSD.DepthOfFieldGuide.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_Y "DLSSD.DepthOfFieldGuide.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_ResponsivityMask          "DLSSD.ResponsivityMask"
#define NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_X "DLSSD.ResponsivityMask.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_Y "DLSSD.ResponsivityMask.Subrect.Base.Y"

#define NVSDK_NGX_Parameter_SuperSamplingDenoising_Available             "SuperSamplingDenoising.Available"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver    "SuperSamplingDenoising.NeedsUpdatedDriver"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor "SuperSamplingDenoising.MinDriverVersionMajor"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor "SuperSamplingDenoising.MinDriverVersionMinor"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult     "SuperSamplingDenoising.FeatureInitResult"
#define NVSDK_NGX_Parameter_DLSSDGetStatsCallback                        "DLSSDGetStatsCallback"
#define NVSDK_NGX_Parameter_SizeInBytes                                  "SizeInBytes"

typedef NVSDK_NGX_Result (RR_NGX_MSABI* PFN_NVSDK_NGX_DLSS_GetStatsCallback)(NVSDK_NGX_Parameter* pInParams);

#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA             "RayReconstruction.Hint.Render.Preset.DLAA"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality     "RayReconstruction.Hint.Render.Preset.UltraQuality"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality          "RayReconstruction.Hint.Render.Preset.Quality"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced         "RayReconstruction.Hint.Render.Preset.Balanced"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance      "RayReconstruction.Hint.Render.Preset.Performance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance "RayReconstruction.Hint.Render.Preset.UltraPerformance"
