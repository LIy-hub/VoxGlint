#include "BridgeAtlasCache.h"

#include "BridgeViewer.h"

#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
    constexpr int32 AtlasKeyBytes = 32;
    constexpr int32 AtlasChecksumBytes = 20;
    constexpr int32 AtlasDiskHeaderBytes = 16 + AtlasKeyBytes + AtlasChecksumBytes;
    constexpr int32 AtlasReferenceBytes = 4 + AtlasKeyBytes + AtlasChecksumBytes;
    constexpr int32 AtlasReadChunkBytes = 1024 * 1024;
    constexpr int32 AtlasInstallBytesPerTick = 8 * 1024 * 1024;
    constexpr uint32 AtlasDiskMagic = 0x4d434154;
    constexpr uint32 AtlasDiskFormat = 1;

#if PLATFORM_WINDOWS
    struct FAtlasReadHandle
    {
        HANDLE Handle;
        ~FAtlasReadHandle()
        {
            if (Handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(Handle);
            }
        }

        bool FinalPath(FString& Result, bool bDirectory) const
        {
            BY_HANDLE_FILE_INFORMATION Info{};
            if (Handle == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(Handle, &Info) ||
                (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != bDirectory)
            {
                return false;
            }
            TCHAR Path[32768];
            const DWORD Size = ::GetFinalPathNameByHandleW(Handle, Path, UE_ARRAY_COUNT(Path), FILE_NAME_NORMALIZED);
            if (Size == 0 || Size >= UE_ARRAY_COUNT(Path))
            {
                return false;
            }
            Result = FString(Size, Path);
            return true;
        }

        bool Read(uint8* Target, DWORD Count) const
        {
            DWORD Bytes = 0;
            return ::ReadFile(Handle, Target, Count, &Bytes, nullptr) && Bytes == Count;
        }
    };

    TUniquePtr<FBridgeAtlasFile> LoadAtlasFile(const FString& Root, const FString& Key,
        const TArray<uint8>& Reference, const FThreadSafeBool& Cancelled)
    {
        // Never accept a path from TCP. Resolve handles and reject reparse points before reading bytes.
        const FString Name = Key + TEXT(".mcat");
        const FString Filename = FPaths::Combine(Root, Name);
        const FAtlasReadHandle Directory{::CreateFileW(*Root, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        const FAtlasReadHandle File{::CreateFileW(*Filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        FString ActualRoot, ActualFile;
        if (!Directory.FinalPath(ActualRoot, true) || !File.FinalPath(ActualFile, false) ||
            !ActualFile.Equals(ActualRoot + TEXT("\\") + Name, ESearchCase::IgnoreCase))
        {
            return nullptr;
        }

        uint8 Header[AtlasDiskHeaderBytes];
        LARGE_INTEGER FileBytes{};
        if (!::GetFileSizeEx(File.Handle, &FileBytes) || !File.Read(Header, sizeof(Header)) ||
            FMemory::Memcmp(Header + 16, Reference.GetData(), Reference.Num()) != 0)
        {
            return nullptr;
        }
        BridgeWire::FReader Reader{Header};
        if (Reader.U32() != AtlasDiskMagic || Reader.U32() != AtlasDiskFormat)
        {
            return nullptr;
        }
        const int32 Width = Reader.U16();
        const int32 Height = Reader.U16();
        const int32 Mips = Reader.U16();
        if (Reader.U16() != 0 || Width < 1 || Height < 1 || Width > BridgeWire::MaxAtlasEdge ||
            Height > BridgeWire::MaxAtlasEdge || Mips < 1 || Mips > BridgeWire::MaxAtlasMips ||
            (FMath::Min(Width, Height) >> (Mips - 1)) == 0)
        {
            return nullptr;
        }
        int64 ExpectedBytes = 0;
        for (int32 Mip = 0; Mip < Mips; ++Mip)
        {
            ExpectedBytes += int64(Width >> Mip) * (Height >> Mip) * 4;
        }
        ExpectedBytes *= int32(BridgeWire::ETerrainAtlas::Count);
        if (FileBytes.QuadPart != AtlasDiskHeaderBytes + ExpectedBytes || Cancelled)
        {
            return nullptr;
        }

        auto Result = MakeUnique<FBridgeAtlasFile>();
        Result->Width = Width;
        Result->Height = Height;
        Result->Mips = Mips;
        Result->Pixels.SetNumUninitialized(int32(ExpectedBytes));
        FSHA1 Checksum;
        for (int32 Offset = 0; Offset < Result->Pixels.Num();)
        {
            const int32 Count = FMath::Min(AtlasReadChunkBytes, Result->Pixels.Num() - Offset);
            uint8* Chunk = Result->Pixels.GetData() + Offset;
            if (Cancelled || !File.Read(Chunk, Count))
            {
                return nullptr;
            }
            Checksum.Update(Chunk, Count);
            Offset += Count;
        }
        Checksum.Final();
        uint8 Hash[AtlasChecksumBytes];
        Checksum.GetHash(Hash);
        if (FMemory::Memcmp(Hash, Header + 16 + AtlasKeyBytes, sizeof(Hash)) != 0)
        {
            return nullptr;
        }
        return Result;
    }
#endif
}

void ABridgeViewer::SetAtlasReply(BridgeWire::EAtlasCacheStatus Status)
{
    AtlasReplyScene = SceneSerial;
    AtlasReplyStatus = Status;
    bInputDirty = true;
}

void ABridgeViewer::ResetAtlasOffer()
{
    if (AtlasReadCancelled)
    {
        *AtlasReadCancelled = true;
    }
    PreparedAtlasFile.Reset();
    AtlasOfferIdentity.Reset();
    AtlasCacheIndex = AtlasCacheMip = AtlasCacheRow = AtlasCacheOffset = 0;
    bAtlasUseDisk = bAtlasStaging = false;
    SetAtlasReply(BridgeWire::EAtlasCacheStatus::None);
}

bool ABridgeViewer::ApplyAtlasReference(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != AtlasReferenceBytes || Height == 0 || Reader.U32() != SceneSerial ||
        !AtlasOfferIdentity.IsEmpty() || TerrainAtlases[0].Texture)
    {
        return false;
    }
    const FString Key = BytesToHex(Reader.Data, AtlasKeyBytes).ToLower();
    AtlasOfferIdentity = BytesToHex(Reader.Data, AtlasKeyBytes + AtlasChecksumBytes);
    if (AtlasOfferIdentity == ReusableAtlasIdentity)
    {
        SetAtlasReply(EAtlasCacheStatus::Ready);
        return true;
    }

    // A changed generation will allocate a new set; release the previous cache's ownership now.
    ReusableAtlasIdentity.Reset();
    for (FBridgeTerrainAtlasUpload& Previous : ReusableTerrainAtlases)
    {
        Previous = {};
    }

#if PLATFORM_WINDOWS
    FString Root;
    if (!FParse::Value(FCommandLine::Get(), TEXT("BridgeAtlasCacheRoot="), Root))
    {
        Root = FPlatformMisc::GetEnvironmentVariable(TEXT("MCUE_ATLAS_CACHE_ROOT"));
    }
    if (!AtlasRead.IsValid() && !Root.IsEmpty() && !FPaths::IsRelative(Root) && FPaths::GetCleanFilename(Root) == TEXT("minecraftue-atlas-cache"))
    {
        AtlasReadCancelled = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);
        TArray<uint8> Reference(Reader.Data, AtlasKeyBytes + AtlasChecksumBytes);
        AtlasRead = Async(EAsyncExecution::ThreadPool,
            [Root = MoveTemp(Root), Key, Reference = MoveTemp(Reference), Cancelled = AtlasReadCancelled]()
            {
                return LoadAtlasFile(Root, Key, Reference, *Cancelled);
            });
        SetAtlasReply(EAtlasCacheStatus::Preparing);
        return true;
    }
#endif
    SetAtlasReply(EAtlasCacheStatus::Rejected);
    return true;
}

bool ABridgeViewer::PollAtlasCache()
{
    using namespace BridgeWire;
    if (AtlasRead.IsValid() && AtlasRead.IsReady())
    {
        TUniquePtr<FBridgeAtlasFile> Result = AtlasRead.Consume();
        AtlasRead = {};
        const bool bCancelled = *AtlasReadCancelled;
        AtlasReadCancelled.Reset();
        if (!bCancelled)
        {
            PreparedAtlasFile = MoveTemp(Result);
            if (!PreparedAtlasFile)
            {
                UE_LOG(LogTemp, Display, TEXT("Minecraft terrain disk cache rejected scene=%u."), SceneSerial);
                SetAtlasReply(EAtlasCacheStatus::Rejected);
            }
        }
    }

    if (!PreparedAtlasFile)
    {
        return true;
    }
    const FBridgeAtlasFile& File = *PreparedAtlasFile;
    int32 Budget = AtlasInstallBytesPerTick;
    bAtlasStaging = true;
    while (Budget > 0 && AtlasCacheIndex < int32(ETerrainAtlas::Count))
    {
        FBridgeTerrainAtlasUpload& Atlas = TerrainAtlases[AtlasCacheIndex];
        if (!Atlas.Texture)
        {
            StartTerrainTexture(Atlas, File.Width, File.Height, File.Mips, AtlasCacheIndex == 0);
        }
        const int32 Width = File.Width >> AtlasCacheMip;
        const int32 HeightPx = File.Height >> AtlasCacheMip;
        const int32 Pitch = Width * 4;
        const int32 Rows = FMath::Min(HeightPx - AtlasCacheRow, FMath::Max(1, Budget / Pitch));
        auto& Mips = Atlas.Texture->GetPlatformData()->Mips;
        if (AtlasCacheRow == 0)
        {
            Mips.Add(new FTexture2DMipMap(Width, HeightPx, 1));
        }
        auto& Pixels = Mips[AtlasCacheMip].BulkData;
        void* Dest = Pixels.Lock(LOCK_READ_WRITE);
        if (AtlasCacheRow == 0)
        {
            Dest = Pixels.Realloc(Pitch * HeightPx);
        }
        FMemory::Memcpy(static_cast<uint8*>(Dest) + AtlasCacheRow * Pitch,
            File.Pixels.GetData() + AtlasCacheOffset, Rows * Pitch);
        Pixels.Unlock();
        Budget -= Rows * Pitch;
        AtlasCacheOffset += Rows * Pitch;
        AtlasCacheRow += Rows;
        if (AtlasCacheRow == HeightPx)
        {
            AtlasCacheRow = 0;
            Atlas.NextMip = ++AtlasCacheMip;
            if (AtlasCacheMip == File.Mips)
            {
                AtlasCacheMip = 0;
                ++AtlasCacheIndex;
            }
        }
    }
    if (AtlasCacheIndex == int32(ETerrainAtlas::Count))
    {
        PreparedAtlasFile.Reset();
        SetAtlasReply(EAtlasCacheStatus::Ready);
    }
    return true;
}

bool ABridgeViewer::ApplyAtlasUse(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != 4 || Reader.U32() != SceneSerial || AtlasReplyScene != SceneSerial ||
        AtlasReplyStatus != EAtlasCacheStatus::Ready || bAtlasUseDisk)
    {
        return false;
    }
    bAtlasUseDisk = true;
    bool bReuse = AtlasOfferIdentity == ReusableAtlasIdentity;
    if (bReuse)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(TerrainAtlases); ++Index)
        {
            TerrainAtlases[Index] = ReusableTerrainAtlases[Index];
        }
        BuildTerrainMaterials();
    }
    else
    {
        bAtlasStaging = false;
        FinishTerrainAtlases();
    }
    UE_LOG(LogTemp, Display, TEXT("Minecraft terrain atlas %s scene=%u (no TCP pixels)."),
        bReuse ? TEXT("GPU reuse") : TEXT("local cache"), SceneSerial);
    return true;
}
