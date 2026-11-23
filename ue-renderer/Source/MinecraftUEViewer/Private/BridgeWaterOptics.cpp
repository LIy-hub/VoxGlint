#include "BridgeWaterOptics.h"

#include "BridgeQuality.h"
#include "BridgeWire.h"
#include "BridgeWaterSun.h"
#include "SystemTextures.h"
#include "McTerrainRegionComponent.h"
#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessInputs.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

namespace
{
    TAutoConsoleVariable<int32> CVarWaterP2(TEXT("mc.Water.P2"), 1, TEXT("Per-pixel water column optics and half-submerged waterline."));
    TAutoConsoleVariable<int32> CVarWaterP2Interface(TEXT("mc.Water.P2.Interface"), 1, TEXT("Use the P2 waterline state for the water underside."));
    TAutoConsoleVariable<int32> CVarWaterP2Debug(TEXT("mc.Water.P2.Debug"), 0,
        TEXT("1: near-plane medium/source, 2: entry/exit/path, 3: opaque depth/water/translucency, 4: transmittance, 5: flat-interface Fresnel, 6: volume inscatter."));
    TAutoConsoleVariable<int32> CVarWaterP2Distortion(TEXT("mc.Water.P2.Distortion"), 1, TEXT("Enable subtle waterline refraction."));
    TAutoConsoleVariable<int32> CVarWaterP2Film(TEXT("mc.Water.P2.Film"), 1, TEXT("Enable lens film on sustained exit from water."));

    TAutoConsoleVariable<int32> CVarWaterP2Volume(TEXT("mc.Water.P2.Volume"), 1, TEXT("Water-side volumetric sunlight."));
    TAutoConsoleVariable<int32> CVarWaterP2Caustics(TEXT("mc.Water.P2.Caustics"), 1, TEXT("Scene-space solar caustics for Balanced and above."));

    constexpr int32 MaxTriangles = 48;
    constexpr double ActivationCm = 35.0;
    constexpr float NoBoundary = 1000.0f;
    constexpr float FilmDuration = 1.8f;
    constexpr uint32 KnownColumn = 1u << 26;
    constexpr uint32 WetColumn = 1u << 27;

    enum class ENearSource : uint8 { Unknown, LoadedDry, Interface, Minecraft, MatchedGrid };

    struct FNearTriangle
    {
        FVector4f A;
        FVector4f B;
        FVector4f C;
    };

    struct FOpticsFrame
    {
        TArray<FNearTriangle, TInlineAllocator<MaxTriangles>> Triangles;
        FBridgeWaterSunFrame Sun;
        TArray<FBridgeWaterLocalLight, TInlineAllocator<4>> LocalLights;
        FVector Origin = FVector::ZeroVector;
        FVector3f Tint = FVector3f(0.0497f, 0.183f, 0.955f);
        FVector2f OpticalScale = FVector2f(1.0f, 1.0f);
        FVector3f Ambient = FVector3f::ZeroVector;
        float EyeSignedCm = NoBoundary;
        float GridTopCm = 0.0f;
        float Clock = 0.0f;
        float Film = 0.0f;
        float MaxPathM = 80.0f;
        int32 Debug = 0;
        ENearSource Source = ENearSource::Unknown;
        bool bActive = false;
        bool bQueryNearPlane = false;
        bool bDistortion = false;
    };

    DECLARE_GPU_STAT_NAMED(McWaterP2Segment, TEXT("MinecraftWaterP2Segment"));
    DECLARE_GPU_STAT_NAMED(McWaterP2Composite, TEXT("MinecraftWaterP2Composite"));

    class FWaterP2SegmentPS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FWaterP2SegmentPS);
        SHADER_USE_PARAMETER_STRUCT(FWaterP2SegmentPS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpaqueDepth)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MergedDepth)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FNearTriangle>, NearTriangles)
            SHADER_PARAMETER(FVector3f, EyeRelativeCm)
            SHADER_PARAMETER(float, EyeSignedCm)
            SHADER_PARAMETER(float, GridTopCm)
            SHADER_PARAMETER(float, MaxPathM)
            SHADER_PARAMETER(uint32, TriangleCount)
            SHADER_PARAMETER(uint32, NearSource)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }
    };

    DECLARE_GPU_STAT_NAMED(McWaterP2Volume, TEXT("MinecraftWaterP2Volume"));

    class FWaterP2VolumePS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FWaterP2VolumePS);
        SHADER_USE_PARAMETER_STRUCT(FWaterP2VolumePS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterSegment)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterGrid)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AirShadow)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterShadow)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, CausticPattern)
            SHADER_PARAMETER_SAMPLER(SamplerState, CausticSampler)
            SHADER_PARAMETER(FVector2f, PatternPhaseM)
            SHADER_PARAMETER(FVector3f, WaterTint)
            SHADER_PARAMETER(FVector2f, WaterOpticalScale)
            SHADER_PARAMETER(FVector3f, EyeInGridM)
            SHADER_PARAMETER(FVector3f, SunAir)
            SHADER_PARAMETER(FVector3f, SunWater)
            SHADER_PARAMETER(FVector3f, SunRadiance)
            SHADER_PARAMETER(FVector3f, ShadowCenterM)
            SHADER_PARAMETER(float, ShadowRadiusM)
            SHADER_PARAMETER(FIntPoint, PixelMin)
            SHADER_PARAMETER(FIntPoint, PixelMax)
            SHADER_PARAMETER(uint32, VolumeSteps)
            SHADER_PARAMETER(uint32, HasSun)
            SHADER_PARAMETER(uint32, LocalLightCount)
            SHADER_PARAMETER_ARRAY(FVector4f, LocalPositionRadius, [4])
            SHADER_PARAMETER_ARRAY(FVector4f, LocalRadiance, [4])
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpaqueDepth)
            SHADER_PARAMETER(uint32, HasCausticPattern)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }
    };

    class FWaterP2CompositePS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FWaterP2CompositePS);
        SHADER_USE_PARAMETER_STRUCT(FWaterP2CompositePS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterVolume)
            SHADER_PARAMETER(uint32, HasVolume)
            SHADER_PARAMETER(FIntPoint, VolumeMax)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterSegment)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpaqueDepth)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MergedDepth)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TransparentColor)
            SHADER_PARAMETER(FIntPoint, PixelMin)
            SHADER_PARAMETER(FIntPoint, PixelMax)
            SHADER_PARAMETER(FVector3f, WaterTint)
            SHADER_PARAMETER(FVector2f, WaterOpticalScale)
            SHADER_PARAMETER(FVector3f, AmbientRadiance)
            SHADER_PARAMETER(float, Clock)
            SHADER_PARAMETER(float, Film)
            SHADER_PARAMETER(float, DistortionPx)
            SHADER_PARAMETER(uint32, DebugMode)
            SHADER_PARAMETER(uint32, HasTransparentColor)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }
    };

    IMPLEMENT_GLOBAL_SHADER(FWaterP2SegmentPS, "/MinecraftUEViewer/Private/McWaterP2.usf", "SegmentPS", SF_Pixel);
    IMPLEMENT_GLOBAL_SHADER(FWaterP2VolumePS, "/MinecraftUEViewer/Private/McWaterP2.usf", "VolumePS", SF_Pixel);
    IMPLEMENT_GLOBAL_SHADER(FWaterP2CompositePS, "/MinecraftUEViewer/Private/McWaterP2.usf", "CompositePS", SF_Pixel);

    FIntVector SectionAt(const FVector& PointCm)
    {
        constexpr double SectionCm = 16.0 * BridgeWire::BlockSize;
        return FIntVector(FMath::FloorToInt32(-PointCm.Y / SectionCm), FMath::FloorToInt32(PointCm.Z / SectionCm),
            FMath::FloorToInt32(PointCm.X / SectionCm));
    }

    FVector SectionOrigin(const FIntVector& Section)
    {
        return BridgeWire::ToUnreal(Section.X * 16.0, Section.Y * 16.0, Section.Z * 16.0) * BridgeWire::BlockSize;
    }
}

class FBridgeUnderwaterView final : public FWorldSceneViewExtension
{
public:
    FBridgeUnderwaterView(const FAutoRegister& AutoReg, UWorld* World)
        : FWorldSceneViewExtension(AutoReg, World) {}

    void SetFrame(FOpticsFrame&& Next) { GameFrame = MoveTemp(Next); }
    void SetLighting(FBridgeWaterSunFrame&& Sun, TConstArrayView<FBridgeWaterLocalLight> Lights)
    {
        GameFrame.Sun = MoveTemp(Sun);
        GameFrame.LocalLights.Reset(Lights.Num());
        GameFrame.LocalLights.Append(Lights.GetData(), Lights.Num());
    }

    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
    {
        return (GameFrame.bActive || GameFrame.Sun.IsActive()) && FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context);
    }

    virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override
    {
        ENQUEUE_RENDER_COMMAND(MinecraftWaterP2Frame)(
            [Self = StaticCastSharedRef<FBridgeUnderwaterView>(AsShared()), Frame = GameFrame](FRHICommandListImmediate&) mutable
            {
                Self->RenderFrame = MoveTemp(Frame);
            });
    }

    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override
    {
        Depths.Reset();
        SunTextures = SunState.Prepare(Graph, Family, RenderFrame.Sun);
    }

    virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& Graph, FSceneView& View,
        const FRenderTargetBindingSlots& Targets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override
    {
        if (!View.bIsGameView || View.bIsSceneCapture || !RenderFrame.bActive)
        {
            return;
        }
        FRDGTextureRef Depth = Targets.DepthStencil.GetTexture();
        if (Depth && Depth->Desc.NumSamples == 1)
        {
            // Single Layer Water can replace this resource or write into it, depending on the prepass.
            FViewDepths& Buffers = Depths.FindOrAdd(&View);
            Buffers.Opaque = Graph.CreateTexture(Depth->Desc, TEXT("Minecraft.WaterP2.OpaqueDepth"));
            AddCopyTexturePass(Graph, Depth, Buffers.Opaque);
        }
    }

    virtual void PrePostProcessPass_RenderThread(FRDGBuilder& Graph, const FSceneView& View,
        const FPostProcessingInputs& Inputs) override
    {
        if (FViewDepths* Buffers = Depths.Find(&View); Buffers && Inputs.SceneTextures)
        {
            Buffers->Merged = Inputs.SceneTextures->GetParameters()->SceneDepthTexture;
        }
    }

    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
        FPostProcessingPassDelegateArray& Callbacks, bool bIsPassEnabled) override
    {
        if (Pass == EPostProcessingPass::BeforeDOF && Depths.Contains(&View))
        {
            Callbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FBridgeUnderwaterView::Composite));
        }
    }

private:
    struct FViewDepths
    {
        FRDGTextureRef Opaque = nullptr;
        FRDGTextureRef Merged = nullptr;
    };

    FScreenPassTexture Composite(FRDGBuilder& Graph, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
    {
        FViewDepths Buffers;
        if (!Depths.RemoveAndCopyValue(&View, Buffers) || !Buffers.Merged)
        {
            return Inputs.ReturnUntouchedSceneColorForPostProcessing(Graph);
        }

        const FScreenPassTexture Scene = FScreenPassTexture::CopyFromSlice(Graph, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
        const FScreenPassTextureViewport Viewport(Scene);
        const FGlobalShaderMap* Shaders = GetGlobalShaderMap(View.GetShaderPlatform());
        const FOpticsFrame& Frame = RenderFrame;
        const FNearTriangle Empty{FVector4f(0), FVector4f(0), FVector4f(0)};
        const TConstArrayView<FNearTriangle> Faces = Frame.bQueryNearPlane && !Frame.Triangles.IsEmpty()
            ? MakeArrayView<const FNearTriangle>(Frame.Triangles.GetData(), Frame.Triangles.Num())
            : MakeArrayView(&Empty, 1);
        FRDGTextureRef Segments = Graph.CreateTexture(FRDGTextureDesc::Create2D(Scene.Texture->Desc.Extent,
            PF_FloatRGBA, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_RenderTargetable),
            TEXT("Minecraft.WaterP2.Segments"));

        {
            RDG_EVENT_SCOPE_STAT(Graph, McWaterP2Segment, "MinecraftWaterP2Segment");
            auto* Parameters = Graph.AllocParameters<FWaterP2SegmentPS::FParameters>();
            Parameters->View = View.ViewUniformBuffer;
            Parameters->OpaqueDepth = Buffers.Opaque;
            Parameters->MergedDepth = Buffers.Merged;
            Parameters->NearTriangles = Graph.CreateSRV(CreateStructuredBuffer(Graph, TEXT("Minecraft.WaterP2.NearTriangles"), Faces));
            Parameters->EyeRelativeCm = FVector3f(View.ViewMatrices.GetViewOrigin() - Frame.Origin);
            Parameters->EyeSignedCm = Frame.EyeSignedCm;
            Parameters->GridTopCm = Frame.GridTopCm;
            Parameters->MaxPathM = Frame.MaxPathM;
            Parameters->TriangleCount = Frame.bQueryNearPlane ? Frame.Triangles.Num() : 0;
            Parameters->NearSource = uint32(Frame.Source);
            Parameters->RenderTargets[0] = FRenderTargetBinding(Segments, ERenderTargetLoadAction::ENoAction);
            AddDrawScreenPass(Graph, RDG_EVENT_NAME("WaterP2.Segments"), View, Viewport, Viewport,
                TShaderMapRef<FWaterP2SegmentPS>(Shaders), Parameters);
        }

#if !UE_BUILD_SHIPPING
        ProbeSegment(Graph, View, Scene.ViewRect.Min + Scene.ViewRect.Size() / 2, Segments);
#endif
        const FIntPoint HalfSize = FIntPoint::DivideAndRoundUp(Scene.ViewRect.Size(), 2);
        FRDGTextureRef Volume = nullptr;
        const bool bSolar = Frame.Sun.Steps > 0 && SunTextures.Grid;
        if ((Frame.Debug == 0 || Frame.Debug == 6) && (bSolar || !Frame.LocalLights.IsEmpty()))
        {
            RDG_EVENT_SCOPE_STAT(Graph, McWaterP2Volume, "MinecraftWaterP2Volume");
            Volume = Graph.CreateTexture(FRDGTextureDesc::Create2D(HalfSize, PF_FloatRGBA,
                FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_RenderTargetable),
                TEXT("Minecraft.WaterP2.Volume"));
            auto* P = Graph.AllocParameters<FWaterP2VolumePS::FParameters>();
            P->View = View.ViewUniformBuffer;
            P->WaterSegment = Segments;
            P->WaterGrid = bSolar ? SunTextures.Grid : GSystemTextures.GetBlackDummy(Graph);
            P->AirShadow = bSolar ? SunTextures.AirShadow : GSystemTextures.GetBlackDummy(Graph);
            P->WaterShadow = bSolar ? SunTextures.WaterShadow : GSystemTextures.GetBlackDummy(Graph);
            P->OpaqueDepth = Buffers.Opaque;
            P->HasSun = bSolar;
            P->LocalLightCount = Frame.LocalLights.Num();
            for (int32 I = 0; I < 4; ++I)
            {
                P->LocalPositionRadius[I] = FVector4f(0);
                P->LocalRadiance[I] = FVector4f(0);
                if (I < Frame.LocalLights.Num())
                {
                    const auto& Light = Frame.LocalLights[I];
                    const FVector3f Rel((Light.Position - View.ViewMatrices.GetViewOrigin()) * 0.01);
                    P->LocalPositionRadius[I] = FVector4f(Rel, Light.RadiusM);
                    P->LocalRadiance[I] = FVector4f(Light.Radiance, 0.0f);
                }
            }
            P->CausticPattern = SunTextures.Pattern ? SunTextures.Pattern : GSystemTextures.GetBlackArrayDummy(Graph);
            P->HasCausticPattern = SunTextures.Pattern != nullptr;
            P->CausticSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI();
            P->PatternPhaseM = Frame.Sun.PatternPhaseM;
            P->EyeInGridM = FVector3f((View.ViewMatrices.GetViewOrigin() - Frame.Sun.Origin) * 0.01);
            P->SunAir = Frame.Sun.SunAir;
            P->SunWater = Frame.Sun.SunWater;
            P->SunRadiance = Frame.Sun.Radiance;
            P->ShadowCenterM = Frame.Sun.CenterM;
            P->ShadowRadiusM = Frame.Sun.RadiusM;
            P->WaterTint = Frame.Tint;
            P->WaterOpticalScale = Frame.OpticalScale;
            P->PixelMin = Scene.ViewRect.Min;
            P->PixelMax = Scene.ViewRect.Max - FIntPoint(1, 1);
            P->VolumeSteps = bSolar ? Frame.Sun.Steps : 6;
            P->RenderTargets[0] = FRenderTargetBinding(Volume, ERenderTargetLoadAction::ENoAction);
            const FScreenPassTextureViewport LowViewport{FScreenPassTexture(Volume)};
            AddDrawScreenPass(Graph, RDG_EVENT_NAME("WaterP2.Volume"), View, LowViewport, Viewport,
                TShaderMapRef<FWaterP2VolumePS>(Shaders), P);
        }

        RDG_EVENT_SCOPE_STAT(Graph, McWaterP2Composite, "MinecraftWaterP2Composite");
        FScreenPassRenderTarget Output = Inputs.OverrideOutput;
        if (!Output.IsValid())
        {
            Output = FScreenPassRenderTarget::CreateFromInput(Graph, Scene, ERenderTargetLoadAction::ENoAction, TEXT("WaterP2.HDR"));
        }
        const FScreenPassTextureSlice Translucency = Inputs.GetInput(EPostProcessMaterialInput::SeparateTranslucency);
        auto* Parameters = Graph.AllocParameters<FWaterP2CompositePS::FParameters>();
        Parameters->View = View.ViewUniformBuffer;
        Parameters->SceneColor = Scene.Texture;
        Parameters->WaterVolume = Volume ? Volume : Segments;
        Parameters->HasVolume = Volume != nullptr;
        Parameters->VolumeMax = HalfSize - FIntPoint(1, 1);
        Parameters->WaterSegment = Segments;
        Parameters->OpaqueDepth = Buffers.Opaque;
        Parameters->MergedDepth = Buffers.Merged;
        Parameters->HasTransparentColor = Translucency.IsValid();
        Parameters->TransparentColor = Translucency.IsValid() ? Translucency.TextureSRV->Desc.Texture : Scene.Texture;
        Parameters->PixelMin = Scene.ViewRect.Min;
        Parameters->PixelMax = Scene.ViewRect.Max - FIntPoint(1, 1);
        Parameters->WaterTint = Frame.Tint;
        Parameters->WaterOpticalScale = Frame.OpticalScale;
        Parameters->AmbientRadiance = Frame.Ambient;
        Parameters->Clock = Frame.Clock;
        Parameters->Film = Frame.Film;
        Parameters->DistortionPx = Frame.bDistortion
            ? FMath::Min(2.0f * Scene.ViewRect.Width() / FMath::Max(1, View.UnscaledViewRect.Width()), 2.0f) : 0.0f;
        Parameters->DebugMode = Frame.Debug;
        Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();
        AddDrawScreenPass(Graph, RDG_EVENT_NAME("WaterP2.Composite"), View, FScreenPassTextureViewport(Output),
            Viewport, TShaderMapRef<FWaterP2CompositePS>(Shaders), Parameters);
        return MoveTemp(Output);
    }

#if !UE_BUILD_SHIPPING
    void ProbeSegment(FRDGBuilder& Graph, const FSceneView& View, FIntPoint Center, FRDGTextureRef Segments)
    {
        static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("BridgeWaterP2Probe"));
        if (!bEnabled)
        {
            return;
        }
        if (!SegmentProbe)
        {
            SegmentProbe = MakeUnique<FRHIGPUTextureReadback>(TEXT("Minecraft.WaterP2.Probe"));
        }
        if (bProbePending)
        {
            if (!SegmentProbe->IsReady()) return;
            int32 Pitch = 0;
            const auto* Pixel = static_cast<const FFloat16Color*>(SegmentProbe->Lock(Pitch));
            if (Pixel)
            {
                const uint32 Source = uint32(FMath::RoundToInt(Pixel->A.GetFloat()));
                UE_LOG(LogTemp, Display,
                    TEXT("WaterP2Probe eyeUE=(%.2f,%.2f,%.2f) entry_m=%.5f end_m=%.5f wet=%.5f source=%u exit=%u tint=%.5f,%.5f,%.5f"),
                    ProbeEye.X, ProbeEye.Y, ProbeEye.Z, Pixel->R.GetFloat(), Pixel->G.GetFloat(), Pixel->B.GetFloat(),
                    Source & 7, Source >> 3, ProbeTint.X, ProbeTint.Y, ProbeTint.Z);
                SegmentProbe->Unlock();
            }
            bProbePending = false;
        }
        const FVector Eye = View.ViewMatrices.GetViewOrigin();
        const FVector Forward = View.GetViewDirection();
        if (ProbeEye.Equals(Eye, 0.001) && ProbeForward.Equals(Forward, 0.0001) && ProbeSource == RenderFrame.Source &&
            ProbeTint.Equals(RenderFrame.Tint, 0.012f)) return;
        ProbeEye = Eye;
        ProbeForward = Forward;
        ProbeSource = RenderFrame.Source;
        ProbeTint = RenderFrame.Tint;
        bProbePending = true;
        AddEnqueueCopyPass(Graph, SegmentProbe.Get(), Segments, FResolveRect(Center.X, Center.Y, Center.X + 1, Center.Y + 1));
    }

    TUniquePtr<FRHIGPUTextureReadback> SegmentProbe;
    FVector ProbeEye = FVector(UE_DOUBLE_BIG_NUMBER);
    FVector ProbeForward = FVector::ZeroVector;
    ENearSource ProbeSource = ENearSource::Unknown;
    FVector3f ProbeTint = FVector3f::ZeroVector;
    bool bProbePending = false;
#endif

    FOpticsFrame GameFrame;
    FOpticsFrame RenderFrame;
    TMap<const FSceneView*, FViewDepths> Depths;
    FBridgeWaterSunRenderState SunState;
    FBridgeWaterSunTextures SunTextures;
};

FBridgeWaterOptics::FBridgeWaterOptics(UWorld* World)
    : Extension(FSceneViewExtensions::NewExtension<FBridgeUnderwaterView>(World)), Sun(MakeUnique<FBridgeWaterSun>()) {}

FBridgeWaterOptics::~FBridgeWaterOptics() = default;

bool FBridgeWaterOptics::IsEnabled()
{
    return CVarWaterP2.GetValueOnGameThread() != 0;
}

bool FBridgeWaterOptics::IsInterfaceEnabled()
{
    return IsEnabled() && CVarWaterP2Interface.GetValueOnGameThread() != 0;
}

void FBridgeWaterOptics::UpdateLighting(const FBridgeWaterSunInputs& Input)
{
    const bool bEnabled = IsEnabled() && Input.Tier > 0;
    const bool bVolume = bEnabled && CVarWaterP2Volume.GetValueOnGameThread() != 0;
    FBridgeWaterSunFrame Frame = Sun->Update(Input, bVolume, bEnabled && CVarWaterP2Caustics.GetValueOnGameThread() != 0);
    bSceneCaustics = Frame.bCaustics;
    Extension->SetLighting(MoveTemp(Frame), bVolume ? MakeArrayView(Input.LocalLights) : TConstArrayView<FBridgeWaterLocalLight>());
}

void FBridgeWaterOptics::CommitSection(const FIntVector& Section, int32 RegionWidth, int32 MinY,
    const McTerrain::FSectionData& Data)
{
    const McTerrain::FLayerData& Water = Data.Layers[McTerrain::Water];
    const int32 Mask = RegionWidth - 1;
    const FVector3f Offset(BridgeWire::ToUnreal((Section.X & Mask) * 16, Section.Y * 16 - MinY,
        (Section.Z & Mask) * 16) * BridgeWire::BlockSize);
    Sun->CommitSection(Section, Offset, Data.Layers[McTerrain::Solid].Positions);
    if (Water.WetFaces.IsEmpty())
    {
        Sections.Remove(Section);
        return;
    }
    auto& Quads = Sections.FindOrAdd(Section);
    Quads.Reset(Water.WetFaces.Num());
    TSet<FVector3f> SideCenters;
    for (const McTerrain::FIndexedFaceData& Face : Water.WetFaces)
    {
        const int32 First = Face.Quad * 4;
        const uint8 Role = Face.Data.Words[0] & 3;
        const FVector3f* Vertices = Water.Positions.GetData() + First;
        const FVector3f Normal = FVector3f::CrossProduct(Vertices[2] - Vertices[0], Vertices[1] - Vertices[0]);
        // The renderer emits a reversed copy of free surfaces for the SLW underside.
        if ((Role == 0 && Normal.Z <= 0.0f) || (Role == 3 && Normal.Z >= 0.0f))
        {
            continue;
        }
        if (Role == 1 || Role == 2)
        {
            const FVector3f Center = (Vertices[0] + Vertices[1] + Vertices[2] + Vertices[3]) * 0.25f;
            bool bDuplicate = false;
            SideCenters.Add(Center, &bDuplicate);
            if (bDuplicate)
            {
                continue;
            }
        }
        FWaterQuad& Quad = Quads.AddDefaulted_GetRef();
        Quad.Role = Role;
        Quad.Tint = Water.Colors[First];
        for (int32 Index = 0; Index < 4; ++Index)
        {
            Quad.Vertices[Index] = Vertices[Index] - Offset;
            Quad.Bounds += Quad.Vertices[Index];
        }
    }
}

void FBridgeWaterOptics::RemoveSection(const FIntVector& Section)
{
    Sections.Remove(Section);
    Sun->RemoveSection(Section);
}

void FBridgeWaterOptics::Reset()
{
    Sections.Reset();
    Sun->Reset();
    bSceneCaustics = false;
    Tint = FVector3f(0.0497f, 0.183f, 0.955f);
    ResetCamera();
}

void FBridgeWaterOptics::ResetCamera()
{
    ExposureWeight = FilmSeconds = 0.0f;
    bHasCamera = bEyeWet = bFilmArmed = false;
    Extension->SetFrame({});
}

void FBridgeWaterOptics::UpdateCamera(const FVector& EyeCm, bool bMinecraftWet, uint32 Column, int32 MinY,
    float DeltaSeconds, const FVector3f& Ambient, float Clock, int32 Tier)
{
    if (!IsEnabled())
    {
        ExposureWeight = bMinecraftWet ? 1.0f : 0.0f;
        bEyeWet = bMinecraftWet;
        bHasCamera = bFilmArmed = false;
        FilmSeconds = 0.0f;
        Extension->SetFrame({});
        return;
    }
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftWaterP2Query);
    const bool bCut = !bHasCamera || FVector::DistSquared(LastEye, EyeCm) > FMath::Square(800.0);
    const FIntVector EyeSection = SectionAt(EyeCm);
    const FIntVector MinSection = SectionAt(EyeCm + FVector(-100, 100, -100));
    const FIntVector MaxSection = SectionAt(EyeCm + FVector(100, -100, 100));
    FOpticsFrame Frame;
    Frame.Origin = SectionOrigin(EyeSection);
    Frame.Debug = FMath::Clamp(CVarWaterP2Debug.GetValueOnGameThread(), 0, 6);
    Frame.OpticalScale = BridgeQuality::WaterOpticalScale();
    Frame.Ambient = Ambient;
    Frame.Clock = Clock;
    Frame.MaxPathM = Tier == 0 ? 45.0f : 80.0f;
    Frame.bDistortion = CVarWaterP2Distortion.GetValueOnGameThread() != 0;

    const bool bKnownDry = (Column & (KnownColumn | WetColumn)) == KnownColumn;
    bEyeWet = bMinecraftWet && !bKnownDry;
    Frame.EyeSignedCm = bEyeWet ? -NoBoundary : NoBoundary;
    Frame.Source = bKnownDry ? ENearSource::LoadedDry : bMinecraftWet ? ENearSource::Minecraft : ENearSource::Unknown;
    float BestDistanceSq = FMath::Square(float(ActivationCm) + 1.0f);
    FVector3f TargetTint = bCut ? FVector3f(0.0497f, 0.183f, 0.955f) : Tint;
    float MatchingTopCm = -NoBoundary;
    const double ActualTop = (MinY + double(Column & 2047u) + double((Column >> 11) & 63u) / 64.0) * BridgeWire::BlockSize;
    const double ActualFloor = (FMath::CeilToDouble(ActualTop * 0.01) - ((Column >> 17) & 31u)) * BridgeWire::BlockSize;
    if (bEyeWet && (Column & WetColumn) && EyeCm.Z <= ActualTop + 1.0 &&
        (EyeCm.Z >= ActualFloor - 1.0 || (Column & (1u << 31)) != 0))
    {
        switch ((Column >> 22) & 15u)
        {
        case 1: TargetTint = FVector3f(0.11f, 0.28f, 0.19f); break;
        case 2: TargetTint = FVector3f(0.085f, 0.53f, 0.78f); break;
        case 3: TargetTint = FVector3f(0.0497f, 0.183f, 0.955f); break;
        case 4: TargetTint = FVector3f(0.056f, 0.15f, 0.69f); break;
        default: break;
        }
    }

    for (int32 Z = MinSection.Z; Z <= MaxSection.Z; ++Z)
    {
        for (int32 X = MinSection.X; X <= MaxSection.X; ++X)
        {
            for (int32 Y = MinSection.Y; Y <= MaxSection.Y; ++Y)
            {
                const FIntVector Section(X, Y, Z);
                const auto* Quads = Sections.Find(Section);
                if (!Quads)
                {
                    continue;
                }
                const FVector WorldOrigin = SectionOrigin(Section);
                const FVector Eye = EyeCm - WorldOrigin;
                const FVector3f Offset(WorldOrigin - Frame.Origin);
                for (const FWaterQuad& Quad : *Quads)
                {
                    if (Quad.Bounds.ComputeSquaredDistanceToPoint(FVector3f(Eye)) > FMath::Square(100.0f))
                    {
                        continue;
                    }
                    for (const int32 Corner : {1, 2})
                    {
                        const FVector3f A = Quad.Vertices[0];
                        const FVector3f B = Quad.Vertices[Corner];
                        const FVector3f C = Quad.Vertices[Corner + 1];
                        const FVector3f Normal = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
                        if (Normal.IsNearlyZero())
                        {
                            continue;
                        }
                        const FVector Closest = FMath::ClosestPointOnTriangleToPoint(Eye, FVector(A), FVector(B), FVector(C));
                        const float DistanceSq = float(FVector::DistSquared(Closest, Eye));
                        if (DistanceSq < BestDistanceSq)
                        {
                            BestDistanceSq = DistanceSq;
                            Frame.EyeSignedCm = FVector::DotProduct(Eye - Closest, FVector(Normal)) < 0.0
                                ? -FMath::Sqrt(DistanceSq) : FMath::Sqrt(DistanceSq);
                            Frame.Source = ENearSource::Interface;
                            const float HeightDelta = float(WorldOrigin.Z + Closest.Z - Frame.Origin.Z);
                            MatchingTopCm = Quad.Role == 0 ? HeightDelta : -NoBoundary;
                            TargetTint = FVector3f(Quad.Tint.R, Quad.Tint.G, Quad.Tint.B) / 255.0f;
                        }
                        if (DistanceSq <= FMath::Square(100.0f) && Frame.Triangles.Num() < MaxTriangles)
                        {
                            Frame.Triangles.Add({FVector4f(A + Offset, float(Quad.Role)),
                                FVector4f(B + Offset, Normal.X), FVector4f(C + Offset, Normal.Y)});
                        }
                    }
                }
            }
        }
    }
    Frame.bQueryNearPlane = Frame.Source == ENearSource::Interface;
    bEyeWet = Frame.EyeSignedCm < 0.0f;
    if (bEyeWet && Frame.bQueryNearPlane && (Column & WetColumn) != 0 && MatchingTopCm > -NoBoundary)
    {
        const double TopCm = (MinY + double(Column & 2047u) + double((Column >> 11) & 63u) / 64.0) * BridgeWire::BlockSize - Frame.Origin.Z;
        if (FMath::Abs(TopCm - MatchingTopCm) < 18.0)
        {
            Frame.GridTopCm = float(TopCm);
            Frame.Source = ENearSource::MatchedGrid;
        }
    }
    Tint = bCut ? TargetTint : FMath::Lerp(Tint, TargetTint, FMath::Clamp(DeltaSeconds * 5.0f, 0.0f, 1.0f));
    Frame.Tint = Tint;
    const float Weight = 1.0f - FMath::SmoothStep(-35.0f, 35.0f, Frame.EyeSignedCm);
    ExposureWeight = bCut ? Weight : FMath::FInterpTo(ExposureWeight, Weight, DeltaSeconds, 6.0f);
    if (bCut || Frame.EyeSignedCm < -6.0f)
    {
        FilmSeconds = 0.0f;
        bFilmArmed = !bCut;
    }
    else if (bFilmArmed && Frame.EyeSignedCm > 8.0f)
    {
        FilmSeconds = FilmDuration;
        bFilmArmed = false;
    }
    FilmSeconds = FMath::Max(0.0f, FilmSeconds - DeltaSeconds);
    Frame.Film = CVarWaterP2Film.GetValueOnGameThread() != 0 ? FilmSeconds / FilmDuration : 0.0f;
    Frame.bActive = bEyeWet || Frame.bQueryNearPlane || Frame.Film > 0.0f || Frame.Debug > 0;
    LastEye = EyeCm;
    bHasCamera = true;
    Extension->SetFrame(MoveTemp(Frame));
}
