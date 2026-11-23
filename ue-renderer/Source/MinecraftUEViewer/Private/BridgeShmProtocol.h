#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMemory.h"
#include "BridgeWire.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace BridgeShm
{
    constexpr uint32 FlagInWorld = 1u << 0;
    constexpr uint32 FlagGameplayLook = 1u << 1;
    constexpr uint32 FlagGuiOpen = 1u << 2;
    constexpr uint32 FlagIsPaused = 1u << 3;
    constexpr uint32 FlagSneaking = 1u << 4;
    constexpr uint32 FlagSprinting = 1u << 5;
    constexpr uint32 FlagSwimming = 1u << 6;
    constexpr uint32 FlagUsingItem = 1u << 7;
    constexpr uint32 FlagHasBlockHit = 1u << 8;
    constexpr uint32 FlagInvertX = 1u << 9;
    constexpr uint32 FlagInvertY = 1u << 10;
    constexpr uint32 FlagHasEyePose = 1u << 11;

    constexpr uint32 LookFlagActive = 1u << 0;
    constexpr uint64 CapWaterBubbles = 1ull << 0;
    constexpr uint64 CapCoveredFaceLights = 1ull << 1;
    constexpr uint64 CapSelectionOutline = 1ull << 2;
    constexpr uint64 CapVegetationShading = 1ull << 3;
    constexpr uint64 CapMaterialLights = 1ull << 4;
    constexpr uint64 CapTextureEmission = 1ull << 5;
    constexpr uint64 CapGlassLighting = 1ull << 6;
    constexpr uint64 CapMineralGlow = 1ull << 7;

    struct alignas(64) FShmHeader
    {
        uint32 Magic;
        uint32 Version;
        uint64 Nonce;
        uint64 UeCapabilities;
        uint64 MappingBytes;
        uint64 Reserved[4];
    };

    struct FMcToUeState
    {
        uint32 WorldSerial;
        uint32 Flags;
        uint64 TickId;
        uint64 TickTimeNs;
        uint64 OverrideEpoch;
        uint32 TeleportEpoch;
        int32 HitBlockX;
        int32 HitBlockY;
        int32 HitBlockZ;
        double PrevX;
        double PrevY;
        double PrevZ;
        double CurrX;
        double CurrY;
        double CurrZ;
        double MouseFactor;
        float PrevEyeHeight;
        float CurrEyeHeight;
        float ForcedYaw;
        float ForcedPitch;
        float VerticalFov;
        float AspectRatio;
        float MainHandSwing;
        float MainHandEquip;
        float OffHandSwing;
        float OffHandEquip;
        float UseProgress;
        uint8 HitFace;
        uint8 UseAction;
        uint16 SelectionRevision;
        uint64 MainHandModelHash;
        uint64 OffHandModelHash;
        double EyeX;
        double EyeY;
        double EyeZ;
    };

    struct FUeToMcLook
    {
        uint64 AckOverrideEpoch;
        uint64 TimestampNs;
        uint32 WorldSerial;
        uint32 CaptureId;
        uint32 LookSerial;
        float Yaw;
        float Pitch;
        uint32 Flags;
    };

    constexpr int32 CameraSampleBytes = 96;
    constexpr uint8 CameraMovementKnown = 1u << 0;
    constexpr uint8 CameraGroundedOnFoot = 1u << 1;
    struct FCameraSample
    {
        uint8 Bytes[CameraSampleBytes]; // Big-endian pose, u32 look capture, u8 movement flags, then two zero bytes.
    };

    template <size_t Bytes>
    struct alignas(64) FSequenceBlock
    {
        static_assert(Bytes % sizeof(uint64) == 0);
        uint64 Seq;
        uint8 PadSeq[56];
        uint64 Words[Bytes / sizeof(uint64)];
    };

    struct alignas(64) FControlSharedMemory
    {
        FShmHeader Header;
        FSequenceBlock<sizeof(FMcToUeState)> McToUe;
        FSequenceBlock<sizeof(FUeToMcLook)> UeToMcLook;
        FSequenceBlock<sizeof(FCameraSample)> Camera;
    };

    constexpr uint16 MaxSelectionEdges = 256;
    struct FSelectionEdge
    {
        FVector3f Start;
        FVector3f End;
    };

    struct FSelectionOutline
    {
        uint16 Revision;
        uint16 EdgeCount;
        uint32 Reserved;
        FSelectionEdge Edges[MaxSelectionEdges];
    };

    struct alignas(64) FExtendedControlSharedMemory
    {
        FControlSharedMemory Base;
        FSequenceBlock<sizeof(FSelectionOutline)> Selection;
    };

    static_assert(std::atomic_ref<uint64>::is_always_lock_free);
    static_assert(sizeof(FShmHeader) == 64);
    static_assert(sizeof(FMcToUeState) == 192);
    static_assert(sizeof(FUeToMcLook) == 40);
    static_assert(sizeof(FControlSharedMemory) == 640);
    static_assert(sizeof(FSelectionEdge) == 24 && offsetof(FSelectionOutline, Edges) == 8);
    static_assert(sizeof(FExtendedControlSharedMemory) == 6912);
    static_assert(offsetof(FExtendedControlSharedMemory, Selection) == 640);
    static_assert(offsetof(FSequenceBlock<sizeof(FSelectionOutline)>, Words) == 64);
    static_assert(offsetof(FControlSharedMemory, McToUe) == 64);
    static_assert(offsetof(FControlSharedMemory, UeToMcLook) == 320);
    static_assert(offsetof(FControlSharedMemory, Camera) == 448);
    static_assert(offsetof(FSequenceBlock<sizeof(FMcToUeState)>, Words) == 64);
    static_assert(offsetof(FSequenceBlock<sizeof(FUeToMcLook)>, Words) == 64);
    static_assert(offsetof(FSequenceBlock<sizeof(FCameraSample)>, Words) == 64);
    static_assert(offsetof(FMcToUeState, MouseFactor) == 96);
    static_assert(offsetof(FMcToUeState, SelectionRevision) == 150);
    static_assert(offsetof(FMcToUeState, MainHandModelHash) == 152);
    static_assert(offsetof(FMcToUeState, EyeX) == 168);
    static_assert(BridgeWire::CameraPayloadBytes + sizeof(uint32) <= CameraSampleBytes);

    constexpr uint32 TerrainHeaderBytes = 64;
    constexpr uint32 TerrainPageBytes = 16 * 1024;
    constexpr uint32 TerrainBulkBytes = 1024 * TerrainPageBytes;
    constexpr uint32 TerrainPriorityBytes = 256 * TerrainPageBytes;
    constexpr uint32 MaxTerrainLightBytes = BridgeWire::SectionLightsHeaderBytes +
        BridgeWire::MaxSectionLights * BridgeWire::SectionLightBytes;
    constexpr uint32 MaxTerrainBodyBytes = BridgeWire::MaxSectionFrame - 1 + MaxTerrainLightBytes;
    constexpr uint32 TerrainLargeBytes = (MaxTerrainBodyBytes + TerrainPageBytes - 1) / TerrainPageBytes * TerrainPageBytes;
    constexpr uint32 TerrainCapacity = TerrainHeaderBytes + TerrainBulkBytes + TerrainPriorityBytes + TerrainLargeBytes;
    constexpr int32 MaxTerrainLeases = 96 + 32 + 1;

    struct FTerrainHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 Capacity;
        uint32 HeaderBytes;
        uint64 Nonce;
    };

    static_assert(offsetof(FTerrainHeader, Nonce) == 16);

    class FTerrainShmClient
    {
    public:
        ~FTerrainShmClient() { Unmap(); }

        bool Map(const FGuid& Id, uint64 Nonce)
        {
            Unmap();
            if (!Id.IsValid())
            {
                return false;
            }
            Region = FPlatformMemory::MapNamedSharedMemoryRegion(Id.ToString(EGuidFormats::DigitsWithHyphensInBraces),
                false, FPlatformMemory::ESharedMemoryAccess::Read, TerrainCapacity);
            const FTerrainHeader* Header = Region ? static_cast<const FTerrainHeader*>(Region->GetAddress()) : nullptr;
            if (!Header || Header->Magic != BridgeWire::Magic || Header->Version != BridgeWire::Version ||
                Header->Capacity != TerrainCapacity || Header->HeaderBytes != TerrainHeaderBytes || Header->Nonce != Nonce)
            {
                Unmap();
                return false;
            }
            return true;
        }

        void Unmap()
        {
            if (Region)
            {
                FPlatformMemory::UnmapNamedSharedMemoryRegion(Region);
                Region = nullptr;
            }
        }

        const uint8* Resolve(uint32 Offset, uint32 Bytes) const
        {
            if (!Region || Offset < TerrainHeaderBytes || Offset > TerrainCapacity ||
                (Offset - TerrainHeaderBytes) % TerrainPageBytes != 0 || Bytes > TerrainCapacity - Offset)
            {
                return nullptr;
            }
            return static_cast<const uint8*>(Region->GetAddress()) + Offset;
        }

    private:
        FPlatformMemory::FSharedMemoryRegion* Region = nullptr;
    };

    inline void ComputeCameraVectors(float YawDeg, float PitchDeg, FVector& OutForwardMC, FVector& OutForwardUE, FVector& OutUpUE)
    {
        // Matches Minecraft 26.2 Camera::setRotation(yRot, xRot) mapped through BridgeWire::ToUnreal(X, Y, Z) = (Z, -X, Y).
        const double YawRad = FMath::DegreesToRadians(double(YawDeg));
        const double PitchRad = FMath::DegreesToRadians(double(PitchDeg));
        const double CosP = FMath::Cos(PitchRad);
        const double SinP = FMath::Sin(PitchRad);
        const double CosY = FMath::Cos(YawRad);
        const double SinY = FMath::Sin(YawRad);
        OutForwardMC = FVector(-SinY * CosP, -SinP, CosY * CosP);
        OutForwardUE = FVector(CosY * CosP, SinY * CosP, -SinP);
        OutUpUE = FVector(CosY * SinP, SinY * SinP, CosP);
    }

    class FControlShmClient
    {
    public:
        ~FControlShmClient() { Unmap(); }

        bool Map(const FGuid& Id, uint64 Nonce)
        {
            Unmap();
            if (!Id.IsValid())
            {
                return false;
            }
            const uint32 Access = FPlatformMemory::ESharedMemoryAccess::Read | FPlatformMemory::ESharedMemoryAccess::Write;
            const FString Name = Id.ToString(EGuidFormats::DigitsWithHyphensInBraces);
            Region = FPlatformMemory::MapNamedSharedMemoryRegion(Name, false, Access, sizeof(FControlSharedMemory));
            if (!Region)
            {
                return false;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            auto* Candidate = static_cast<FControlSharedMemory*>(Region->GetAddress());
            if (!Candidate || Candidate->Header.Magic != BridgeWire::Magic ||
                Candidate->Header.Version != BridgeWire::Version || Candidate->Header.Nonce != Nonce)
            {
                Unmap();
                return false;
            }
            Memory = Candidate;
            if (Candidate->Header.MappingBytes == sizeof(FExtendedControlSharedMemory))
            {
                if (auto* ExtendedRegion = FPlatformMemory::MapNamedSharedMemoryRegion(
                    Name, false, Access, sizeof(FExtendedControlSharedMemory)))
                {
                    FPlatformMemory::UnmapNamedSharedMemoryRegion(Region);
                    Region = ExtendedRegion;
                    auto* Extended = static_cast<FExtendedControlSharedMemory*>(Region->GetAddress());
                    Memory = &Extended->Base;
                    Selection = &Extended->Selection;
                }
            }
            WriteSeq = std::atomic_ref(Memory->UeToMcLook.Seq).load(std::memory_order_acquire) & ~uint64(1);
            return true;
        }

        void Unmap()
        {
            Memory = nullptr;
            Selection = nullptr;
            WriteSeq = 0;
            if (Region)
            {
                FPlatformMemory::UnmapNamedSharedMemoryRegion(Region);
                Region = nullptr;
            }
        }

        bool IsMapped() const { return Memory != nullptr; }
        bool HasSelectionOutline() const { return Selection != nullptr; }

        void AdvertiseCapabilities(uint64 Capabilities)
        {
            if (Memory)
            {
                std::atomic_ref(Memory->Header.UeCapabilities).store(Capabilities, std::memory_order_release);
            }
        }

        bool PollState(FMcToUeState& OutState) const
        {
            uint64 Seq = 0;
            return Memory && ReadSnapshot(Memory->McToUe, OutState, Seq);
        }

        bool PollCamera(FCameraSample& OutPose, uint64& Seq) const
        {
            return Memory && ReadSnapshot(Memory->Camera, OutPose, Seq);
        }

        bool PollSelectionOutline(FSelectionOutline& OutOutline, uint64& Seq) const
        {
            return Selection && ReadSnapshot(*Selection, OutOutline, Seq);
        }

        void WriteLook(const FUeToMcLook& InLook)
        {
            if (!Memory)
            {
                return;
            }
            auto& Block = Memory->UeToMcLook;
            std::atomic_ref Seq(Block.Seq);
            Seq.store(WriteSeq + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            uint64 Words[sizeof(FUeToMcLook) / sizeof(uint64)];
            FMemory::Memcpy(Words, &InLook, sizeof(Words));
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Words); ++Index)
            {
                std::atomic_ref(Block.Words[Index]).store(Words[Index], std::memory_order_relaxed);
            }
            WriteSeq += 2;
            Seq.store(WriteSeq, std::memory_order_release);
        }

    private:
        template <typename T>
        static bool ReadSnapshot(FSequenceBlock<sizeof(T)>& Block, T& Out, uint64& LastSeq)
        {
            std::atomic_ref Seq(Block.Seq);
            for (int32 Retry = 0; Retry < 4; ++Retry)
            {
                const uint64 Before = Seq.load(std::memory_order_acquire);
                if ((Before & 1) != 0)
                {
                    continue;
                }
                if (Before == LastSeq)
                {
                    return false;
                }
                uint64 Words[sizeof(T) / sizeof(uint64)];
                for (int32 Index = 0; Index < UE_ARRAY_COUNT(Words); ++Index)
                {
                    Words[Index] = std::atomic_ref(Block.Words[Index]).load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (Before == Seq.load(std::memory_order_relaxed))
                {
                    FMemory::Memcpy(&Out, Words, sizeof(T));
                    LastSeq = Before;
                    return true;
                }
            }
            return false;
        }

        FPlatformMemory::FSharedMemoryRegion* Region = nullptr;
        FControlSharedMemory* Memory = nullptr;
        FSequenceBlock<sizeof(FSelectionOutline)>* Selection = nullptr;
        uint64 WriteSeq = 0;
    };
}
