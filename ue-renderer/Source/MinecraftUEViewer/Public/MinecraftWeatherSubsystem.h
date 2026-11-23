#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MinecraftWeatherSubsystem.generated.h"

class ABridgeViewer;
class UTexture2D;

UENUM(BlueprintType)
enum class EMinecraftWeatherCondition : uint8 { Unknown, Clear, Rain, Thunderstorm };

UENUM(BlueprintType)
enum class EMinecraftWeatherCapability : uint8 { Unknown, Supported, Disabled };

UENUM(BlueprintType)
enum class EMinecraftWeatherDimension : uint8 { Overworld, Nether, End, Other, Unknown };

UENUM(BlueprintType)
enum class EMinecraftPrecipitation : uint8 { Unknown, None, Rain, Snow };

UENUM(BlueprintType)
enum class EMinecraftClimate : uint8 { Unknown, Temperate, Dry, Cold, Humid };

UENUM(BlueprintType)
enum class EMinecraftWeatherColumnSource : uint8 { Unknown, MinecraftMotionBlockingHeightmap };

USTRUCT(BlueprintType)
struct MINECRAFTUEVIEWER_API FMinecraftWeatherSnapshot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bHasWorld = false;

    /** Monotonic in this UE world, including reconnects that reuse a Minecraft scene serial. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 WorldGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 SceneSerial = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftWeatherCondition Condition = EMinecraftWeatherCondition::Unknown;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftWeatherCapability Capability = EMinecraftWeatherCapability::Unknown;

    /** Sky-box classification. Weather capability comes separately from Minecraft. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftWeatherDimension Dimension = EMinecraftWeatherDimension::Unknown;

    /** Raw MC global rain level [0, 1]; it supplies the intensity for snow as well as rain. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float GlobalPrecipitation = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Thunder = 0.0f;

    /** Raw world flash from Minecraft; OnLightning carries strike positions. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float SkyFlash = 0.0f;

    /** Minecraft's weather booleans, inferred from the raw levels on legacy frames. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bIsRaining = false;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bIsThundering = false;

    /** Raw MC snapshots at the player sampled in 3D. They do not describe arbitrary points or roof coverage. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float ReportedCameraRain = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float ReportedCameraSnow = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float GameTimeSeconds = 0.0f;
};

USTRUCT(BlueprintType)
struct MINECRAFTUEVIEWER_API FMinecraftWeatherSample
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftWeatherColumnSource Source = EMinecraftWeatherColumnSource::Unknown;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    FIntPoint ColumnXZ = FIntPoint::ZeroValue;

    /** Exact first Y above Minecraft's motion-blocking column, in blocks. Valid when Source is known. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int32 ColumnTopY = 0;

    /** True below ColumnTopY. Check Source for unknown coverage. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bCovered = false;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftClimate Climate = EMinecraftClimate::Unknown;

    /** Precipitation allowed by Minecraft's biome sampled at this column's top; it can differ from the biome at the query altitude. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftPrecipitation ColumnType = EMinecraftPrecipitation::Unknown;

    /** Actual precipitation at the query height, accounting for the world state and the column cover. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    EMinecraftPrecipitation Precipitation = EMinecraftPrecipitation::Unknown;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Intensity = 0.0f;
};

USTRUCT(BlueprintType)
struct MINECRAFTUEVIEWER_API FMinecraftClimateWeights
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Temperate = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Dry = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Cold = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Humid = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float Unknown = 1.0f;
};

USTRUCT(BlueprintType)
struct MINECRAFTUEVIEWER_API FMinecraftPrecipitationGrid
{
    GENERATED_BODY()

    /** Point-sampled RGBA16F. R is approximate absolute UE Z in cm; G is 0/.5/1 for None/Rain/Snow;
        B marks a known height within half-float range. Use the CPU query for exact heights or missing texels.
        Texel = floor(Minecraft XZ) - OriginXZ. Read only when bValid and inside the grid. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    TObjectPtr<UTexture2D> Texture = nullptr;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    FIntPoint OriginXZ = FIntPoint::ZeroValue;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int32 SizeInBlocks = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bValid = false;
};

USTRUCT(BlueprintType)
struct MINECRAFTUEVIEWER_API FMinecraftLightningEvent
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 Sequence = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 WorldGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 SceneSerial = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 EntityId = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    int64 Seed = 0;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    FVector MinecraftPosition = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    FVector WorldLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    float ReportedRemainingSeconds = 0.0f;

    /** A later flash carried by the same Minecraft entity in this scene. */
    UPROPERTY(BlueprintReadOnly, Category="Minecraft|Weather")
    bool bIsReflash = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMinecraftLightningReceived, const FMinecraftLightningEvent&, Event);

/** Minecraft's weather data for Blueprint and native consumers, scoped to the current UE world. */
UCLASS(BlueprintType)
class MINECRAFTUEVIEWER_API UMinecraftWeatherSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintPure, Category="Minecraft|Weather")
    FMinecraftWeatherSnapshot GetWeatherSnapshot() const;

    /** Sample Minecraft's weather using an absolute UE position in centimeters. */
    UFUNCTION(BlueprintPure, Category="Minecraft|Weather")
    FMinecraftWeatherSample SampleWeatherAt(FVector WorldLocation) const;

    /** Bilinear blend of four column profiles for presentation. Weights sum to one, including missing data. */
    UFUNCTION(BlueprintPure, Category="Minecraft|Weather")
    FMinecraftClimateWeights SampleClimateWeightsAt(FVector WorldLocation) const;

    /** Grid and metadata for binding a GPU material or Niagara user parameter once per update. */
    UFUNCTION(BlueprintPure, Category="Minecraft|Weather")
    FMinecraftPrecipitationGrid GetPrecipitationGrid() const;

    /** Most recent Minecraft lightning entity event in this scene, or false after a world reset. */
    UFUNCTION(BlueprintPure, Category="Minecraft|Weather")
    bool GetLastLightningEvent(FMinecraftLightningEvent& Event) const;

    /** Deduplication holds the newest 1024 entities and up to 16 reported flashes per entity. */
    UPROPERTY(BlueprintAssignable, Category="Minecraft|Weather")
    FMinecraftLightningReceived OnLightning;

private:
    friend class ABridgeViewer;

    void ResetMinecraftWorld(bool bHasWorld);
    bool RecordLightning(uint32 Scene, uint32 EntityId, uint32 Seed, const FVector& PositionMC, float Remaining);

    static constexpr int32 MaxTrackedLightningEntities = 1024;
    static constexpr int32 MaxTrackedFlashesPerEntity = 16;
    TWeakObjectPtr<ABridgeViewer> Bridge;
    TMap<uint32, TArray<uint32, TInlineAllocator<4>>> SeenLightningSeeds;
    uint32 RecentLightningEntities[MaxTrackedLightningEntities];
    int32 NextLightningEntity = 0;
    TOptional<FMinecraftLightningEvent> LastLightningEvent;
    int64 WorldGeneration = 0;
    int64 LightningSequence = 0;
};
