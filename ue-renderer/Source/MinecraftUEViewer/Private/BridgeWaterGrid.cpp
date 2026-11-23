#include "BridgeViewer.h"

#include "Engine/Texture2D.h"

namespace
{
    constexpr int32 WaterGridSize = 256;
    constexpr int32 ShoreLimit = 15;
    constexpr int32 FetchLimit = 32;
    constexpr uint32 Known = 1u << 26;
    constexpr uint32 Present = 1u << 27;
    constexpr uint32 Sky = 1u << 28;
    constexpr uint32 Ice = 1u << 29;
    constexpr uint32 Obstacle = 1u << 30;
    constexpr uint32 HeightMask = (1u << 17) - 1;
    const FIntPoint Neighbors[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    bool HasWater(uint32 Cell)
    {
        return (Cell & (Known | Present)) == (Known | Present);
    }

    int32 FixedHeight(uint32 Cell)
    {
        return (Cell & 2047) * 64 + ((Cell & HeightMask) >> 11);
    }

    bool SameLayer(uint32 A, uint32 B)
    {
        return HasWater(B) && FMath::Abs(FixedHeight(A) - FixedHeight(B)) <= 35;
    }

    void Upload(UTexture2D& Texture, const FIntRect& Rect, TArray<FColor>&& Colors)
    {
        auto* Pixels = new TArray<FColor>(MoveTemp(Colors));
        auto* Region = new FUpdateTextureRegion2D(Rect.Min.X, Rect.Min.Y, 0, 0, Rect.Width(), Rect.Height());
        Texture.UpdateTextureRegions(0, 1, Region, Rect.Width() * sizeof(FColor), sizeof(FColor),
            reinterpret_cast<uint8*>(Pixels->GetData()),
            [Pixels](uint8*, const FUpdateTextureRegion2D* Updated)
            {
                delete Pixels;
                delete Updated;
            });
    }

    FIntRect Intersection(const FIntRect& A, const FIntRect& B)
    {
        return FIntRect(FIntPoint(FMath::Max(A.Min.X, B.Min.X), FMath::Max(A.Min.Y, B.Min.Y)),
            FIntPoint(FMath::Min(A.Max.X, B.Max.X), FMath::Min(A.Max.Y, B.Max.Y)));
    }

    FIntRect Expanded(const FIntRect& Rect, int32 Radius)
    {
        const FIntPoint Pad(Radius, Radius);
        return FIntRect(Rect.Min - Pad, Rect.Max + Pad);
    }

    // The Chebyshev metric is a conservative lower bound to the Euclidean clearance.
    // The caller also subtracts the half-cell extent and the interpolation error.
    void PropagateClearance(TArray<uint8>& Distance, int32 Width, int32 Height)
    {
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            const int32 Step = Pass == 0 ? 1 : -1;
            const int32 Start = Pass == 0 ? 0 : Distance.Num() - 1;
            for (int32 Index = Start; Index >= 0 && Index < Distance.Num(); Index += Step)
            {
                uint8& Here = Distance[Index];
                if (Here == 0) continue;
                const int32 X = Index % Width;
                const int32 Y = Index / Width;
                const bool bBefore = X - Step >= 0 && X - Step < Width;
                const bool bAfter = X + Step >= 0 && X + Step < Width;
                if (bBefore) Here = FMath::Min(Here, uint8(Distance[Index - Step] + 1));
                if (Y - Step < 0 || Y - Step >= Height) continue;
                const int32 PreviousRow = Index - Step * Width;
                Here = FMath::Min(Here, uint8(Distance[PreviousRow] + 1));
                if (bBefore) Here = FMath::Min(Here, uint8(Distance[PreviousRow - Step] + 1));
                if (bAfter) Here = FMath::Min(Here, uint8(Distance[PreviousRow + Step] + 1));
            }
        }
    }
}

void ABridgeViewer::InitializeWaterGrid()
{
    for (auto* Target : {&WaterGridTexture, &WaterFieldTexture})
    {
        *Target = UTexture2D::CreateTransient(WaterGridSize, WaterGridSize, PF_B8G8R8A8);
        if (UTexture2D* Texture = Target->Get())
        {
            Texture->SRGB = false;
            Texture->Filter = TF_Nearest;
            Texture->AddressX = TA_Clamp;
            Texture->AddressY = TA_Clamp;
            Texture->NeverStream = true;
            Texture->UpdateResource();
        }
    }
}

void ABridgeViewer::ResetWaterGrid()
{
    ChunkWaterData.Reset();
    DirtyWaterChunks.Reset();
    bWaterGridReady = false;
    ++WaterGridRevision;
    WaterGridOrigin = FIntPoint::ZeroValue;
}

bool ABridgeViewer::ApplyChunkWaterGrid(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != ChunkWaterGridBytes) return false;
    const uint32 Scene = Reader.U32();
    const int32 X = Reader.I32();
    const int32 Z = Reader.I32();
    if (Scene != SceneSerial || Height == 0 || X < -MaxChunkCoordinate || X > MaxChunkCoordinate ||
        Z < -MaxChunkCoordinate || Z > MaxChunkCoordinate) return false;

    TArray<uint32> Cells;
    Cells.SetNumUninitialized(256);
    for (uint32& Cell : Cells)
    {
        Cell = Reader.U32();
        if ((!HasWater(Cell) && Cell != 0 && Cell != Known) ||
            (HasWater(Cell) && (FixedHeight(Cell) > Height * 64 || ((Cell >> 17) & 31) == 0)))
        {
            return false;
        }
    }
    const FIntPoint Chunk(X, Z);
    if (!ChunkWaterData.Contains(Chunk) && ChunkWaterData.Num() >= MaxChunks) return false;
    if (!Cells.ContainsByPredicate([](uint32 Cell) { return Cell != 0; }))
    {
        if (ChunkWaterData.Remove(Chunk)) DirtyWaterChunks.Add(Chunk);
        return true;
    }
    ChunkWaterData.Add(Chunk, MoveTemp(Cells));
    DirtyWaterChunks.Add(Chunk);
    return true;
}

uint32 ABridgeViewer::WaterColumn(FIntPoint Block) const
{
    const auto* Cells = ChunkWaterData.Find(FIntPoint(Block.X >> 4, Block.Y >> 4));
    return Cells ? (*Cells)[(Block.Y & 15) * 16 + (Block.X & 15)] : 0;
}

void ABridgeViewer::UpdateWaterGrid()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftWaterGridUpdate);
    if (!WaterGridTexture || !WaterFieldTexture || CameraFrames == 0) return;
    const FIntPoint Center(FMath::FloorToInt(CameraPositionMC.X / 16.0), FMath::FloorToInt(CameraPositionMC.Z / 16.0));
    const FIntPoint Origin = (Center - FIntPoint(8, 8)) * 16;
    const FIntRect Window(Origin, Origin + FIntPoint(WaterGridSize, WaterGridSize));
    const bool bMoved = !bWaterGridReady || Origin != WaterGridOrigin;
    if (!bMoved && DirtyWaterChunks.IsEmpty()) return;
    if (bMoved && ChunkWaterData.IsEmpty())
    {
        bWaterGridReady = false;
        DirtyWaterChunks.Reset();
        return;
    }

    FIntRect Raw = Window;
    if (bMoved)
    {
        for (auto It = ChunkWaterData.CreateIterator(); It; ++It)
        {
            const FIntPoint Delta = It.Key() - Center;
            if (FMath::Max(FMath::Abs(Delta.X), FMath::Abs(Delta.Y)) > 12) It.RemoveCurrent();
        }
    }
    else
    {
        bool bFirst = true;
        for (const FIntPoint Chunk : DirtyWaterChunks)
        {
            const FIntRect ChunkRect(Chunk * 16, Chunk * 16 + FIntPoint(16, 16));
            if (bFirst)
            {
                Raw = ChunkRect;
                bFirst = false;
            }
            else
            {
                Raw.Min = FIntPoint(FMath::Min(Raw.Min.X, ChunkRect.Min.X), FMath::Min(Raw.Min.Y, ChunkRect.Min.Y));
                Raw.Max = FIntPoint(FMath::Max(Raw.Max.X, ChunkRect.Max.X), FMath::Max(Raw.Max.Y, ChunkRect.Max.Y));
            }
        }
    }
    DirtyWaterChunks.Reset();
    WaterGridOrigin = Origin;
    bWaterGridReady = true;
    ++WaterGridRevision;

    const FIntRect RawUpload = Intersection(Raw, Window);
    if (RawUpload.Width() > 0 && RawUpload.Height() > 0)
    {
        TArray<FColor> Pixels;
        Pixels.Reserve(RawUpload.Area());
        for (int32 Z = RawUpload.Min.Y; Z < RawUpload.Max.Y; ++Z)
        {
            for (int32 X = RawUpload.Min.X; X < RawUpload.Max.X; ++X)
            {
                const uint32 Cell = WaterColumn(FIntPoint(X, Z));
                Pixels.Emplace(uint8(Cell), uint8(Cell >> 8), uint8(Cell >> 16), uint8(Cell >> 24));
            }
        }
        Upload(*WaterGridTexture, FIntRect(RawUpload.Min - Origin, RawUpload.Max - Origin), MoveTemp(Pixels));
    }

    const FIntRect Field = Intersection(Expanded(Raw, FetchLimit), Window);
    if (Field.Width() <= 0 || Field.Height() <= 0) return;
    const FIntRect Work = Expanded(Field, FetchLimit + 1);
    const auto WorkIndex = [&](FIntPoint P)
    {
        return (P.Y - Work.Min.Y) * Work.Width() + (P.X - Work.Min.X);
    };
    TArray<uint32> Cells;
    TArray<uint8> Shore;
    TArray<uint8> Clearance;
    TArray<uint8> IceDistance;
    TArray<FIntPoint> Queue;
    TArray<FIntPoint> IceQueue;
    Cells.SetNumUninitialized(Work.Area());
    Shore.Init(ShoreLimit * 2, Work.Area());
    Clearance.Init(0, Work.Area());
    IceDistance.Init(4, Work.Area());
    for (int32 Z = Work.Min.Y; Z < Work.Max.Y; ++Z)
    {
        for (int32 X = Work.Min.X; X < Work.Max.X; ++X)
        {
            const FIntPoint P(X, Z);
            Cells[WorkIndex(P)] = WaterColumn(P);
        }
    }
    for (int32 Z = Work.Min.Y + 1; Z < Work.Max.Y - 1; ++Z)
    {
        for (int32 X = Work.Min.X + 1; X < Work.Max.X - 1; ++X)
        {
            const FIntPoint P(X, Z);
            const int32 Index = WorkIndex(P);
            const uint32 Cell = Cells[Index];
            if (!HasWater(Cell)) continue;
            bool bOpenSurface = (Cell & (Sky | Ice | Obstacle)) == Sky && ((Cell >> 17) & 31u) >= 2;
            if (Cell & Ice)
            {
                IceDistance[Index] = 0;
                IceQueue.Add(P);
            }
            for (const FIntPoint Step : Neighbors)
            {
                const uint32 N = Cells[WorkIndex(P + Step)];
                bOpenSurface &= HasWater(N) && FMath::Abs(FixedHeight(Cell) - FixedHeight(N)) <= 1;
                if (Shore[Index] != 1 && (N & Known) && !SameLayer(Cell, N))
                {
                    Shore[Index] = 1; // Cell centre is half a block from this shore.
                    Queue.Add(P);
                }
            }
            Clearance[Index] = bOpenSurface ? 8 : 0;
        }
    }
    PropagateClearance(Clearance, Work.Width(), Work.Height());
    for (int32 I = 0; I < Queue.Num(); ++I)
    {
        const FIntPoint P = Queue[I];
        const int32 From = WorkIndex(P);
        const uint8 NextDistance = Shore[From] + 2;
        if (NextDistance >= ShoreLimit * 2) continue;
        for (const FIntPoint Step : Neighbors)
        {
            const FIntPoint N = P + Step;
            if (!Work.Contains(N)) continue;
            const int32 To = WorkIndex(N);
            if (NextDistance < Shore[To] && SameLayer(Cells[From], Cells[To]))
            {
                Shore[To] = NextDistance;
                Queue.Add(N);
            }
        }
    }

    for (int32 I = 0; I < IceQueue.Num(); ++I)
    {
        const FIntPoint P = IceQueue[I];
        const int32 From = WorkIndex(P);
        const uint8 NextDistance = IceDistance[From] + 1;
        if (NextDistance >= 4) continue;
        for (const FIntPoint Step : Neighbors)
        {
            const FIntPoint N = P + Step;
            if (!Work.Contains(N)) continue;
            const int32 To = WorkIndex(N);
            if (NextDistance < IceDistance[To] && SameLayer(Cells[From], Cells[To]))
            {
                IceDistance[To] = NextDistance;
                IceQueue.Add(N);
            }
        }
    }

    // Wind heading is shared with PushEnvironmentMaterialParameters, in UE axes (1, 0.42).
    // Fetch marches in the opposite direction, in Minecraft axes (+0.42, -1).
    TArray<FColor> FieldPixels;
    FieldPixels.Reserve(Field.Area());
    for (int32 Z = Field.Min.Y; Z < Field.Max.Y; ++Z)
    {
        for (int32 X = Field.Min.X; X < Field.Max.X; ++X)
        {
            const FIntPoint P(X, Z);
            const int32 From = WorkIndex(P);
            const uint32 Cell = Cells[From];
            if (!HasWater(Cell))
            {
                FieldPixels.Emplace(0, 0, 0, 0);
                continue;
            }
            int32 Fetch = 0;
            for (int32 I = 1; I <= FetchLimit; ++I)
            {
                const uint32 N = Cells[WorkIndex(P + FIntPoint((I * 42 + 50) / 100, -I))];
                if (!SameLayer(Cell, N) || (N & Obstacle))
                {
                    break;
                }
                Fetch = I;
            }
            bool bRaisedLip = false;
            if (Shore[From] <= 4 && ((Cell >> 17) & 31u) > 1)
            {
                // Includes the source between four spreading arms even if its cardinal neighbours
                // are at nearly the same height. Compare only known water surfaces in this fallback.
                for (int32 DZ = -1; DZ <= 1 && !bRaisedLip; ++DZ)
                    for (int32 DX = -1; DX <= 1; ++DX)
                    {
                        const uint32 N = Cells[WorkIndex(P + FIntPoint(DX, DZ))];
                        if (HasWater(N) && FixedHeight(Cell) - FixedHeight(N) >= 64)
                        {
                            bRaisedLip = true;
                            break;
                        }
                    }
            }
            const uint8 IceWeight = uint8((4 - IceDistance[From]) * 127 / 4);
            FieldPixels.Emplace(Shore[From] * 8, FMath::Min(255, Fetch * 8), Clearance[From] * 16,
                (bRaisedLip ? 128 : 0) | IceWeight);
        }
    }
    Upload(*WaterFieldTexture, FIntRect(Field.Min - Origin, Field.Max - Origin), MoveTemp(FieldPixels));
    UE_CLOG(bTrace, LogTemp, VeryVerbose, TEXT("WaterGrid scene=%u raw=%d derived=%d uploadedBytes=%d"),
        SceneSerial, FMath::Max(0, RawUpload.Width()) * FMath::Max(0, RawUpload.Height()),
        Field.Area(), (FMath::Max(0, RawUpload.Width()) * FMath::Max(0, RawUpload.Height()) + Field.Area()) * 4);
}

