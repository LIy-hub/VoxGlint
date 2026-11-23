#include "BridgeViewer.h"

#include "Camera/CameraActor.h"
#include "Components/LineBatchComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

namespace
{
    using namespace BridgeWire;

    constexpr int32 MaxActiveCracks = 64;
    constexpr double CrackSurfaceOffset = 0.15; // Centimeters, including terrain's cutout lift.

    FIntVector BlockSection(const FIntVector& Block)
    {
        return FIntVector(Block.X >> 4, Block.Y >> 4, Block.Z >> 4);
    }

    UProceduralMeshComponent* MakeEffectMesh(AActor& Owner, const FVector& Location, UMaterialInterface* Material)
    {
        auto* Mesh = NewObject<UProceduralMeshComponent>(&Owner);
        Mesh->SetupAttachment(Owner.GetRootComponent());
        Mesh->SetRelativeLocation(Location);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetCastShadow(false);
        Mesh->SetMaterial(0, Material);
        Mesh->SetVisibility(false);
        Mesh->RegisterComponent();
        return Mesh;
    }

    bool IsAtlasCoordinate(float Value)
    {
        return Value >= 0.0f && Value <= 1.0f;
    }
}

bool ABridgeViewer::ApplyCrackAtlas(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != 4 + CrackStages * CrackTextureBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    if (!CrackMaterials.IsEmpty())
    {
        return false;
    }

    auto* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Bridge/M_BlockCrack.M_BlockCrack"));
    if (!Base)
    {
        UE_LOG(LogTemp, Error, TEXT("Could not load Minecraft block crack material."));
        return false;
    }
    for (int32 Stage = 0; Stage < CrackStages; ++Stage)
    {
        auto* Texture = UTexture2D::CreateTransient(CrackTextureEdge, CrackTextureEdge, PF_B8G8R8A8, NAME_None,
            TConstArrayView64<uint8>(Reader.Data + Stage * CrackTextureBytes, CrackTextureBytes));
        if (!Texture)
        {
            return false;
        }
        Texture->SRGB = true;
        Texture->Filter = TF_Nearest;
        Texture->AddressX = TA_Wrap;
        Texture->AddressY = TA_Wrap;
        Texture->UpdateResource();
        auto* Material = UMaterialInstanceDynamic::Create(Base, this);
        Material->SetTextureParameterValue(TEXT("CrackStage"), Texture);
        CrackMaterials.Add(Material);
    }
    UE_CLOG(bTrace, LogTemp, Display, TEXT("Minecraft block crack textures ready: scene=%u stages=%d."),
        SceneSerial, CrackMaterials.Num());
    return true;
}

bool ABridgeViewer::ApplyCrackUpdate(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    constexpr int32 HeaderBytes = 21;
    if (Size < HeaderBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const int32 SourceId = Reader.I32();
    const int32 X = Reader.I32();
    const int32 Y = Reader.I32();
    const int32 Z = Reader.I32();
    const FIntVector Block(X, Y, Z);
    const uint8 Stage = Reader.Byte();
    if (X < -MaxBlockCoordinate || X > MaxBlockCoordinate || Y < MinY || Y >= MinY + Height ||
        Z < -MaxBlockCoordinate || Z > MaxBlockCoordinate)
    {
        return false;
    }
    const FCrackState* Previous = Cracks.Find(Block);
    if (Stage == ClearCrackStage)
    {
        if (Size != HeaderBytes)
        {
            return false;
        }
        if (Previous && Previous->SourceId == SourceId)
        {
            RemoveCrack(Block);
            UE_CLOG(bTrace, LogTemp, Display,
                TEXT("Minecraft block crack cleared: scene=%u block=%s source=%d."), SceneSerial, *Block.ToString(), SourceId);
        }
        return true;
    }
    if (Stage >= CrackStages || Size < HeaderBytes + 4 || !CrackMaterials.IsValidIndex(Stage))
    {
        return false;
    }
    const uint32 Count = Reader.U32();
    if (Count > MaxCrackVertices || Count % 4 != 0 || Size != HeaderBytes + 4 + int32(Count) * CrackVertexBytes)
    {
        return false;
    }
    if (Count == 0)
    {
        RemoveCrack(Block);
        return true;
    }
    if (!Previous && Cracks.Num() >= MaxActiveCracks)
    {
        return true;
    }

    TArray<FVector> Vertices;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<int32> Triangles;
    Vertices.Reserve(Count);
    Normals.Reserve(Count);
    UVs.Reserve(Count);
    Triangles.Reserve(Count / 4 * 6);
    for (int32 Start = 0; Start < int32(Count); Start += 4)
    {
        for (int32 Index = 0; Index < 4; ++Index)
        {
            const float LocalX = Reader.Float();
            const float LocalY = Reader.Float();
            const float LocalZ = Reader.Float();
            const float U = Reader.Float();
            const float V = Reader.Float();
            if (!(FMath::Abs(LocalX) <= 4 && FMath::Abs(LocalY) <= 4 && FMath::Abs(LocalZ) <= 4 &&
                  FMath::Abs(U) <= 64 && FMath::Abs(V) <= 64))
            {
                return false;
            }
            Vertices.Add(ToUnreal(LocalX, LocalY, LocalZ) * BlockSize);
            UVs.Emplace(U, V);
        }
        const FVector EdgeU = Vertices[Start + 1] - Vertices[Start];
        const FVector EdgeV = Vertices[Start + 2] - Vertices[Start];
        const FVector Normal = FVector::CrossProduct(EdgeV, EdgeU).GetSafeNormal();
        for (int32 Index = 0; Index < 4; ++Index)
        {
            Vertices[Start + Index] += Normal * CrackSurfaceOffset;
            Normals.Add(Normal);
        }
        Triangles.Append({Start, Start + 1, Start + 2, Start, Start + 2, Start + 3});
    }

    const bool bChanged = !Previous || Previous->Stage != Stage || Previous->SourceId != SourceId;
    Cracks.Add(Block, {SourceId, Stage});
    auto& Mesh = CrackMeshes.FindOrAdd(Block);
    if (!Mesh)
    {
        Mesh = MakeEffectMesh(*this, ToUnreal(Block) * BlockSize, CrackMaterials[Stage]);
    }
    Mesh->SetMaterial(0, CrackMaterials[Stage]);
    Mesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, {}, {}, false);
    UE_CLOG(bTrace && bChanged, LogTemp, Display,
        TEXT("Minecraft block crack: scene=%u block=%s source=%d stage=%u vertices=%u."),
        SceneSerial, *Block.ToString(), SourceId, uint32(Stage), Count);
    return true;
}

void ABridgeViewer::TickBlockEffects()
{
    DrawBlockSelectionOutline();
    for (const auto& Pair : CrackMeshes)
    {
        Pair.Value->SetVisibility(HasTerrainSection(BlockSection(Pair.Key)));
    }
}

void ABridgeViewer::DrawBlockSelectionOutline()
{
    using namespace BridgeWire;
    if (!bHasMcState || McState.WorldSerial != SceneSerial || Height == 0 || bUiOpaque ||
        (McState.Flags & BridgeShm::FlagHasBlockHit) == 0 ||
        (McState.Flags & BridgeShm::FlagGuiOpen) != 0)
    {
        return;
    }
    const FIntVector Block(McState.HitBlockX, McState.HitBlockY, McState.HitBlockZ);
    if (!HasTerrainSection(BlockSection(Block)))
    {
        return;
    }
    UWorld* World = GetWorld();
    ULineBatchComponent* LineBatcher = World ? World->GetLineBatcher(UWorld::ELineBatcherType::World) : nullptr;
    if (!LineBatcher)
    {
        return;
    }
    constexpr double Expand = 0.0025;
    const FLinearColor OutlineColor(0.0f, 0.0f, 0.0f, 0.45f);
    if (McState.SelectionRevision != 0)
    {
        ControlShm.PollSelectionOutline(SelectionOutline, SelectionOutlineSequence);
        if (!Camera || SelectionOutline.Revision != McState.SelectionRevision ||
            SelectionOutline.EdgeCount > BridgeShm::MaxSelectionEdges)
        {
            return;
        }
        const FVector Eye = Camera->GetActorLocation();
        const auto ToPoint = [&Block, &Eye](const FVector3f& Offset)
        {
            const FVector Point = ToUnreal(Block.X + double(Offset.X), Block.Y + double(Offset.Y),
                Block.Z + double(Offset.Z)) * BlockSize;
            // Bias toward the eye so lines remain visible on the block surface.
            return Point + (Eye - Point).GetSafeNormal() * (Expand * BlockSize);
        };
        for (uint16 Index = 0; Index < SelectionOutline.EdgeCount; ++Index)
        {
            const BridgeShm::FSelectionEdge& Edge = SelectionOutline.Edges[Index];
            if (!Edge.Start.ContainsNaN() && !Edge.End.ContainsNaN())
            {
                LineBatcher->DrawLine(ToPoint(Edge.Start), ToPoint(Edge.End), OutlineColor, SDPG_World, 1.5f, 0.0f);
            }
        }
        return;
    }
    const double X0 = double(Block.X) - Expand;
    const double X1 = double(Block.X) + 1.0 + Expand;
    const double Y0 = double(Block.Y) - Expand;
    const double Y1 = double(Block.Y) + 1.0 + Expand;
    const double Z0 = double(Block.Z) - Expand;
    const double Z1 = double(Block.Z) + 1.0 + Expand;
    const FVector Corners[8] = {
        ToUnreal(X0, Y0, Z0) * BlockSize, ToUnreal(X1, Y0, Z0) * BlockSize,
        ToUnreal(X1, Y0, Z1) * BlockSize, ToUnreal(X0, Y0, Z1) * BlockSize,
        ToUnreal(X0, Y1, Z0) * BlockSize, ToUnreal(X1, Y1, Z0) * BlockSize,
        ToUnreal(X1, Y1, Z1) * BlockSize, ToUnreal(X0, Y1, Z1) * BlockSize
    };
    static constexpr int32 Edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    for (const auto& Edge : Edges)
    {
        LineBatcher->DrawLine(Corners[Edge[0]], Corners[Edge[1]], OutlineColor, SDPG_World, 1.5f, 0.0f);
    }
}

void ABridgeViewer::RemoveCrack(const FIntVector& Block)
{
    Cracks.Remove(Block);
    TObjectPtr<UProceduralMeshComponent> Mesh;
    if (CrackMeshes.RemoveAndCopyValue(Block, Mesh))
    {
        Mesh->DestroyComponent();
    }
}

void ABridgeViewer::DropBlockEffects(const FIntPoint& Column)
{
    for (auto It = CrackMeshes.CreateIterator(); It; ++It)
    {
        const FIntVector Section = BlockSection(It.Key());
        if (Section.X == Column.X && Section.Z == Column.Y)
        {
            Cracks.Remove(It.Key());
            It.Value()->DestroyComponent();
            It.RemoveCurrent();
        }
    }
}

void ABridgeViewer::ResetBlockEffects()
{
    SelectionOutline.Revision = 0;
    SelectionOutlineSequence = 0;
    for (const auto& Pair : CrackMeshes)
    {
        Pair.Value->DestroyComponent();
    }
    CrackMeshes.Reset();
    Cracks.Reset();
    CrackMaterials.Reset();
}
