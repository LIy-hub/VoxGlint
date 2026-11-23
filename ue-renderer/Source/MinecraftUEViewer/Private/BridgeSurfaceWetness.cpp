#include "BridgeSurfaceWetness.h"

#include "BridgeWire.h"

namespace
{
    constexpr float WetnessIntervalSeconds = 0.2f;
    constexpr float SoakPerSecond = 0.22f;
    constexpr float PuddleThreshold = 0.35f;
    constexpr float FillPerSecond = 0.18f;
    constexpr float DryPerSecond = 0.028f;
    constexpr float DrainPerSecond = 0.055f;

    uint16 RainSurfaceHeight(uint16 Cell)
    {
        return (Cell & BridgeWire::PrecipValidBit) != 0 && BridgeWire::PrecipType(Cell) == 1
            ? Cell & BridgeWire::PrecipHeightMask : 0;
    }
}

bool FBridgeSurfaceWetness::ReplaceChunk(FIntPoint Position, TConstArrayView<uint16> Precipitation)
{
    check(Precipitation.Num() == ColumnsPerChunk);
    FChunk* History = Chunks.Find(Position);
    bool bHasRainSurfaces = false;
    bool bHasMoisture = false;
    bool bCanSoak = false;
    bool bClearedMoisture = false;
    const bool bRainPending = PendingIntervals.ContainsByPredicate([](float Interval) { return Interval > 0.0f; });
    for (int32 Index = 0; Index < ColumnsPerChunk; ++Index)
    {
        const uint16 SurfaceHeight = RainSurfaceHeight(Precipitation[Index]);
        if (!History && SurfaceHeight != 0)
        {
            History = &Chunks.Add(Position, FChunk{});
        }
        if (!History)
        {
            continue;
        }
        FSurface& Surface = History->Surfaces[Index];
        if (Surface.RelativeHeight != SurfaceHeight)
        {
            bClearedMoisture |= Surface.Moisture.Wetness > 0.0f || Surface.Moisture.Puddles > 0.0f;
            Surface = {{}, SurfaceHeight, bRainPending};
        }
        bHasRainSurfaces |= SurfaceHeight != 0;
        bHasMoisture |= Surface.Moisture.Wetness > 0.0f || Surface.Moisture.Puddles > 0.0f;
        bCanSoak |= SurfaceHeight != 0 && Surface.Moisture.Puddles < 1.0f;
    }
    if (!bHasRainSurfaces)
    {
        Chunks.Remove(Position);
    }
    else
    {
        History->bHasMoisture = bHasMoisture;
        History->bCanSoak = bCanSoak;
    }
    return bClearedMoisture;
}

bool FBridgeSurfaceWetness::DropChunk(FIntPoint Position)
{
    const FChunk* History = Chunks.Find(Position);
    const bool bClearedMoisture = History && History->bHasMoisture;
    Chunks.Remove(Position);
    return bClearedMoisture;
}

void FBridgeSurfaceWetness::Reset()
{
    Chunks.Reset();
    PendingIntervals.Reset();
    PendingSeconds = 0.0f;
    GlobalMoisture = {};
}

void FBridgeSurfaceWetness::ResetMoisture()
{
    PendingIntervals.Reset();
    PendingSeconds = 0.0f;
    GlobalMoisture = {};
    for (auto& Entry : Chunks)
    {
        for (FSurface& Surface : Entry.Value.Surfaces)
        {
            Surface.Moisture = {};
            Surface.bNewThisInterval = false;
        }
        Entry.Value.bHasMoisture = false;
        Entry.Value.bCanSoak = true;
    }
}

void FBridgeSurfaceWetness::Integrate(FMoisture& Moisture, float RainOrDryingSeconds)
{
    if (RainOrDryingSeconds > 0.0f)
    {
        // Only the part of this interval after the soil or rock has soaked starts filling shallow depressions.
        const float UntilPuddles = FMath::Max(0.0f, PuddleThreshold - Moisture.Wetness) / SoakPerSecond;
        Moisture.Wetness = FMath::Min(1.0f, Moisture.Wetness + SoakPerSecond * RainOrDryingSeconds);
        Moisture.Puddles = FMath::Min(1.0f, Moisture.Puddles + FillPerSecond * FMath::Max(0.0f, RainOrDryingSeconds - UntilPuddles));
    }
    else
    {
        Moisture.Wetness = FMath::Max(0.0f, Moisture.Wetness + DryPerSecond * RainOrDryingSeconds);
        Moisture.Puddles = FMath::Max(0.0f, Moisture.Puddles + DrainPerSecond * RainOrDryingSeconds);
    }
}

bool FBridgeSurfaceWetness::AdvanceChunk(FChunk& Chunk) const
{
    if (PendingIntervals.Num() == 1 &&
        (PendingIntervals[0] > 0.0f ? !Chunk.bCanSoak : !Chunk.bHasMoisture))
    {
        return false;
    }
    bool bChanged = false;
    Chunk.bHasMoisture = false;
    Chunk.bCanSoak = false;
    for (FSurface& Surface : Chunk.Surfaces)
    {
        if (Surface.RelativeHeight == 0)
        {
            continue;
        }
        if (Surface.bNewThisInterval)
        {
            // The previous roof or surface owned the pending rain. This replacement starts dry.
            Surface.bNewThisInterval = false;
        }
        else
        {
            const FMoisture Before = Surface.Moisture;
            for (float Interval : PendingIntervals)
            {
                Integrate(Surface.Moisture, Interval);
            }
            bChanged |= Before.Wetness != Surface.Moisture.Wetness || Before.Puddles != Surface.Moisture.Puddles;
        }
        Chunk.bHasMoisture |= Surface.Moisture.Wetness > 0.0f || Surface.Moisture.Puddles > 0.0f;
        Chunk.bCanSoak |= Surface.Moisture.Puddles < 1.0f;
    }
    return bChanged;
}

bool FBridgeSurfaceWetness::Advance(float DeltaSeconds, float RainRate, float DryingScale, const FIntRect& ProjectedChunks)
{
    if (DeltaSeconds <= 0.0f)
    {
        return false;
    }
    const float Interval = DeltaSeconds * (RainRate > 0.0f ? RainRate : -DryingScale);
    Integrate(GlobalMoisture, Interval);
    if (!PendingIntervals.IsEmpty() && PendingIntervals.Last() * Interval > 0.0f)
    {
        PendingIntervals.Last() += Interval;
    }
    else
    {
        PendingIntervals.Add(Interval);
    }
    PendingSeconds += DeltaSeconds;
    if (PendingSeconds < WetnessIntervalSeconds)
    {
        return false;
    }

    bool bVisibleChanged = false;
    for (auto& Entry : Chunks)
    {
        bVisibleChanged |= AdvanceChunk(Entry.Value) && ProjectedChunks.Contains(Entry.Key);
    }
    PendingIntervals.Reset();
    PendingSeconds = 0.0f;
    return bVisibleChanged;
}

FBridgeSurfaceWetness::FMoisture FBridgeSurfaceWetness::Sample(FIntPoint Block) const
{
    const FChunk* History = Chunks.Find(FIntPoint(Block.X >> 4, Block.Y >> 4));
    return History ? History->Surfaces[(Block.Y & 15) * 16 + (Block.X & 15)].Moisture : FMoisture{};
}

bool FBridgeSurfaceWetness::ReadChunk(FIntPoint Position, TArrayView<FMoisture> Destination) const
{
    check(Destination.Num() == ColumnsPerChunk);
    if (const FChunk* History = Chunks.Find(Position))
    {
        for (int32 Index = 0; Index < ColumnsPerChunk; ++Index)
        {
            Destination[Index] = History->Surfaces[Index].Moisture;
        }
        return true;
    }
    return false;
}
