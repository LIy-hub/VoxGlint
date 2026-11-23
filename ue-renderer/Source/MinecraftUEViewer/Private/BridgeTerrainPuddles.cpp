#include "BridgeViewer.h"
#include "BridgeQuality.h"

#include "Engine/Texture2D.h"
#include "Math/Float16Color.h"
#include "PixelFormat.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
    constexpr double FootstepLifetime = 1.1;
    constexpr double FootstepSpacing = 0.64;
    constexpr double FootstepSide = 0.13;
    constexpr float MinimumPuddle = 0.28f;

    UTexture2D* CreateLinearTexture(int32 Width, int32 Height, EPixelFormat Format)
    {
        if (!GPixelFormats[Format].Supported)
        {
            return nullptr;
        }
        UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, Format);
        if (Texture)
        {
            Texture->SRGB = false;
            Texture->Filter = TF_Nearest;
            Texture->AddressX = TA_Clamp;
            Texture->AddressY = TA_Clamp;
            Texture->NeverStream = true;
            Texture->UpdateResource();
        }
        return Texture;
    }

    template <typename PixelType>
    bool UploadLinearTexture(UTexture2D* Texture, TArray<PixelType>&& Pixels)
    {
        if (!Texture || !Texture->GetResource())
        {
            return false;
        }
        struct FUpload
        {
            FUpdateTextureRegion2D Region;
            TArray<PixelType> Data;
        };
        const int32 Width = Texture->GetSizeX();
        const int32 Height = Texture->GetSizeY();
        check(Pixels.Num() == Width * Height);
        auto* Upload = new FUpload{FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height), MoveTemp(Pixels)};
        Texture->UpdateTextureRegions(0, 1, &Upload->Region, Width * sizeof(PixelType), sizeof(PixelType),
            reinterpret_cast<uint8*>(Upload->Data.GetData()),
            [Upload](uint8*, const FUpdateTextureRegion2D*) { delete Upload; });
        return true;
    }

    FIntPoint ColumnOf(const FVector& PositionMC)
    {
        return FIntPoint(FMath::FloorToInt32(PositionMC.X), FMath::FloorToInt32(PositionMC.Z));
    }

    double DepthBelowColumnTop(int32 TopY, double SurfaceY)
    {
        return (double(TopY) - SurfaceY) * BridgeWire::BlockSize;
    }

    bool IsInsideTopLayer(double DepthCm)
    {
        return DepthCm >= -8.0 && DepthCm < 99.0;
    }

    float PuddlePlacement(const FVector& PositionMC, float DepthCm)
    {
        // Match M_McSolid's stable world-space basin mask, including coordinates across zero.
        const FVector2d Phase(FMath::Fmod(PositionMC.Z * BridgeWire::BlockSize, 65536.0) * (UE_DOUBLE_TWO_PI / 65536.0),
                             FMath::Fmod(-PositionMC.X * BridgeWire::BlockSize, 65536.0) * (UE_DOUBLE_TWO_PI / 65536.0));
        const float Noise = float(0.6 * (0.5 + 0.5 * FMath::Sin(Phase.X * 57.0 + FMath::Sin(Phase.Y * 46.0))) +
            0.4 * (0.5 + 0.5 * FMath::Cos(Phase.Y * 89.0 - FMath::Cos(Phase.X * 79.0 + 1.1))));
        const float Basin = FMath::Max(0.92f * FMath::SmoothStep(4.0f, 12.0f, DepthCm),
            0.57f * FMath::SmoothStep(0.61f, 0.81f, Noise));
        return Basin * (1.0f - FMath::SmoothStep(87.0f, 99.0f, DepthCm));
    }
}

void ABridgeViewer::InitializeTerrainWetnessTextures(int32 GridSize)
{
    TerrainWetHistoryTexture = CreateLinearTexture(GridSize, GridSize, PF_FloatRGBA);
    TerrainPuddleFootstepTexture = CreateLinearTexture(MaxTerrainFootsteps, 1, PF_A32B32G32R32F);
}

void ABridgeViewer::UpdateTerrainWetHistory(float DeltaSeconds, float SunAltitude)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainWetHistory);
    const int32 Size = TerrainWetHistoryTexture ? TerrainWetHistoryTexture->GetSizeX() : 0;
    const int32 ChunksWide = Size / 16;
    FIntRect Projected(FIntPoint::ZeroValue, FIntPoint::ZeroValue);
    if (bPrecipGridReady)
    {
        Projected = FIntRect(PrecipGridOriginChunk, PrecipGridOriginChunk + FIntPoint(ChunksWide, ChunksWide));
    }
    const float DryingScale = FMath::Lerp(0.55f, 1.15f, FMath::Clamp(SunAltitude, 0.0f, 1.0f) *
        (1.0f - 0.65f * WeatherSim.Overcast));
    bTerrainWetHistoryDirty |= TerrainWetness.Advance(DeltaSeconds, SurfaceRainRate, DryingScale, Projected);
    if (!bPrecipGridReady || !TerrainWetHistoryTexture || (!bTerrainWetHistoryDirty && bTerrainWetHistoryReady) ||
        BridgeQuality::RainWetnessMode() == 0)
    {
        return;
    }

    TArray<FFloat16Color> Pixels;
    Pixels.SetNumZeroed(Size * Size);
    FBridgeSurfaceWetness::FMoisture ChunkValues[FBridgeSurfaceWetness::ColumnsPerChunk];
    for (int32 ChunkZ = Projected.Min.Y; ChunkZ < Projected.Max.Y; ++ChunkZ)
    {
        for (int32 ChunkX = Projected.Min.X; ChunkX < Projected.Max.X; ++ChunkX)
        {
            if (!TerrainWetness.ReadChunk(FIntPoint(ChunkX, ChunkZ), MakeArrayView(ChunkValues)))
            {
                continue;
            }
            const int32 TexelX = (ChunkX - Projected.Min.X) * 16;
            const int32 TexelZ = (ChunkZ - Projected.Min.Y) * 16;
            for (int32 Z = 0; Z < 16; ++Z)
            {
                for (int32 X = 0; X < 16; ++X)
                {
                    const auto& Moisture = ChunkValues[Z * 16 + X];
                    Pixels[(TexelZ + Z) * Size + TexelX + X] = FFloat16Color(FLinearColor(Moisture.Wetness, Moisture.Puddles, 0.0f, 0.0f));
                }
            }
        }
    }

    bTerrainWetHistoryReady = UploadLinearTexture(TerrainWetHistoryTexture.Get(), MoveTemp(Pixels));
#if !UE_BUILD_SHIPPING
    TerrainWetHistoryUploads += bTerrainWetHistoryReady;
#endif
    bTerrainWetHistoryDirty = !bTerrainWetHistoryReady;
}

void ABridgeViewer::InvalidateTerrainWetHistory()
{
    if (bTerrainWetHistoryReady && TerrainWetHistoryTexture)
    {
        TArray<FFloat16Color> Clear;
        Clear.SetNumZeroed(TerrainWetHistoryTexture->GetSizeX() * TerrainWetHistoryTexture->GetSizeY());
        if (UploadLinearTexture(TerrainWetHistoryTexture.Get(), MoveTemp(Clear)))
        {
#if !UE_BUILD_SHIPPING
            ++TerrainWetHistoryUploads;
#endif
        }
    }
    bTerrainWetHistoryDirty = false;
    bTerrainWetHistoryReady = false;
}

FBridgeSurfaceWetness::FMoisture ABridgeViewer::SampleTerrainMoisture(const FVector& PositionMC) const
{
    if (PositionMC.ContainsNaN() || FMath::Abs(PositionMC.X) > BridgeWire::MaxBlockCoordinate ||
        FMath::Abs(PositionMC.Z) > BridgeWire::MaxBlockCoordinate)
    {
        return {};
    }

    const FIntPoint Block = ColumnOf(PositionMC);
    int32 TopY = 0;
    uint8 Type = 0;
    if (!SamplePrecipColumn(Block.X, Block.Y, TopY, Type) || Type != 1 ||
        !IsInsideTopLayer(DepthBelowColumnTop(TopY, PositionMC.Y)))
    {
        return {};
    }
    return TerrainWetness.Sample(Block);
}

bool ABridgeViewer::IsTerrainPuddleContact(const FVector& PositionMC) const
{
    const FIntPoint Block = ColumnOf(PositionMC);
    int32 TopY = 0;
    uint8 Type = 0;
    if (!SamplePrecipColumn(Block.X, Block.Y, TopY, Type) || Type != 1 || TopY == MinY ||
        FMath::Abs(float(TopY) * BridgeWire::BlockSize) > 65504.0f)
    {
        return false;
    }
    const double DepthCm = DepthBelowColumnTop(TopY, PositionMC.Y);
    if (!IsInsideTopLayer(DepthCm))
    {
        return false;
    }
    const float StoredPuddle = TerrainWetness.Sample(Block).Puddles;
    // Match the lower bound of the shader's normal-curvature multiplier, 1 + 0.16 * Micro.
    const float DepthPuddle = 0.84f * StoredPuddle * PuddlePlacement(PositionMC, float(DepthCm));
    if (DepthPuddle <= MinimumPuddle)
    {
        return false;
    }

    const FIntPoint Origin = PrecipGridOriginChunk * 16;
    const int32 Size = PrecipGridTexture->GetSizeX();
    const FIntRect Domain(Origin, Origin + FIntPoint(Size, Size));
    if (!Domain.Contains(Block))
    {
        return false;
    }
    // Match the shader's one-sided 12 cm puddle fade. Dry, missing or covered neighbors contribute zero.
    const auto EdgeFade = [&](FIntPoint Neighbor, double InCell)
    {
        const float Distance = float(FMath::Min(InCell, 1.0 - InCell));
        if (Distance >= 0.12f)
        {
            return 1.0f;
        }
        int32 NeighborTop = 0;
        uint8 NeighborType = 0;
        float Adjacent = 0.0f;
        if (Domain.Contains(Neighbor) && SamplePrecipColumn(Neighbor.X, Neighbor.Y, NeighborTop, NeighborType) && NeighborType == 1)
        {
            const double NeighborDepth = DepthBelowColumnTop(NeighborTop, PositionMC.Y);
            if (NeighborDepth >= -28.0 && NeighborDepth < 99.0 &&
                FMath::Abs(float(NeighborTop) * BridgeWire::BlockSize) <= 65504.0f)
            {
                Adjacent = TerrainWetness.Sample(Neighbor).Puddles *
                    (1.0f - FMath::SmoothStep(87.0f, 99.0f, float(NeighborDepth)));
            }
        }
        return FMath::Lerp(FMath::Clamp(Adjacent / StoredPuddle, 0.0f, 1.0f), 1.0f,
            FMath::SmoothStep(0.0f, 0.12f, Distance));
    };
    const double WithinX = PositionMC.X - Block.X;
    const double WithinZ = PositionMC.Z - Block.Y;
    const float Edge = FMath::Min(EdgeFade(Block + FIntPoint(WithinX < 0.5 ? -1 : 1, 0), WithinX),
        EdgeFade(Block + FIntPoint(0, WithinZ < 0.5 ? -1 : 1), WithinZ));
    return DepthPuddle * Edge > MinimumPuddle;
}

bool ABridgeViewer::TrackTerrainPuddleFootsteps()
{
    auto& Motion = TerrainFootstepMotion;
    const bool bNewPose = Motion.CameraFrame != CameraFrames;
    const bool bPoseCut = Motion.View != LocalPlayerView || Motion.TeleportEpoch != LastTeleportEpoch;
    if (!bNewPose && !bPoseCut)
    {
        return false;
    }
    const FVector Start = Motion.LastFeet;
    const double SampleGap = TerrainPuddleSeconds - Motion.LastSampleSeconds;
    const bool bContinue = Motion.bTracked && !bPoseCut;
    Motion.CameraFrame = CameraFrames;
    Motion.View = LocalPlayerView;
    Motion.TeleportEpoch = LastTeleportEpoch;
    Motion.LastSampleSeconds = TerrainPuddleSeconds;
    Motion.LastFeet = PlayerFeetMC;
    // The teleport epoch can arrive ahead of the authoritative pose. Do not seed a new stride with the old feet.
    Motion.bTracked = bNewPose && CameraFrames > 0 && LocalPlayerView != BridgeWire::ELocalPlayerView::None &&
        bPlayerGroundedForFootsteps;

    const FVector Travel = PlayerFeetMC - Start;
    const double Distance = FVector2d(Travel.X, Travel.Z).Size();
    if (!Motion.bTracked || !bContinue || SampleGap > 0.35 || FMath::Abs(Travel.Y) > 0.65 ||
        Distance > FMath::Clamp(SampleGap * 12.0, 0.28, 1.45))
    {
        Motion.SinceStep = 0.0;
        Motion.bLeftFoot = true;
        return false;
    }
    if (Distance < 0.0001)
    {
        return false;
    }

    const FVector Left = FVector(Travel.Z, 0.0, -Travel.X) * (FootstepSide / Distance);
    bool bAdded = false;
    while (Motion.SinceStep + Distance >= FootstepSpacing)
    {
        const double AlongSegment = (FootstepSpacing - Motion.SinceStep) / Distance;
        const FVector Step = FMath::Lerp(Start, PlayerFeetMC, AlongSegment) + (Motion.bLeftFoot ? Left : -Left);
        if (IsTerrainPuddleContact(Step))
        {
            if (TerrainPuddleImpacts.Num() == MaxTerrainFootsteps)
            {
                TerrainPuddleImpacts.RemoveAt(0, EAllowShrinking::No);
            }
            TerrainPuddleImpacts.Add({Step, TerrainPuddleSeconds});
            ++TerrainPuddleStepCount;
            bAdded = true;
        }
        Motion.SinceStep -= FootstepSpacing;
        Motion.bLeftFoot = !Motion.bLeftFoot;
    }
    Motion.SinceStep += Distance;
    return bAdded;
}

void ABridgeViewer::UpdateTerrainPuddleFootsteps(float DeltaSeconds, bool bEnabled)
{
    TerrainPuddleSeconds += DeltaSeconds;
    bool bChanged = false;
    if (!bEnabled || !bPrecipGridReady)
    {
        TerrainFootstepMotion = {};
        bChanged = !TerrainPuddleImpacts.IsEmpty();
        TerrainPuddleImpacts.Reset();
    }
    else
    {
        bChanged = TerrainPuddleImpacts.RemoveAll([this](const FTerrainPuddleImpact& Impact)
        {
            return TerrainPuddleSeconds - Impact.SpawnSeconds >= FootstepLifetime || !IsTerrainPuddleContact(Impact.MinecraftPosition);
        }) > 0;
        bChanged |= TrackTerrainPuddleFootsteps();
    }
    if (!bChanged && (TerrainPuddleImpacts.IsEmpty() || TerrainFootstepOriginChunk == PrecipGridOriginChunk))
    {
        return;
    }

    const FIntPoint Origin = bPrecipGridReady ? PrecipGridOriginChunk * 16 : FIntPoint::ZeroValue;
    TArray<FLinearColor> Data;
    Data.Init(FLinearColor(0.0f, 0.0f, 0.0f, -1.0f), MaxTerrainFootsteps);
    for (int32 Index = 0; Index < TerrainPuddleImpacts.Num(); ++Index)
    {
        const FTerrainPuddleImpact& Impact = TerrainPuddleImpacts[Index];
        const FVector& Position = Impact.MinecraftPosition;
        Data[Index] = FLinearColor(float((Position.Z - Origin.Y) * BridgeWire::BlockSize),
            float((Origin.X - Position.X) * BridgeWire::BlockSize), float(Position.Y * BridgeWire::BlockSize),
            float(Impact.SpawnSeconds));
    }
    TerrainFootstepOriginChunk = UploadLinearTexture(TerrainPuddleFootstepTexture.Get(), MoveTemp(Data))
        ? PrecipGridOriginChunk : FIntPoint(INT32_MIN, INT32_MIN);
}

void ABridgeViewer::ResetTerrainPuddleFootsteps()
{
    TerrainFootstepMotion = {};
    TerrainPuddleSeconds = 0.0;
    TerrainPuddleStepCount = 0;
    TerrainFootstepOriginChunk = FIntPoint(INT32_MIN, INT32_MIN);
    TerrainPuddleImpacts.Reset();
    bPlayerGroundedForFootsteps = false;
}
