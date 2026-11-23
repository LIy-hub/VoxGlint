#pragma once

#include "CoreMinimal.h"

class UNiagaraComponent;
class UNiagaraSystem;
class UWorld;

struct FBridgeBubbleColumn
{
    FIntVector Bottom;
    uint16 Height;
    bool bDown;
};

struct FBridgeWaterfallFX
{
    FVector Position;
    float Strength;
};

class FBridgeWaterParticles
{
public:
    explicit FBridgeWaterParticles(UWorld* InWorld);
    ~FBridgeWaterParticles();

    void ReplaceChunk(FIntPoint Chunk, TArray<FBridgeBubbleColumn>&& Runs);
    void Tick(float DeltaSeconds, const FVector& Eye, bool bWet, int32 Tier,
        TFunctionRef<bool(const FVector&)> CanSpawnPlankton);
    void TickSurfaceEffects(float DeltaSeconds, bool bEnabled, TConstArrayView<FBridgeWaterfallFX> Impacts);
    void SpawnSurfaceSplash(const FVector& Position, bool bRain, float Scale);
    void Reset();

private:
    struct FParticle
    {
        TWeakObjectPtr<UNiagaraComponent> Component;
        TOptional<FIntPoint> Chunk;
        float TimeLeft;
    };

    void Spawn(UNiagaraSystem* Template, const FVector& World, bool bDown, float Time, TOptional<FIntPoint> Chunk);

    TWeakObjectPtr<UWorld> World;
    TStrongObjectPtr<UNiagaraSystem> Plankton;
    TStrongObjectPtr<UNiagaraSystem> Bubbles;
    TStrongObjectPtr<UNiagaraSystem> WaterfallMist;
    TStrongObjectPtr<UNiagaraSystem> RainCrown;
    TStrongObjectPtr<UNiagaraSystem> EntrySplash;
    TMap<FIntPoint, TArray<FBridgeBubbleColumn>> Columns;
    TArray<FParticle> Live;
    TArray<FParticle> Splashes;
    TMap<FIntVector, TWeakObjectPtr<UNiagaraComponent>> WaterfallComponents;
    FRandomStream Random{3240257};
    FVector LastEye = FVector::ZeroVector;
    float SpawnIn = 0.0f;
    bool bInitialized = false;
};
