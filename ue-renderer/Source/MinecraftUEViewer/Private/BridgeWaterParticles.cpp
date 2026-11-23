#include "BridgeWaterParticles.h"

#include "BridgeViewer.h"
#include "BridgeWire.h"
#include "Components/LocalFogVolumeComponent.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

namespace
{
    TAutoConsoleVariable<int32> CVarWaterPlankton(TEXT("mc.Water.P2.Plankton"), 1, TEXT("Underwater world-space plankton."));
    TAutoConsoleVariable<int32> CVarWaterBubbles(TEXT("mc.Water.P2.Bubbles"), 1, TEXT("Render Fabric bubble columns."));

    constexpr int32 HeaderBytes = 16;
    constexpr int32 RunBytes = 6;
    constexpr int32 MaxRuns = 4096;
    constexpr int32 MaxLive = 88;

    void AddWaterfallFog(UNiagaraComponent& Effect, UWorld& World)
    {
        // Keep a continuous, shadowed mist core below the animated Niagara wisps.
        ULocalFogVolumeComponent* Fog = NewObject<ULocalFogVolumeComponent>(&Effect);
        Fog->SetupAttachment(&Effect);
        Fog->SetRelativeLocation(FVector(0.0, 0.0, 93.0));
        Fog->SetRelativeScale3D(FVector(138.0 / ULocalFogVolumeComponent::GetBaseVolumeSize()));
        // The froxel path multiplies the radial and height coverage.
        Fog->RadialFogExtinction = 0.88f;
        Fog->HeightFogExtinction = 0.82f;
        Fog->HeightFogFalloff = 145.0f;
        Fog->FogAlbedo = FLinearColor(0.71f, 0.80f, 0.85f);
        Fog->FogPhaseG = 0.41f;
        Fog->RegisterComponentWithWorld(&World);
    }

    void DestroyWaterfall(UNiagaraComponent& Effect)
    {
        TArray<USceneComponent*> Children;
        Effect.GetChildrenComponents(false, Children);
        for (USceneComponent* Child : Children)
        {
            if (Child->IsA<ULocalFogVolumeComponent>()) Child->DestroyComponent();
        }
        Effect.DestroyComponent();
    }
}

FBridgeWaterParticles::FBridgeWaterParticles(UWorld* InWorld)
    : World(InWorld)
{
    Plankton.Reset(LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Bridge/NS_McUnderwaterPlankton.NS_McUnderwaterPlankton"),
        nullptr, LOAD_NoWarn));
    Bubbles.Reset(LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Bridge/NS_McWaterBubbles.NS_McWaterBubbles"),
        nullptr, LOAD_NoWarn));
    WaterfallMist.Reset(LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Bridge/NS_McWaterfallMist.NS_McWaterfallMist"),
        nullptr, LOAD_NoWarn));
    RainCrown.Reset(LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Bridge/NS_McRainCrown.NS_McRainCrown"),
        nullptr, LOAD_NoWarn));
    EntrySplash.Reset(LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Bridge/NS_McWaterEntrySplash.NS_McWaterEntrySplash"),
        nullptr, LOAD_NoWarn));
}

FBridgeWaterParticles::~FBridgeWaterParticles()
{
    Reset();
}

void FBridgeWaterParticles::ReplaceChunk(FIntPoint Chunk, TArray<FBridgeBubbleColumn>&& Runs)
{
    for (int32 I = Live.Num() - 1; I >= 0; --I)
    {
        if (Live[I].Chunk.IsSet() && Live[I].Chunk.GetValue() == Chunk)
        {
            if (UNiagaraComponent* FX = Live[I].Component.Get()) FX->DestroyComponent();
            Live.RemoveAtSwap(I, EAllowShrinking::No);
        }
    }
    if (Runs.IsEmpty()) Columns.Remove(Chunk);
    else Columns.Add(Chunk, MoveTemp(Runs));
}

void FBridgeWaterParticles::Reset()
{
    TickSurfaceEffects(0.0f, false, {});
    for (const FParticle& P : Live)
    {
        if (UNiagaraComponent* FX = P.Component.Get()) FX->DestroyComponent();
    }
    Live.Reset();
    Columns.Reset();
    SpawnIn = 0.0f;
    bInitialized = false;
}

void FBridgeWaterParticles::SpawnSurfaceSplash(const FVector& Position, bool bRain, float Scale)
{
    UNiagaraSystem* System = bRain ? RainCrown.Get() : EntrySplash.Get();
    if (!System || !World.IsValid() || Splashes.Num() >= 22) return;
    if (UNiagaraComponent* FX = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World.Get(), System, Position,
        FRotator::ZeroRotator, FVector(Scale), true, true, ENCPoolMethod::None, true))
    {
        Splashes.Add({FX, {}, bRain ? 0.36f : 0.92f});
    }
}

void FBridgeWaterParticles::TickSurfaceEffects(float DeltaSeconds, bool bEnabled, TConstArrayView<FBridgeWaterfallFX> Impacts)
{
    bEnabled &= World.IsValid();
    for (int32 I = Splashes.Num() - 1; I >= 0; --I)
    {
        FParticle& P = Splashes[I];
        P.TimeLeft -= DeltaSeconds;
        if (!bEnabled || P.TimeLeft <= 0.0f || !P.Component.IsValid())
        {
            if (UNiagaraComponent* FX = P.Component.Get()) FX->DestroyComponent();
            Splashes.RemoveAtSwap(I, EAllowShrinking::No);
        }
    }
    TSet<FIntVector> Updated;
    if (bEnabled && WaterfallMist)
    {
        for (const FBridgeWaterfallFX& Impact : Impacts)
        {
            const FVector Cell = Impact.Position / 100.0;
            const FIntVector Key(FMath::FloorToInt32(Cell.X), FMath::FloorToInt32(Cell.Y), FMath::RoundToInt32(Cell.Z));
            if (Updated.Contains(Key)) continue;
            Updated.Add(Key);
            auto& Existing = WaterfallComponents.FindOrAdd(Key);
            if (UNiagaraComponent* FX = Existing.Get())
            {
                FX->SetWorldLocation(Impact.Position);
                continue;
            }
            const float Scale = FMath::Lerp(0.72f, 1.32f, FMath::SmoothStep(0.58f, 1.75f, Impact.Strength));
            if (UNiagaraComponent* FX = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World.Get(), WaterfallMist.Get(),
                Impact.Position, FRotator::ZeroRotator, FVector(Scale), false, true, ENCPoolMethod::None, false))
            {
                FX->SetSystemFixedBounds(FBox(FVector(-480, -480, -180), FVector(480, 480, 500)));
                AddWaterfallFog(*FX, *World.Get());
                Existing = FX;
            }
        }
    }
    for (auto It = WaterfallComponents.CreateIterator(); It; ++It)
    {
        if (Updated.Contains(It.Key())) continue;
        if (UNiagaraComponent* FX = It.Value().Get()) DestroyWaterfall(*FX);
        It.RemoveCurrent();
    }
}

void FBridgeWaterParticles::Spawn(UNiagaraSystem* Template, const FVector& Position, bool bDown, float Time,
    TOptional<FIntPoint> Chunk)
{
    if (!Template || Live.Num() >= MaxLive)
    {
        return;
    }
    const FRotator Rotation = bDown ? FRotator(180.0, 0.0, 0.0) : FRotator::ZeroRotator;
    if (UNiagaraComponent* FX = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World.Get(), Template, Position,
        Rotation, FVector::OneVector, false, true, ENCPoolMethod::None, false))
    {
        Live.Add({FX, Chunk, Time});
    }
}

void FBridgeWaterParticles::Tick(float DeltaSeconds, const FVector& Eye, bool bWet, int32 Tier,
    TFunctionRef<bool(const FVector&)> CanSpawnPlankton)
{
    if (!World.IsValid())
    {
        return;
    }
    const bool bCut = !bInitialized || FVector::DistSquared(LastEye, Eye) > FMath::Square(800.0);
    LastEye = Eye;
    bInitialized = true;
    const bool bActive = FBridgeWaterOptics::IsEnabled() && Tier > 0;
    for (int32 I = Live.Num() - 1; I >= 0; --I)
    {
        FParticle& P = Live[I];
        P.TimeLeft -= DeltaSeconds;
        if (bCut || !bActive || P.TimeLeft <= 0.0f || !P.Component.IsValid())
        {
            if (UNiagaraComponent* FX = P.Component.Get()) FX->DestroyComponent();
            Live.RemoveAtSwap(I, EAllowShrinking::No);
        }
    }
    if (!bActive || (SpawnIn -= DeltaSeconds) > 0.0f)
    {
        return;
    }
    SpawnIn = 0.15f;
    if (bWet && Plankton.IsValid() && CVarWaterPlankton.GetValueOnGameThread() != 0)
    {
        for (int32 I = 0; I < (Tier > 1 ? 5 : 3); ++I)
        {
            const FVector At = Eye + FVector(Random.FRandRange(-730.0f, 730.0f),
                Random.FRandRange(-730.0f, 730.0f), Random.FRandRange(-730.0f, 730.0f));
            if (CanSpawnPlankton(At)) Spawn(Plankton.Get(), At, false, 3.15f, {});
        }
    }
    if (Bubbles.IsValid() && CVarWaterBubbles.GetValueOnGameThread() != 0)
    {
        const int32 ChunkX = FMath::FloorToInt32(-Eye.Y / 1600.0);
        const int32 ChunkZ = FMath::FloorToInt32(Eye.X / 1600.0);
        int32 Count = 0;
        for (int32 Z = ChunkZ - 1; Z <= ChunkZ + 1 && Count < 8; ++Z)
        {
            for (int32 X = ChunkX - 1; X <= ChunkX + 1 && Count < 8; ++X)
            {
                const FIntPoint Chunk(X, Z);
                const auto* Runs = Columns.Find(Chunk);
                if (!Runs) continue;
                for (const FBridgeBubbleColumn& Run : *Runs)
                {
                    const FVector Bottom = BridgeWire::ToUnreal(Run.Bottom.X + 0.5, Run.Bottom.Y, Run.Bottom.Z + 0.5) * 100.0;
                    const float Increase = float(Run.Height) * 100.0f;
                    const FVector Closest(Bottom.X, Bottom.Y, FMath::Clamp(Eye.Z, Bottom.Z, Bottom.Z + Increase));
                    if (FVector::DistSquared(Closest, Eye) > FMath::Square(1800.0)) continue;
                    const float MinZ = Run.bDown ? 41.0f : 7.0f;
                    const float MaxZ = Increase - (Run.bDown ? 7.0f : 41.0f);
                    const FVector Point = Bottom + FVector(Random.FRandRange(-26.0f, 26.0f),
                        Random.FRandRange(-26.0f, 26.0f), Random.FRandRange(MinZ, MaxZ));
                    Spawn(Bubbles.Get(), Point, Run.bDown, 1.12f, Chunk);
                    if (++Count == 8) break;
                }
            }
        }
    }
}

bool ABridgeViewer::ApplyChunkWaterBubbles(BridgeWire::FReader Reader, int32 Size)
{
    if (Size < HeaderBytes)
    {
        return false;
    }
    const uint32 Scene = Reader.U32();
    const FIntPoint Chunk(Reader.I32(), 0);
    const int32 Z = Reader.I32();
    const uint16 Count = Reader.U16();
    if (Reader.U16() != 0 || Count > MaxRuns || Size != HeaderBytes + int32(Count) * RunBytes ||
        FMath::Abs(int64(Chunk.X)) > 1875000 || FMath::Abs(int64(Z)) > 1875000)
    {
        return false;
    }
    if (Scene != SceneSerial || !WaterParticles)
    {
        return true;
    }
    TArray<FBridgeBubbleColumn> Runs;
    Runs.Reserve(Count);
    uint16 Ends[256]{};
    for (uint16 I = 0; I < Count; ++I)
    {
        const uint8 Local = Reader.Byte();
        const uint8 Direction = Reader.Byte();
        const uint16 Start = Reader.U16();
        const uint16 Length = Reader.U16();
        if (Direction > 1 || Length == 0 || Start < Ends[Local] || int32(Start) + Length > Height)
        {
            return false;
        }
        Ends[Local] = Start + Length;
        Runs.Add({FIntVector(Chunk.X * 16 + (Local & 15), MinY + Start, Z * 16 + (Local >> 4)), Length, Direction != 0});
    }
    WaterParticles->ReplaceChunk(FIntPoint(Chunk.X, Z), MoveTemp(Runs));
    UE_CLOG(bTrace, LogTemp, Display, TEXT("WaterBubbles scene=%u chunk=%d,%d runs=%d."), Scene, Chunk.X, Z, Count);
    return true;
}
