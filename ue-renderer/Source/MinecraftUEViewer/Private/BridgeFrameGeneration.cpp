#include "BridgeFrameGeneration.h"

#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "Features/IModularFeatures.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

#if PLATFORM_WINDOWS
#include "Windows/IDXGISwapchainProvider.h"
#endif

#if MCUE_WITH_XESS
#include "XeFGBlueprintLibrary.h"
#include "XeFGRHI.h"
#endif

#if MCUE_WITH_FSR
#include "FFXFrameInterpolationModule.h"
#include "IFFXFrameInterpolation.h"
#include "Modules/ModuleObserver.h"
#endif

#if MCUE_WITH_DLSSG
#include "StreamlineLibraryDLSSG.h"
#include "StreamlineLibraryReflex.h"
#endif

extern ENGINE_API float GAverageFPS;

namespace BridgeFrameGeneration
{
    namespace
    {
        enum EBackend { XeSS = 1, FSR, DLSS };
        constexpr int32 Automatic = 8;
        int32 StartupBackend = FSR;
        int32 ActiveMultiplier = 0;
        int32 AppliedMultiplier = INDEX_NONE;
        bool bFsrSwapchainAvailable = false;

#if MCUE_WITH_DLSSG
        constexpr EStreamlineDLSSGMode DlssModes[] = {
            EStreamlineDLSSGMode::Off, EStreamlineDLSSGMode::Off, EStreamlineDLSSGMode::On2X,
            EStreamlineDLSSGMode::On3X, EStreamlineDLSSGMode::On4X, EStreamlineDLSSGMode::On5X,
            EStreamlineDLSSGMode::On6X, EStreamlineDLSSGMode::OnDynamic, EStreamlineDLSSGMode::Auto
        };
#endif

        int32 ResolveBackend(int32 Value)
        {
            return Value == INDEX_NONE ? FSR : FMath::Clamp(Value, int32(XeSS), int32(DLSS));
        }

        bool HasGameSwapchain()
        {
            const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
            const TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
            if (Window && FSlateApplication::IsInitialized() && !FSlateApplication::Get().IsRenderingOffScreen())
            {
                if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
                {
                    FRHIViewport* Viewport = static_cast<FRHIViewport*>(Renderer->GetViewportResource(*Window));
                    return Viewport && Viewport->GetNativeSwapChain();
                }
            }
            return false;
        }

        int32 SupportedModes()
        {
            if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12 ||
                GMaxRHIFeatureLevel < ERHIFeatureLevel::SM6 || !HasGameSwapchain())
            {
                return 0;
            }
            int32 Mask = 0;
            switch (StartupBackend)
            {
#if MCUE_WITH_XESS
            case XeSS:
                if (FModuleManager::Get().IsModuleLoaded(TEXT("XeFGCore")) && UXeFGBlueprintLibrary::IsXeFGSupported())
                {
                    for (EXeFGMode Mode : UXeFGBlueprintLibrary::GetSupportedXeFGModes())
                    {
                        const int32 Value = int32(Mode);
                        if (Value >= 2 && Value <= 4)
                        {
                            Mask |= 1 << Value;
                        }
                    }
                }
                break;
#endif
            case FSR:
                if (bFsrSwapchainAvailable && FModuleManager::Get().IsModuleLoaded(TEXT("FFXFrameInterpolation")))
                {
                    Mask = 1 << 2;
                }
                break;
#if MCUE_WITH_DLSSG
            case DLSS:
                if (FModuleManager::Get().IsModuleLoaded(TEXT("StreamlineDLSSGBlueprint")) &&
                    UStreamlineLibraryDLSSG::IsDLSSGSupported())
                {
                    for (int32 Mode = 2; Mode <= Automatic; ++Mode)
                    {
                        if (UStreamlineLibraryDLSSG::IsDLSSGModeSupported(DlssModes[Mode]))
                        {
                            Mask |= 1 << Mode;
                        }
                    }
                }
                break;
#endif
            default:
                break;
            }
            return Mask;
        }

        void ApplyMultiplier(int32 Multiplier)
        {
            switch (StartupBackend)
            {
#if MCUE_WITH_XESS
            case XeSS:
                UXeFGBlueprintLibrary::SetXeFGMode(static_cast<EXeFGMode>(Multiplier));
                break;
#endif
            case FSR:
                if (IConsoleVariable* Enabled = IConsoleManager::Get().FindConsoleVariable(TEXT("r.FidelityFX.FI.Enabled")))
                {
                    static const FName Tag(TEXT("MinecraftBridgeFrameGeneration"));
                    Enabled->Set(Multiplier != 0 ? 1 : 0, ECVF_SetByGameOverride, Tag);
                }
                break;
#if MCUE_WITH_DLSSG
            case DLSS:
                if (Multiplier != 0)
                {
                    UStreamlineLibraryReflex::SetReflexMode(EStreamlineReflexMode::Enabled);
                }
                UStreamlineLibraryDLSSG::SetDLSSGMode(DlssModes[Multiplier]);
                break;
#endif
            default:
                break;
            }
        }
    }

    void ConfigureStartup(int32 Backend)
    {
        StartupBackend = ResolveBackend(Backend);
        if (IsRunningCommandlet())
        {
            return;
        }
        if (FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen")) ||
            FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
        {
#if MCUE_WITH_FSR
            // FSR 4.1.2 registers viewport hooks even with interpolation disabled.
            static const FName InterpolationModule(TEXT("FFXFrameInterpolation"));
            static FModuleObserver OffscreenFsr(InterpolationModule, FSimpleDelegate::CreateLambda([]
            {
                if (auto* Module = FModuleManager::GetModulePtr<IFFXFrameInterpolationModule>(InterpolationModule))
                {
                    IFFXFrameInterpolation* Interpolation = Module->GetImpl();
                    UGameViewportClient::OnViewportCreated().RemoveAll(Interpolation);
                    FCoreDelegates::GetOnPostEngineInit().RemoveAll(Interpolation);
                }
            }), FSimpleDelegate());
#endif
            return;
        }
        const FString Section = FString::Printf(TEXT("MCUE.FrameGeneration.%s"), Name(StartupBackend));
        UE::ConfigUtilities::ApplyCVarSettingsFromIni(*Section, *GEngineIni, ECVF_SetByGameOverride);
    }

    void PrepareGameViewport()
    {
#if PLATFORM_WINDOWS
        auto& Features = IModularFeatures::Get();
        const FName Type = IDXGISwapchainProvider::GetModularFeatureName();
        bFsrSwapchainAvailable = false;
        // AMD registers its provider even with swapchain override disabled. UE uses the first compatible provider.
        for (IDXGISwapchainProvider* Provider : Features.GetModularFeatureImplementations<IDXGISwapchainProvider>(Type))
        {
            const FStringView ProviderName(Provider->GetProviderName());
            const bool bFsr = ProviderName.Equals(TEXT("FSRSwapchainProvider"));
            bFsrSwapchainAvailable |= bFsr && StartupBackend == FSR;
            if ((StartupBackend != FSR && bFsr) ||
                (StartupBackend != XeSS && ProviderName.Equals(TEXT("FXeFGDXGISwapChainProvider"))) ||
                (StartupBackend != DLSS && ProviderName.Equals(TEXT("FStreamlineD3D12DXGISwapchainProvider"))))
            {
                Features.UnregisterModularFeature(Type, Provider);
            }
        }
#endif
    }

    FStatus Update(int32 RequestedBackend, int32 RequestedMultiplier)
    {
        const int32 Mask = SupportedModes();
        const int32 Requested = RequestedMultiplier == INDEX_NONE ? 2 : RequestedMultiplier;
        const int32 Mode = (Mask & (1 << Requested)) != 0 ? Requested : (Requested != 0 && (Mask & (1 << 2)) ? 2 : 0);
        if (Mode != AppliedMultiplier && (Mode != 0 || AppliedMultiplier > 0))
        {
            ApplyMultiplier(Mode);
        }
        AppliedMultiplier = Mode;
        bool bNeedsRestart = ResolveBackend(RequestedBackend) != StartupBackend;
        ActiveMultiplier = Mode;
#if MCUE_WITH_XESS
        if (StartupBackend == XeSS && Mode != 0 && UXeFGBlueprintLibrary::IfRelaunchRequiredByXeFG())
        {
            ActiveMultiplier = 0;
            bNeedsRestart = true;
        }
#endif
        return {StartupBackend, Mask, ActiveMultiplier, bNeedsRestart};
    }

    bool WritesHudCoverage()
    {
        return ActiveMultiplier != 0 && StartupBackend != FSR;
    }

    FString TraceStatus()
    {
        static const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("BridgeFrameGenTrace"));
        if (!bTrace)
        {
            return {};
        }
        const float RenderedFPS = GAverageFPS;
        float PresentedFPS = ActiveMultiplier == 0 ? RenderedFPS : 0.0f;
        int32 FramesPresented = 0;
#if MCUE_WITH_XESS || MCUE_WITH_FSR || MCUE_WITH_DLSSG
        if (ActiveMultiplier > 0)
        {
            switch (StartupBackend)
            {
#if MCUE_WITH_XESS
            case XeSS:
            {
                static TAtomic<float> OutputFPS{0.0f};
                PresentedFPS = OutputFPS.Load();
                ENQUEUE_RENDER_COMMAND(SampleXeSSFrameGeneration)(
                    [](FRHICommandListImmediate& CommandList)
                    {
                        CommandList.EnqueueLambda([](FRHICommandListBase&)
                        {
                            OutputFPS.Store(GXeFGAverageFPS);
                        });
                    });
                break;
            }
#endif
#if MCUE_WITH_FSR
            case FSR:
                if (auto* Module = FModuleManager::GetModulePtr<IFFXFrameInterpolationModule>(TEXT("FFXFrameInterpolation")))
                {
                    float PresentedMilliseconds = 0.0f;
                    Module->GetImpl()->GetAverageFrameTimes(PresentedMilliseconds, PresentedFPS);
                }
                break;
#endif
#if MCUE_WITH_DLSSG
            case DLSS:
                UStreamlineLibraryDLSSG::GetDLSSGFrameTiming(PresentedFPS, FramesPresented);
                break;
#endif
            default:
                break;
            }
        }
#endif
        return FString::Printf(TEXT("FrameGenRenderedFPS=%.2f FrameGenPresentedFPS=%.2f FrameGenFramesPresented=%d\n"),
            RenderedFPS, PresentedFPS, FramesPresented);
    }

    const TCHAR* Name(int32 Backend)
    {
        switch (Backend)
        {
        case XeSS: return TEXT("XeSS");
        case DLSS: return TEXT("DLSS");
        default: return TEXT("FSR");
        }
    }
}
