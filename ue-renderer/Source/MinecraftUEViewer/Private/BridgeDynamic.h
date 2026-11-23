#pragma once

#include "CoreMinimal.h"

namespace BridgeDynamic
{
    constexpr uint32 BakedPoseFlag = 0x80000000u;

    struct FVertex
    {
        FVector3f Position;
        FVector3f Normal;
        FVector2f UV;
        FColor Color;
        uint8 Light;
    };

    struct FMesh
    {
        uint32 MaterialKey = 0;
        FBox3f Bounds{ForceInit};
        TArray<FVertex> Vertices;
        TArray<int32> Indices;
    };

    struct FInstance
    {
        uint32 Mesh;
        uint32 PartId;
        FVector Position;
        FQuat Rotation;
        float Scale;

        bool operator==(const FInstance& Other) const
        {
            return Mesh == Other.Mesh && Position == Other.Position && Rotation == Other.Rotation && Scale == Other.Scale;
        }
    };

    struct FBatch
    {
        float VerticalFov = 0;
        double ReceivedAt = 0;
        TArray<FInstance> Instances;
    };

    struct FRigidMotion
    {
        FInstance From;
        FInstance To;
        double Start;
        double Duration;
        bool bRayTracing = false;

        FInstance Sample(double Now) const;
    };

    struct FSpace
    {
        TMap<uint32, FMesh> Meshes;
        TOptional<FBatch> Pending;
        TMap<uint32, TArray<FInstance>> Presented;
        TMap<uint32, FRigidMotion> Rigid;
        TArray<uint32> PartOrder;
        double LastBatchAt = 0;
        double NextRigidTrace = 0;
        int32 ResidentVertices = 0;
        int32 LastInstances = 0;
        uint32 MeshFrames = 0;
        uint32 BatchFrames = 0;
        uint32 RigidUploads = 0;
        uint32 RigidTransformUpdates = 0;
        uint32 RigidSnaps = 0;
    };

    struct FTextureUpload
    {
        uint32 Id = 0;
        int32 Width = 0;
        int32 Height = 0;
        int32 NextRow = 0;
        TArray<uint8> Pixels;
    };
}
