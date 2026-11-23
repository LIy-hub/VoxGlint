#pragma once

#include "CoreMinimal.h"

namespace BridgeWire
{
    constexpr int32 Port = 39182;
    constexpr uint32 Magic = 0x4D435545;
    constexpr uint32 Version = 28;
    constexpr int32 ResetPayloadBytes = 16;
    constexpr int32 ControlShmBytes = 32;
    constexpr int32 TerrainShmBytes = 25;
    constexpr int32 CameraPayloadBytes = 89;
    enum class ELocalPlayerView : uint8
    {
        None, FirstPerson, World
    };
    constexpr int32 MaxHeight = 1024;
    constexpr int32 MaxViewDistance = 32;
    constexpr int32 MaxChunks = (2 * MaxViewDistance + 1) * (2 * MaxViewDistance + 1);
    constexpr int32 TerrainLayers = 6;
    constexpr int32 SectionHeaderBytes = 12 + TerrainLayers * 4;
    constexpr int32 SectionVertexBytes = 25;
    constexpr int32 SectionQuadBytes = 1 + 4 * SectionVertexBytes;
    constexpr uint8 GeometricNormal = 6;
    constexpr uint8 FaceIndexMask = 0x07;
    constexpr uint8 FaceEmissionMask = 0x38;
    constexpr uint8 FaceVegetationFlag = 0x80;
    constexpr int32 MaxSectionVertices = 1024 * 1024;
    constexpr int32 SectionWaterHeaderBytes = 8;
    constexpr int32 WaterFaceBytes = 16;
    constexpr int32 ShoreSegmentBytes = 17;
    constexpr int32 MaxShoreSegmentsPerQuad = 2;
    constexpr int32 ChunkWaterGridBytes = 12 + 256 * 4;
    constexpr int32 EnvironmentStateBytes = 52;
    constexpr int32 ChunkPrecipitationBytes = 12 + 256 * 2;
    constexpr uint16 PrecipHeightMask = 2047;
    constexpr uint16 PrecipValidBit = 1u << 14;
    constexpr uint8 WeatherCapabilityKnown = 1u << 0;
    constexpr uint8 WeatherCapabilityAllowed = 1u << 1;
    constexpr uint8 WeatherRaining = 1u << 2;
    constexpr uint8 WeatherThundering = 1u << 3;

    inline uint8 PrecipType(uint16 Cell) { return uint8((Cell >> 12) & 3u); }
    inline uint8 PrecipClimate(uint16 Cell) { return uint8(((Cell >> 11) & 1u) | ((Cell >> 14) & 2u)); }

    inline bool IsKnownPrecipColumn(uint16 Cell, int32 WorldHeight)
    {
        return (Cell & PrecipValidBit) != 0 && PrecipType(Cell) != 3 && (Cell & PrecipHeightMask) <= WorldHeight;
    }

    constexpr int32 LightningStrikeBytes = 40;
    constexpr int32 SectionLightsHeaderBytes = 18;
    constexpr int32 SectionLightBytes = 7;
    constexpr int32 MaxSectionLights = 16 * 16 * 16;
    constexpr int32 MaxMaterialPatches = 32;
    constexpr int32 MaterialPatchBytes = 23;
    constexpr int32 MaterialSampleBytes = 24;
    constexpr int32 SectionGlassHeaderBytes = 24;
    constexpr int32 GlassMaterialBytes = 16;
    constexpr int32 GlassBoxBytes = 28;
    constexpr int32 MaxGlassPartEntries = 4096;
    constexpr int32 MaxGlassParts = 256;
    constexpr int32 MaxMaterialLightBytes = SectionLightsHeaderBytes + MaxSectionLights *
        (SectionLightBytes + 5 + MaxMaterialPatches * (MaterialPatchBytes + MaterialSampleBytes));
    constexpr int32 MaxSectionFrame = 1 + SectionHeaderBytes + SectionWaterHeaderBytes + MaxSectionVertices / 4 *
        (SectionQuadBytes + MaxShoreSegmentsPerQuad * ShoreSegmentBytes);
    constexpr int32 MaxFrame = 1024 * 1024; // Transport frames exclude section bodies.
    constexpr int32 MaxAtlasEdge = 8192;
    constexpr int32 MaxAtlasMips = 14;
    constexpr int32 AtlasRowsHeaderBytes = 12;
    constexpr int32 AtlasPacketBytes = 256 * 1024;
    constexpr int32 AtlasPatchHeaderBytes = 10;
    constexpr int32 AtlasPatchRegionBytes = 10;
    constexpr int32 MaxAtlasPatchRegions = 256;
    constexpr int32 MaxUiEdge = 8192;
    constexpr int32 MaxUiPixels = 4096 * 4096;
    constexpr int32 UiSlots = 3;
    constexpr int32 UiSlotBytes = MaxUiPixels * 4;
    constexpr int32 MaxBlockCoordinate = 32000000;
    constexpr int32 MaxChunkCoordinate = MaxBlockCoordinate / 16;
    constexpr double BlockSize = 100.0;
    constexpr int32 CrackStages = 10;
    constexpr int32 CrackTextureEdge = 16;
    constexpr int32 CrackTextureBytes = CrackTextureEdge * CrackTextureEdge * 4;
    constexpr uint8 ClearCrackStage = 255;
    constexpr int32 CrackVertexBytes = 20;
    constexpr int32 MaxCrackVertices = 8192;
    constexpr int32 DynamicTextureHeaderBytes = 16;
    constexpr int32 DynamicMeshHeaderBytes = 19;
    constexpr int32 DynamicBatchHeaderBytes = 11;
    constexpr int32 DynamicInstanceBytes = 52;
    constexpr int32 DynamicVertexBytes = 28;
    constexpr int32 MaxDynamicTextures = 512;
    constexpr int32 MaxDynamicTexturePixels = 4096 * 1024;
    constexpr int32 MaxDynamicScenePixels = 16 * 1024 * 1024;
    constexpr int32 MaxDynamicPartVertices = 8192;
    constexpr int32 MaxDynamicWorldVertices = 32768;
    constexpr int32 MaxDynamicHandVertices = 8192;
    constexpr int32 MaxDynamicLocalPlayerVertices = 8192;
    constexpr int32 MaxParticleVertices = 65536;
    constexpr float MaxParticleLocalPosition = 4096.0f;
    constexpr uint8 BlendMasked = 0;
    constexpr uint8 BlendAlpha = 1;
    constexpr uint8 BlendAddAlpha = 2;
    constexpr uint8 BlendPremultiplied = 3;
    constexpr uint8 BlendAdd = 4;
    constexpr uint8 ParticleBlendMask = 7;
    constexpr uint8 ParticleNoDepthTest = 8;
    constexpr int32 MaxDynamicInstances = 512;
    constexpr uint32 DynamicWhite = 0;
    constexpr uint32 DynamicBlockAtlas = 1;
    constexpr int32 DynamicWorld = 0;
    constexpr int32 DynamicHand = 1;
    constexpr int32 DynamicLocalPlayer = 2;
    constexpr int32 DynamicParticles = 3;
    constexpr int32 DynamicSpaceCount = 4;
    constexpr int32 InputBaseSize = 65;
    constexpr int32 MaxTerrainReleases = 32;
    constexpr int32 ReleasedUiSlotsOffset = 25;
    constexpr int32 InputEventSize = 6;
    constexpr int32 MaxInputEventsPerFrame = 64;
    constexpr int32 MaxInputFrameBytes = 4 + InputBaseSize + InputEventSize * MaxInputEventsPerFrame + MaxTerrainReleases * 8;
    constexpr int32 MaxPendingInputEvents = 512;
    constexpr int32 MaxFileDropBytes = 32 * 1024;
    constexpr int32 MaxDroppedFiles = 32;
    constexpr int32 MaxFilePathBytes = 4096;
    constexpr float MaxMouseDelta = 16384.0f;
    constexpr int32 PointerScale = 8192;
    constexpr float MaxWheelDelta = 512.0f;
    constexpr uint8 ScrollHorizontal = 1;
    constexpr uint8 ScrollScreen = 2;

    enum class EMessage : uint8
    {
        Reset = 1,
        SectionMesh = 2,
        DropChunk = 3,
        AtlasRows = 6,
        Input = 8,
        InputAvailability = 9,
        Ui = 10,
        World = 11,
        Window = 12,
        Close = 13,
        WindowMode = 14,
        SceneReady = 15,
        VideoSettings = 16,
        CrackAtlas = 17,
        CrackUpdate = 18,
        FileDrop = 20,
        ControlShm = 21,
        GpuUi = 23,
        EnvironmentState = 24,
        SectionLights = 25,
        NormalAtlasRows = 27,
        PbrAtlasRows = 28,
        DynamicTextureRows = 29,
        DynamicMesh = 30,
        InstanceBatch = 31,
        AtlasPatch = 32,
        AtlasReference = 33,
        AtlasUse = 34,
        ChunkPrecipitation = 35,
        LightningStrike = 36,
        ChunkWaterGrid = 37,
        TerrainShm = 38,
        ChunkWaterBubbles = 39,
        SectionGlass = 40
    };

    enum class EAtlasCacheStatus : uint8 { None, Rejected, Ready, Preparing };

    enum class ETerrainAtlas : uint8 { Albedo, Normal, Pbr, Count };

    enum class EDimension : uint8
    {
        Overworld,
        Nether,
        End,
        Other
    };

    enum class ECameraMedium : uint8
    {
        Other,
        Underwater,
        InLava
    };

    struct FEnvironmentState
    {
        uint32 Scene = 0;
        EDimension Dimension = EDimension::Overworld;
        ECameraMedium Medium = ECameraMedium::Other;
        uint8 MoonPhase = 0;
        uint8 Flags = 0;
        float SunAngle = 0.0f;
        float GlobalRain = 0.0f;
        float Thunder = 0.0f;
        float LocalRain = 0.0f;
        float LocalSnow = 0.0f;
        float SkyFlash = 0.0f;
        FColor SkyColor = FColor::White;
        FVector3f FogRgb = FVector3f::ZeroVector;
        float GameTimeSec = 0.0f;
    };

    struct FLightningStrike
    {
        uint32 EntityId = 0;
        FVector PositionMC = FVector::ZeroVector;
        uint32 Seed = 0;
        float RemainingSeconds = 0.0f;
        float TotalSeconds = 0.0f;
    };

    struct FEmitterFootprint
    {
        uint32 UV[4]{};
        uint8 Mode = 0;
        bool operator==(const FEmitterFootprint& Other) const
        {
            return Mode == Other.Mode && FMemory::Memcmp(UV, Other.UV, sizeof(UV)) == 0;
        }
        friend uint32 GetTypeHash(const FEmitterFootprint& Key)
        {
            return HashCombineFast(HashCombineFast(Key.UV[0], Key.UV[1]),
                HashCombineFast(HashCombineFast(Key.UV[2], Key.UV[3]), Key.Mode));
        }
    };

    struct FEmitterPatch
    {
        FVector3f Center, Normal, WidthAxis; // Block-local Minecraft coordinates.
        float Width, Height, Power;
        FColor Color;
        FEmitterFootprint Footprint;
        FColor SampleTint = FColor::White;
        float SampleArea = 0;
    };

    struct FSectionLight
    {
        FIntVector Block;
        FColor Color;
        uint8 Level; // Nominal radius in blocks at one light level per block.
        uint8 Flags;
        bool bMaterial = false;
        TArray<FEmitterPatch> Patches;
    };

    enum class EInputMode : uint8
    {
        Unavailable,
        Gameplay,
        Screen,
        Text,
        Keyboard
    };

    enum class ETerrainFiltering : uint8
    {
        None,
        Rgss,
        Anisotropic
    };

    enum class EInputAction : uint8
    {
        Release,
        Press,
        Repeat,
        Character,
        PointerMove,
        PointerDown,
        PointerUp,
        GameplayDown,
        GameplayUp,
        Scroll
    };

    struct FInputEvent
    {
        EInputAction Action;
        uint32 Value;
        uint8 Flags; // GLFW modifiers for buttons and keys, axis and mode for scroll.
    };

    inline bool IsUiSize(int32 Width, int32 Height)
    {
        return Width > 0 && Height > 0 && Width <= MaxUiEdge && Height <= MaxUiEdge && Width * Height <= MaxUiPixels;
    }

    inline uint32 ReadBigEndian(const uint8* Data)
    {
        return (uint32(Data[0]) << 24) | (uint32(Data[1]) << 16) | (uint32(Data[2]) << 8) | Data[3];
    }

    inline void WriteBigEndian(uint8* Data, uint32 Value)
    {
        Data[0] = uint8(Value >> 24);
        Data[1] = uint8(Value >> 16);
        Data[2] = uint8(Value >> 8);
        Data[3] = uint8(Value);
    }

    inline void WriteFloat(uint8* Data, float Value)
    {
        uint32 Bits;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        WriteBigEndian(Data, Bits);
    }

    // Handlers validate payload sizes before advancing this reader.
    struct FReader
    {
        const uint8* Data;

        uint8 Byte()
        {
            return *Data++;
        }

        uint16 U16()
        {
            const uint16 Value = (uint16(Data[0]) << 8) | Data[1];
            Data += 2;
            return Value;
        }

        uint32 U32()
        {
            const uint32 Value = ReadBigEndian(Data);
            Data += 4;
            return Value;
        }

        int32 I32()
        {
            return static_cast<int32>(U32());
        }

        float Float()
        {
            const uint32 Bits = U32();
            float Value;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value));
            return Value;
        }

        double Double()
        {
            const uint64 High = U32();
            const uint64 Bits = (High << 32) | U32();
            double Value;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value));
            return Value;
        }
    };

    inline FVector ToUnreal(double X, double Y, double Z)
    {
        // Minecraft south, west and up become Unreal forward, right and up.
        return FVector(Z, -X, Y);
    }

    inline FVector ToUnreal(const FIntVector& Position)
    {
        return ToUnreal(Position.X, Position.Y, Position.Z);
    }
}
