#include "BridgeViewer.h"

#include "BridgeViewportClient.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Texture2D.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

struct ABridgeViewer::FUiUpload
{
    TArray<uint8> Pixels;
    FUpdateTextureRegion2D Region;
};

bool ABridgeViewer::ApplyWindow(BridgeWire::FReader Reader, int32 Size)
{
    if (bWindowReceived || Size != 13)
    {
        return false;
    }
    const FIntPoint Position{Reader.I32(), Reader.I32()};
    const FIntPoint Dimensions{Reader.U16(), Reader.U16()};
    const uint8 Flags = Reader.Byte();
    if (Flags > 1 || !BridgeWire::IsUiSize(Dimensions.X, Dimensions.Y))
    {
        return false;
    }
    bSingleWindow = Flags == 1;
    bWindowReceived = true;
    Camera->GetCameraComponent()->SetConstraintAspectRatio(!bSingleWindow);
    if (bSingleWindow)
    {
        if (UBridgeViewportClient* Viewport = InputViewport.Get())
        {
            Viewport->ApplyInitialMinecraftWindow(Position, Dimensions);
        }
    }
    bInputDirty = true;
    UE_LOG(LogTemp, Display, TEXT("Minecraft window: client origin=%d,%d size=%dx%d singleWindow=%d."),
        Position.X, Position.Y, Dimensions.X, Dimensions.Y, int32(bSingleWindow));
    return true;
}

bool ABridgeViewer::ApplyWindowMode(BridgeWire::FReader Reader, int32 Size)
{
    if (!bWindowReceived || !bSingleWindow || Size != 1)
    {
        return false;
    }
    const uint8 Fullscreen = Reader.Byte();
    if (Fullscreen > 1)
    {
        return false;
    }
    if (UBridgeViewportClient* Viewport = InputViewport.Get())
    {
        Viewport->SetMinecraftFullscreen(Fullscreen == 1);
    }
    return true;
}

bool ABridgeViewer::ApplyUi(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (!bWindowReceived || Size != 26)
    {
        return false;
    }
    const FGuid MemoryId{Reader.U32(), Reader.U32(), Reader.U32(), Reader.U32()};
    const FIntPoint Dimensions{Reader.U16(), Reader.U16()};
    const int32 Slot = Reader.Byte();
    const uint8 Flags = Reader.Byte();
    const uint32 ReadbackMicros = Reader.U32();
    if (!MemoryId.IsValid() || !IsUiSize(Dimensions.X, Dimensions.Y) || Slot >= UiSlots || Flags > 1 ||
        Slot == PendingUiSlot || (ReleasedUiSlots & (1 << Slot)))
    {
        return false;
    }
    if (!UiMemory)
    {
        UiMemory = FPlatformMemory::MapNamedSharedMemoryRegion(MemoryId.ToString(EGuidFormats::DigitsWithHyphensInBraces),
            false, FPlatformMemory::ESharedMemoryAccess::Read, UiSlots * UiSlotBytes);
        UiMemoryId = MemoryId;
    }
    if (!UiMemory || UiMemoryId != MemoryId)
    {
        return false;
    }

    if (PendingUiSlot != INDEX_NONE)
    {
        ReleasedUiSlots |= 1 << PendingUiSlot;
        bInputDirty = true;
    }
    PendingUiSlot = Slot;
    PendingUiSize = Dimensions;
    bPendingUiOpaque = Flags == 1;
    UiReadbackMs = ReadbackMicros / 1000.0;
    return true;
}

void ABridgeViewer::UploadUi()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftHudUpload);
    if (PendingUiSlot == INDEX_NONE || !UiMemory)
    {
        return;
    }
    const double StartedAt = FPlatformTime::Seconds();
    const int32 Width = PendingUiSize.X;
    const int32 HeightPx = PendingUiSize.Y;
    const int32 Pitch = Width * 4;

    TSharedPtr<FUiUpload> Upload;
    for (TSharedPtr<FUiUpload>& Buffer : UiUploads)
    {
        if (!Buffer)
        {
            Buffer = MakeShared<FUiUpload>();
        }
        if (Buffer.IsUnique())
        {
            Upload = Buffer;
            break;
        }
    }
    if (!Upload)
    {
        // Keep the latest notice until a render-thread upload completes or a newer notice arrives.
        return;
    }
    TArray<uint8>& Pixels = Upload->Pixels;
    Pixels.SetNumUninitialized(Pitch * HeightPx, EAllowShrinking::No);
    // The producer cannot reuse a slot until we acknowledge this copy.
    FPlatformMisc::MemoryBarrier();
    const uint8* SharedPixels = static_cast<const uint8*>(UiMemory->GetAddress());
    FMemory::Memcpy(Pixels.GetData(), SharedPixels + PendingUiSlot * BridgeWire::UiSlotBytes, Pixels.Num());
    ReleasedUiSlots |= 1 << PendingUiSlot;
    PendingUiSlot = INDEX_NONE;
    bInputDirty = true;

    // MC's transparent target contains premultiplied sRGB bytes. UE samples straight sRGB.
    if (!bPendingUiOpaque)
    {
        static const struct FUnpremulTable
        {
            uint8 Values[256][256]{};
            FUnpremulTable()
            {
                for (int32 Alpha = 1; Alpha < 255; ++Alpha)
                {
                    for (int32 Channel = 0; Channel < 256; ++Channel)
                    {
                        Values[Alpha][Channel] = uint8(FMath::Min(255, (Channel * 255 + Alpha / 2) / Alpha));
                    }
                }
            }
        } Table;

        uint32* Words = reinterpret_cast<uint32*>(Pixels.GetData());
        const int32 PixelCount = Pixels.Num() / 4;
        for (int32 Index = 0; Index < PixelCount; ++Index)
        {
            const uint32 Word = Words[Index];
            const uint32 Alpha = Word >> 24;
            if (Alpha != 0 && Alpha != 255 && (Word & 0x00FFFFFFu) != 0)
            {
                const uint8* Row = Table.Values[Alpha];
                uint8* Color = reinterpret_cast<uint8*>(&Words[Index]);
                Color[0] = Row[Color[0]];
                Color[1] = Row[Color[1]];
                Color[2] = Row[Color[2]];
            }
        }
    }

    if (!UiTexture || UiTexture->GetSizeX() != Width || UiTexture->GetSizeY() != HeightPx)
    {
        UiTexture = UTexture2D::CreateTransient(Width, HeightPx, PF_R8G8B8A8, NAME_None, Pixels);
        if (!UiTexture)
        {
            return;
        }
        UiTexture->SRGB = true;
        UiTexture->Filter = TF_Nearest;
        UiTexture->AddressX = TA_Clamp;
        UiTexture->AddressY = TA_Clamp;
        UiTexture->UpdateResource();
    }
    else
    {
        Upload->Region = FUpdateTextureRegion2D(0, 0, 0, 0, Width, HeightPx);
        UiTexture->UpdateTextureRegions(0, 1, &Upload->Region, Pitch, 4, Upload->Pixels.GetData(),
            [Upload](uint8*, const FUpdateTextureRegion2D*) {});
    }
    bUiOpaque = bPendingUiOpaque;
    UiUploadMs = (FPlatformTime::Seconds() - StartedAt) * 1000;
    ++UiFrames;
}
