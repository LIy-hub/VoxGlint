#include "BridgeGlassLighting.h"

#include "Algo/AllOf.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/SkyAtmosphereCommonData.h"
#include "RHIGlobals.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftGlassLighting, Log, All);

namespace
{
    constexpr int32 TextureWidth = 512;
    constexpr float BlockCm = float(BridgeWire::BlockSize);

    TAutoConsoleVariable<int32> GlassLightingEnabled(TEXT("mcue.Glass.Lighting"), 0,
        TEXT("Enable experimental direct light filtering by streamed glass volumes (0=off by default, 1=on)."));

    TAutoConsoleVariable<int32> GlassLocalRayShadowBudget(TEXT("mcue.Glass.LocalRayShadowBudget"), 12,
        TEXT("Maximum affected local light samples using hardware ray traced shadows. "
            "-1 uses hardware when available, 0 uses native shadow maps, positive values switch to shadow maps above budget."));

    int32 LocalRayShadowBudget()
    {
        static const TOptional<int32> Override = []
        {
            int32 Budget;
            if (FParse::Value(FCommandLine::Get(), TEXT("BridgeGlassLocalRayShadowBudget="), Budget))
            {
                return TOptional<int32>(Budget);
            }
            return TOptional<int32>();
        }();
        return FMath::Max(-1, Override.Get(GlassLocalRayShadowBudget.GetValueOnGameThread()));
    }

    uint8 VisibleChannels(const FLinearColor& Color)
    {
        return (Color.R > 0.f ? 1u : 0u) | (Color.G > 0.f ? 2u : 0u) | (Color.B > 0.f ? 4u : 0u);
    }

    int32 SamplesPerEmitter(const ULightComponent& Light)
    {
        return Light.IsA<URectLightComponent>() ? 4 : 1;
    }

    FLinearColor Parameter(const FVector3f& Value, float W = 0.f)
    {
        return FLinearColor(Value.X, Value.Y, Value.Z, W);
    }

    float Extinction(float T, float ReferenceDepth)
    {
        // Reserve a negative value for exact zero transmission through any positive thickness.
        return T == 0.f ? -1.f : float(FMath::Min(1.e30, -FMath::Log2(double(T)) / double(ReferenceDepth)));
    }

    FIntPoint PlanarAxes(int32 Normal)
    {
        return FIntPoint((Normal + 1) % 3, (Normal + 2) % 3);
    }

    void ApplyWaterCaustics(UMaterialInstanceDynamic& Target, UMaterialInterface* Function)
    {
        auto* Source = Cast<UMaterialInstanceDynamic>(Function);
        const FName Strength(TEXT("P2CausticStrength"));
        const float Factor = Source ? Source->K2_GetScalarParameterValue(Strength) : 0.f;
        Target.SetScalarParameterValue(Strength, Factor);
        if (Factor <= 0.f)
        {
            return;
        }
        for (const FName Name : {FName(TEXT("P2GridOriginCm")), FName(TEXT("P2ShadowCenterM")),
            FName(TEXT("P2SunAir")), FName(TEXT("P2SunWater")), FName(TEXT("P2PatternPhaseM"))})
        {
            Target.SetVectorParameterValue(Name, Source->K2_GetVectorParameterValue(Name));
        }
        for (const FName Name : {FName(TEXT("P2WaterGrid")), FName(TEXT("P2AirShadow")),
            FName(TEXT("P2WaterShadow")), FName(TEXT("CausticsTex"))})
        {
            Target.SetTextureParameterValue(Name, Source->K2_GetTextureParameterValue(Name));
        }
        const FName Radius(TEXT("P2ShadowRadiusM"));
        Target.SetScalarParameterValue(Radius, Source->K2_GetScalarParameterValue(Radius));
    }
}

bool FBridgeGlassLighting::Receive(const FIntVector& Section, BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < SectionGlassHeaderBytes - 16)
    {
        return false;
    }
    const uint16 Part = Reader.U16();
    const uint16 Parts = Reader.U16();
    const uint16 MaterialCount = Reader.U16();
    const uint16 BoxCount = Reader.U16();
    if (Parts == 0 || Parts > MaxGlassParts || Part >= Parts || MaterialCount > MaxGlassPartEntries ||
        BoxCount > MaxGlassPartEntries || Size != SectionGlassHeaderBytes - 16 +
            MaterialCount * GlassMaterialBytes + BoxCount * GlassBoxBytes ||
        (BoxCount == 0 && (Part != 0 || Parts != 1 || MaterialCount != 0)))
    {
        return false;
    }

    TArray<FVector3f> Materials;
    Materials.Reserve(MaterialCount);
    for (uint16 I = 0; I < MaterialCount; ++I)
    {
        const float R = Reader.Float();
        const float G = Reader.Float();
        const float B = Reader.Float();
        const float Depth = Reader.Float();
        if (!(R >= 0.f && R <= 1.f && G >= 0.f && G <= 1.f && B >= 0.f && B <= 1.f &&
            Depth > 0.f && FMath::IsFinite(Depth)))
        {
            return false;
        }
        Materials.Emplace(Extinction(R, Depth), Extinction(G, Depth), Extinction(B, Depth));
    }

    TArray<FVolume> Volumes;
    Volumes.Reserve(BoxCount);
    for (uint16 I = 0; I < BoxCount; ++I)
    {
        const uint16 Cell = Reader.U16();
        const uint16 Material = Reader.U16();
        const float X0 = Reader.Float(), Y0 = Reader.Float(), Z0 = Reader.Float();
        const float X1 = Reader.Float(), Y1 = Reader.Float(), Z1 = Reader.Float();
        if (Cell >= 4096 || Material >= MaterialCount ||
            !FMath::IsFinite(X0) || !FMath::IsFinite(Y0) || !FMath::IsFinite(Z0) ||
            !FMath::IsFinite(X1) || !FMath::IsFinite(Y1) || !FMath::IsFinite(Z1) ||
            !(X0 < X1 && Y0 < Y1 && Z0 < Z1))
        {
            return false;
        }
        if (Materials[Material] != FVector3f::ZeroVector)
        {
            const float X = Cell & 15u;
            const float Y = (Cell >> 4u) & 15u;
            const float Z = (Cell >> 8u) & 15u;
            Volumes.Add({FVector3f(Z + Z0, -(X + X1), Y + Y0), FVector3f(Z + Z1, -(X + X0), Y + Y1), Materials[Material]});
        }
    }

    if (Parts > 1)
    {
        FPendingSection* State = Pending.Find(Section);
        if (Part == 0)
        {
            State = &Pending.Add(Section, FPendingSection{0, Parts, {}});
        }
        if (!State || State->Parts != Parts || State->Next != Part)
        {
            return false;
        }
        State->Volumes.Append(MoveTemp(Volumes));
        if (++State->Next < Parts)
        {
            return true;
        }
        Volumes = MoveTemp(State->Volumes);
    }
    Pending.Remove(Section);

    if (Volumes.IsEmpty())
    {
        if (Sections.Remove(Section)) ++Revision;
    }
    else if (const TArray<FVolume>* Old = Sections.Find(Section); !Old || *Old != Volumes)
    {
        Sections.Add(Section, MoveTemp(Volumes));
        ++Revision;
    }
    return true;
}

void FBridgeGlassLighting::Drop(const FIntVector& Section)
{
    Pending.Remove(Section);
    if (Sections.Remove(Section)) ++Revision;
}

void FBridgeGlassLighting::Reset()
{
    for (auto& Pair : Replacements)
    {
        Release(*Pair.Key, Pair.Value, true);
    }
    Replacements.Reset();
    PendingHandoffs = 0;
    PackedPlanes = 0;
    Sections.Reset();
    Pending.Reset();
    Nodes.Reset();
    GridOccupancy.Reset();
    VolumeTexture.Reset();
    bReportedVolumeLimit = false;
    ++Revision;
}

void FBridgeGlassLighting::Forget(ULocalLightComponent& Light)
{
    if (FReplacement* Replacement = Replacements.Find(&Light))
    {
        if (!Replacement->bSourceSuppressed) --PendingHandoffs;
        Release(Light, *Replacement, false);
        Replacements.Remove(&Light);
    }
}

bool FBridgeGlassLighting::Replaces(ULightComponent& Light) const
{
    const FReplacement* Replacement = Replacements.Find(&Light);
    return Replacement && Replacement->bSourceSuppressed;
}

FBridgeGlassLighting::FLocalShadowCounts FBridgeGlassLighting::LocalShadowCounts() const
{
    FLocalShadowCounts Counts;
    for (const auto& Pair : Replacements)
    {
        const FReplacement& Source = Pair.Value;
        if (!Source.bSun && Source.bSourceSuppressed)
        {
            if (Source.bHardwareShadows) ++Counts.RayTraced;
            else ++Counts.ShadowMapped;
        }
    }
    return Counts;
}

void FBridgeGlassLighting::BuildNodes(FVolume* Begin, FVolume* End, FVolume* Root)
{
    FBox3f Bounds(ForceInit);
    FBox3f Centers(ForceInit);
    for (const FVolume* It = Begin; It != End; ++It)
    {
        Bounds += It->Min;
        Bounds += It->Max;
        Centers += It->Min * 0.5f + It->Max * 0.5f;
    }
    const int32 Index = Nodes.AddDefaulted();
    Nodes[Index].Bounds = Bounds;
    if (End - Begin == 1)
    {
        Nodes[Index].Volume = int32(Begin - Root);
        Nodes[Index].GridAxis = Begin->GridAxis;
        Nodes[Index].FirstGridCell = Begin->FirstGridCell;
    }
    else
    {
        const FVector3f Extent = Centers.GetSize();
        const int32 Axis = Extent.X >= Extent.Y && Extent.X >= Extent.Z ? 0 : (Extent.Y >= Extent.Z ? 1 : 2);
        FVolume* Middle = Begin + (End - Begin) / 2;
        std::nth_element(Begin, Middle, End, [Axis](const FVolume& A, const FVolume& B)
        {
            return A.Min[Axis] * 0.5f + A.Max[Axis] * 0.5f < B.Min[Axis] * 0.5f + B.Max[Axis] * 0.5f;
        });
        BuildNodes(Begin, Middle, Root);
        BuildNodes(Middle, End, Root);
    }
    Nodes[Index].Escape = Nodes.Num();
}

bool FBridgeGlassLighting::AppendPlanarGrid(const TArray<FVolume>& Section, const FVector3f& Offset,
    TArray<FVolume>& Gathered, TArray<FVector4f>& GridData)
{
    if (Section.Num() < 32)
    {
        return false;
    }
    FBox3f Bounds(ForceInit);
    for (const FVolume& Cell : Section)
    {
        const FVector3f& P = Cell.Min;
        if (P.X < 0.f || P.X >= 16.f || P.Y < -16.f || P.Y >= 0.f || P.Z < 0.f || P.Z >= 16.f ||
            P.X != FMath::FloorToFloat(P.X) || P.Y != FMath::FloorToFloat(P.Y) || P.Z != FMath::FloorToFloat(P.Z) ||
            Cell.Max != P + FVector3f::OneVector)
        {
            return false;
        }
        Bounds += P;
        Bounds += Cell.Max;
    }
    const FVector3f Span = Bounds.GetSize();
    int32 Normal = INDEX_NONE;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (Span[Axis] == 1.f)
        {
            Normal = Axis;
            break;
        }
    }
    if (Normal == INDEX_NONE)
    {
        return false;
    }
    const FIntPoint Axes = PlanarAxes(Normal);
    const int32 Width = int32(Span[Axes.X]);
    const int32 CellCount = Width * int32(Span[Axes.Y]);
    if (CellCount > 6 * (Section.Num() - 1))
    {
        // Retain the existing texture capacity guarantee even for a very sparse wall.
        return false;
    }
    TArray<FVector4f, TInlineAllocator<256>> Cells;
    Cells.SetNumZeroed(CellCount);
    for (const FVolume& Volume : Section)
    {
        const FVector3f Local = Volume.Min - Bounds.Min;
        const int32 Index = int32(Local[Axes.X]) + Width * int32(Local[Axes.Y]);
        if (Cells[Index].W != 0.f)
        {
            return false;
        }
        Cells[Index] = FVector4f(Volume.Extinction, 1.f);
    }
    const int32 FirstCell = GridData.Num();
    GridData.Append(Cells.GetData(), Cells.Num());
    for (const FVector4f& Cell : Cells)
    {
        GridOccupancy.Add(Cell.W != 0.f);
    }
    Gathered.Add({Bounds.Min + Offset, Bounds.Max + Offset, FVector3f::ZeroVector, Normal, FirstCell});
    return true;
}

void FBridgeGlassLighting::Rebuild(AActor& Owner, const FVector& CameraCm)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftGlassBuildVolumes);
    BuiltRevision = Revision;
    PackedPlanes = 0;
    Nodes.Reset();
    GridOccupancy.Reset();
    VolumeTexture.Reset();
    if (Sections.IsEmpty())
    {
        return;
    }
    AnchorCm = CameraCm.GridSnap(128.0 * BlockCm);
    // A binary tree needs two nodes per box, minus its missing last branch. The device's
    // maximum 2D texture size is the limit for packing its three-texel nodes.
    const int32 MaxTextureDimension = int32(GetMax2DTextureDimension());
    if (MaxTextureDimension < TextureWidth)
    {
        return;
    }
    const int32 MaxRenderedBoxes = (TextureWidth * MaxTextureDimension / 3 + 1) / 2;
    struct FSectionVolumes
    {
        FVector Offset;
        const TArray<FVolume>* Volumes;
    };
    TArray<FSectionVolumes> Loaded;
    Loaded.Reserve(Sections.Num());
    int64 BoxCount = 0;
    for (const auto& Pair : Sections)
    {
        const FVector Offset = BridgeWire::ToUnreal(Pair.Key) * 16.0 - AnchorCm / BlockCm;
        Loaded.Add({Offset, &Pair.Value});
        BoxCount += Pair.Value.Num();
    }
    if (BoxCount > MaxRenderedBoxes)
    {
        UE_CLOG(!bReportedVolumeLimit, LogMinecraftGlassLighting, Warning,
            TEXT("Glass volume limit reached. Rendering the nearest %d of %lld boxes."), MaxRenderedBoxes, BoxCount);
        bReportedVolumeLimit = true;
        Loaded.Sort([](const FSectionVolumes& A, const FSectionVolumes& B)
        {
            return A.Offset.SizeSquared() < B.Offset.SizeSquared();
        });
    }
    TArray<FVolume> Volumes;
    Volumes.Reserve(int32(FMath::Min<int64>(MaxRenderedBoxes, BoxCount)));
    TArray<FVector4f> GridData;
    int32 UploadedBoxes = 0;
    static const bool bPackPlanes = !FParse::Param(FCommandLine::Get(), TEXT("BridgeGlassUseBoxBvh"));
    for (const FSectionVolumes& Section : Loaded)
    {
        const FVector3f Offset(Section.Offset);
        const int32 Count = FMath::Min(Section.Volumes->Num(), MaxRenderedBoxes - UploadedBoxes);
        if (bPackPlanes && Count == Section.Volumes->Num() && AppendPlanarGrid(*Section.Volumes, Offset, Volumes, GridData))
        {
            ++PackedPlanes;
        }
        else
        {
            for (int32 I = 0; I < Count; ++I)
            {
                const FVolume& Volume = (*Section.Volumes)[I];
                Volumes.Add({Volume.Min + Offset, Volume.Max + Offset, Volume.Extinction});
            }
        }
        UploadedBoxes += Count;
        if (UploadedBoxes == MaxRenderedBoxes) break;
    }

    Nodes.Reserve(Volumes.Num() * 2 - 1);
    BuildNodes(Volumes.GetData(), Volumes.GetData() + Volumes.Num(), Volumes.GetData());
    const int32 NodeTexels = Nodes.Num() * 3;
    const int32 Rows = FMath::DivideAndRoundUp(NodeTexels + GridData.Num(), TextureWidth);
    TArray<FVector4f> Data;
    Data.SetNumZeroed(TextureWidth * Rows);
    for (int32 I = 0; I < Nodes.Num(); ++I)
    {
        const FNode& Node = Nodes[I];
        Data[I * 3 + 0] = FVector4f(Node.Bounds.Min, float(Node.Escape));
        // Type 0 is a branch, 1 a box, and 2–4 a grid with the corresponding normal axis.
        const float Type = Node.Volume == INDEX_NONE ? 0.f : float(Node.GridAxis + 2);
        Data[I * 3 + 1] = FVector4f(Node.Bounds.Max, Type);
        if (Node.GridAxis != INDEX_NONE)
        {
            Data[I * 3 + 2] = FVector4f(float(NodeTexels + Node.FirstGridCell), 0.f, 0.f, 0.f);
        }
        else if (Node.Volume != INDEX_NONE)
        {
            Data[I * 3 + 2] = FVector4f(Volumes[Node.Volume].Extinction, 0.f);
        }
    }
    if (!GridData.IsEmpty())
    {
        FMemory::Memcpy(Data.GetData() + NodeTexels, GridData.GetData(), GridData.Num() * sizeof(FVector4f));
    }
    VolumeTexture.Reset(UTexture2D::CreateTransient(TextureWidth, Rows, PF_A32B32G32R32F, NAME_None,
        TConstArrayView64<uint8>(reinterpret_cast<const uint8*>(Data.GetData()), int64(Data.Num()) * sizeof(FVector4f))));
    if (VolumeTexture)
    {
        VolumeTexture->SRGB = false;
        VolumeTexture->NeverStream = true;
        VolumeTexture->Filter = TF_Nearest;
        VolumeTexture->UpdateResource();
    }
    UE_CLOG(FParse::Param(FCommandLine::Get(), TEXT("BridgeTrace")), LogMinecraftGlassLighting, Display,
        TEXT("Glass volumes: sections=%d uploaded=%d received=%lld nodes=%d planarGrids=%d."),
        Sections.Num(), UploadedBoxes, BoxCount, Nodes.Num(), PackedPlanes);
}

bool FBridgeGlassLighting::OverlapsGridCell(const FNode& Node, const FVector3f& Center, float RadiusSquared) const
{
    const FIntPoint Axes = PlanarAxes(Node.GridAxis);
    const FVector3f Span = Node.Bounds.GetSize();
    const int32 Width = int32(Span[Axes.X]);
    const int32 Height = int32(Span[Axes.Y]);
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            const int32 Index = Node.FirstGridCell + X + Y * Width;
            if (!GridOccupancy[Index]) continue;
            FVector3f Min = Node.Bounds.Min;
            Min[Axes.X] += float(X);
            Min[Axes.Y] += float(Y);
            const FBox3f Cell(Min, Min + FVector3f::OneVector);
            if (Cell.ComputeSquaredDistanceToPoint(Center) <= RadiusSquared) return true;
        }
    }
    return false;
}

bool FBridgeGlassLighting::OverlapsLocalLight(const ULocalLightComponent& Light) const
{
    const FVector3f Center((Light.GetComponentLocation() - AnchorCm) / BlockCm);
    const float RadiusSquared = FMath::Square(Light.AttenuationRadius / BlockCm);
    for (int32 Index = 0; Index < Nodes.Num();)
    {
        const FNode& Node = Nodes[Index];
        if (Node.Bounds.ComputeSquaredDistanceToPoint(Center) > RadiusSquared)
        {
            Index = Node.Escape;
        }
        else if (Node.Volume != INDEX_NONE && (Node.GridAxis == INDEX_NONE || OverlapsGridCell(Node, Center, RadiusSquared)))
        {
            return true;
        }
        else
        {
            ++Index;
        }
    }
    return false;
}

bool FBridgeGlassLighting::UpdateReplacement(AActor& Owner, ULightComponent& Source, bool bSun, bool bHardwareShadows,
    const FLinearColor& AtmosphereTransmission)
{
    const FLinearColor Color = Source.GetLightColor() * AtmosphereTransmission;
    const float Radiance[] = {Color.R, Color.G, Color.B};
    const uint8 ChannelMask = VisibleChannels(Color);
    if (ChannelMask == 0 || Source.Intensity <= 0.f)
    {
        return false;
    }

    const auto* Rect = Cast<URectLightComponent>(&Source);
    const auto* Point = Cast<UPointLightComponent>(&Source);
    const auto* Directional = Cast<UDirectionalLightComponent>(&Source);
    if (FReplacement* Previous = Replacements.Find(&Source); Previous && Previous->ChannelMask != ChannelMask)
    {
        Release(Source, *Previous, true);
        Replacements.Remove(&Source);
    }
    FReplacement* State = Replacements.Find(&Source);
    const bool bNew = State == nullptr;
    if (!State)
    {
        State = &Replacements.Add(&Source, {});
        State->bSun = bSun;
        State->ChannelMask = ChannelMask;
        State->IndirectIntensity = Source.IndirectLightingIntensity;
        State->DiffuseScale = Source.DiffuseScale;
        State->SpecularScale = Source.SpecularScale;
        State->bAffectsTranslucency = Source.bAffectTranslucentLighting;
        State->bAffectsReflection = Source.bAffectReflection;
        State->bAffectsGi = Source.bAffectGlobalIllumination;
    }

    const auto ShadowMode = bHardwareShadows ? ECastRayTracedShadow::Enabled : ECastRayTracedShadow::Disabled;
    const int32 SampleCount = SamplesPerEmitter(Source);
    const FVector SourcePosition = Source.GetComponentLocation();
    const FQuat Rotation = Source.GetComponentQuat();
    const FVector3f TowardSun = bSun ? FVector3f(-Source.GetForwardVector()) : FVector3f::ZeroVector;
    int32 Index = 0;
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        FVector Position = SourcePosition;
        if (Rect)
        {
            const double Side = (Sample & 1) != 0 ? 0.25 : -0.25;
            const double Up = (Sample & 2) != 0 ? 0.25 : -0.25;
            Position += Rotation.RotateVector(FVector(0, Side * Rect->SourceWidth, Up * Rect->SourceHeight));
        }
        const FVector3f Destination = bSun ? TowardSun : FVector3f((Position - AnchorCm) / BlockCm);
        const FVector4f Ray(Destination, bSun ? 1.f : 0.f);
        for (int32 Channel = 0; Channel < 3; ++Channel)
        {
            if ((ChannelMask & (1u << Channel)) == 0) continue;
            if (bNew)
            {
                auto& SampleState = State->Samples.AddDefaulted_GetRef();
                SampleState.Light.Reset(NewObject<ULightComponent>(&Owner, Source.GetClass(), NAME_None, RF_Transient));
                SampleState.Filter.Reset(UMaterialInstanceDynamic::Create(Base.Get(), SampleState.Light.Get()));
                ULightComponent& Light = *SampleState.Light.Get();
                Light.SetMobility(EComponentMobility::Movable);
                Light.bAllowMegaLights = false;
                Light.SetCastShadows(true);
                Light.SetLightFunctionMaterial(SampleState.Filter.Get());
                Light.SetLightFunctionFadeDistance(1.e9f);
                Light.SetLightFunctionDisabledBrightness(1.f);
                Light.SetLightColor(FLinearColor(Channel == 0, Channel == 1, Channel == 2));
                // A delayed UE PSO may leave this registered component without a scene proxy.
                // Keep any proxies that arrive early from contributing while the original light is active.
                Light.SetLightingChannels(false, false, false);
                Light.SetIndirectLightingIntensity(0.f);
                Light.SetAffectReflection(false);
                Light.SetAffectGlobalIllumination(false);
                Light.SetAffectTranslucentLighting(false);
                Light.SetVolumetricScatteringIntensity(0.f);
                const FVector3f ChannelVector(Channel == 0, Channel == 1, Channel == 2);
                SampleState.Filter->SetVectorParameterValue(TEXT("GlassChannel"), Parameter(ChannelVector));
                if (Point)
                {
                    auto& Target = *CastChecked<UPointLightComponent>(&Light);
                    Target.SetIESBrightnessScale(Point->IESBrightnessScale);
                    Target.SetUseIESBrightness(Point->bUseIESBrightness);
                    Target.SetIESTexture(Point->IESTexture);
                }
                if (Directional)
                {
                    auto& Target = *CastChecked<UDirectionalLightComponent>(&Light);
                    Target.SetAtmosphereSunLight(false);
                    Target.SetLightSourceAngle(Directional->LightSourceAngle);
                    Target.SetLightSourceSoftAngle(Directional->LightSourceSoftAngle);
                    Target.ContactShadowLength = Directional->ContactShadowLength;
                    Target.ContactShadowLengthInWS = Directional->ContactShadowLengthInWS;
                    Target.ContactShadowCastingIntensity = Directional->ContactShadowCastingIntensity;
                    Target.ContactShadowNonCastingIntensity = Directional->ContactShadowNonCastingIntensity;
                }
            }
            auto& SampleState = State->Samples[Index++];
            ULightComponent& Light = *SampleState.Light.Get();
            UMaterialInstanceDynamic& Filter = *SampleState.Filter.Get();
            if (SampleState.UploadedRevision != BuiltRevision)
            {
                const FVector3f High(AnchorCm);
                const FVector3f Low(AnchorCm - FVector(High));
                Filter.SetVectorParameterValue(TEXT("GlassAnchorHigh"), Parameter(High));
                Filter.SetVectorParameterValue(TEXT("GlassAnchorLow"), Parameter(Low));
                Filter.SetScalarParameterValue(TEXT("GlassNodeCount"), float(Nodes.Num()));
                Filter.SetTextureParameterValue(TEXT("GlassVolumes"), VolumeTexture.Get());
                SampleState.UploadedRevision = BuiltRevision;
            }
            if (bNew || Ray != SampleState.LastRay)
            {
                Filter.SetVectorParameterValue(TEXT("GlassRay"), Parameter(FVector3f(Ray.X, Ray.Y, Ray.Z), Ray.W));
                SampleState.LastRay = Ray;
            }
            if (bNew || Light.GetComponentLocation() != Position || !Light.GetComponentQuat().Equals(Rotation))
            {
                Light.SetWorldLocationAndRotation(Position, Rotation);
            }
            if (bNew || State->bHardwareShadows != bHardwareShadows)
            {
                Light.SetCastRaytracedShadows(ShadowMode);
            }
            Light.SetIntensity(Source.Intensity * Radiance[Channel] / float(SampleCount));
            Light.SetDiffuseScale(State->bSourceSuppressed ? (bSun ? State->DiffuseScale : Source.DiffuseScale) : 0.f);
            Light.SetSpecularScale(State->bSourceSuppressed ? (bSun ? State->SpecularScale : Source.SpecularScale) : 0.f);
            if (const auto* Local = Cast<ULocalLightComponent>(&Source))
            {
                auto& Target = *CastChecked<ULocalLightComponent>(&Light);
                Target.SetIntensityUnits(Local->IntensityUnits);
                Target.SetAttenuationRadius(Local->AttenuationRadius);
            }
            if (Rect)
            {
                auto& Target = *CastChecked<URectLightComponent>(&Light);
                Target.SetSourceWidth(FMath::Max(0.01f, Rect->SourceWidth * 0.5f));
                Target.SetSourceHeight(FMath::Max(0.01f, Rect->SourceHeight * 0.5f));
                Target.SetBarnDoorAngle(Rect->BarnDoorAngle);
                Target.SetBarnDoorLength(Rect->BarnDoorLength);
            }
            else if (Point)
            {
                auto& Target = *CastChecked<UPointLightComponent>(&Light);
                Target.SetSourceRadius(Point->SourceRadius);
                Target.SetSoftSourceRadius(Point->SoftSourceRadius);
            }
            else if (bSun)
            {
                ApplyWaterCaustics(Filter, Source.LightFunctionMaterial);
            }
            if (bNew)
            {
                Light.RegisterComponentWithWorld(Owner.GetWorld());
            }
        }
    }
    State->bHardwareShadows = bHardwareShadows;
    return true;
}

bool FBridgeGlassLighting::TryActivateReplacement(ULightComponent& Source, FReplacement& Replacement)
{
    if (Replacement.bSourceSuppressed)
    {
        return true;
    }
    if (!Algo::AllOf(Replacement.Samples, [](const FEmitterSample& Sample) { return Sample.Light->SceneProxy != nullptr; }))
    {
        return false;
    }

    for (const FEmitterSample& Sample : Replacement.Samples)
    {
        ULightComponent& Light = *Sample.Light.Get();
        Light.SetLightingChannels(true, false, false);
        Light.SetDiffuseScale(Replacement.bSun ? Replacement.DiffuseScale : Source.DiffuseScale);
        Light.SetSpecularScale(Replacement.bSun ? Replacement.SpecularScale : Source.SpecularScale);
        Light.SetIndirectLightingIntensity(Replacement.IndirectIntensity);
        Light.SetAffectReflection(Replacement.bAffectsReflection);
        Light.SetAffectGlobalIllumination(Replacement.bAffectsGi);
        Light.SetAffectTranslucentLighting(Replacement.bAffectsTranslucency);
        Light.SetVolumetricScatteringIntensity(Replacement.bSun ? 0.f : Source.VolumetricScatteringIntensity);
    }
    if (Replacement.bSun)
    {
        // The atmospheric sun keeps its sky and water state while the filtered lamps supply the surface light.
        Source.SetLightingChannels(false, false, false);
        Source.SetDiffuseScale(0.f);
        Source.SetSpecularScale(0.f);
        Source.SetAffectTranslucentLighting(false);
        Source.SetAffectReflection(false);
        Source.SetAffectGlobalIllumination(false);
        Source.SetIndirectLightingIntensity(0.f);
    }
    else
    {
        Source.SetVisibility(false);
    }
    Replacement.bSourceSuppressed = true;
    return true;
}

void FBridgeGlassLighting::Release(ULightComponent& Source, FReplacement& Replacement, bool bRestore)
{
    for (auto& Sample : Replacement.Samples)
    {
        Sample.Light->DestroyComponent();
    }
    if (!bRestore || !Replacement.bSourceSuppressed)
    {
        return;
    }
    if (Replacement.bSun)
    {
        Source.SetLightingChannels(true, false, false);
        Source.SetDiffuseScale(Replacement.DiffuseScale);
        Source.SetSpecularScale(Replacement.SpecularScale);
        Source.SetAffectTranslucentLighting(Replacement.bAffectsTranslucency);
        Source.SetAffectReflection(Replacement.bAffectsReflection);
        Source.SetAffectGlobalIllumination(Replacement.bAffectsGi);
        Source.SetIndirectLightingIntensity(Replacement.IndirectIntensity);
    }
    else
    {
        Source.SetVisibility(true);
    }
}

bool FBridgeGlassLighting::IsEnabled() const
{
    static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("BridgeGlassDisabled"));
    return !bDisabled && GlassLightingEnabled.GetValueOnGameThread() != 0;
}

void FBridgeGlassLighting::Update(AActor& Owner, const FVector& CameraCm, UDirectionalLightComponent& Sun,
    const USkyAtmosphereComponent* Atmosphere, TConstArrayView<ULocalLightComponent*> Lights,
    uint64 LocalLightsRevision, bool bHardwareShadows)
{
    const bool bEnabled = IsEnabled() && !Sections.IsEmpty();
    if (bEnabled && !bTriedMaterial)
    {
        bTriedMaterial = true;
        Base.Reset(LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/Bridge/M_McGlassLightFunction.M_McGlassLightFunction"), nullptr, LOAD_NoWarn));
        if (Base)
        {
            Base->EnsureIsComplete();
        }
        else
        {
            UE_LOG(LogMinecraftGlassLighting, Warning, TEXT("The cooked glass light function is unavailable."));
        }
    }

    TSet<ULightComponent*> Keep;
    bool bRecheckLocal = true;
    if (bEnabled && Base)
    {
        if (BuiltRevision != Revision || FVector::DistSquared(CameraCm, AnchorCm) > FMath::Square(1024.0 * BlockCm))
        {
            // Moving the anchor also invalidates per-light material parameters.
            if (BuiltRevision == Revision) ++Revision;
            Rebuild(Owner, CameraCm);
        }
        if (VolumeTexture)
        {
            if (Sun.IsVisible())
            {
                static const IConsoleVariable* SkyEnabled = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkyAtmosphere"));
                const FLinearColor Transmission = Atmosphere && Atmosphere->IsVisible() && (!SkyEnabled || SkyEnabled->GetInt())
                    ? FAtmosphereSetup(*Atmosphere).GetTransmittanceAtGroundLevel(-Sun.GetForwardVector()) : FLinearColor::White;
                const bool bUseSunHardwareShadow = bHardwareShadows && Sun.GetCastRaytracedShadow() == ECastRayTracedShadow::Enabled;
                if (UpdateReplacement(Owner, Sun, true, bUseSunHardwareShadow, Transmission))
                {
                    Keep.Add(&Sun);
                }
            }
            const int32 Budget = LocalRayShadowBudget();
            const bool bPolicyChanged = LastLocalRayShadowBudget != Budget || bLocalHardwareShadows != bHardwareShadows;
            bRecheckLocal = LastLocalLightsRevision != LocalLightsRevision || LastLocalVolumesRevision != BuiltRevision || bPolicyChanged;
            if (bRecheckLocal)
            {
                LastLocalLightsRevision = LocalLightsRevision;
                LastLocalVolumesRevision = BuiltRevision;
                LastLocalRayShadowBudget = Budget;
                bLocalHardwareShadows = bHardwareShadows;
                TArray<ULocalLightComponent*, TInlineAllocator<64>> Affected;
                int32 FilteredLightCount = 0;
                for (ULocalLightComponent* Light : Lights)
                {
                    if (Light->Intensity > 0.f && OverlapsLocalLight(*Light))
                    {
                        Affected.Add(Light);
                        FilteredLightCount += FMath::CountBits(uint32(VisibleChannels(Light->GetLightColor()))) * SamplesPerEmitter(*Light);
                    }
                }

                // Enter native shadow maps above the budget and wait for headroom before
                // switching back. This avoids thrashing shadow caches as lights stream in and out.
                const int32 Threshold = bPolicyChanged || bLocalUsesRayShadows ? Budget : Budget - Budget / 4;
                const bool bUseRayShadows = bHardwareShadows && (Budget < 0 || (Budget > 0 && FilteredLightCount <= Threshold));
                UE_CLOG((bPolicyChanged || bUseRayShadows != bLocalUsesRayShadows) && FParse::Param(FCommandLine::Get(), TEXT("BridgeTrace")),
                    LogMinecraftGlassLighting, Display, TEXT("Glass local shadows: mode=%s affected=%d filteredLights=%d rayBudget=%d."),
                    bUseRayShadows ? TEXT("ray tracing") : TEXT("shadow maps"), Affected.Num(), FilteredLightCount, Budget);
                bLocalUsesRayShadows = bUseRayShadows;

                for (ULocalLightComponent* Light : Affected)
                {
                    if (UpdateReplacement(Owner, *Light, false, bUseRayShadows))
                    {
                        Keep.Add(Light);
                    }
                }
            }
        }
    }
    else
    {
        PackedPlanes = 0;
        Nodes.Reset();
        GridOccupancy.Reset();
        VolumeTexture.Reset();
        BuiltRevision = MAX_uint64;
    }
    if (!bEnabled || !VolumeTexture)
    {
        LastLocalVolumesRevision = MAX_uint64;
        LastLocalRayShadowBudget = MAX_int32;
    }
    PendingHandoffs = 0;
    for (auto It = Replacements.CreateIterator(); It; ++It)
    {
        if ((It.Value().bSun || bRecheckLocal) && !Keep.Contains(It.Key()))
        {
            Release(*It.Key(), It.Value(), true);
            It.RemoveCurrent();
        }
        else if (!TryActivateReplacement(*It.Key(), It.Value()))
        {
            ++PendingHandoffs;
        }
    }
}
