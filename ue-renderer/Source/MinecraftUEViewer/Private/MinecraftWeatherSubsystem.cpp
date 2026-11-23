#include "MinecraftWeatherSubsystem.h"

#include "BridgeViewer.h"
#include "Engine/Texture2D.h"

namespace
{
    bool ToMinecraftPosition(const FVector& WorldLocation, FVector& PositionMC)
    {
        PositionMC = FVector(-WorldLocation.Y, WorldLocation.Z, WorldLocation.X) / BridgeWire::BlockSize;
        return !PositionMC.ContainsNaN() && FMath::Abs(PositionMC.X) <= BridgeWire::MaxBlockCoordinate &&
            FMath::Abs(PositionMC.Z) <= BridgeWire::MaxBlockCoordinate;
    }
}

FMinecraftWeatherSnapshot UMinecraftWeatherSubsystem::GetWeatherSnapshot() const
{
    const ABridgeViewer* Viewer = Bridge.Get();
    FMinecraftWeatherSnapshot Snapshot;
    Snapshot.WorldGeneration = WorldGeneration;
    if (!Viewer || Viewer->Height == 0)
    {
        return Snapshot;
    }
    Snapshot.bHasWorld = true;
    Snapshot.SceneSerial = Viewer->SceneSerial;
    if (!Viewer->Environment)
    {
        return Snapshot;
    }

    const BridgeWire::FEnvironmentState& State = *Viewer->Environment;
    Snapshot.Dimension = static_cast<EMinecraftWeatherDimension>(State.Dimension);
    Snapshot.GlobalPrecipitation = State.GlobalRain;
    Snapshot.Thunder = State.Thunder;
    Snapshot.SkyFlash = State.SkyFlash;
    Snapshot.ReportedCameraRain = State.LocalRain;
    Snapshot.ReportedCameraSnow = State.LocalSnow;
    Snapshot.GameTimeSeconds = State.GameTimeSec;
    if ((State.Flags & BridgeWire::WeatherCapabilityKnown) != 0)
    {
        Snapshot.Capability = (State.Flags & BridgeWire::WeatherCapabilityAllowed) != 0
            ? EMinecraftWeatherCapability::Supported : EMinecraftWeatherCapability::Disabled;
        Snapshot.bIsRaining = (State.Flags & BridgeWire::WeatherRaining) != 0;
        Snapshot.bIsThundering = (State.Flags & BridgeWire::WeatherThundering) != 0;
        if (Snapshot.Capability == EMinecraftWeatherCapability::Disabled)
        {
            Snapshot.Condition = EMinecraftWeatherCondition::Clear;
        }
        else
        {
            Snapshot.Condition = Snapshot.bIsThundering ? EMinecraftWeatherCondition::Thunderstorm :
                Snapshot.bIsRaining ? EMinecraftWeatherCondition::Rain : EMinecraftWeatherCondition::Clear;
        }
        return Snapshot;
    }

    const bool bLegacyWeather = State.Dimension != BridgeWire::EDimension::Nether &&
        State.Dimension != BridgeWire::EDimension::End;
    Snapshot.bIsRaining = bLegacyWeather && State.GlobalRain > 0.2f;
    Snapshot.bIsThundering = bLegacyWeather && State.Thunder > 0.9f;
    if (!Snapshot.bIsRaining)
    {
        Snapshot.Condition = EMinecraftWeatherCondition::Clear;
    }
    else if (State.Dimension == BridgeWire::EDimension::Overworld)
    {
        Snapshot.Condition = Snapshot.bIsThundering ? EMinecraftWeatherCondition::Thunderstorm : EMinecraftWeatherCondition::Rain;
    }
    return Snapshot;
}

FMinecraftWeatherSample UMinecraftWeatherSubsystem::SampleWeatherAt(FVector WorldLocation) const
{
    const ABridgeViewer* Viewer = Bridge.Get();
    FVector PositionMC;
    if (!Viewer || !ToMinecraftPosition(WorldLocation, PositionMC))
    {
        return {};
    }

    FMinecraftWeatherSample Sample;
    Sample.ColumnXZ = FIntPoint(FMath::FloorToInt32(PositionMC.X), FMath::FloorToInt32(PositionMC.Z));
    const uint16 Cell = Viewer->GetPrecipColumnCode(Sample.ColumnXZ.X, Sample.ColumnXZ.Y);
    if (Cell != 0)
    {
        Sample.Source = EMinecraftWeatherColumnSource::MinecraftMotionBlockingHeightmap;
        Sample.ColumnTopY = Viewer->MinY + int32(Cell & BridgeWire::PrecipHeightMask);
        Sample.bCovered = PositionMC.Y < double(Sample.ColumnTopY);
        Sample.Climate = static_cast<EMinecraftClimate>(1u + BridgeWire::PrecipClimate(Cell));
        Sample.ColumnType = static_cast<EMinecraftPrecipitation>(1u + BridgeWire::PrecipType(Cell));
    }

    const FMinecraftWeatherSnapshot Snapshot = GetWeatherSnapshot();
    if (Sample.bCovered || Sample.ColumnType == EMinecraftPrecipitation::None ||
        Snapshot.Condition == EMinecraftWeatherCondition::Clear ||
        (Snapshot.Capability == EMinecraftWeatherCapability::Supported && !Snapshot.bIsRaining))
    {
        Sample.Precipitation = EMinecraftPrecipitation::None;
    }
    else if (Cell != 0 && (Snapshot.Condition == EMinecraftWeatherCondition::Rain ||
        Snapshot.Condition == EMinecraftWeatherCondition::Thunderstorm))
    {
        Sample.Precipitation = Sample.ColumnType;
        Sample.Intensity = Snapshot.GlobalPrecipitation;
    }
    return Sample;
}

FMinecraftClimateWeights UMinecraftWeatherSubsystem::SampleClimateWeightsAt(FVector WorldLocation) const
{
    const ABridgeViewer* Viewer = Bridge.Get();
    FVector PositionMC;
    if (!Viewer || !ToMinecraftPosition(WorldLocation, PositionMC))
    {
        return {};
    }

    const FVector2D Centered(PositionMC.X - 0.5, PositionMC.Z - 0.5);
    const FIntPoint Corner(FMath::FloorToInt32(Centered.X), FMath::FloorToInt32(Centered.Y));
    const float X = float(Centered.X - Corner.X);
    const float Z = float(Centered.Y - Corner.Y);
    const float Blend[] = {(1.0f - X) * (1.0f - Z), X * (1.0f - Z), (1.0f - X) * Z, X * Z};
    float Weights[5]{};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Blend); ++Index)
    {
        const uint16 Cell = Viewer->GetPrecipColumnCode(Corner.X + (Index & 1), Corner.Y + (Index >> 1));
        const uint8 Climate = Cell != 0 ? uint8(1u + BridgeWire::PrecipClimate(Cell)) : 0;
        Weights[Climate] += Blend[Index];
    }
    return {Weights[1], Weights[2], Weights[3], Weights[4], Weights[0]};
}

FMinecraftPrecipitationGrid UMinecraftWeatherSubsystem::GetPrecipitationGrid() const
{
    const ABridgeViewer* Viewer = Bridge.Get();
    FMinecraftPrecipitationGrid Grid;
    if (Viewer && Viewer->bPrecipGridReady && Viewer->PrecipGridTexture)
    {
        Grid.Texture = Viewer->PrecipGridTexture;
        Grid.OriginXZ = Viewer->PrecipGridOriginChunk * 16;
        Grid.SizeInBlocks = Grid.Texture->GetSizeX();
        Grid.bValid = true;
    }
    return Grid;
}

bool UMinecraftWeatherSubsystem::GetLastLightningEvent(FMinecraftLightningEvent& Event) const
{
    Event = LastLightningEvent.Get(FMinecraftLightningEvent{});
    return LastLightningEvent.IsSet();
}

void UMinecraftWeatherSubsystem::ResetMinecraftWorld(bool bHasWorld)
{
    if (bHasWorld)
    {
        ++WorldGeneration;
    }
    SeenLightningSeeds.Reset();
    NextLightningEntity = 0;
    LastLightningEvent.Reset();
}

bool UMinecraftWeatherSubsystem::RecordLightning(uint32 Scene, uint32 EntityId, uint32 Seed,
    const FVector& PositionMC, float Remaining)
{
    if (!SeenLightningSeeds.Contains(EntityId))
    {
        // TCP orders flashes. Keep a finite history well beyond the lifetime of an active bolt.
        if (SeenLightningSeeds.Num() == MaxTrackedLightningEntities)
        {
            SeenLightningSeeds.Remove(RecentLightningEntities[NextLightningEntity]);
        }
        RecentLightningEntities[NextLightningEntity] = EntityId;
        NextLightningEntity = (NextLightningEntity + 1) % MaxTrackedLightningEntities;
    }
    auto& Seeds = SeenLightningSeeds.FindOrAdd(EntityId);
    if (Seeds.Contains(Seed))
    {
        return false;
    }

    FMinecraftLightningEvent Event;
    Event.Sequence = ++LightningSequence;
    Event.WorldGeneration = WorldGeneration;
    Event.SceneSerial = Scene;
    Event.EntityId = EntityId;
    Event.Seed = Seed;
    Event.MinecraftPosition = PositionMC;
    Event.WorldLocation = BridgeWire::ToUnreal(PositionMC.X, PositionMC.Y, PositionMC.Z) * BridgeWire::BlockSize;
    Event.ReportedRemainingSeconds = Remaining;
    Event.bIsReflash = !Seeds.IsEmpty();
    if (Seeds.Num() == MaxTrackedFlashesPerEntity)
    {
        Seeds.RemoveAt(0, EAllowShrinking::No);
    }
    Seeds.Add(Seed);
    LastLightningEvent = Event;
    OnLightning.Broadcast(Event);
    return true;
}
