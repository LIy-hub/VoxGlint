#include "BridgeWaterAssetsCommandlet.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "Stateless/NiagaraStatelessEmitter.h"
#include "Stateless/Modules/NiagaraStatelessModule_AddVelocity.h"
#include "Stateless/Modules/NiagaraStatelessModule_GravityForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_InitializeParticle.h"
#include "Stateless/Modules/NiagaraStatelessModule_ShapeLocation.h"
#include "Stateless/Modules/NiagaraStatelessModule_SolveVelocitiesAndForces.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    enum class EParticle { Plankton, Bubble, Mist, FallSpray, Rain, Entry, MistVeil };

    struct FParticleRecipe
    {
        const TCHAR* Material;
        FVector2f Lifetime;
        float WidthCm;
        FLinearColor Color;
        int32 Burst;
        float Rate;
    };

    const FParticleRecipe Recipes[] = {
        {TEXT("M_McPlankton"), {2.95f, 2.95f}, 1.6f, {0.54f, 0.77f, 0.83f, 0.24f}, 1, 0.0f},
        {TEXT("M_McWaterBubble"), {1.04f, 1.04f}, 4.1f, {0.43f, 0.76f, 0.92f, 0.62f}, 1, 0.0f},
        {TEXT("M_McWaterfallVolume"), {1.65f, 2.65f}, 230.0f, FLinearColor::White, 1, 4.0f},
        {TEXT("M_McWaterSpray"), {0.36f, 0.72f}, 4.6f, {0.73f, 0.89f, 1.0f, 0.71f}, 1, 21.0f},
        {TEXT("M_McWaterSpray"), {0.15f, 0.29f}, 2.3f, {0.60f, 0.79f, 0.91f, 0.62f}, 5, 0.0f},
        {TEXT("M_McWaterSpray"), {0.38f, 0.78f}, 6.8f, {0.80f, 0.92f, 1.0f, 0.86f}, 32, 0.0f},
        {TEXT("M_McWaterfallVeil"), {1.40f, 2.10f}, 185.0f, FLinearColor::White, 1, 3.0f}
    };

    bool ConfigureEmitter(UNiagaraStatelessEmitter& Emitter, EParticle Type)
    {
        const FParticleRecipe& Recipe = Recipes[int32(Type)];
        const FString MaterialPath = FString::Printf(TEXT("/Game/Bridge/%s.%s"), Recipe.Material, Recipe.Material);
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (!Material) return false;
        const bool bMist = Type == EParticle::Mist || Type == EParticle::MistVeil;
        const bool bRain = Type == EParticle::Rain;
        const bool bBubble = Type == EParticle::Bubble;
        const bool bFountain = Type == EParticle::FallSpray || Type == EParticle::Entry;
        const bool bContinuous = Recipe.Rate > 0.0f;

        FNiagaraStatelessSpawnInfo* Spawn = Emitter.GetSpawnInfoByIndex(0);
        if (!Spawn) Spawn = &Emitter.AddSpawnInfo();
        Spawn->Type = bContinuous ? ENiagaraStatelessSpawnInfoType::Rate : ENiagaraStatelessSpawnInfoType::Burst;
        Spawn->Amount = FNiagaraDistributionRangeInt(Recipe.Burst);
        Spawn->Rate = FNiagaraDistributionRangeFloat(Recipe.Rate);
        Spawn->SpawnTime = 0.0f;
        Spawn->bEnabled = true;
        Spawn->bSpawnProbabilityEnabled = false;
        Spawn->bLoopCountLimitEnabled = !bContinuous;
        Spawn->LoopCountLimit = FNiagaraDistributionRangeInt(1);
        for (int32 I = 1; I < Emitter.GetNumSpawnInfos(); ++I)
        {
            Emitter.GetSpawnInfoByIndex(I)->bEnabled = false;
        }

        bool bInit = false;
        bool bVelocity = false;
        bool bShape = false;
        for (UNiagaraStatelessModule* Module : Emitter.GetModules())
        {
            if (auto* Init = Cast<UNiagaraStatelessModule_InitializeParticle>(Module))
            {
                Init->LifetimeDistribution = Recipe.Lifetime.X == Recipe.Lifetime.Y
                    ? FNiagaraDistributionRangeFloat(Recipe.Lifetime.X)
                    : FNiagaraDistributionRangeFloat(Recipe.Lifetime.X, Recipe.Lifetime.Y);
                Init->SpriteSizeDistribution.InitConstant(FVector2f(Recipe.WidthCm));
                Init->ColorDistribution = FNiagaraDistributionColor(Recipe.Color);
                Init->InitialPositionDistribution = FNiagaraDistributionPosition(FVector3f(0.0f, 0.0f, bMist ? 40.0f : 0.0f));
                Init->SetIsModuleEnabled(true);
                bInit = true;
            }
            else if (auto* Velocity = Cast<UNiagaraStatelessModule_AddVelocity>(Module))
            {
                Velocity->CoordinateSpace = ENiagaraCoordinateSpace::Local;
                if (bRain)
                {
                    Velocity->VelocityType = ENSM_VelocityType::FromPoint;
                    Velocity->PointOrigin = FVector3f(0.0f, 0.0f, -2.5f);
                    Velocity->PointVelocityDistribution = FNiagaraDistributionRangeFloat(46.0f, 74.0f);
                }
                else if (bFountain)
                {
                    Velocity->VelocityType = ENSM_VelocityType::InCone;
                    Velocity->ConeRotationType = ENSM_ConeRotationType::Direction;
                    Velocity->ConeDirection.InitConstant(FVector3f::UpVector);
                    Velocity->ConeAngle = Type == EParticle::Entry ? 115.0f : 122.0f;
                    Velocity->InnerCone = Type == EParticle::Entry ? 0.25f : 0.20f;
                    Velocity->ConeVelocityDistribution = Type == EParticle::Entry
                        ? FNiagaraDistributionRangeFloat(145.0f, 360.0f)
                        : FNiagaraDistributionRangeFloat(98.0f, 318.0f);
                }
                else
                {
                    Velocity->VelocityType = ENSM_VelocityType::Linear;
                    const FVector3f Drift = bMist ? FVector3f(20.0f, -11.0f, 32.0f) :
                        (bBubble ? FVector3f(3.1f, 1.9f, 32.0f) : FVector3f(2.1f, -1.6f, 3.2f));
                    Velocity->LinearVelocityDistribution.InitConstant(Drift);
                }
                Velocity->SetIsModuleEnabled(true);
                bVelocity = true;
            }
            else if (auto* Shape = Cast<UNiagaraStatelessModule_ShapeLocation>(Module))
            {
                Shape->ShapePrimitive = (bRain || Type == EParticle::Entry) ? ENSM_ShapePrimitive::Ring : ENSM_ShapePrimitive::Box;
                Shape->RingRadius.InitConstant(Type == EParticle::Entry ? 26.0f : 2.6f);
                Shape->BoxSize.InitConstant(bMist ? FVector3f(165, 135, 32) : FVector3f(42, 42, 10));
                Shape->SetIsModuleEnabled(bRain || bMist || Type == EParticle::FallSpray || Type == EParticle::Entry);
                bShape = true;
            }
            else if (auto* Gravity = Cast<UNiagaraStatelessModule_GravityForce>(Module))
            {
                Gravity->GravityDistribution.InitConstant(FVector3f(0, 0, bRain ? -330.0f : -800.0f));
                Gravity->SetIsModuleEnabled(bRain || bFountain);
            }
            else
            {
                Module->SetIsModuleEnabled(Module->IsA<UNiagaraStatelessModule_SolveVelocitiesAndForces>());
            }
        }
        bool bRenderer = false;
        for (UNiagaraRendererProperties* Render : Emitter.GetRenderers())
        {
            if (auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Render))
            {
                Sprite->Material = Material;
                bRenderer = true;
            }
        }
        Emitter.PostEditChange();
        return bInit && bVelocity && bRenderer && (!(bRain || bMist) || bShape);
    }

    bool GenerateSystem(const TCHAR* Name, EParticle Type)
    {
        UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr,
            TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight"));
        if (!Template || Template->GetNumEmitters() != 1) return false;
        const FString Path = FString::Printf(TEXT("/Game/Bridge/%s"), Name);
        const FString ObjectPath = Path + TEXT(".") + Name;
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
        const bool bNew = System == nullptr;
        UPackage* Package = bNew ? CreatePackage(*Path) : System->GetOutermost();
        if (bNew)
        {
            System = DuplicateObject<UNiagaraSystem>(Template, Package, FName(Name));
        }
        else
        {
            Package->FullyLoad();
            while (System->GetNumEmitters() > 1)
            {
                const FNiagaraEmitterHandle Extra = System->GetEmitterHandle(System->GetNumEmitters() - 1);
                System->RemoveEmitterHandle(Extra);
            }
        }
        System->ClearFlags(RF_Transient);
        System->SetFlags(RF_Public | RF_Standalone);
        UNiagaraStatelessEmitter* Emitter = System->GetNumEmitters() == 1
            ? System->GetEmitterHandle(0).GetStatelessEmitter() : nullptr;
        bool bConfigured = Emitter && Emitter->IsIn(System) && ConfigureEmitter(*Emitter, Type);
        if (bConfigured && Type == EParticle::Mist)
        {
            const FNiagaraEmitterHandle Source = System->GetEmitterHandle(0);
            const auto AddEmitter = [&](const TCHAR* EmitterName, EParticle Recipe)
            {
                const FNiagaraEmitterHandle Handle = System->DuplicateEmitterHandle(Source, FName(EmitterName));
                UNiagaraStatelessEmitter* Added = Handle.GetStatelessEmitter();
                return Added && Added != Emitter && Added->IsIn(System) && ConfigureEmitter(*Added, Recipe);
            };
            bConfigured = AddEmitter(TEXT("WaterfallSpray"), EParticle::FallSpray) &&
                AddEmitter(TEXT("WaterfallVeil"), EParticle::MistVeil);
        }
        if (!bConfigured)
        {
            UE_LOG(LogTemp, Error, TEXT("Niagara sprite modules are incomplete for %s."), Name);
            return false;
        }
        System->PostEditChange();
        System->RequestCompile(true);
        System->WaitForCompilationComplete(true, false);
        if (bNew) FAssetRegistryModule::AssetCreated(System);
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        const FString File = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
        const bool bSaved = UPackage::SavePackage(Package, System, *File, Args);
        UE_CLOG(bSaved, LogTemp, Display, TEXT("Water Niagara saved: %s"), *Path);
        return bSaved;
    }
}
#endif

int32 UBridgeWaterAssetsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
    bool bSuccess = true;
    if (!FParse::Param(*Params, TEXT("P3Only")))
    {
        bSuccess &= GenerateSystem(TEXT("NS_McUnderwaterPlankton"), EParticle::Plankton);
        bSuccess &= GenerateSystem(TEXT("NS_McWaterBubbles"), EParticle::Bubble);
    }
    bSuccess &= GenerateSystem(TEXT("NS_McWaterfallMist"), EParticle::Mist);
    bSuccess &= GenerateSystem(TEXT("NS_McRainCrown"), EParticle::Rain);
    bSuccess &= GenerateSystem(TEXT("NS_McWaterEntrySplash"), EParticle::Entry);
    return bSuccess ? 0 : 1;
#else
    return 1;
#endif
}
