#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"

class FBridgeSurfaceWetness
{
public:
    struct FMoisture
    {
        float Wetness = 0.0f;
        float Puddles = 0.0f;
    };

    static constexpr int32 ColumnsPerChunk = 16 * 16;
    bool ReplaceChunk(FIntPoint Chunk, TConstArrayView<uint16> Precipitation);
    bool DropChunk(FIntPoint Chunk);
    void Reset();
    void ResetMoisture();

    bool Advance(float DeltaSeconds, float RainRate, float DryingScale, const FIntRect& ProjectedChunks);
    FMoisture Sample(FIntPoint Block) const;
    bool ReadChunk(FIntPoint Chunk, TArrayView<FMoisture> Destination) const;
    FMoisture GetLegacyMoisture() const { return GlobalMoisture; }

private:
    struct FSurface
    {
        FMoisture Moisture;
        uint16 RelativeHeight = 0; // 0 means no rain-exposed solid surface.
        bool bNewThisInterval = false;
    };

    struct FChunk
    {
        TStaticArray<FSurface, ColumnsPerChunk> Surfaces;
        bool bHasMoisture = false;
        bool bCanSoak = false;
    };

    static void Integrate(FMoisture& Moisture, float RainOrDryingSeconds);
    bool AdvanceChunk(FChunk& Chunk) const;

    TMap<FIntPoint, FChunk> Chunks;
    TArray<float, TInlineAllocator<4>> PendingIntervals;
    float PendingSeconds = 0.0f;
    FMoisture GlobalMoisture;
};
