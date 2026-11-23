#include "BridgeWeatherProbe.h"

#if !UE_BUILD_SHIPPING
#include "BridgeQuality.h"
#include "BridgeViewer.h"
#include "DynamicRHI.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Math/Float16Color.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MinecraftWeatherSubsystem.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"

namespace
{
    struct FWeatherGridReadback
    {
        FIntPoint Origin = FIntPoint::ZeroValue;
        int32 Size = 0;
        TArray<FFloat16Color> Pixels;
    };

    struct FWeatherGridSource
    {
        TWeakObjectPtr<UTexture2D> Texture;
        FIntPoint Origin = FIntPoint::ZeroValue;
        int32 Size = 0;
    };

    FWeatherGridReadback ReadWeatherGrid(const FMinecraftPrecipitationGrid& Grid, FWeatherGridSource& Last)
    {
        static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("BridgeWeatherProbeGpu"));
        if (!bEnabled || !GDynamicRHI || RHIGetInterfaceType() != ERHIInterfaceType::D3D12)
        {
            return {};
        }

        // Consumers may retain the texture after grid invalidation. Read that same object to verify its old data is cleared.
        if (Grid.bValid && Grid.Texture)
        {
            Last.Texture = Grid.Texture.Get();
            Last.Origin = Grid.OriginXZ;
            Last.Size = Grid.SizeInBlocks;
        }
        const UTexture2D* Texture = Last.Texture.Get();
        const FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
        if (!Resource || Last.Size <= 0)
        {
            return {};
        }

        FWeatherGridReadback Result;
        Result.Origin = Last.Origin;
        Result.Size = Last.Size;
        ENQUEUE_RENDER_COMMAND(ReadMinecraftWeatherGrid)(
            [Resource, &Result](FRHICommandListImmediate& Cmd)
            {
                FRHITexture* Source = Resource->TextureRHI.GetReference();
                if (Source && Source->GetDesc().Format == PF_FloatRGBA)
                {
                    // The read runs after UpdateTextureRegions and keeps the raw half-float height in centimeters.
                    Cmd.ReadSurfaceFloatData(Source, FIntRect(0, 0, Result.Size, Result.Size), Result.Pixels,
                        FReadSurfaceDataFlags(RCM_MinMax));
                }
            });
        FlushRenderingCommands();
        return Result;
    }
}

void PollBridgeWeatherProbe(const UMinecraftWeatherSubsystem& Weather, const ABridgeViewer& Viewer)
{
    static const FString Directory = []
    {
        FString Result;
        FParse::Value(FCommandLine::Get(), TEXT("BridgeWeatherProbeDir="), Result);
        return Result;
    }();
    if (Directory.IsEmpty())
    {
        return;
    }

    const FString QueryPath = Directory / TEXT("weather-query.tsv");
    FString Request;
    if (!FFileHelper::LoadFileToString(Request, *QueryPath))
    {
        return;
    }
    IFileManager::Get().Delete(*QueryPath);

    TArray<FString> Lines;
    Request.ParseIntoArrayLines(Lines, true);
    int64 Id = 0;
    if (Lines.IsEmpty() || Lines.Num() > 65)
    {
        return;
    }
    FString IdText = Lines[0];
    FString RequestMode;
    Lines[0].Split(TEXT("\t"), &IdText, &RequestMode);
    if (!LexTryParseString(Id, *IdText))
    {
        return;
    }

    const FMinecraftWeatherSnapshot Snapshot = Weather.GetWeatherSnapshot();
    const FMinecraftPrecipitationGrid Grid = Weather.GetPrecipitationGrid();
    static FWeatherGridSource LastPrecipGrid;
    static FWeatherGridSource LastWetGrid;
    const bool bWantsGpu = RequestMode == TEXT("gpu");
    const FWeatherGridReadback Readback = bWantsGpu ? ReadWeatherGrid(Grid, LastPrecipGrid) : FWeatherGridReadback{};
    const bool bReadback = Readback.Size > 0 && Readback.Pixels.Num() == FMath::Square(Readback.Size);
    FMinecraftPrecipitationGrid WetGrid = Grid;
    WetGrid.Texture = Viewer.TerrainWetHistoryTexture;
    WetGrid.bValid = Grid.bValid && Viewer.bTerrainWetHistoryReady && BridgeQuality::RainWetnessMode() >= 1;
    const FWeatherGridReadback WetReadback = bWantsGpu ? ReadWeatherGrid(WetGrid, LastWetGrid) : FWeatherGridReadback{};
    const bool bWetReadback = WetReadback.Size > 0 && WetReadback.Pixels.Num() == FMath::Square(WetReadback.Size);
    FMinecraftLightningEvent Strike;
    const bool bStrike = Weather.GetLastLightningEvent(Strike);
    FString Response = FString::Printf(TEXT("request\t%lld\n"), Id);
    Response += FString::Printf(TEXT("world\t%d\t%lld\t%lld\t%d\t%d\t%d\t%.9g\t%.9g\t%.9g\t%d\t%d\t%.9g\t%.9g\t%.9g\n"),
        Snapshot.bHasWorld, Snapshot.WorldGeneration, Snapshot.SceneSerial, int32(Snapshot.Dimension),
        int32(Snapshot.Condition), int32(Snapshot.Capability), Snapshot.GlobalPrecipitation, Snapshot.Thunder,
        Snapshot.SkyFlash, Snapshot.bIsRaining, Snapshot.bIsThundering, Snapshot.ReportedCameraRain,
        Snapshot.ReportedCameraSnow, Snapshot.GameTimeSeconds);
    Response += FString::Printf(TEXT("grid\t%d\t%d\t%d\t%d\t%d\n"), Grid.bValid, Grid.Texture != nullptr,
        Grid.OriginXZ.X, Grid.OriginXZ.Y, Grid.SizeInBlocks);
    if (bReadback)
    {
        Response += FString::Printf(TEXT("gpu-grid\t%d\t%d\t%d\n"), Readback.Origin.X, Readback.Origin.Y, Readback.Size);
    }
    Response += FString::Printf(TEXT("wet-history\t%d\t%.9g\t%d\t%d\t%llu\t%d\t%.9g\n"),
        WetGrid.bValid, Viewer.SurfaceRainRate, BridgeQuality::RainWetnessMode(), Viewer.bPlayerGroundedForFootsteps,
        Viewer.TerrainPuddleStepCount, Viewer.TerrainPuddleImpacts.Num(), Viewer.TerrainPuddleSeconds);
    Response += FString::Printf(TEXT("teleport-epoch\t%u\n"), Viewer.LastTeleportEpoch);
    Response += FString::Printf(TEXT("wet-history-uploads\t%llu\n"), Viewer.TerrainWetHistoryUploads);
    Response += FString::Printf(TEXT("puddle-depressions\t%d\n"), BridgeQuality::PuddleDepressionsEnabled());
    Response += FString::Printf(TEXT("player-feet\t%.17g\t%.17g\t%.17g\n"),
        Viewer.PlayerFeetMC.X, Viewer.PlayerFeetMC.Y, Viewer.PlayerFeetMC.Z);
    if (bWetReadback)
    {
        Response += FString::Printf(TEXT("gpu-wet-history\t%d\t%d\t%d\n"), WetReadback.Origin.X, WetReadback.Origin.Y, WetReadback.Size);
    }
    for (const auto& Impact : Viewer.TerrainPuddleImpacts)
    {
        Response += FString::Printf(TEXT("puddle-impact\t%.17g\t%.17g\t%.17g\t%.9g\n"), Impact.MinecraftPosition.X,
            Impact.MinecraftPosition.Y, Impact.MinecraftPosition.Z, Viewer.TerrainPuddleSeconds - Impact.SpawnSeconds);
    }
    Response += FString::Printf(TEXT("camera\t%.17g\t%.17g\t%.17g\n"),
        Viewer.CameraPositionMC.X, Viewer.CameraPositionMC.Y, Viewer.CameraPositionMC.Z);
    Response += FString::Printf(TEXT("lightning\t%d\t%lld\t%lld\t%lld\t%lld\t%lld\t%d\t%.9g\t%.17g\t%.17g\t%.17g\t%.17g\t%.17g\t%.17g\n"),
        bStrike, Strike.Sequence, Strike.WorldGeneration, Strike.SceneSerial, Strike.EntityId, Strike.Seed,
        Strike.bIsReflash, Strike.ReportedRemainingSeconds, Strike.MinecraftPosition.X, Strike.MinecraftPosition.Y,
        Strike.MinecraftPosition.Z, Strike.WorldLocation.X, Strike.WorldLocation.Y, Strike.WorldLocation.Z);

    for (int32 Index = 1; Index < Lines.Num(); ++Index)
    {
        TArray<FString> Parts;
        Lines[Index].ParseIntoArray(Parts, TEXT("\t"), false);
        FVector WorldLocation;
        if (Parts.Num() != 4 || !LexTryParseString(WorldLocation.X, *Parts[1]) ||
            !LexTryParseString(WorldLocation.Y, *Parts[2]) || !LexTryParseString(WorldLocation.Z, *Parts[3]))
        {
            return;
        }
        const FMinecraftWeatherSample Sample = Weather.SampleWeatherAt(WorldLocation);
        const FMinecraftClimateWeights Weights = Weather.SampleClimateWeightsAt(WorldLocation);
        const FVector PositionMC = FVector(-WorldLocation.Y, WorldLocation.Z, WorldLocation.X) / BridgeWire::BlockSize;
        const auto Moisture = Viewer.SampleTerrainMoisture(PositionMC);
        Response += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\n"),
            *Parts[0], int32(Sample.Source), Sample.ColumnXZ.X, Sample.ColumnXZ.Y, Sample.ColumnTopY, Sample.bCovered,
            int32(Sample.Climate), int32(Sample.ColumnType), int32(Sample.Precipitation), Sample.Intensity,
            Weights.Temperate, Weights.Dry, Weights.Cold, Weights.Humid, Weights.Unknown);
        Response += FString::Printf(TEXT("moisture\t%s\t%.9g\t%.9g\n"), *Parts[0], Moisture.Wetness, Moisture.Puddles);
        const FIntPoint Texel = Sample.ColumnXZ - Readback.Origin;
        if (bReadback && Texel.X >= 0 && Texel.Y >= 0 && Texel.X < Readback.Size && Texel.Y < Readback.Size)
        {
            const FLinearColor Pixel = Readback.Pixels[Texel.Y * Readback.Size + Texel.X].GetFloats();
            Response += FString::Printf(TEXT("texel\t%s\t%.9g\t%.9g\t%.9g\t%.9g\n"),
                *Parts[0], Pixel.R, Pixel.G, Pixel.B, Pixel.A);
        }
        const FIntPoint WetTexel = Sample.ColumnXZ - WetReadback.Origin;
        if (bWetReadback && WetTexel.X >= 0 && WetTexel.Y >= 0 && WetTexel.X < WetReadback.Size && WetTexel.Y < WetReadback.Size)
        {
            const FLinearColor Pixel = WetReadback.Pixels[WetTexel.Y * WetReadback.Size + WetTexel.X].GetFloats();
            Response += FString::Printf(TEXT("wet-texel\t%s\t%.9g\t%.9g\n"), *Parts[0], Pixel.R, Pixel.G);
        }
    }

    // Separate replies avoid replacing a file while the Windows replay process holds a read handle.
    const FString Result = Directory / FString::Printf(TEXT("weather-response-%lld.tsv"), Id);
    const FString Pending = Result + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(Response, *Pending, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Move(*Result, *Pending, true, false, false, true);
    }
}
#endif
