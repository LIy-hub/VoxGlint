#include "BridgeViewer.h"
#include "BridgeFrameGeneration.h"

#if PLATFORM_WINDOWS
#include "BatchedElements.h"
#include "BridgeQuality.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "GlobalShader.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PipelineStateCache.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RHIStaticStates.h"
#include "ShaderParameterStruct.h"
#include "SimpleElementShaders.h"
#include "Windows/WindowsHWrapper.h"

#include <atomic>

namespace
{
    constexpr int32 MailboxBytes = 208;
    constexpr int32 RendererPidOffset = 40;
    constexpr int32 TextureHandleOffset = 160;
    constexpr int32 FenceHandleOffset = 184;
    constexpr int32 StatusOffset = 0;
    constexpr int32 AbandonedOffset = 4;
    constexpr int32 TargetFpsOffset = 8;
    constexpr int32 DrawnOffset = 16;
    constexpr int32 PublishedOffset = 24;
    constexpr int32 DroppedOffset = 32;
    constexpr int32 SlotsOffset = 64;
    constexpr int32 SlotBytes = 32;
    constexpr int32 ReleasedOffset = 8;
    constexpr int32 OpaqueOffset = 16;

    class FHudCompositePS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FHudCompositePS);
        SHADER_USE_PARAMETER_STRUCT(FHudCompositePS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_TEXTURE(Texture2D, HudTexture)
            SHADER_PARAMETER_SAMPLER(SamplerState, HudSampler)
            SHADER_PARAMETER(uint32, HudOpaque)
            SHADER_PARAMETER(float, OutputGamma)
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }
    };

    IMPLEMENT_GLOBAL_SHADER(FHudCompositePS, "/MinecraftUEViewer/HudComposite.usf", "Main", SF_Pixel);

    class FHudCompositeParameters : public FBatchedElementParameters
    {
    public:
        FHudCompositeParameters(bool bInOpaque, bool bInWriteCoverage)
            : bOpaque(bInOpaque), bWriteCoverage(bInWriteCoverage) {}

        void BindShaders(FRHICommandList& RHICmdList, FGraphicsPipelineStateInitializer& Pipeline, ERHIFeatureLevel::Type FeatureLevel,
            const FMatrix& Transform, float Gamma, const FMatrix&, const FTexture* Texture) override
        {
            TShaderMapRef<FSimpleElementVS> VertexShader(GetGlobalShaderMap(FeatureLevel));
            TShaderMapRef<FHudCompositePS> PixelShader(GetGlobalShaderMap(FeatureLevel));
            Pipeline.BlendState = bOpaque ? TStaticBlendState<>::GetRHI() :
                bWriteCoverage ? TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI() :
                TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_One>::GetRHI();
            Pipeline.BoundShaderState.VertexDeclarationRHI = GSimpleElementVertexDeclaration.VertexDeclarationRHI;
            Pipeline.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
            Pipeline.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
            Pipeline.PrimitiveType = PT_TriangleList;
            RHICmdList.ApplyCachedRenderTargets(Pipeline);
            SetGraphicsPipelineState(RHICmdList, Pipeline, 0);
            SetShaderParametersLegacyVS(RHICmdList, VertexShader, Transform);
            FHudCompositePS::FParameters Parameters;
            Parameters.HudTexture = Texture->TextureRHI;
            Parameters.HudSampler = Texture->SamplerStateRHI;
            Parameters.HudOpaque = bOpaque ? 1 : 0;
            Parameters.OutputGamma = Gamma;
            SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), Parameters);
        }

    private:
        bool bOpaque;
        bool bWriteCoverage;
    };
}

struct ABridgeViewer::FGpuUi
{
    struct FSlot
    {
        FTexture Texture;
        TRefCountPtr<ID3D12Fence> Fence;
        HANDLE TextureHandle = nullptr;
        HANDLE FenceHandle = nullptr;
        uint64 Retiring = 0;
        uint64 Released = 0;

        ~FSlot()
        {
            if (FenceHandle) ::CloseHandle(FenceHandle);
            if (TextureHandle) ::CloseHandle(TextureHandle);
        }
    };

    ~FGpuUi()
    {
        if (Mailbox)
        {
            FPlatformMemory::UnmapNamedSharedMemoryRegion(Mailbox);
        }
    }

    template<typename T>
    std::atomic_ref<T> At(int32 Offset) const
    {
        return std::atomic_ref<T>(*reinterpret_cast<T*>(static_cast<uint8*>(Mailbox->GetAddress()) + Offset));
    }

    bool Create(const FString& Name, FIntPoint Dimensions, LUID ProducerAdapter, uint32 ProducerNodes)
    {
        if (FParse::Param(FCommandLine::Get(), TEXT("BridgeCpuHud")) || !IsRHID3D12() ||
            FRHIGPUMask::All() != FRHIGPUMask::GPU0() || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
        {
            return false;
        }
        ID3D12DynamicRHI* Rhi = GetID3D12DynamicRHI();
        ID3D12Device* Device = Rhi->RHIGetDevice(0);
        const LUID Adapter = Device->GetAdapterLuid();
        if (Adapter.LowPart != ProducerAdapter.LowPart || Adapter.HighPart != ProducerAdapter.HighPart ||
            (Rhi->RHIGetDeviceNodeMask(0) & ProducerNodes) == 0)
        {
            return false;
        }

        if (FParse::Value(FCommandLine::Get(), TEXT("BridgeHudMaxFps="), HudFpsOverride))
        {
            HudFpsOverride = FMath::Clamp(HudFpsOverride, 10, 250);
        }
        Size = Dimensions;
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Heap.CreationNodeMask = Heap.VisibleNodeMask = Rhi->RHIGetDeviceNodeMask(0);
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width = Size.X;
        Desc.Height = Size.Y;
        Desc.DepthOrArraySize = Desc.MipLevels = 1;
        Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.SampleDesc.Count = 1;
        Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        const FSamplerStateRHIRef Sampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();

        for (int32 Index = 0; Index < BridgeWire::UiSlots; ++Index)
        {
            FSlot& Slot = Slots[Index];
            TRefCountPtr<ID3D12Resource> Resource;
            const FString TextureName = FString::Printf(TEXT("%s-T%d"), *Name, Index);
            const FString FenceName = FString::Printf(TEXT("%s-F%d"), *Name, Index);
            const HRESULT TextureResult = Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &Desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(Resource.GetInitReference()));
            if (FAILED(TextureResult) ||
                FAILED(Device->CreateSharedHandle(Resource, nullptr, GENERIC_ALL, *TextureName, &Slot.TextureHandle)) ||
                FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(Slot.Fence.GetInitReference()))) ||
                FAILED(Device->CreateSharedHandle(Slot.Fence, nullptr, GENERIC_ALL, *FenceName, &Slot.FenceHandle)))
            {
                return false;
            }
            Slot.Texture.TextureRHI = Rhi->RHICreateTexture2DFromResource(PF_R8G8B8A8,
                TexCreate_ShaderResource | TexCreate_Shared, FClearValueBinding::None, Resource);
            Slot.Texture.SamplerStateRHI = Sampler;
            if (!Slot.Texture.TextureRHI)
            {
                return false;
            }
        }
        return true;
    }

    int32 MaxFps() const
    {
        return HudFpsOverride > 0 ? HudFpsOverride : BridgeQuality::HudFpsLimit();
    }

    int32 TargetFps(uint16 ViewerLimit) const
    {
        return ViewerLimit ? FMath::Min<int32>(ViewerLimit, MaxFps()) : MaxFps();
    }

    FPlatformMemory::FSharedMemoryRegion* Mailbox = nullptr;
    FSlot Slots[BridgeWire::UiSlots];
    FIntPoint Size = FIntPoint::ZeroValue;
    int32 HudFpsOverride = 0;
    bool bVulkan = false;
    int32 DisplayedSlot = INDEX_NONE;
    uint64 DisplayedSequence = 0;
    uint64 LastDrawnSequence = 0;
    uint64 Composited = 0;
    uint64 TracePublished = 0;
    uint64 TraceDropped = 0;
    uint64 TraceComposited = 0;
    uint32 TraceDrawn = 0;
    double TraceTime = 0;
};
#endif

bool ABridgeViewer::ApplyGpuUi(BridgeWire::FReader Reader, int32 Size)
{
    if (!bWindowReceived || !bSingleWindow || Size != 33)
    {
        return false;
    }
#if PLATFORM_WINDOWS
    const FGuid Id{Reader.U32(), Reader.U32(), Reader.U32(), Reader.U32()};
    const FIntPoint Dimensions{Reader.U16(), Reader.U16()};
    LUID Adapter{Reader.U32(), Reader.I32()};
    const uint32 Nodes = Reader.U32();
    const uint8 Backend = Reader.Byte();
    if (!Id.IsValid() || !BridgeWire::IsUiSize(Dimensions.X, Dimensions.Y) || Backend > 1)
    {
        return false;
    }

    ResetGpuUi();
    const FString Name = TEXT("MCUE-HUD-") + Id.ToString(EGuidFormats::DigitsWithHyphens).ToUpper();
    TSharedPtr<FGpuUi> Offer = MakeShared<FGpuUi>();
    Offer->Mailbox = FPlatformMemory::MapNamedSharedMemoryRegion(Id.ToString(EGuidFormats::DigitsWithHyphensInBraces), false,
        FPlatformMemory::ESharedMemoryAccess::Read | FPlatformMemory::ESharedMemoryAccess::Write, MailboxBytes);
    if (!Offer->Mailbox || Offer->At<int32>(AbandonedOffset).load(std::memory_order_acquire) != 0)
    {
        return true;
    }
    const bool bOffered = Offer->Create(Name, Dimensions, Adapter, Nodes);
    Offer->bVulkan = Backend == 1;
    if (bOffered && Offer->bVulkan)
    {
        Offer->At<uint32>(RendererPidOffset).store(::GetCurrentProcessId(), std::memory_order_relaxed);
        for (int32 Index = 0; Index < BridgeWire::UiSlots; ++Index)
        {
            Offer->At<uint64>(TextureHandleOffset + Index * sizeof(uint64)).store(
                reinterpret_cast<uint64>(Offer->Slots[Index].TextureHandle), std::memory_order_relaxed);
            Offer->At<uint64>(FenceHandleOffset + Index * sizeof(uint64)).store(
                reinterpret_cast<uint64>(Offer->Slots[Index].FenceHandle), std::memory_order_relaxed);
        }
    }
    Offer->At<int32>(TargetFpsOffset).store(Offer->TargetFps(FpsLimit),
        std::memory_order_relaxed);
    Offer->At<int32>(StatusOffset).store(bOffered ? 1 : -1, std::memory_order_release);
    if (bOffered)
    {
        UE_LOG(LogTemp, Display, TEXT("Minecraft UE HUD: offered %s/D3D12 shared textures %dx%d (max %d FPS)."),
            Offer->bVulkan ? TEXT("Vulkan") : TEXT("OpenGL"), Dimensions.X, Dimensions.Y, Offer->MaxFps());
        GpuUi = MoveTemp(Offer);
    }
#endif
    return true;
}

void ABridgeViewer::PollGpuUi()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftHudSharedTextures);
#if PLATFORM_WINDOWS
    if (!GpuUi)
    {
        return;
    }
    FGpuUi& Pool = *GpuUi;
    if (Pool.At<int32>(AbandonedOffset).load(std::memory_order_acquire) != 0)
    {
        ResetGpuUi();
        return;
    }
    Pool.At<int32>(TargetFpsOffset).store(Pool.TargetFps(FpsLimit),
        std::memory_order_release);

    // Only consider frames whose producer writes have finished. Never queue a wait for a future frame.
    uint64 Ready[BridgeWire::UiSlots]{};
    int32 Latest = Pool.DisplayedSlot;
    uint64 LatestSequence = Pool.DisplayedSequence;
    for (int32 Index = 0; Index < BridgeWire::UiSlots; ++Index)
    {
        auto& Slot = Pool.Slots[Index];
        const int32 Offset = SlotsOffset + Index * SlotBytes;
        const uint64 Sequence = Pool.At<uint64>(Offset).load(std::memory_order_acquire);
        const uint64 Completed = Slot.Fence->GetCompletedValue();
        if (Completed == MAX_uint64)
        {
            ResetGpuUi();
            return;
        }
        if (Slot.Retiring > Slot.Released && Completed >= 2 * Slot.Retiring)
        {
            Slot.Released = Slot.Retiring;
            Pool.At<uint64>(Offset + ReleasedOffset).store(Slot.Released, std::memory_order_release);
        }
        if (Sequence <= Slot.Retiring || Sequence > (MAX_uint64 / 2) || Completed < 2 * Sequence - 1)
        {
            continue;
        }
        Ready[Index] = Sequence;
        if (Sequence > LatestSequence)
        {
            Latest = Index;
            LatestSequence = Sequence;
        }
    }
    for (int32 Index = 0; Index < BridgeWire::UiSlots; ++Index)
    {
        if (Index == Latest || Ready[Index] == 0)
        {
            continue;
        }
        auto& Slot = Pool.Slots[Index];
        Slot.Retiring = Ready[Index];
        // This render command follows Canvas work from previous frames. The RHI
        // adds the fence signal after the draws have been submitted to graphics.
        ENQUEUE_RENDER_COMMAND(ReleaseMinecraftHudSlot)(
            [Owner = GpuUi, Index, Value = 2 * Slot.Retiring](FRHICommandListImmediate& RHICmdList)
            {
                FRHICommandListScopedPipeline Graphics(RHICmdList, ERHIPipeline::Graphics);
                RHICmdList.EnqueueLambda([Owner, Index, Value](FRHICommandListImmediate& Executing)
                {
                    GetID3D12DynamicRHI()->RHISignalManualFence(Executing, Owner->Slots[Index].Fence, Value);
                });
            });
    }
    Pool.DisplayedSlot = Latest;
    Pool.DisplayedSequence = LatestSequence;
    if (Latest != INDEX_NONE)
    {
        bUiOpaque = Pool.At<int32>(SlotsOffset + Latest * SlotBytes + OpaqueOffset).load(std::memory_order_relaxed) != 0;
    }
#endif
}

bool ABridgeViewer::HasGpuUi() const
{
#if PLATFORM_WINDOWS
    return GpuUi && GpuUi->DisplayedSlot != INDEX_NONE;
#else
    return false;
#endif
}

void ABridgeViewer::DrawGpuUi(UCanvas* Canvas)
{
#if PLATFORM_WINDOWS
    FGpuUi& Pool = *GpuUi;
    const FTexture& Texture = Pool.Slots[Pool.DisplayedSlot].Texture;
    FCanvasTileItem Overlay(FVector2D::ZeroVector, &Texture, FVector2D(Canvas->SizeX, Canvas->SizeY),
        FVector2D(0, 1), FVector2D(1, 0), FLinearColor::White);
    Overlay.BlendMode = bUiOpaque ? SE_BLEND_Opaque : SE_BLEND_Translucent;
    Overlay.BatchedElementParameters = new FHudCompositeParameters(bUiOpaque, BridgeFrameGeneration::WritesHudCoverage());
    Canvas->DrawItem(Overlay);
    if (Pool.LastDrawnSequence != Pool.DisplayedSequence)
    {
        Pool.LastDrawnSequence = Pool.DisplayedSequence;
        ++Pool.Composited;
    }
#endif
}

void ABridgeViewer::ResetGpuUi()
{
#if PLATFORM_WINDOWS
    if (GpuUi)
    {
        GpuUi->At<int32>(StatusOffset).store(-1, std::memory_order_release);
        // Canvas batches hold a pointer to FTexture until their render command runs.
        ENQUEUE_RENDER_COMMAND(RetireMinecraftHudPool)(
            [Retired = MoveTemp(GpuUi)](FRHICommandListImmediate&) {});
    }
#endif
}

void ABridgeViewer::TraceGpuUi(double Now)
{
#if PLATFORM_WINDOWS
    if (!GpuUi)
    {
        return;
    }
    FGpuUi& Pool = *GpuUi;
    const uint32 Drawn = Pool.At<uint32>(DrawnOffset).load(std::memory_order_acquire);
    const uint64 Published = Pool.At<uint64>(PublishedOffset).load(std::memory_order_acquire);
    const uint64 Dropped = Pool.At<uint64>(DroppedOffset).load(std::memory_order_acquire);
    if (Pool.TraceTime > 0 && Now > Pool.TraceTime)
    {
        const double Interval = Now - Pool.TraceTime;
        UE_LOG(LogTemp, Display, TEXT("HUD path=gpu backend=%s size=%dx%d target=%d mcDrawFps=%.1f publishFps=%.1f compositeFps=%.1f noSlotFps=%.1f"),
            Pool.bVulkan ? TEXT("Vulkan") : TEXT("OpenGL"), Pool.Size.X, Pool.Size.Y, Pool.At<int32>(TargetFpsOffset).load(std::memory_order_relaxed),
            (Drawn - Pool.TraceDrawn) / Interval, (Published - Pool.TracePublished) / Interval,
            (Pool.Composited - Pool.TraceComposited) / Interval, (Dropped - Pool.TraceDropped) / Interval);
    }
    Pool.TraceTime = Now;
    Pool.TraceDrawn = Drawn;
    Pool.TracePublished = Published;
    Pool.TraceComposited = Pool.Composited;
    Pool.TraceDropped = Dropped;
#endif
}
