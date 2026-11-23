#include "BridgeUpscaling.h"

#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "TemporalUpscaler.h"

#if MCUE_WITH_DLSS
#include "DLSSLibrary.h"
#endif

#if MCUE_WITH_XESS
#include "XeSSBlueprintLibrary.h"
#endif

namespace BridgeUpscaling
{
    namespace
    {
        constexpr const TCHAR* EnableVariables[] = {
            nullptr, TEXT("r.NGX.DLSS.Enable"), TEXT("r.FidelityFX.FSR.Enabled"), TEXT("r.XeSS.Enabled")
        };

        class FUpscalerObserver final : public FSceneViewExtensionBase
        {
        public:
            explicit FUpscalerObserver(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

            int32 GetPriority() const override { return MIN_int32; }

            void PreRenderViewFamily_RenderThread(FRDGBuilder&, FSceneViewFamily& Family) override
            {
                if (Family.Views.IsEmpty() || !Family.Views[0]->bIsGameView || Family.Views[0]->bIsSceneCapture ||
                    Family.Views[0]->bIsReflectionCapture || Family.Views[0]->bIsPlanarReflection)
                {
                    return;
                }
                EMethod Method = EMethod::TSR;
                if (const auto* Upscaler = Family.GetTemporalUpscalerInterface())
                {
                    const FStringView DebugName(Upscaler->GetDebugName());
                    for (EMethod Candidate : {EMethod::DLSS, EMethod::FSR, EMethod::XeSS})
                    {
                        if (DebugName.Contains(Name(Candidate), ESearchCase::IgnoreCase))
                        {
                            Method = Candidate;
                            break;
                        }
                    }
                }
                Observed.Store(Method);
                ObservedRevision.Store(RenderRevision);
            }

            TAtomic<EMethod> Observed{EMethod::Pending};
            TAtomic<uint32> ObservedRevision{0};
            uint32 RequestedRevision = 0;
            uint32 RenderRevision = 0;
        };

        TSharedPtr<FUpscalerObserver, ESPMode::ThreadSafe> Observer;

        void SetInt(const TCHAR* Name, int32 Value)
        {
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
                Variable && Variable->GetInt() != Value)
            {
                static const FName Tag(TEXT("MinecraftBridgeUpscaler"));
                Variable->Set(Value, ECVF_SetByGameOverride, Tag);
            }
        }
    }

    FSettings Resolve(int32 Requested, int32 Quality)
    {
        FSettings Result;
        Result.Requested = static_cast<EMethod>(FMath::Clamp(Requested, 0, 3));
        Result.Quality = FMath::Clamp(Quality, 0, 2);
        if (Result.Requested == EMethod::TSR || !GDynamicRHI ||
            GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12 || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM6)
        {
            return Result;
        }

        const int32 Mode = Result.Quality;
        switch (Result.Requested)
        {
#if MCUE_WITH_DLSS
        case EMethod::DLSS:
        {
            if (!FModuleManager::Get().IsModuleLoaded(TEXT("DLSS")))
            {
                break;
            }
            constexpr UDLSSMode Modes[] = {UDLSSMode::Quality, UDLSSMode::Balanced, UDLSSMode::Performance};
            bool bSupported = false;
            bool bFixed = false;
            float Min = 0.0f, Max = 0.0f, Sharpness = 0.0f;
            UDLSSLibrary::GetDLSSModeInformation(Modes[Mode], FVector2D::ZeroVector, bSupported,
                Result.RenderPercentage, bFixed, Min, Max, Sharpness);
            if (!bSupported)
            {
                Result.RenderPercentage = 0.0f;
            }
            break;
        }
#endif
        case EMethod::FSR:
            if (FModuleManager::Get().IsModuleLoaded(TEXT("FFXFSRTemporalUpscaling")))
            {
                Result.Active = EMethod::FSR;
                return Result;
            }
            break;
#if MCUE_WITH_XESS
        case EMethod::XeSS:
        {
            constexpr EXeSSQualityMode Modes[] = {
                EXeSSQualityMode::Quality, EXeSSQualityMode::Balanced, EXeSSQualityMode::Performance
            };
            if (FModuleManager::Get().IsModuleLoaded(TEXT("XeSSCore")) && UXeSSBlueprintLibrary::IsXeSSSupported())
            {
                UXeSSBlueprintLibrary::GetXeSSQualityModeInformation(Modes[Mode], Result.RenderPercentage);
            }
            break;
        }
#endif
        default:
            break;
        }
        if (Result.RenderPercentage > 0.0f)
        {
            Result.Active = Result.Requested;
        }
        return Result;
    }

    void Apply(const FSettings& Settings)
    {
        if (!Observer)
        {
            Observer = FSceneViewExtensions::NewExtension<FUpscalerObserver>();
        }
        // Older view families already queued on the render thread belong to the previous settings.
        const uint32 Revision = ++Observer->RequestedRevision;
        ENQUEUE_RENDER_COMMAND(BridgeUpscalerSettingsChanged)(
            [Revision, Target = Observer](FRHICommandListImmediate&)
            {
                Target->RenderRevision = Revision;
            });

        const int32 Active = int32(Settings.Active);
        const IConsoleVariable* FsrEnabled = IConsoleManager::Get().FindConsoleVariable(EnableVariables[int32(EMethod::FSR)]);
        const bool bLeavingFsr = Settings.Active != EMethod::FSR && FsrEnabled && FsrEnabled->GetInt() != 0;
        for (int32 Index = 1; Index < UE_ARRAY_COUNT(EnableVariables); ++Index)
        {
            if (Index != Active)
            {
                SetInt(EnableVariables[Index], 0);
            }
        }
        if (bLeavingFsr && FsrEnabled->GetInt() == 0)
        {
            // FSR restores the previous value and flags but leaves its SetByCode history entry.
            IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
            if (ScreenPercentage && (ScreenPercentage->GetFlags() & ECVF_SetByMask) < ECVF_SetByCode)
            {
                ScreenPercentage->Unset(ECVF_SetByCode);
            }
        }
        if (Settings.Active == EMethod::FSR)
        {
            SetInt(TEXT("r.FidelityFX.FSR.QualityMode"), Settings.Quality + 1);
        }
        if (Settings.Active == EMethod::XeSS)
        {
            SetInt(TEXT("r.XeSS.Quality"), 3 - Settings.Quality);
        }
        if (Active != 0)
        {
            SetInt(EnableVariables[Active], 1);
        }
    }

    EMethod ObservedMethod()
    {
        return Observer && Observer->ObservedRevision.Load() == Observer->RequestedRevision
            ? Observer->Observed.Load() : EMethod::Pending;
    }

    const TCHAR* Name(EMethod Method)
    {
        switch (Method)
        {
        case EMethod::TSR: return TEXT("TSR");
        case EMethod::DLSS: return TEXT("DLSS");
        case EMethod::FSR: return TEXT("FSR");
        case EMethod::XeSS: return TEXT("XeSS");
        default: return TEXT("Pending");
        }
    }
}
