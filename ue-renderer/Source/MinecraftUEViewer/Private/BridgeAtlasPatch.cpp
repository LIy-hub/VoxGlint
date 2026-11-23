#include "BridgeViewer.h"

#include "Engine/Texture2D.h"
#include "Rendering/Texture2DResource.h"
#include "RHICommandList.h"

namespace
{
    struct FAtlasPatchRegion
    {
        int32 Mip;
        FUpdateTextureRegion2D Region;
        int32 Offset;
        int32 Pitch;
    };
}

bool ABridgeViewer::ApplyAtlasPatch(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < AtlasPatchHeaderBytes || Size > AtlasPacketBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const uint32 Id = Reader.U32();
    const int32 Count = Reader.U16();
    if (Count < 1 || Count > MaxAtlasPatchRegions)
    {
        return false;
    }
    const bool bBlockAtlas = Id == DynamicBlockAtlas;
    const auto& Albedo = TerrainAtlases[int32(ETerrainAtlas::Albedo)];
    UTexture2D* Texture = bBlockAtlas ? Albedo.Texture.Get() : DynamicTextures.FindRef(Id).Get();
    if (!Texture || (bBlockAtlas && (!Albedo.IsComplete() || TerrainMaterials.IsEmpty())))
    {
        return false;
    }
    FTexturePlatformData* Data = Texture->GetPlatformData();
    FTexture2DResource* Resource = Texture->GetResource() ? Texture->GetResource()->GetTexture2DResource() : nullptr;
    if (!Data || Data->PixelFormat != PF_B8G8R8A8 || !Resource)
    {
        return false;
    }

    const uint8* Payload = Reader.Data;
    const int32 PayloadBytes = Size - AtlasPatchHeaderBytes;
    const uint8* End = Payload + PayloadBytes;
    TArray<FAtlasPatchRegion> Regions;
    Regions.Reserve(Count);
    uint32 MipMask = 0;
    int32 PixelBytes = 0;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        if (End - Reader.Data < AtlasPatchRegionBytes)
        {
            return false;
        }
        const int32 Mip = Reader.U16();
        const int32 X = Reader.U16();
        const int32 Y = Reader.U16();
        const int32 Width = Reader.U16();
        const int32 HeightPx = Reader.U16();
        const int32 Pitch = Width * 4;
        const int64 Bytes = int64(Pitch) * HeightPx;
        if (!Data->Mips.IsValidIndex(Mip) || Width == 0 || HeightPx == 0 || Bytes > End - Reader.Data ||
            X + Width > Data->Mips[Mip].SizeX || Y + HeightPx > Data->Mips[Mip].SizeY)
        {
            return false;
        }
        Regions.Add({Mip, FUpdateTextureRegion2D(X, Y, 0, 0, Width, HeightPx), int32(Reader.Data - Payload), Pitch});
        MipMask |= 1u << Mip;
        PixelBytes += int32(Bytes);
        Reader.Data += Bytes;
    }
    if (Reader.Data != End)
    {
        return false;
    }

    McTerrain::FShadowAtlasCoverage ChangedAlpha;
    if (bBlockAtlas)
    {
        // A terrain filtering change can recreate the RHI texture from these mips.
        for (const FAtlasPatchRegion& Patch : Regions)
        {
            auto& Mip = Data->Mips[Patch.Mip];
            auto* Pixels = static_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
            const uint32 AtlasPitch = Mip.SizeX * 4;
            const FUpdateTextureRegion2D& Region = Patch.Region;
            uint8* Dest = Pixels + Region.DestY * AtlasPitch + Region.DestX * 4;
            const uint8* Src = Payload + Patch.Offset;
            bool bAlphaChanged = false;
            for (uint32 Row = 0; Row < Region.Height; ++Row)
            {
                uint8* DestRow = Dest + Row * AtlasPitch;
                const uint8* SrcRow = Src + Row * Patch.Pitch;
                for (int32 A = 3; !bAlphaChanged && A < Patch.Pitch; A += 4)
                {
                    bAlphaChanged = DestRow[A] != SrcRow[A];
                }
                FMemory::Memcpy(DestRow, SrcRow, Patch.Pitch);
            }
            Mip.BulkData.Unlock();
            if (bAlphaChanged)
            {
                // Include bilinear taps and the sub-texel sharpening offset at this mip.
                const FIntPoint Min(Region.DestX, Region.DestY);
                const FIntPoint Max = Min + FIntPoint(Region.Width, Region.Height);
                ChangedAlpha.Include(Min - FIntPoint(2, 2), Max + FIntPoint(2, 2), FIntPoint(Mip.SizeX, Mip.SizeY));
            }
        }
    }

    TArray<uint8> Pixels(Payload, PayloadBytes);
    // Init, texture updates, and release are ordered on the render thread.
    ENQUEUE_RENDER_COMMAND(ApplyMinecraftAtlasPatch)(
        [Resource, Regions = MoveTemp(Regions), Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& RHICmdList) mutable
        {
            FRHITexture* RhiTexture = Resource->TextureRHI.GetReference();
            if (!RhiTexture)
            {
                return;
            }
            const int32 FirstMip = Resource->GetPostInitState().AssetLODBias;
            for (const FAtlasPatchRegion& Patch : Regions)
            {
                const int32 Mip = Patch.Mip - FirstMip;
                if (Mip >= 0 && Mip < int32(RhiTexture->GetNumMips()))
                {
                    RHICmdList.UpdateTexture2D(RhiTexture, Mip, Patch.Region, Patch.Pitch, Pixels.GetData() + Patch.Offset);
                }
            }
            RHICmdList.EnqueueLambda([Pixels = MoveTemp(Pixels)](FRHICommandList&) {});
        });

    ++AtlasPatchFrames;
    AtlasPatchPixelBytes += PixelBytes;
    if (bBlockAtlas)
    {
        BlockAtlasPatchRegions += Count;
        BlockAtlasPatchMips |= MipMask;
        if (!ChangedAlpha.IsEmpty())
        {
            // Anisotropic and rotated-grid taps can extend far along a grazing footprint.
            const bool bWideFilter = !bTerrainMip0 && (TerrainFiltering == ETerrainFiltering::Rgss ||
                (TerrainFiltering == ETerrainFiltering::Anisotropic && !bTerrainNoAF && RequestedAnisotropy >= 2));
            for (const auto& Pair : TerrainRegions)
            {
                Pair.Value->InvalidateMaskedShadows(bWideFilter ? nullptr : &ChangedAlpha);
            }
        }
    }
    else
    {
        DynamicAtlasPatchRegions += Count;
    }
    return true;
}
