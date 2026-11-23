#include "BridgeWaterSun.h"

#include "BridgeWire.h"
#include "CommonRenderResources.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GlobalShader.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

namespace
{
    constexpr int32 ShadowSize = 512;
    constexpr int32 MaxCasterQuads = 196608;
    constexpr float WaterIOR = 1.333f;

    DECLARE_GPU_STAT_NAMED(McWaterP2Shadows, TEXT("MinecraftWaterP2Shadows"));

    class FWaterSunShadowVS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FWaterSunShadowVS);
        SHADER_USE_PARAMETER_STRUCT(FWaterSunShadowVS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, CasterPositions)
            SHADER_PARAMETER(FVector3f, ShadowAxis)
            SHADER_PARAMETER(FVector3f, ShadowCenterM)
            SHADER_PARAMETER(float, ShadowRadiusM)
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
        {
            return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
        }
    };

    class FWaterSunShadowPS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FWaterSunShadowPS);
        SHADER_USE_PARAMETER_STRUCT(FWaterSunShadowPS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WaterGrid)
            SHADER_PARAMETER(uint32, WaterOnly)
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
        {
            return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
        }
    };

    IMPLEMENT_GLOBAL_SHADER(FWaterSunShadowVS, "/MinecraftUEViewer/Private/McWaterSunShadow.usf", "MainVS", SF_Vertex);
    IMPLEMENT_GLOBAL_SHADER(FWaterSunShadowPS, "/MinecraftUEViewer/Private/McWaterSunShadow.usf", "MainPS", SF_Pixel);

    BEGIN_SHADER_PARAMETER_STRUCT(FWaterSunShadowPass, )
        SHADER_PARAMETER_STRUCT_INCLUDE(FWaterSunShadowVS::FParameters, VS)
        SHADER_PARAMETER_STRUCT_INCLUDE(FWaterSunShadowPS::FParameters, PS)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()

    FVector SectionOrigin(const FIntVector& Section)
    {
        return BridgeWire::ToUnreal(Section.X * 16.0, Section.Y * 16.0, Section.Z * 16.0) * BridgeWire::BlockSize;
    }

    UTextureRenderTarget2D* MakeShadow(UObject* Owner)
    {
        auto* Shadow = NewObject<UTextureRenderTarget2D>(Owner);
        Shadow->ClearColor = FLinearColor::Black;
        Shadow->Filter = TF_Nearest;
        Shadow->AddressX = Shadow->AddressY = TA_Clamp;
        Shadow->InitCustomFormat(ShadowSize, ShadowSize, PF_R32_FLOAT, true);
        return Shadow;
    }

    FRDGTextureRef RegisterTexture(FRDGBuilder& Graph, FTextureResource* Resource, const TCHAR* Name)
    {
        if (!Resource || !Resource->TextureRHI)
        {
            return nullptr;
        }
        return RegisterExternalTexture(Graph, Resource->TextureRHI, Name);
    }

    void DrawShadow(FRDGBuilder& Graph, const FGlobalShaderMap* ShaderMap, const FBridgeWaterSunFrame& Frame,
        FRDGBufferSRVRef Casters, FRDGTextureRef Grid, FRDGTextureRef Target, bool bWater)
    {
        const FIntPoint Size = Target->Desc.Extent;
        FRDGTextureRef Depth = Graph.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_DepthStencil, FClearValueBinding::DepthFar,
            TexCreate_DepthStencilTargetable), TEXT("Minecraft.WaterP2.ShadowDepth"));
        auto* Parameters = Graph.AllocParameters<FWaterSunShadowPass>();
        Parameters->VS.CasterPositions = Casters;
        Parameters->VS.ShadowAxis = bWater ? Frame.SunWater : Frame.SunAir;
        Parameters->VS.ShadowCenterM = Frame.CenterM;
        Parameters->VS.ShadowRadiusM = Frame.RadiusM;
        Parameters->PS.WaterGrid = Grid;
        Parameters->PS.WaterOnly = bWater;
        Parameters->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::EClear);
        Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(Depth, ERenderTargetLoadAction::EClear,
            ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
        const TShaderMapRef<FWaterSunShadowVS> VS(ShaderMap);
        const TShaderMapRef<FWaterSunShadowPS> PS(ShaderMap);
        const uint32 Count = Frame.Casters->Num() / 2;
        Graph.AddPass(bWater ? RDG_EVENT_NAME("WaterP2.RefractedShadow") : RDG_EVENT_NAME("WaterP2.AirShadow"),
            Parameters, ERDGPassFlags::Raster | ERDGPassFlags::NeverCull,
            [Parameters, VS, PS, Size, Count](FRHICommandList& RHICmd)
            {
                RHICmd.SetViewport(0, 0, 0.0f, Size.X, Size.Y, 1.0f);
                FGraphicsPipelineStateInitializer Pipeline;
                RHICmd.ApplyCachedRenderTargets(Pipeline);
                Pipeline.BlendState = TStaticBlendState<>::GetRHI();
                Pipeline.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                Pipeline.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();
                Pipeline.PrimitiveType = PT_TriangleList;
                Pipeline.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
                Pipeline.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
                Pipeline.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
                SetGraphicsPipelineState(RHICmd, Pipeline, 0);
                SetShaderParameters(RHICmd, VS, VS.GetVertexShader(), Parameters->VS);
                SetShaderParameters(RHICmd, PS, PS.GetPixelShader(), Parameters->PS);
                RHICmd.DrawPrimitive(0, Count, 1);
            });
    }
}

FBridgeWaterSun::FBridgeWaterSun() = default;
FBridgeWaterSun::~FBridgeWaterSun() = default;

void FBridgeWaterSun::CommitSection(const FIntVector& Section, const FVector3f& RegionOffset,
    TConstArrayView<FVector3f> Positions)
{
    if (Positions.IsEmpty())
    {
        RemoveSection(Section);
        return;
    }
    FWaterCasterPositions& Casters = SectionCasters.FindOrAdd(Section);
    if (Casters.Num() == Positions.Num())
    {
        bool bEqual = true;
        for (int32 I = 0; I < Positions.Num(); ++I)
        {
            if (Casters[I] != (Positions[I] - RegionOffset) * 0.01f)
            {
                bEqual = false;
                break;
            }
        }
        if (bEqual) return;
    }
    Casters.SetNumUninitialized(Positions.Num());
    for (int32 I = 0; I < Positions.Num(); ++I)
    {
        Casters[I] = (Positions[I] - RegionOffset) * 0.01f;
    }
    ++Revision;
}

void FBridgeWaterSun::RemoveSection(const FIntVector& Section)
{
    if (SectionCasters.Remove(Section))
    {
        ++Revision;
    }
}

void FBridgeWaterSun::Reset()
{
    if (BoundLight.IsValid())
    {
        SetLightFunction(*BoundLight.Get(), false);
    }
    CombinedCasters.Reset();
    SectionCasters.Reset();
    ++Revision;
}

void FBridgeWaterSun::SetLightFunction(UDirectionalLightComponent& Light, bool bEnabled)
{
    UMaterialInstanceDynamic* Material = bEnabled ? LightFunction.Get() : nullptr;
    if (Light.LightFunctionMaterial != Material)
    {
        Light.SetLightFunctionMaterial(Material);
        Light.SetLightFunctionFadeDistance(8200.0f);
        Light.SetLightFunctionDisabledBrightness(1.0f);
    }
    BoundLight = bEnabled ? &Light : nullptr;
}

FBridgeWaterSunFrame FBridgeWaterSun::Update(const FBridgeWaterSunInputs& Input, bool bVolume, bool bCaustics)
{
    FBridgeWaterSunFrame Frame;
    constexpr double SnapCm = 1600.0;
    if (!Input.Light || !Input.Grid || !Input.bNearWater || Input.Direction.Z < 0.035f || (!bVolume && !bCaustics))
    {
        if (BoundLight.IsValid()) SetLightFunction(*BoundLight.Get(), false);
        return Frame;
    }

    if (!AirShadow.IsValid())
    {
        AirShadow.Reset(MakeShadow(Input.Light));
        WaterShadow.Reset(MakeShadow(Input.Light));
    }
    if (bCaustics && !bTriedMaterial)
    {
        bTriedMaterial = true;
        auto* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Bridge/M_McSunCausticsLF.M_McSunCausticsLF"),
            nullptr, LOAD_NoWarn);
        if (Base) LightFunction.Reset(UMaterialInstanceDynamic::Create(Base, Input.Light));
    }

    const FIntVector Cell(FMath::FloorToInt(Input.Eye.X / SnapCm), FMath::FloorToInt(Input.Eye.Y / SnapCm),
        FMath::FloorToInt(Input.Eye.Z / SnapCm));
    const FVector Center = FVector(Cell) * SnapCm + FVector(SnapCm * 0.5);
    const bool bWide = Input.Tier >= 2;
    Frame.RadiusM = bWide ? 80.0f : 56.0f;
    if (!CombinedCasters || CachedCenterCell != Cell || CachedGridOrigin != Input.GridOrigin || CombinedRevision != Revision)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftWaterP2CasterGather);
        struct FCandidate { const FIntVector* Section; const FWaterCasterPositions* Data; double Distance; };
        TArray<FCandidate> Candidates;
        const double Limit = 100.0 * (84.0 + 14.0);
        for (const auto& Pair : SectionCasters)
        {
            const FVector Delta = SectionOrigin(Pair.Key) + FVector(800, -800, 800) - Center;
            if (FMath::Abs(Delta.X) < Limit && FMath::Abs(Delta.Y) < Limit && FMath::Abs(Delta.Z) < Limit)
            {
                Candidates.Add({&Pair.Key, &Pair.Value, Delta.SquaredLength()});
            }
        }
        Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });
        auto Combined = MakeShared<FWaterCasterPositions, ESPMode::ThreadSafe>();
        const int32 LimitVertices = MaxCasterQuads * 4;
        for (const FCandidate& Candidate : Candidates)
        {
            const FVector3f Offset((SectionOrigin(*Candidate.Section) - Input.GridOrigin) * 0.01);
            const int32 Count = FMath::Min(Candidate.Data->Num(), LimitVertices - Combined->Num());
            for (int32 I = 0; I < Count; ++I)
            {
                Combined->Add((*Candidate.Data)[I] + Offset);
            }
            if (Combined->Num() == LimitVertices) break;
        }
        CombinedCasters = MoveTemp(Combined);
        CombinedRevision = Revision;
        CachedCenterCell = Cell;
        CachedGridOrigin = Input.GridOrigin;
    }
    if (CombinedCasters->IsEmpty())
    {
        if (BoundLight.IsValid()) SetLightFunction(*BoundLight.Get(), false);
        return Frame;
    }
    Frame.Casters = CombinedCasters;
    Frame.Revision = CombinedRevision;
    Frame.Grid = Input.Grid;
    Frame.GridRevision = Input.GridRevision;
    Frame.Pattern = Input.CausticPattern;
    Frame.AirShadow = AirShadow->GetResource();
    Frame.WaterShadow = WaterShadow->GetResource();
    Frame.Origin = Input.GridOrigin;
    Frame.PatternPhaseM = FVector2f(float(FMath::Fmod(Frame.Origin.X, 900.0) * 0.01),
        float(FMath::Fmod(Frame.Origin.Y, 900.0) * 0.01));
    Frame.CenterM = FVector3f((Center - Frame.Origin) * 0.01);
    Frame.SunAir = Input.Direction.GetSafeNormal();
    const FVector2f Horizontal = FVector2f(Frame.SunAir.X, Frame.SunAir.Y) / WaterIOR;
    Frame.SunWater = FVector3f(Horizontal.X, Horizontal.Y, FMath::Sqrt(FMath::Max(0.0f, 1.0f - Horizontal.SquaredLength())));
    Frame.Radiance = Input.Radiance;
    Frame.Steps = bVolume ? (Input.Tier >= 3 ? 14 : bWide ? 10 : 6) : 0;
    Frame.bCaustics = bCaustics && LightFunction.IsValid() && Input.CausticTexture;

    if (Frame.bCaustics)
    {
        auto& Mat = *LightFunction.Get();
        const auto Vector = [](const FVector3f& V) { return FLinearColor(V.X, V.Y, V.Z); };
        Mat.SetVectorParameterValue(TEXT("P2GridOriginCm"), Vector(FVector3f(Frame.Origin)));
        Mat.SetVectorParameterValue(TEXT("P2ShadowCenterM"), Vector(Frame.CenterM));
        Mat.SetVectorParameterValue(TEXT("P2SunAir"), Vector(Frame.SunAir));
        Mat.SetVectorParameterValue(TEXT("P2SunWater"), Vector(Frame.SunWater));
        Mat.SetVectorParameterValue(TEXT("P2PatternPhaseM"), FLinearColor(Frame.PatternPhaseM.X, Frame.PatternPhaseM.Y, 0.0f));
        Mat.SetScalarParameterValue(TEXT("P2ShadowRadiusM"), Frame.RadiusM);
        Mat.SetScalarParameterValue(TEXT("P2CausticStrength"), 0.34f);
        Mat.SetTextureParameterValue(TEXT("P2WaterGrid"), Input.GridTexture);
        Mat.SetTextureParameterValue(TEXT("P2AirShadow"), AirShadow.Get());
        Mat.SetTextureParameterValue(TEXT("P2WaterShadow"), WaterShadow.Get());
        Mat.SetTextureParameterValue(TEXT("CausticsTex"), Input.CausticTexture);
    }
    SetLightFunction(*Input.Light, Frame.bCaustics);
    return Frame;
}

FBridgeWaterSunTextures FBridgeWaterSunRenderState::Prepare(FRDGBuilder& Graph, const FSceneViewFamily& Family,
    const FBridgeWaterSunFrame& Frame)
{
    if (!Frame.IsActive() || Family.GetFeatureLevel() < ERHIFeatureLevel::SM5)
    {
        return {};
    }
    FBridgeWaterSunTextures Textures;
    Textures.Grid = RegisterTexture(Graph, Frame.Grid, TEXT("Minecraft.WaterP2.Grid"));
    Textures.Pattern = RegisterTexture(Graph, Frame.Pattern, TEXT("Minecraft.WaterP2.Pattern"));
    Textures.AirShadow = RegisterTexture(Graph, Frame.AirShadow, TEXT("Minecraft.WaterP2.AirShadow"));
    Textures.WaterShadow = RegisterTexture(Graph, Frame.WaterShadow, TEXT("Minecraft.WaterP2.WaterShadow"));
    if (!Textures.Grid || !Textures.AirShadow || !Textures.WaterShadow || !Frame.Casters)
    {
        return {};
    }
    const bool bNewGeometry = !CasterBuffer || GeometryRevision != Frame.Revision || CachedOrigin != Frame.Origin ||
        CachedCenter != Frame.CenterM;
    if (bNewGeometry || GridRevision != Frame.GridRevision || !Frame.SunAir.Equals(CachedSun, 0.0035f) || CachedRadius != Frame.RadiusM)
    {
        RDG_EVENT_SCOPE_STAT(Graph, McWaterP2Shadows, "MinecraftWaterP2Shadows");
        FRDGBufferRef Buffer;
        if (bNewGeometry)
        {
            Buffer = CreateStructuredBuffer(Graph, TEXT("Minecraft.WaterP2.Casters"), *Frame.Casters);
            Graph.QueueBufferExtraction(Buffer, &CasterBuffer);
        }
        else
        {
            Buffer = Graph.RegisterExternalBuffer(CasterBuffer);
        }
        FRDGBufferSRVRef Casters = Graph.CreateSRV(Buffer);
        const FGlobalShaderMap* Shaders = GetGlobalShaderMap(Family.GetShaderPlatform());
        DrawShadow(Graph, Shaders, Frame, Casters, Textures.Grid, Textures.AirShadow, false);
        DrawShadow(Graph, Shaders, Frame, Casters, Textures.Grid, Textures.WaterShadow, true);
        FRDGViewableResource* Outputs[] = {Textures.AirShadow, Textures.WaterShadow};
        Graph.UseExternalAccessMode(Outputs, ERHIAccess::SRVMask);
        GeometryRevision = Frame.Revision;
        GridRevision = Frame.GridRevision;
        CachedOrigin = Frame.Origin;
        CachedSun = Frame.SunAir;
        CachedCenter = Frame.CenterM;
        CachedRadius = Frame.RadiusM;
    }
    return Textures;
}
