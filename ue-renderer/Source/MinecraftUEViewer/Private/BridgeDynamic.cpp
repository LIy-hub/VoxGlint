#include "BridgeDynamic.h"

#include "BridgeQuality.h"
#include "BridgeViewer.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftDynamic, Log, All);

namespace
{
    using namespace BridgeWire;
    using namespace BridgeDynamic;

    int32 VertexBudget(int32 Space)
    {
        if (Space == DynamicParticles)
        {
            return MaxParticleVertices;
        }
        if (Space == DynamicLocalPlayer)
        {
            return MaxDynamicLocalPlayerVertices;
        }
        return Space == DynamicHand ? MaxDynamicHandVertices : MaxDynamicWorldVertices;
    }

    FVector ToDynamicSpace(int32 Space, double X, double Y, double Z)
    {
        // View vertices use Minecraft's OpenGL camera basis (+X right, +Y up, -Z forward).
        return Space == DynamicHand ? FVector(-Z, X, Y) : ToUnreal(X, Y, Z);
    }

    bool Within(double Value, double Limit)
    {
        return FMath::Abs(Value) <= Limit;
    }

    bool ValidBlend(int32 Space, uint8 Blend)
    {
        if (Space == DynamicWorld) return Blend <= 7;
        if (Space != DynamicParticles)
        {
            return Blend <= BlendAlpha;
        }
        return (Blend & ~(ParticleBlendMask | ParticleNoDepthTest)) == 0 && (Blend & ParticleBlendMask) <= BlendAdd;
    }

    uint32 MaterialKey(int32 Space, uint32 Texture, uint8 Blend)
    {
        return (uint32(Space) << 16) | (Texture << 4) | Blend;
    }

    uint32 TextureId(uint32 Key)
    {
        return (Key & 0xffffu) >> 4;
    }

    void BuildIndices(TArray<int32>& Indices, int32 Topology, int32 Count)
    {
        if (Topology == 3)
        {
            Indices.Reserve(Count);
            for (int32 I = 0; I < Count; ++I)
            {
                Indices.Add(I);
            }
        }
        else if (Topology == 4)
        {
            Indices.Reserve(Count / 4 * 6);
            for (int32 I = 0; I < Count; I += 4)
            {
                Indices.Append({I, I + 1, I + 2, I, I + 2, I + 3});
            }
        }
        else
        {
            Indices.Reserve((Count - 2) * 3);
            for (int32 I = 2; I < Count; ++I)
            {
                if (Topology == 6)
                {
                    Indices.Append({0, I - 1, I});
                }
                else if ((I & 1) == 0)
                {
                    Indices.Append({I - 2, I - 1, I});
                }
                else
                {
                    Indices.Append({I - 1, I - 2, I});
                }
            }
        }
    }

    void UpdateComponent(UProceduralMeshComponent& Component, const FSpace& Space, const TArray<FInstance>& Instances,
        const FVector& Origin, bool bSetOrigin = true)
    {
        TArray<FVector> Positions;
        TArray<FVector> Normals;
        TArray<FVector2D> UVs;
        TArray<FVector2D> Light;
        TArray<FColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        TArray<int32> Indices;
        int32 VertexCount = 0;
        int32 IndexCount = 0;
        for (const FInstance& Instance : Instances)
        {
            const FMesh& Mesh = Space.Meshes.FindChecked(Instance.Mesh);
            VertexCount += Mesh.Vertices.Num();
            IndexCount += Mesh.Indices.Num();
        }
        Positions.Reserve(VertexCount);
        Normals.Reserve(VertexCount);
        UVs.Reserve(VertexCount);
        Light.Reserve(VertexCount);
        Colors.Reserve(VertexCount);
        Tangents.Reserve(VertexCount);
        Indices.Reserve(IndexCount);
        for (const FInstance& Instance : Instances)
        {
            const FMesh& Mesh = Space.Meshes.FindChecked(Instance.Mesh);
            const int32 Base = Positions.Num();
            const FVector Offset = Instance.Position - Origin;
            for (const FVertex& Vertex : Mesh.Vertices)
            {
                Positions.Add(Instance.Rotation.RotateVector(FVector(Vertex.Position) * Instance.Scale) + Offset);
                const FVector Normal = Instance.Rotation.RotateVector(FVector(Vertex.Normal));
                Normals.Add(Normal);
                const FVector Axis = FMath::Abs(Normal.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector;
                Tangents.Emplace(FVector::CrossProduct(Axis, Normal).GetSafeNormal(), false);
                UVs.Emplace(Vertex.UV);
                Light.Emplace((Vertex.Light & 0x0f) / 15.0, (Vertex.Light >> 4) / 15.0);
                Colors.Add(Vertex.Color);
            }
            for (int32 Index : Mesh.Indices)
            {
                Indices.Add(Base + Index);
            }
        }
        if (bSetOrigin)
        {
            Component.SetRelativeLocation(Origin);
        }
        const FProcMeshSection* Existing = Component.GetProcMeshSection(0);
        if (Existing && Existing->ProcVertexBuffer.Num() == Positions.Num() &&
            Existing->ProcIndexBuffer.Num() == Indices.Num() &&
            FMemory::Memcmp(Existing->ProcIndexBuffer.GetData(), Indices.GetData(), Indices.Num() * sizeof(int32)) == 0)
        {
            Component.UpdateMeshSection(0, Positions, Normals, UVs, Light, {}, {}, Colors, Tangents);
        }
        else
        {
            Component.CreateMeshSection(0, Positions, Indices, Normals, UVs, Light, {}, {}, Colors, Tangents, false);
        }
    }
}

BridgeDynamic::FInstance BridgeDynamic::FRigidMotion::Sample(double Now) const
{
    if (Duration <= 0 || Now >= Start + Duration)
    {
        return To;
    }
    const double Alpha = FMath::Clamp((Now - Start) / Duration, 0.0, 1.0);
    FInstance Result = To;
    Result.Position = FMath::Lerp(From.Position, To.Position, Alpha);
    Result.Rotation = FQuat::Slerp(From.Rotation, To.Rotation, Alpha);
    Result.Scale = FMath::Lerp(From.Scale, To.Scale, float(Alpha));
    return Result;
}

void ABridgeViewer::LoadDynamicMaterials()
{
    static const TCHAR* Paths[] = {
        TEXT("/Game/Bridge/M_McDynamicMasked.M_McDynamicMasked"),
        TEXT("/Game/Bridge/M_McDynamicTranslucent.M_McDynamicTranslucent"),
        TEXT("/Game/Bridge/M_McDynamicHandMasked.M_McDynamicHandMasked")
    };
    for (const TCHAR* Path : Paths)
    {
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path);
        DynamicMaterialBases.Add(Material);
        if (Material)
        {
            Material->EnsureIsComplete();
        }
        else
        {
            UE_LOG(LogMinecraftDynamic, Error, TEXT("Could not load Minecraft dynamic material %s."), Path);
        }
    }
    // Two copies of the material modes, with normal depth testing and with depth testing disabled.
    const TCHAR* ParticleModes[] = {TEXT("Masked"), TEXT("Alpha"), TEXT("Additive"), TEXT("Premultiplied")};
    for (const TCHAR* Suffix : {TEXT(""), TEXT("Overlay")})
    {
        for (const TCHAR* Mode : ParticleModes)
        {
            const FString Name = FString::Printf(TEXT("M_McParticle%s%s"), Mode, Suffix);
            const FString Path = FString::Printf(TEXT("/Game/Bridge/%s.%s"), *Name, *Name);
            UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path);
            ParticleMaterialBases.Add(Material);
            if (Material)
            {
                Material->EnsureIsComplete();
            }
            else
            {
                UE_LOG(LogMinecraftDynamic, Error, TEXT("Could not load Minecraft particle material %s."), *Path);
            }
        }
    }
}

bool ABridgeViewer::ApplyDynamicTextureRows(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < DynamicTextureHeaderBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const uint32 Id = Reader.U32();
    const int32 Width = Reader.U16();
    const int32 TexHeight = Reader.U16();
    const int32 First = Reader.U16();
    const int32 Rows = Reader.U16();
    if (Id < 2 || Id >= MaxDynamicTextures + 2 || Width < 1 || Width > MaxAtlasEdge ||
        TexHeight < 1 || TexHeight > MaxAtlasEdge || Width * TexHeight > MaxDynamicTexturePixels ||
        Rows < 1 || First + Rows > TexHeight || Width * Rows * 4 > AtlasPacketBytes ||
        Size != DynamicTextureHeaderBytes + Width * Rows * 4)
    {
        return false;
    }
    UTexture2D* Existing = DynamicTextures.FindRef(Id);
    const int32 OldPixels = Existing ? Existing->GetSizeX() * Existing->GetSizeY() : 0;
    const int32 Pixels = Width * TexHeight;
    auto& Pending = DynamicTextureUpload;
    if (First == 0)
    {
        if (DynamicTexturePixels - OldPixels + Pixels > MaxDynamicScenePixels)
        {
            return false;
        }
        Pending.Id = Id;
        Pending.Width = Width;
        Pending.Height = TexHeight;
        Pending.NextRow = 0;
        Pending.Pixels.SetNumUninitialized(Pixels * 4);
    }
    if (Id != Pending.Id || Width != Pending.Width || TexHeight != Pending.Height || First != Pending.NextRow)
    {
        return false;
    }
    FMemory::Memcpy(Pending.Pixels.GetData() + First * Width * 4, Reader.Data, Width * Rows * 4);
    Pending.NextRow += Rows;
    if (Pending.NextRow == TexHeight)
    {
        if (Existing && Width == Existing->GetSizeX() && TexHeight == Existing->GetSizeY())
        {
            struct FTextureUpdate
            {
                TArray<uint8> Pixels;
                FUpdateTextureRegion2D Region;
            };
            auto Update = MakeShared<FTextureUpdate>(FTextureUpdate{MoveTemp(Pending.Pixels),
                FUpdateTextureRegion2D(0, 0, 0, 0, Width, TexHeight)});
            Existing->UpdateTextureRegions(0, 1, &Update->Region, Width * 4, 4, Update->Pixels.GetData(),
                [Update](uint8*, const FUpdateTextureRegion2D*) {});
            Pending = {};
            return true;
        }
        auto* Texture = UTexture2D::CreateTransient(Width, TexHeight, PF_B8G8R8A8, NAME_None,
            TConstArrayView64<uint8>(Pending.Pixels.GetData(), Pending.Pixels.Num()));
        if (!Texture)
        {
            return false;
        }
        Texture->SRGB = true;
        Texture->Filter = TF_Nearest;
        Texture->AddressX = TA_Wrap;
        Texture->AddressY = TA_Wrap;
        Texture->NeverStream = true;
        Texture->UpdateResource();
        DynamicTextures.Add(Id, Texture);
        DynamicTexturePixels += Pixels - OldPixels;
        for (auto& Pair : DynamicMaterials)
        {
            if (TextureId(Pair.Key) == Id)
            {
                Pair.Value->SetTextureParameterValue(TEXT("DynamicTexture"), Texture);
            }
        }
        Pending = {};
        UE_CLOG(bTrace, LogMinecraftDynamic, Verbose, TEXT("Dynamic texture scene=%u id=%u %dx%d."),
            SceneSerial, Id, Width, TexHeight);
    }
    return true;
}

bool ABridgeViewer::ApplyDynamicMesh(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < DynamicMeshHeaderBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const uint8 Space = Reader.Byte();
    const uint32 Id = Reader.U32();
    const uint32 Texture = Reader.U32();
    const uint8 Blend = Reader.Byte();
    const uint8 Topology = Reader.Byte();
    const uint32 Count = Reader.U32();
    if (Space >= DynamicSpaceCount || Id == 0 || Topology < 3 || Topology > 6 || !ValidBlend(Space, Blend) ||
        Count < 3 || Count > MaxDynamicPartVertices || (Topology <= 4 && Count % Topology != 0) ||
        Size != DynamicMeshHeaderBytes + int32(Count) * DynamicVertexBytes ||
        (Texture == DynamicBlockAtlas && !TerrainAtlases[int32(ETerrainAtlas::Albedo)].IsComplete()) ||
        (Texture > DynamicBlockAtlas && !DynamicTextures.Contains(Texture)))
    {
        return false;
    }
    auto& State = DynamicSpaces[Space];
    if (State.Meshes.Contains(Id) || State.Meshes.Num() >= 2 * MaxDynamicInstances ||
        State.ResidentVertices + int32(Count) > 2 * VertexBudget(Space))
    {
        return false;
    }
    BridgeDynamic::FMesh Mesh;
    Mesh.MaterialKey = MaterialKey(Space, Texture, Blend);
    Mesh.Vertices.Reserve(Count);
    const float PositionLimit = Space == DynamicParticles ? MaxParticleLocalPosition : 128.0f;
    for (uint32 I = 0; I < Count; ++I)
    {
        const float X = Reader.Float();
        const float Y = Reader.Float();
        const float Z = Reader.Float();
        const float U = Reader.Float();
        const float V = Reader.Float();
        const uint8 R = Reader.Byte();
        const uint8 G = Reader.Byte();
        const uint8 B = Reader.Byte();
        const uint8 A = Reader.Byte();
        const int8 Nx = int8(Reader.Byte());
        const int8 Ny = int8(Reader.Byte());
        const int8 Nz = int8(Reader.Byte());
        const uint8 Light = Reader.Byte();
        if (!Within(X, PositionLimit) || !Within(Y, PositionLimit) || !Within(Z, PositionLimit) ||
            !Within(U, 1024) || !Within(V, 1024))
        {
            return false;
        }
        const FVector Normal = ToDynamicSpace(Space, Nx, Ny, Nz).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
        const FVector3f Position(ToDynamicSpace(Space, X, Y, Z) * BlockSize);
        Mesh.Bounds += Position;
        Mesh.Vertices.Add({Position, FVector3f(Normal), FVector2f(U, V), FColor(R, G, B, A), Light});
    }
    BuildIndices(Mesh.Indices, Topology, Count);
    State.Meshes.Add(Id, MoveTemp(Mesh));
    State.ResidentVertices += Count;
    ++State.MeshFrames;
    return true;
}

bool ABridgeViewer::ApplyInstanceBatch(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < DynamicBatchHeaderBytes)
    {
        return false;
    }
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const uint8 Space = Reader.Byte();
    const float Fov = Reader.Float();
    const int32 Count = Reader.U16();
    if (Space >= DynamicSpaceCount || Count > MaxDynamicInstances ||
        Size != DynamicBatchHeaderBytes + Count * DynamicInstanceBytes ||
        (Space == DynamicHand ? !(Fov > 0.1f && Fov < 179.0f) : Fov != 0.0f))
    {
        return false;
    }
    auto& State = DynamicSpaces[Space];
    BridgeDynamic::FBatch Batch;
    Batch.VerticalFov = Fov;
    Batch.ReceivedAt = FPlatformTime::Seconds();
    Batch.Instances.Reserve(Count);
    TSet<uint32> Referenced;
    TSet<uint32> TrackedParts;
    int32 RenderVertices = 0;
    for (int32 I = 0; I < Count; ++I)
    {
        const uint32 Id = Reader.U32();
        const uint32 PartId = Reader.U32();
        const double X = Reader.Double();
        const double Y = Reader.Double();
        const double Z = Reader.Double();
        const float Qx = Reader.Float();
        const float Qy = Reader.Float();
        const float Qz = Reader.Float();
        const float Qw = Reader.Float();
        const float Scale = Reader.Float();
        const auto* Mesh = State.Meshes.Find(Id);
        const double Limit = Space == DynamicWorld || Space == DynamicParticles ? MaxBlockCoordinate : 128.0;
        float LengthSq = Qx * Qx + Qy * Qy + Qz * Qz + Qw * Qw;
        if (!Mesh || (PartId && (Space != DynamicWorld || PartId == BridgeDynamic::BakedPoseFlag || TrackedParts.Contains(PartId))) ||
            !Within(X, Limit) || !Within(Y, Limit) || !Within(Z, Limit) ||
            !Within(LengthSq - 1.0f, 0.01f) || !(Scale > 0.0f && Scale <= 16.0f))
        {
            return false;
        }
        RenderVertices += Mesh->Vertices.Num();
        if (RenderVertices > VertexBudget(Space))
        {
            return false;
        }
        // Both basis changes are reflections. A quaternion's vector part transforms as an axial vector.
        const FVector Q = -ToDynamicSpace(Space, Qx, Qy, Qz);
        Batch.Instances.Add({Id, PartId, ToDynamicSpace(Space, X, Y, Z) * BlockSize,
            FQuat(Q.X, Q.Y, Q.Z, Qw).GetNormalized(), Scale});
        if (PartId)
        {
            TrackedParts.Add(PartId);
        }
        Referenced.Add(Id);
    }
    // Only a complete snapshot advances visibility and retires meshes from the previous snapshot.
    for (auto It = State.Meshes.CreateIterator(); It; ++It)
    {
        if (!Referenced.Contains(It.Key()))
        {
            State.ResidentVertices -= It.Value().Vertices.Num();
            It.RemoveCurrent();
        }
    }
    State.Pending.Emplace(MoveTemp(Batch));
    ++State.BatchFrames;
    return true;
}

UMaterialInstanceDynamic* ABridgeViewer::GetDynamicMaterial(uint32 Key)
{
    if (UMaterialInstanceDynamic* Existing = DynamicMaterials.FindRef(Key))
    {
        return Existing;
    }
    using namespace BridgeWire;
    const uint32 Space = Key >> 16;
    const uint32 Blend = Key & (Space == DynamicWorld ? 1u : ParticleBlendMask);
    const bool bOverlay = (Key & ParticleNoDepthTest) != 0;
    const int32 ParticleMode = Blend == BlendAdd ? BlendAddAlpha : Blend;
    const int32 DynamicMode = Blend == BlendAlpha ? 1 : Space == DynamicHand ? 2 : 0;
    UMaterialInterface* MaterialBase = Space == DynamicParticles
        ? ParticleMaterialBases[ParticleMode + (bOverlay ? 4 : 0)] : DynamicMaterialBases[DynamicMode];
    if (!MaterialBase)
    {
        return nullptr;
    }
    auto* Material = UMaterialInstanceDynamic::Create(MaterialBase, this);
    ApplyLightFallback(*Material, Space == DynamicParticles);
    if (Space == DynamicWorld) Material->SetScalarParameterValue(TEXT("BlockEmissionMode"), float((Key >> 1) & 3u));
    if (Space == DynamicParticles && Blend == BlendAdd)
    {
        Material->SetScalarParameterValue(TEXT("AdditiveUsesAlpha"), 0.0f);
    }
    const uint32 Id = TextureId(Key);
    if (Id != BridgeWire::DynamicWhite)
    {
        const bool bBlockAtlas = Id == BridgeWire::DynamicBlockAtlas;
        UTexture2D* Texture = bBlockAtlas
            ? TerrainAtlases[int32(BridgeWire::ETerrainAtlas::Albedo)].Texture.Get() : DynamicTextures.FindRef(Id).Get();
        Material->SetTextureParameterValue(TEXT("DynamicTexture"), Texture);
        if (bBlockAtlas)
        {
            Material->SetScalarParameterValue(TEXT("PointSampleBlockAtlas"), 1.0f);
        }
    }
    DynamicMaterials.Add(Key, Material);
    return Material;
}

void ABridgeViewer::UpdateDynamicLighting()
{
    for (const auto& Pair : DynamicMaterials)
    {
        ApplyLightFallback(*Pair.Value, (Pair.Key >> 16) == BridgeWire::DynamicParticles);
    }
}

void ABridgeViewer::PresentDynamicMeshes()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftDynamicMeshes);
    using namespace BridgeWire;
    using namespace BridgeDynamic;
    if (!Camera || CameraFrames == 0)
    {
        return;
    }
    const double Now = FPlatformTime::Seconds();
    const bool bHardware = BridgeQuality::HardwareLighting();
    const bool bPlayerPresent = LocalPlayerView != ELocalPlayerView::None;
    const bool bFirstPerson = LocalPlayerView == ELocalPlayerView::FirstPerson;
    int32 RigidUploads = 0;
    int32 RigidTransformUpdates = 0;
    for (int32 Space = 0; Space < DynamicSpaceCount; ++Space)
    {
        auto& State = DynamicSpaces[Space];
        if (!State.Pending)
        {
            continue;
        }
        const bool bHand = Space == DynamicHand;
        const bool bLocalPlayer = Space == DynamicLocalPlayer;
        const bool bParticles = Space == DynamicParticles;
        if (bHand)
        {
            HandVerticalFov = State.Pending->VerticalFov;
        }
        TMap<uint32, TArray<FInstance>> Draws;
        TSet<uint32> ReferencedRigid;
        State.PartOrder.Reset();
        const double Interval = State.Pending->ReceivedAt - State.LastBatchAt;
        State.LastBatchAt = State.Pending->ReceivedAt;
        for (const FInstance& Instance : State.Pending->Instances)
        {
            const uint32 Key = State.Meshes.FindChecked(Instance.Mesh).MaterialKey;
            if (Instance.PartId == 0)
            {
                Draws.FindOrAdd(Key).Add(Instance);
                continue;
            }
            const uint32 Part = Instance.PartId;
            UProceduralMeshComponent* Component = RigidDynamicComponents.FindRef(Part);
            const bool bNew = !Component;
            if (bNew)
            {
                UMaterialInstanceDynamic* Material = GetDynamicMaterial(Key);
                if (!Material)
                {
                    continue;
                }
                Component = NewObject<UProceduralMeshComponent>(this);
                Component->SetupAttachment(GetRootComponent());
                Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Component->SetCastShadow((Key & 1u) == 0);
                Component->SetVisibleInRayTracing(false);
                Component->SetMaterial(0, Material);
                RigidDynamicComponents.Add(Part, Component);
            }
            ReferencedRigid.Add(Part);
            State.PartOrder.Add(Part);
            FRigidMotion* Existing = State.Rigid.Find(Part);
            if (!Existing || Existing->To.Mesh != Instance.Mesh)
            {
                // Upload one part in local coordinates. A transform-only snapshot never touches its vertices.
                const FInstance Local{Instance.Mesh, Part, FVector::ZeroVector, FQuat::Identity, 1.0f};
                UpdateComponent(*Component, State, {Local}, FVector::ZeroVector, false);
                Component->SetMaterial(0, GetDynamicMaterial(Key));
                ++RigidUploads;
            }
            const bool bDisplaced = Existing &&
                FVector::DistSquared(Existing->To.Position, Instance.Position) > FMath::Square(4.0 * BlockSize);
            // Animated meshes can replace their vertices while their world-space root continues to interpolate.
            const bool bContinuous = Existing && !bDisplaced && Interval < 0.2;
            State.RigidSnaps += bDisplaced;
            const bool bWasRayTracing = Existing && Existing->bRayTracing;
            const FInstance From = bContinuous ? Existing->Sample(Now) : Instance;
            const double Duration = bContinuous ? FMath::Clamp(Interval, 1.0 / 120.0, 1.0 / 15.0) : 0.0;
            State.Rigid.Add(Part, {From, Instance, Now, Duration, bWasRayTracing});
            if (bNew)
            {
                Component->SetRelativeTransform(FTransform(Instance.Rotation, Instance.Position, FVector(Instance.Scale)));
                Component->RegisterComponent();
            }
        }
        if (Space == DynamicWorld)
        {
            for (auto It = RigidDynamicComponents.CreateIterator(); It; ++It)
            {
                if (!ReferencedRigid.Contains(It.Key()))
                {
                    It.Value()->DestroyComponent();
                    State.Rigid.Remove(It.Key());
                    It.RemoveCurrent();
                }
            }
        }
        for (const auto& Previous : State.Presented)
        {
            if (!Draws.Contains(Previous.Key))
            {
                if (UProceduralMeshComponent* Component = DynamicComponents.FindRef(Previous.Key))
                {
                    Component->DestroyComponent();
                    DynamicComponents.Remove(Previous.Key);
                }
            }
        }
        for (const auto& Draw : Draws)
        {
            const TArray<FInstance>* Previous = State.Presented.Find(Draw.Key);
            if (Previous && *Previous == Draw.Value)
            {
                continue;
            }
            UProceduralMeshComponent* Component = DynamicComponents.FindRef(Draw.Key);
            if (!Component)
            {
                UMaterialInstanceDynamic* Material = GetDynamicMaterial(Draw.Key);
                if (!Material)
                {
                    continue;
                }
                Component = NewObject<UProceduralMeshComponent>(this);
                USceneComponent* Parent = bHand ? Camera->GetCameraComponent() : GetRootComponent();
                Component->SetupAttachment(bLocalPlayer ? LocalPlayerRoot.Get() : Parent);
                Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Component->SetCastShadow(!bHand && !bParticles && (Draw.Key & 1u) == 0);
                Component->bCastContactShadow = !bHand && !bParticles;
                Component->SetVisibleInRayTracing(false);
                Component->SetMaterial(0, Material);
                if (bParticles)
                {
                    Component->SetReceivesDecals(false);
                    if ((Draw.Key & ParticleNoDepthTest) != 0)
                    {
                        Component->SetTranslucentSortPriority(1);
                    }
                }
                if (bHand)
                {
                    Component->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
                    Component->SetBoundsScale(3.0f);
                    Component->SetTranslucentSortPriority(10);
                    Component->SetVisibility(bFirstPerson);
                }
                if (bLocalPlayer)
                {
                    Component->SetCastHiddenShadow(bFirstPerson && Component->CastShadow);
                    Component->SetHiddenInGame(bFirstPerson);
                    Component->SetVisibility(bPlayerPresent);
                }
                Component->RegisterComponent();
                DynamicComponents.Add(Draw.Key, Component);
            }
            const FVector Origin = Space == DynamicWorld || bParticles
                ? Draw.Value[0].Position.GridSnap(16.0 * BlockSize) : FVector::ZeroVector;
            UpdateComponent(*Component, State, Draw.Value, Origin);
        }
        const int32 InstanceCount = State.Pending->Instances.Num();
        if (bParticles && bTrace && ((InstanceCount == 0) != (State.LastInstances == 0) ||
            (InstanceCount > 0 && Now >= State.NextRigidTrace)))
        {
            UE_LOG(LogMinecraftDynamic, Display,
                TEXT("ParticleBatch scene=%u draws=%d meshes=%d vertices=%d snapshot=%u."),
                SceneSerial, Draws.Num(), InstanceCount, State.ResidentVertices, State.BatchFrames);
            State.NextRigidTrace = Now + 1.0;
        }
        State.LastInstances = InstanceCount;
        State.Pending.Reset();
        State.Presented = MoveTemp(Draws);
    }
    auto& World = DynamicSpaces[DynamicWorld];
    int32 RayTracingParts = 0;
    const int32 BaseTraceRange = BridgeQuality::DynamicRayRange();
    const int32 RayTracingBudget = BridgeQuality::DynamicRayBudget();
    for (const auto& Draw : DynamicSpaces[DynamicLocalPlayer].Presented)
    {
        if (UProceduralMeshComponent* Component = DynamicComponents.FindRef(Draw.Key))
        {
            // Use the world shadow pass for the player's procedural mesh in both camera modes.
            Component->SetCastHiddenShadow(bFirstPerson && Component->CastShadow);
            Component->SetHiddenInGame(bFirstPerson);
            Component->SetVisibility(bPlayerPresent);
            const bool bTracePlayer = bPlayerPresent && bHardware && Component->CastShadow &&
                BaseTraceRange > 0 && RayTracingParts < RayTracingBudget;
            Component->SetVisibleInRayTracing(bTracePlayer);
            RayTracingParts += bTracePlayer;
        }
    }
    for (const auto& Draw : DynamicSpaces[DynamicHand].Presented)
    {
        if (UProceduralMeshComponent* Component = DynamicComponents.FindRef(Draw.Key))
        {
            Component->SetVisibility(bFirstPerson);
        }
    }
    const int32 PlayerRayTracingParts = RayTracingParts;
    for (uint32 Part : World.PartOrder)
    {
        FRigidMotion& Motion = World.Rigid.FindChecked(Part);
        UProceduralMeshComponent* Component = RigidDynamicComponents.FindRef(Part);
        const FInstance Pose = Motion.Sample(Now);
        const FTransform Transform(Pose.Rotation, Pose.Position, FVector(Pose.Scale));
        if (!Transform.Equals(Component->GetRelativeTransform(), UE_DOUBLE_SMALL_NUMBER))
        {
            Component->SetRelativeTransform(Transform);
            ++RigidTransformUpdates;
        }
        const double TraceRange = (BaseTraceRange + (Motion.bRayTracing ? 4.0 : 0.0)) * BlockSize;
        const bool bRayTracing = bHardware && BaseTraceRange > 0 && RayTracingParts < RayTracingBudget && Component->CastShadow &&
            FVector::DistSquared(Pose.Position, Camera->GetActorLocation()) <= FMath::Square(TraceRange);
        RayTracingParts += bRayTracing;
        if (Motion.bRayTracing != bRayTracing)
        {
            Motion.bRayTracing = bRayTracing;
            Component->SetVisibleInRayTracing(bRayTracing);
        }
    }
    World.RigidUploads += RigidUploads;
    World.RigidTransformUpdates += RigidTransformUpdates;
    if (bTrace && Now >= World.NextRigidTrace && !World.Rigid.IsEmpty())
    {
        World.NextRigidTrace = Now + 1.0;
        int32 BakedParts = 0;
        for (const auto& Part : World.Rigid)
        {
            BakedParts += (Part.Key & BakedPoseFlag) != 0;
        }
        UE_LOG(LogMinecraftDynamic, Display,
            TEXT("RigidDynamic scene=%u parts=%d uploads=%u transformUpdates=%u rayTracingParts=%d bakedParts=%d snapMoves=%u"),
            SceneSerial, World.Rigid.Num(), World.RigidUploads, World.RigidTransformUpdates,
            RayTracingParts - PlayerRayTracingParts, BakedParts, World.RigidSnaps);
    }
    if (DynamicSpaces[DynamicHand].LastInstances > 0)
    {
        auto* CameraComponent = Camera->GetCameraComponent();
        const double TanHalfFov = FMath::Tan(FMath::DegreesToRadians(HandVerticalFov * 0.5));
        const double HalfHorizontal = FMath::Atan(TanHalfFov * CameraComponent->AspectRatio);
        CameraComponent->SetFirstPersonFieldOfView(FMath::RadiansToDegrees(2.0 * HalfHorizontal));
    }
}

void ABridgeViewer::ResetDynamicMeshes()
{
    LocalPlayerView = BridgeWire::ELocalPlayerView::None;
    for (const auto& Pair : RigidDynamicComponents)
    {
        Pair.Value->DestroyComponent();
    }
    RigidDynamicComponents.Reset();
    for (const auto& Pair : DynamicComponents)
    {
        Pair.Value->DestroyComponent();
    }
    DynamicComponents.Reset();
    DynamicMaterials.Reset();
    DynamicTextures.Reset();
    DynamicTexturePixels = 0;
    DynamicTextureUpload = {};
    for (auto& State : DynamicSpaces)
    {
        State = {};
    }
}
