#include "BridgeWaterSim.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "PixelFormat.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

DECLARE_GPU_STAT_NAMED(McWaterFFT, TEXT("Minecraft Water FFT"));
DECLARE_GPU_STAT_NAMED(McWaterCaustics, TEXT("Minecraft Water Caustics"));
DECLARE_GPU_STAT_NAMED(McWaterRipples, TEXT("Minecraft Shallow Water Ripples"));

namespace
{
    constexpr int32 WaveSize = 128;
    constexpr int32 Cascades = 3;
    constexpr int32 CausticSize = 256;
    constexpr int32 TileSize = 8;
    constexpr ETextureCreateFlags OutputFlags = TexCreate_UAV | TexCreate_ShaderResource;
    static_assert(FMath::IsPowerOfTwo(WaveSize) && FMath::IsPowerOfTwo(CausticSize));

    TAutoConsoleVariable<float> CVarWaterLabPbrNormalWeight(TEXT("mcue.Water.LabPbrNormalWeight"), 0.0f,
        TEXT("Blend weight [0,1] for high-resolution LabPBR water normals."));

    class FWaterTextureResource final : public FTextureResource
    {
    public:
        FWaterTextureResource(const UBridgeWaterTexture& Owner, int32 InSize, int32 Slices, EPixelFormat Format, int32 Mips)
            : Size(InSize)
            , Desc((Slices ? FRHITextureCreateDesc::Create2DArray(TEXT("MinecraftWater.MaterialTexture"), Size, Size, Slices, Format)
                           : FRHITextureCreateDesc::Create2D(TEXT("MinecraftWater.RippleSurface"), Size, Size, Format))
                .SetNumMips(Mips)
                .SetFlags(OutputFlags)
                .SetInitialState(ERHIAccess::SRVMask))
        {
            SetOwnerName(Owner.GetFName());
        }

        void InitRHI(FRHICommandListBase& RHICmdList) override
        {
            TextureRHI = RHICmdList.CreateTexture(Desc);
            SamplerStateRHI = Desc.IsTextureArray()
                ? TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI()
                : TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            DeferredPassSamplerStateRHI = SamplerStateRHI;
            RHICmdList.UpdateTextureReference(TextureReferenceRHI, TextureRHI);
        }

        void ReleaseRHI() override
        {
            RHIClearTextureReference(TextureReferenceRHI);
            FTextureResource::ReleaseRHI();
        }

        uint32 GetSizeX() const override { return Size; }
        uint32 GetSizeY() const override { return Size; }

    private:
        int32 Size;
        FRHITextureCreateDesc Desc;
    };

    class FWaterShader : public FGlobalShader
    {
    public:
        FWaterShader() = default;
        FWaterShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }

        static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("WATER_FFT_SIZE"), WaveSize);
            Environment.SetDefine(TEXT("WATER_CAUSTIC_SIZE"), CausticSize);
            Environment.SetDefine(TEXT("WATER_TILE_SIZE"), TileSize);
        }
    };

    class FWaterSpectrumCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterSpectrumCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterSpectrumCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER(float, SimulationTime)
            SHADER_PARAMETER(float, WindStrength)
            SHADER_PARAMETER(FVector2f, WindDirection)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutSpectrum)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FWaterStockhamCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterStockhamCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterStockhamCS, FWaterShader);

        class FVertical : SHADER_PERMUTATION_BOOL("WATER_FFT_VERTICAL");
        using FPermutationDomain = TShaderPermutationDomain<FVertical>;

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, FFTInput)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, FFTOutput)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FWaterResolveCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterResolveCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterResolveCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER(float, SimulationTime)
            SHADER_PARAMETER(float, WindStrength)
            SHADER_PARAMETER(float, FoamDecay)
            SHADER_PARAMETER(float, Rain)
            SHADER_PARAMETER(uint32, ResetHistory)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, FFTInput)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, FoamHistory)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutDisplacement)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutDerivatives)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutMoments)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FWaterMipCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterMipCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterMipCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER(FIntPoint, MipExtent)
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InDisplacement)
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InDerivatives)
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InMoments)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutDisplacement)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutDerivatives)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutMoments)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FWaterCausticsCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterCausticsCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterCausticsCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER(FVector3f, SunDirection)
            SHADER_PARAMETER(float, Rain)
            SHADER_PARAMETER_SAMPLER(SamplerState, WaveSampler)
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InDerivatives)
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InMoments)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutCaustics)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip1)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip2)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip3)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FWaterCausticMipCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FWaterCausticMipCS);
        SHADER_USE_PARAMETER_STRUCT(FWaterCausticMipCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, InCaustics)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip4)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip5)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip6)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip7)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, CausticMip8)
        END_SHADER_PARAMETER_STRUCT()
    };

    BEGIN_SHADER_PARAMETER_STRUCT(FRippleGridParameters, )
        SHADER_PARAMETER(FIntPoint, OriginInGridUnits)
        SHADER_PARAMETER(int32, TexelUnits)
        SHADER_PARAMETER(int32, RippleSize)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RippleWaterGrid)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RippleWaterField)
    END_SHADER_PARAMETER_STRUCT()

    class FRippleForcesCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FRippleForcesCS);
        SHADER_USE_PARAMETER_STRUCT(FRippleForcesCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_INCLUDE(FRippleGridParameters, Grid)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileOffsets)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileImpulseIndices)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRippleImpulse>, RippleImpulses)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutForcing)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FRippleStepCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FRippleStepCS);
        SHADER_USE_PARAMETER_STRUCT(FRippleStepCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_INCLUDE(FRippleGridParameters, Grid)
            SHADER_PARAMETER(FIntPoint, GridShift)
            SHADER_PARAMETER(float, RippleStep)
            SHADER_PARAMETER(uint32, ResetRipple)
            SHADER_PARAMETER(uint32, ApplyForcing)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, PreviousRipple)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, RippleForcing)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutRipple)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FRippleResolveCS : public FWaterShader
    {
        DECLARE_GLOBAL_SHADER(FRippleResolveCS);
        SHADER_USE_PARAMETER_STRUCT(FRippleResolveCS, FWaterShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_STRUCT_INCLUDE(FRippleGridParameters, Grid)
            SHADER_PARAMETER(FIntPoint, GridShift)
            SHADER_PARAMETER(uint32, ResetRipple)
            SHADER_PARAMETER(float, RippleFrameSeconds)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CurrentRipple)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, RippleForcing)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PreviousSurface)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSurface)
        END_SHADER_PARAMETER_STRUCT()
    };

    IMPLEMENT_GLOBAL_SHADER(FWaterSpectrumCS, "/MinecraftUEViewer/Private/McWaterFFT.usf", "SpectrumCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FWaterStockhamCS, "/MinecraftUEViewer/Private/McWaterFFT.usf", "StockhamCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FWaterResolveCS, "/MinecraftUEViewer/Private/McWaterFFT.usf", "ResolveWavesCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FWaterMipCS, "/MinecraftUEViewer/Private/McWaterFFT.usf", "WaveMipCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FWaterCausticsCS, "/MinecraftUEViewer/Private/McWaterCaustics.usf", "CausticsCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FWaterCausticMipCS, "/MinecraftUEViewer/Private/McWaterCaustics.usf", "CausticMipCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FRippleForcesCS, "/MinecraftUEViewer/Private/McShallowWaterRipple.usf", "RippleForcesCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FRippleStepCS, "/MinecraftUEViewer/Private/McShallowWaterRipple.usf", "RippleStepCS", SF_Compute);
    IMPLEMENT_GLOBAL_SHADER(FRippleResolveCS, "/MinecraftUEViewer/Private/McShallowWaterRipple.usf", "RippleResolveCS", SF_Compute);
    static_assert(FBridgeRippleDomain::TileSize == TileSize && sizeof(FBridgeRippleImpulse) == 32);

    struct FWaterFrame
    {
        float Time = 0.0f;
        float WindStrength = 0.0f;
        FVector2f WindDirection{1.0f, 0.0f};
        float Rain = 0.0f;
        float DeltaSeconds = 0.0f;
        FVector3f SunDirection{0.0f, 0.0f, 1.0f};
        bool bSunlit = false;
        bool bActive = false;
    };

    FRDGTextureSRVRef ReadMip(FRDGBuilder& Graph, FRDGTextureRef Texture, int32 Mip)
    {
        return Graph.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(Texture, Mip));
    }

    FRDGTextureUAVRef WriteMip(FRDGBuilder& Graph, FRDGTextureRef Texture, int32 Mip)
    {
        return Graph.CreateUAV(FRDGTextureUAVDesc(Texture, Mip));
    }

    FIntVector TileGroups(int32 Size, int32 Slices)
    {
        return FIntVector(FMath::DivideAndRoundUp(Size, TileSize), FMath::DivideAndRoundUp(Size, TileSize), Slices);
    }

    FRDGTextureRef StockhamTransform(FRDGBuilder& Graph, FGlobalShaderMap* Shaders, FRDGTextureRef Input,
        bool bVertical)
    {
        FRDGTextureRef Output = Graph.CreateTexture(Input->Desc,
            bVertical ? TEXT("MinecraftWater.Spatial") : TEXT("MinecraftWater.FFTRows"));
        auto* Parameters = Graph.AllocParameters<FWaterStockhamCS::FParameters>();
        Parameters->FFTInput = Input;
        Parameters->FFTOutput = WriteMip(Graph, Output, 0);
        FWaterStockhamCS::FPermutationDomain Permutation;
        Permutation.Set<FWaterStockhamCS::FVertical>(bVertical);
        FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterStockham %s", bVertical ? TEXT("Y") : TEXT("X")),
            TShaderMapRef<FWaterStockhamCS>(Shaders, Permutation), Parameters, FIntVector(WaveSize, Cascades, 1));
        return Output;
    }

    void BinRippleImpulses(const FBridgeRippleFrame& Frame, TArray<uint32>& Offsets, TArray<uint32>& Indices)
    {
        const int32 Tiles = Frame.Domain.Size / TileSize;
        const float InvTileMeters = 1.0f / (Frame.Domain.GetTexelMeters() * TileSize);
        Offsets.SetNumZeroed(Tiles * Tiles + 1);
        TArray<FIntRect, TInlineAllocator<128>> Bounds;
        Bounds.Reserve(Frame.Impulses.Num());
        for (const FBridgeRippleImpulse& Impulse : Frame.Impulses)
        {
            const FVector2f Pad(Impulse.Shape.X);
            const FVector2f Lo = (Impulse.Position - Pad) * InvTileMeters;
            const FVector2f Hi = (Impulse.Position + Pad) * InvTileMeters;
            const auto Limit = [Tiles](float X) { return FMath::Clamp(FMath::FloorToInt32(X), 0, Tiles - 1); };
            const FIntRect Rect(Limit(Lo.X), Limit(Lo.Y), Limit(Hi.X) + 1, Limit(Hi.Y) + 1);
            Bounds.Add(Rect);
            for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
                for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
                    ++Offsets[Y * Tiles + X + 1];
        }
        for (int32 I = 1; I < Offsets.Num(); ++I) Offsets[I] += Offsets[I - 1];
        Indices.SetNumUninitialized(Offsets.Last());
        TArray<uint32> Cursors = Offsets;
        for (int32 I = 0; I < Bounds.Num(); ++I)
        {
            const FIntRect& Rect = Bounds[I];
            for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
                for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
                    Indices[Cursors[Y * Tiles + X]++] = uint32(I);
        }
    }
}

class FBridgeWaterViewExtension final : public FWorldSceneViewExtension
{
public:
    FBridgeWaterViewExtension(const FAutoRegister& AutoRegister, UWorld* World)
        : FWorldSceneViewExtension(AutoRegister, World) {}

    void PreRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override
    {
        if (!Frame.bActive || !Displacement || Family.Views.IsEmpty() || LastFrame == Family.FrameNumber ||
            Family.GetFeatureLevel() < ERHIFeatureLevel::SM5)
        {
            return;
        }
        LastFrame = Family.FrameNumber;
        FGlobalShaderMap* Shaders = GetGlobalShaderMap(Family.GetShaderPlatform());
        FRDGTextureRef Disp = Graph.RegisterExternalTexture(Displacement);
        FRDGTextureRef Deriv = Graph.RegisterExternalTexture(Derivatives);
        FRDGTextureRef Moment = Graph.RegisterExternalTexture(SlopeMoments);
        FRDGTextureRef Focus = Graph.RegisterExternalTexture(Caustics);
        {
            RDG_EVENT_SCOPE_STAT(Graph, McWaterFFT, "MinecraftWaterFFT");
            const FRDGTextureDesc WorkDesc = FRDGTextureDesc::Create2DArray(FIntPoint(WaveSize), PF_A32B32G32R32F,
                FClearValueBinding::None, OutputFlags, Cascades);
            FRDGTextureRef Spectrum = Graph.CreateTexture(WorkDesc, TEXT("MinecraftWater.Spectrum"));
            auto* Build = Graph.AllocParameters<FWaterSpectrumCS::FParameters>();
            Build->SimulationTime = Frame.Time;
            Build->WindStrength = Frame.WindStrength;
            Build->WindDirection = Frame.WindDirection;
            Build->OutSpectrum = WriteMip(Graph, Spectrum, 0);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterSpectrum"), TShaderMapRef<FWaterSpectrumCS>(Shaders),
                Build, TileGroups(WaveSize, Cascades));
            FRDGTextureRef Rows = StockhamTransform(Graph, Shaders, Spectrum, false);
            FRDGTextureRef Spatial = StockhamTransform(Graph, Shaders, Rows, true);

            auto* Resolve = Graph.AllocParameters<FWaterResolveCS::FParameters>();
            Resolve->SimulationTime = Frame.Time;
            Resolve->WindStrength = Frame.WindStrength;
            Resolve->Rain = Frame.Rain;
            Resolve->FoamDecay = FMath::Exp(-1.1f * Frame.DeltaSeconds);
            Resolve->ResetHistory = !bHasHistory;
            Resolve->FFTInput = Spatial;
            Resolve->FoamHistory = WriteMip(Graph, Graph.RegisterExternalTexture(FoamHistory), 0);
            Resolve->OutDisplacement = WriteMip(Graph, Disp, 0);
            Resolve->OutDerivatives = WriteMip(Graph, Deriv, 0);
            Resolve->OutMoments = WriteMip(Graph, Moment, 0);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterDisplacementDerivativesFoam"),
                TShaderMapRef<FWaterResolveCS>(Shaders), Resolve, TileGroups(WaveSize, Cascades));

            for (int32 Mip = 1; Mip < Disp->Desc.NumMips; ++Mip)
            {
                const int32 Size = WaveSize >> Mip;
                auto* Downsample = Graph.AllocParameters<FWaterMipCS::FParameters>();
                Downsample->MipExtent = FIntPoint(Size);
                Downsample->InDisplacement = ReadMip(Graph, Disp, Mip - 1);
                Downsample->InDerivatives = ReadMip(Graph, Deriv, Mip - 1);
                Downsample->InMoments = ReadMip(Graph, Moment, Mip - 1);
                Downsample->OutDisplacement = WriteMip(Graph, Disp, Mip);
                Downsample->OutDerivatives = WriteMip(Graph, Deriv, Mip);
                Downsample->OutMoments = WriteMip(Graph, Moment, Mip);
                FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterWaveMip %d", Mip),
                    TShaderMapRef<FWaterMipCS>(Shaders), Downsample, TileGroups(Size, Cascades));
            }
            bHasHistory = true;
        }

        if (Frame.bSunlit)
        {
            RDG_EVENT_SCOPE_STAT(Graph, McWaterCaustics, "MinecraftWaterCaustics");
            auto* Parameters = Graph.AllocParameters<FWaterCausticsCS::FParameters>();
            Parameters->SunDirection = Frame.SunDirection;
            Parameters->Rain = Frame.Rain;
            Parameters->WaveSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI();
            Parameters->InDerivatives = ReadMip(Graph, Deriv, 0);
            Parameters->InMoments = ReadMip(Graph, Moment, 0);
            Parameters->OutCaustics = WriteMip(Graph, Focus, 0);
            Parameters->CausticMip1 = WriteMip(Graph, Focus, 1);
            Parameters->CausticMip2 = WriteMip(Graph, Focus, 2);
            Parameters->CausticMip3 = WriteMip(Graph, Focus, 3);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterCausticFocus 256x256"),
                TShaderMapRef<FWaterCausticsCS>(Shaders), Parameters, TileGroups(CausticSize, 1));
            auto* Downsample = Graph.AllocParameters<FWaterCausticMipCS::FParameters>();
            Downsample->InCaustics = ReadMip(Graph, Focus, 3);
            Downsample->CausticMip4 = WriteMip(Graph, Focus, 4);
            Downsample->CausticMip5 = WriteMip(Graph, Focus, 5);
            Downsample->CausticMip6 = WriteMip(Graph, Focus, 6);
            Downsample->CausticMip7 = WriteMip(Graph, Focus, 7);
            Downsample->CausticMip8 = WriteMip(Graph, Focus, 8);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterCausticMipTail"),
                TShaderMapRef<FWaterCausticMipCS>(Shaders), Downsample, FIntVector(1, 1, 1));
            bCausticsCleared = false;
        }
        else if (!bCausticsCleared)
        {
            for (int32 Mip = 0; Mip < Focus->Desc.NumMips; ++Mip)
            {
                AddClearUAVPass(Graph, WriteMip(Graph, Focus, Mip), 0.0f);
            }
            bCausticsCleared = true;
        }

        // Material texture references are bound through the mesh pass, outside RDG parameter tracking.
        FRDGViewableResource* MaterialTextures[] = {Disp, Deriv, Moment, Focus};
        Graph.UseExternalAccessMode(MaterialTextures, ERHIAccess::SRVMask);
        RenderRipples(Graph, Shaders);
    }

    void RenderRipples(FRDGBuilder& Graph, FGlobalShaderMap* Shaders)
    {
        if (!RippleFrame.bActive || !Surface || !RippleFrame.Grid->GetTextureRHI() || !RippleFrame.Field->GetTextureRHI())
        {
            bHasRippleHistory = false;
            return;
        }
        RDG_EVENT_SCOPE_STAT(Graph, McWaterRipples, "MinecraftShallowWaterRipples");
        const FBridgeRippleDomain& Domain = RippleFrame.Domain;
        const FIntPoint Shift = Domain.Origin - PreviousRippleOrigin;
        const bool bReset = !bHasRippleHistory || FMath::Abs(Shift.X) >= Domain.Size || FMath::Abs(Shift.Y) >= Domain.Size;
        const FIntVector Groups = TileGroups(Domain.Size, 1);
        FRippleGridParameters Grid;
        Grid.OriginInGridUnits = Domain.Origin * Domain.TexelUnits - RippleFrame.GridOrigin * FBridgeRippleDomain::UnitsPerBlock;
        Grid.TexelUnits = Domain.TexelUnits;
        Grid.RippleSize = Domain.Size;
        Grid.RippleWaterGrid = RegisterExternalTexture(Graph, RippleFrame.Grid->GetTextureRHI(), TEXT("MinecraftWater.ColumnGrid"));
        Grid.RippleWaterField = RegisterExternalTexture(Graph, RippleFrame.Field->GetTextureRHI(), TEXT("MinecraftWater.FieldGrid"));

        const FRDGTextureDesc ForceDesc = FRDGTextureDesc::Create2D(FIntPoint(Domain.Size), PF_G16R16F,
            FClearValueBinding::None, OutputFlags);
        FRDGTextureRef Forcing = Graph.CreateTexture(ForceDesc, TEXT("MinecraftWater.RippleForcing"));
        if (RippleFrame.Impulses.IsEmpty())
        {
            AddClearUAVPass(Graph, WriteMip(Graph, Forcing, 0), FVector4f(0.0f));
        }
        else
        {
            TArray<uint32> Offsets, Indices;
            BinRippleImpulses(RippleFrame, Offsets, Indices);
            auto* Forces = Graph.AllocParameters<FRippleForcesCS::FParameters>();
            Forces->Grid = Grid;
            Forces->TileOffsets = Graph.CreateSRV(CreateStructuredBuffer(Graph, TEXT("MinecraftWater.RippleOffsets"), MakeConstArrayView(Offsets)));
            Forces->TileImpulseIndices = Graph.CreateSRV(CreateStructuredBuffer(Graph, TEXT("MinecraftWater.RippleIndices"), MakeConstArrayView(Indices)));
            Forces->RippleImpulses = Graph.CreateSRV(CreateStructuredBuffer(Graph, TEXT("MinecraftWater.RippleImpulses"), MakeConstArrayView(RippleFrame.Impulses)));
            Forces->OutForcing = WriteMip(Graph, Forcing, 0);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterRippleForces %d", RippleFrame.Impulses.Num()),
                TShaderMapRef<FRippleForcesCS>(Shaders), Forces, Groups);
        }

        FRDGTextureRef States[] = {Graph.RegisterExternalTexture(RippleA), Graph.RegisterExternalTexture(RippleB)};
        // A 12.5 ms step at 4 m/s keeps both tiers below the two-dimensional CFL limit.
        // Even pairs leave the result in A.
        const float Seconds = FMath::Clamp(RippleFrame.DeltaSeconds, 0.0f, 0.050f);
        const int32 Steps = 2 * FMath::Clamp(FMath::CeilToInt32(40.0f * Seconds), 1, 2);
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            auto* Params = Graph.AllocParameters<FRippleStepCS::FParameters>();
            Params->Grid = Grid;
            Params->GridShift = Step == 0 ? Shift : FIntPoint::ZeroValue;
            Params->ResetRipple = Step == 0 && bReset;
            Params->ApplyForcing = Step == 0 && !RippleFrame.Impulses.IsEmpty();
            Params->RippleStep = Seconds / Steps;
            Params->PreviousRipple = States[Step & 1];
            Params->RippleForcing = Forcing;
            Params->OutRipple = WriteMip(Graph, States[1 - (Step & 1)], 0);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterRippleStep %d", Step),
                TShaderMapRef<FRippleStepCS>(Shaders), Params, Groups);
        }

        FRDGTextureRef MaterialSurface = Graph.RegisterExternalTexture(Surface);
        FRDGTextureRef Resolved = Graph.CreateTexture(MaterialSurface->Desc, TEXT("MinecraftWater.RippleResolve"));
        auto* Resolve = Graph.AllocParameters<FRippleResolveCS::FParameters>();
        Resolve->Grid = Grid;
        Resolve->GridShift = Shift;
        Resolve->ResetRipple = bReset;
        Resolve->RippleFrameSeconds = Seconds;
        Resolve->CurrentRipple = States[0];
        Resolve->RippleForcing = Forcing;
        Resolve->PreviousSurface = MaterialSurface;
        Resolve->OutSurface = WriteMip(Graph, Resolved, 0);
        FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("WaterRippleResolve"),
            TShaderMapRef<FRippleResolveCS>(Shaders), Resolve, Groups);
        AddCopyTexturePass(Graph, Resolved, MaterialSurface);
        Graph.UseExternalAccessMode(MaterialSurface, ERHIAccess::SRVMask);
        PreviousRippleOrigin = Domain.Origin;
        bHasRippleHistory = true;
        RippleFrame.Impulses.Reset();
    }

    FWaterFrame Frame;
    FBridgeRippleFrame RippleFrame;
    TRefCountPtr<IPooledRenderTarget> Displacement, Derivatives, SlopeMoments, Caustics, FoamHistory;
    TRefCountPtr<IPooledRenderTarget> RippleA, RippleB, Surface;
    FIntPoint PreviousRippleOrigin = FIntPoint::ZeroValue;
    uint32 LastFrame = MAX_uint32;
    bool bHasHistory = false;
    bool bCausticsCleared = false;
    bool bHasRippleHistory = false;
};

void UBridgeWaterTexture::Init(int32 InSize, int32 InSlices, EPixelFormat InFormat, int32 InMips)
{
    Size = InSize;
    Slices = InSlices;
    Mips = InMips ? InMips : FMath::FloorLog2(Size) + 1;
    Format = InFormat;
    SRGB = false;
    NeverStream = true;
    UpdateResource();
}

FTextureResource* UBridgeWaterTexture::CreateResource()
{
    return Size > 0 ? new FWaterTextureResource(*this, Size, Slices, Format, Mips) : nullptr;
}

ETextureClass UBridgeWaterTexture::GetTextureClass() const { return Slices ? ETextureClass::Array : ETextureClass::TwoD; }
EMaterialValueType UBridgeWaterTexture::GetMaterialType() const { return Slices ? MCT_Texture2DArray : MCT_Texture2D; }

FBridgeRippleDomain FBridgeRippleDomain::Around(const FVector& EyeMC, int32 Tier)
{
    FBridgeRippleDomain Domain;
    Domain.Size = Tier >= 3 ? 512 : 256;
    Domain.TexelUnits = Tier >= 3 ? 3 : 4;
    const double InvTexel = double(UnitsPerBlock) / Domain.TexelUnits;
    Domain.Origin = FIntPoint(FMath::FloorToInt32(EyeMC.X * InvTexel), FMath::FloorToInt32(EyeMC.Z * InvTexel))
        - FIntPoint(Domain.Size / 2);
    return Domain;
}

FBridgeWaterSim::FBridgeWaterSim(UWorld* World)
{
    if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5 || !FApp::CanEverRender())
    {
        return;
    }
    Extension = FSceneViewExtensions::NewExtension<FBridgeWaterViewExtension>(World);
    const auto NewTexture = [](TStrongObjectPtr<UBridgeWaterTexture>& Texture, int32 Size, int32 Layers, EPixelFormat Format)
    {
        Texture.Reset(NewObject<UBridgeWaterTexture>(GetTransientPackage(), NAME_None, RF_Transient));
        Texture->Init(Size, Layers, Format);
        return Texture->GetResource();
    };
    FTextureResource* Disp = NewTexture(Displacement, WaveSize, Cascades, PF_FloatRGBA);
    FTextureResource* Deriv = NewTexture(Derivatives, WaveSize, Cascades, PF_FloatRGBA);
    FTextureResource* Moment = NewTexture(SlopeMoments, WaveSize, Cascades, PF_FloatRGBA);
    FTextureResource* Focus = NewTexture(Caustics, CausticSize, 1, PF_R16F);
    ENQUEUE_RENDER_COMMAND(InitMinecraftWater)([Ext = Extension, Disp, Deriv, Moment, Focus](FRHICommandListImmediate& RHICmdList)
    {
        Ext->Displacement = CreateRenderTarget(Disp->GetTextureRHI(), TEXT("MinecraftWater.DisplacementFoam"));
        Ext->Derivatives = CreateRenderTarget(Deriv->GetTextureRHI(), TEXT("MinecraftWater.Derivatives"));
        Ext->SlopeMoments = CreateRenderTarget(Moment->GetTextureRHI(), TEXT("MinecraftWater.SlopeMoments"));
        Ext->Caustics = CreateRenderTarget(Focus->GetTextureRHI(), TEXT("MinecraftWater.Caustics"));
        const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2DArray(TEXT("MinecraftWater.FoamHistory"),
            WaveSize, WaveSize, Cascades, PF_R32_FLOAT).SetFlags(OutputFlags).SetInitialState(ERHIAccess::UAVCompute);
        FTextureRHIRef History = RHICmdList.CreateTexture(Desc);
        Ext->FoamHistory = CreateRenderTarget(History, TEXT("MinecraftWater.FoamHistory"));
    });
}

void FBridgeWaterSim::Bind(UMaterialInstanceDynamic& Material) const
{
    if (Extension)
    {
        Material.SetTextureParameterValue(TEXT("DisplacementFoamTex"), Displacement.Get());
        Material.SetTextureParameterValue(TEXT("DerivativesTex"), Derivatives.Get());
        Material.SetTextureParameterValue(TEXT("SlopeMomentsTex"), SlopeMoments.Get());
        Material.SetTextureParameterValue(TEXT("CausticsTex"), Caustics.Get());
        Material.SetScalarParameterValue(TEXT("WaterSimReady"), bStarted ? 1.0f : 0.0f);
        if (RippleSurface) Material.SetTextureParameterValue(TEXT("WaterRipple"), RippleSurface.Get());
    }
}

void FBridgeWaterSim::Update(UMaterialInstanceDynamic& Material, float EnvClock, float DeltaSeconds,
    const FVector2f& WindVector, float WindStrength, float Rain, const FVector3f& SunDirection, float SunPower)
{
    if (!Extension)
    {
        return;
    }
    // Use continuous environment-clock deltas. Changes of world time do not teleport waves or foam.
    const float ClockDelta = EnvClock - LastEnvClock;
    const float Step = bStarted && ClockDelta > 0.0f && ClockDelta <= 0.125f ? ClockDelta : DeltaSeconds;
    LastEnvClock = EnvClock;
    SimulationTime += Step;
    bStarted = true;
    FWaterFrame Frame;
    Frame.Time = float(SimulationTime);
    Frame.DeltaSeconds = Step;
    if (!WindVector.IsNearlyZero())
    {
        Frame.WindDirection = WindVector.GetSafeNormal();
    }
    Frame.WindStrength = FMath::Clamp(WindStrength, 0.0f, 1.0f);
    Frame.Rain = FMath::Clamp(Rain, 0.0f, 1.0f);
    Frame.SunDirection = SunDirection;
    Frame.bSunlit = SunPower > 0.0f;
    Frame.bActive = true;
    Material.SetScalarParameterValue(TEXT("WaterSimReady"), 1.0f);
    Material.SetScalarParameterValue(TEXT("SunCaustics"), SunPower);
    Material.SetVectorParameterValue(TEXT("WaterSunDirection"),
        FLinearColor(SunDirection.X, SunDirection.Y, SunDirection.Z));
    Material.SetScalarParameterValue(TEXT("WaterLabPbrNormalWeight"),
        FMath::Clamp(CVarWaterLabPbrNormalWeight.GetValueOnGameThread(), 0.0f, 1.0f));
    ENQUEUE_RENDER_COMMAND(UpdateMinecraftWater)([Ext = Extension, Frame](FRHICommandListImmediate&)
    {
        Ext->Frame = Frame;
    });
}

void FBridgeWaterSim::UpdateRipples(UMaterialInstanceDynamic& Material, FBridgeRippleFrame&& Frame)
{
    if (!Extension) return;
    Frame.bActive = Frame.bActive && Frame.Grid && Frame.Field && GPixelFormats[PF_G16R16F].Supported;
    if (Frame.bActive && (!RippleSurface || RippleSurface->GetSurfaceWidth() != Frame.Domain.Size))
    {
        const int32 Size = Frame.Domain.Size;
        RippleSurface.Reset(NewObject<UBridgeWaterTexture>(GetTransientPackage(), NAME_None, RF_Transient));
        RippleSurface->Init(Size, 0, PF_FloatRGBA, 1);
        Material.SetTextureParameterValue(TEXT("WaterRipple"), RippleSurface.Get());
        FTextureResource* Resource = RippleSurface->GetResource();
        ENQUEUE_RENDER_COMMAND(InitMinecraftRipples)([Ext = Extension, Resource, Size](FRHICommandListImmediate& RHICmdList)
        {
            const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("MinecraftWater.RippleState"),
                Size, Size, PF_G16R16F).SetFlags(OutputFlags).SetInitialState(ERHIAccess::SRVMask);
            Ext->RippleA = CreateRenderTarget(RHICmdList.CreateTexture(Desc), TEXT("MinecraftWater.RippleA"));
            Ext->RippleB = CreateRenderTarget(RHICmdList.CreateTexture(Desc), TEXT("MinecraftWater.RippleB"));
            Ext->Surface = CreateRenderTarget(Resource->GetTextureRHI(), TEXT("MinecraftWater.RippleSurface"));
            Ext->bHasRippleHistory = false;
        });
    }
    const FBridgeRippleDomain& Domain = Frame.Domain;
    const FVector2d Origin = Domain.GetOriginMeters() - FVector2d(Frame.GridOrigin.X, Frame.GridOrigin.Y);
    Material.SetVectorParameterValue(TEXT("WaterRippleInfo"), Frame.bActive
        ? FLinearColor(float(Origin.X), float(Origin.Y), Domain.GetWidthMeters(), Domain.GetTexelMeters())
        : FLinearColor(0, 0, 0, 0));
    ENQUEUE_RENDER_COMMAND(UpdateMinecraftRipples)([Ext = Extension, Frame = MoveTemp(Frame)](FRHICommandListImmediate&) mutable
    {
        Ext->RippleFrame = MoveTemp(Frame);
    });
}

void FBridgeWaterSim::Reset()
{
    SimulationTime = 0.0;
    LastEnvClock = 0.0f;
    bStarted = false;
    if (Extension)
    {
        ENQUEUE_RENDER_COMMAND(ResetMinecraftWater)([Ext = Extension](FRHICommandListImmediate&)
        {
            Ext->Frame = {};
            Ext->RippleFrame = {};
            Ext->bHasHistory = false;
            Ext->bHasRippleHistory = false;
        });
    }
}
