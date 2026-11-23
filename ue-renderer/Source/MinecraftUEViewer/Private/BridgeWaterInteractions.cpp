#include "BridgeViewer.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
    constexpr uint32 SurfaceFlags = 3u << 26;
    constexpr uint32 OpenSky = 1u << 28;
    constexpr int32 MaxImpulses = 128;

    FIntPoint WaterBlock(const FVector& PositionMC)
    {
        return FIntPoint(FMath::FloorToInt32(PositionMC.X), FMath::FloorToInt32(PositionMC.Z));
    }

    float SurfaceY(uint32 Cell, int32 MinY)
    {
        return float(MinY + int32(Cell & 2047u)) + float((Cell >> 11) & 63u) / 64.0f;
    }

    FVector ToMinecraft(const FVector& PositionUE)
    {
        return FVector(-PositionUE.Y, PositionUE.Z, PositionUE.X) / BridgeWire::BlockSize;
    }

    FVector ToUnreal(const FVector& PositionMC)
    {
        return BridgeWire::ToUnreal(PositionMC.X, PositionMC.Y, PositionMC.Z) * BridgeWire::BlockSize;
    }
}

void ABridgeViewer::FindWaterfallImpacts(const FVector& EyeMC, int32 Limit)
{
    TArray<McTerrain::FWaterfallContact, TInlineAllocator<64>> Candidates;
    const auto SectionAt = [](const FVector& P)
    {
        return FIntVector(FMath::FloorToInt32(P.X / 16.0), FMath::FloorToInt32(P.Y / 16.0), FMath::FloorToInt32(P.Z / 16.0));
    };
    const FIntVector Min = SectionAt(EyeMC - FVector(48.0));
    const FIntVector Max = SectionAt(EyeMC + FVector(48.0));
    for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
    {
        for (int32 X = Min.X; X <= Max.X; ++X)
        {
            for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
            {
                const auto* Section = WaterfallSections.Find(FIntVector(X, Y, Z));
                if (!Section) continue;
                for (const McTerrain::FWaterfallContact& Contact : *Section)
                {
                    if (FVector::DistSquared(EyeMC, Contact.PositionMC) > FMath::Square(48.0)) continue;
                    if (Contact.bPoolSurface)
                    {
                        Candidates.Add(Contact);
                        continue;
                    }
                    const FIntPoint Column = WaterBlock(Contact.PositionMC);
                    double Closest = 3.25;
                    TOptional<FVector> Pool;
                    for (int32 DZ = -1; DZ <= 1; ++DZ)
                    {
                        for (int32 DX = -1; DX <= 1; ++DX)
                        {
                            const FIntPoint Sample = Column + FIntPoint(DX, DZ);
                            const uint32 Cell = WaterColumn(Sample);
                            if ((Cell & SurfaceFlags) != SurfaceFlags) continue;
                            const double HeightDelta = SurfaceY(Cell, MinY) - Contact.PositionMC.Y;
                            if (HeightDelta < -1.85 || HeightDelta > 0.62) continue;
                            const FVector At(Sample.X + 0.5, SurfaceY(Cell, MinY), Sample.Y + 0.5);
                            const double Distance = FVector::DistSquared(Contact.PositionMC, At);
                            if (Distance >= Closest || FVector::DistSquared(EyeMC, At) > FMath::Square(48.0)) continue;
                            Pool = At;
                            Closest = Distance;
                        }
                    }
                    if (Pool.IsSet()) Candidates.Add({Pool.GetValue(), Contact.Drop, false});
                }
            }
        }
    }

    Candidates.Sort([&EyeMC](const auto& A, const auto& B)
    {
        return FVector::DistSquared(A.PositionMC, EyeMC) - (A.bPoolSurface ? 2.0 : 0.0) <
            FVector::DistSquared(B.PositionMC, EyeMC) - (B.bPoolSurface ? 2.0 : 0.0);
    });
    NearbyWaterfalls.Reset(Limit);
    for (const auto& Candidate : Candidates)
    {
        if (NearbyWaterfalls.ContainsByPredicate([&Candidate](const auto& Existing)
        {
            return FVector::DistSquared(Candidate.PositionMC, Existing.PositionMC) < FMath::Square(1.65);
        })) continue;
        NearbyWaterfalls.Add(Candidate);
        if (NearbyWaterfalls.Num() == Limit) break;
    }
}

void ABridgeViewer::ResetWaterInteractions()
{
    WaterfallSections.Reset();
    NearbyWaterfalls.Reset();
    WaterfallScanIn = 0.0f;
    LastWaterfallGridRevision = LastRippleGridRevision = 0;
    LastRippleCameraBlock = FIntPoint(INT32_MIN, INT32_MIN);
    LastRippleTier = 0;
    bRippleHasWater = false;
    PlayerWaterMover = {};
    EntityWaterMovers.Reset();
    RippleRainRandom.Reset();
    RainRippleCarry = RainCrownIn = RippleActivity = 0.0f;
}

void ABridgeViewer::UpdateWaterInteractions(UMaterialInstanceDynamic& Material, float DeltaSeconds, int32 Tier, float StormRain)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftShallowWaterSources);
    FBridgeRippleFrame Frame;
    const bool bEnabled = Tier >= 2 && CameraFrames > 0 && Camera;
    if (!bEnabled)
    {
        PlayerWaterMover = {};
        EntityWaterMovers.Reset();
        RippleActivity = RainRippleCarry = RainCrownIn = 0.0f;
        if (WaterParticles) WaterParticles->TickSurfaceEffects(DeltaSeconds, false, {});
        WaterSim->UpdateRipples(Material, MoveTemp(Frame));
        return;
    }

    const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.05f);
    const FVector Eye = CameraPositionMC;
    const FIntPoint CameraBlock = WaterBlock(Eye);
    const bool bCut = FMath::Abs(int64(CameraBlock.X) - LastRippleCameraBlock.X) > 8 ||
        FMath::Abs(int64(CameraBlock.Y) - LastRippleCameraBlock.Y) > 8;
    if (bCut)
    {
        PlayerWaterMover = {};
        EntityWaterMovers.Reset();
        WaterfallScanIn = RippleActivity = 0.0f;
    }
    Frame.Domain = FBridgeRippleDomain::Around(Eye, Tier);
    Frame.DeltaSeconds = Dt;
    Frame.GridOrigin = WaterGridOrigin;
    if (bWaterGridReady)
    {
        Frame.Grid = WaterGridTexture->GetResource();
        Frame.Field = WaterFieldTexture->GetResource();
    }
    const FVector2d Origin = Frame.Domain.GetOriginMeters();
    const float Width = Frame.Domain.GetWidthMeters();
    if (LastRippleGridRevision != WaterGridRevision || LastRippleCameraBlock != CameraBlock || LastRippleTier != Tier)
    {
        bRippleHasWater = false;
        if (bWaterGridReady)
        {
            const int32 MinX = FMath::FloorToInt32(Origin.X), MinZ = FMath::FloorToInt32(Origin.Y);
            for (int32 Z = MinZ; Z <= MinZ + int32(Width) && !bRippleHasWater; ++Z)
            {
                for (int32 X = MinX; X <= MinX + int32(Width); ++X)
                {
                    const uint32 Cell = WaterColumn(FIntPoint(X, Z));
                    if ((Cell & SurfaceFlags) != SurfaceFlags || FMath::Abs(SurfaceY(Cell, MinY) - Eye.Y) > 24.0) continue;
                    bRippleHasWater = true;
                    break;
                }
            }
        }
        LastRippleGridRevision = WaterGridRevision;
        LastRippleCameraBlock = CameraBlock;
        LastRippleTier = Tier;
    }

    const auto AddImpulse = [&](const FVector& P, float Radius, float Velocity, float Foam, const FVector2f& Heading)
    {
        const FVector2f Local(FVector2d(P.X, P.Z) - Origin);
        if (Frame.Impulses.Num() >= MaxImpulses || Local.X < -Radius || Local.Y < -Radius || Local.X > Width + Radius ||
            Local.Y > Width + Radius || FMath::Abs(P.Y - Eye.Y) > 24.0) return;
        Frame.Impulses.Add({Local, Heading, FVector4f(Radius, Velocity, Foam, float(P.Y - MinY))});
    };

    const bool bTraceP3 = FParse::Param(FCommandLine::Get(), TEXT("BridgeWaterP3Trace"));
    const auto MoveThroughWater = [&](FWaterMover& Mover, const FVector& Contact, float Radius, bool bPlayer, bool bHull)
    {
        const FWaterMover Previous = Mover;
        Mover.Position = Contact;
        Mover.bValid = true;
        Mover.bWet = false;
        Mover.EntryCooldown = FMath::Max(0.0f, Previous.EntryCooldown - Dt);
        const FVector Travel = Contact - Previous.Position;
        const bool bContinuous = Previous.bValid && Dt > UE_SMALL_NUMBER && Travel.SizeSquared() <= 9.0;
        const bool bMoved = bContinuous && Travel.SizeSquared() > 1e-10;
        const FVector Instant = bMoved ? (Travel / FMath::Max(0.001f, DeltaSeconds)).GetClampedToMaxSize(18.0)
                                       : FVector::ZeroVector;
        if (!bContinuous)
        {
            Mover.Velocity = FVector::ZeroVector;
            Mover.PeakFallSpeed = 0.0f;
        }
        else if (bMoved)
        {
            Mover.Velocity = FMath::Lerp(Previous.Velocity, Instant, FMath::Min(1.0f, 25.0f * Dt));
            Mover.PeakFallSpeed = Instant.Y < -0.25
                ? FMath::Max(Previous.PeakFallSpeed, float(-Instant.Y))
                : FMath::Max(0.0f, Previous.PeakFallSpeed - 14.0f * Dt);
        }
        else
        {
            // Keep recent velocity across frames rendered between Minecraft camera updates.
            Mover.Velocity = Previous.Velocity * FMath::Exp(-8.0f * Dt);
            Mover.PeakFallSpeed = FMath::Max(0.0f, Previous.PeakFallSpeed - 10.0f * Dt);
        }
        const uint32 Cell = WaterColumn(WaterBlock(Contact));
        if ((Cell & SurfaceFlags) != SurfaceFlags) return;
        const float Top = SurfaceY(Cell, MinY);
        const float Bottom = FMath::CeilToFloat(Top) - float((Cell >> 17) & 31u);
        const double Depth = Top - Contact.Y;
        Mover.Surface = Top;
        const double SurfaceMargin = bPlayer ? -0.02 : -0.09;
        Mover.bWet = Depth >= SurfaceMargin && Depth <= (bPlayer ? 2.1 : bHull ? 0.75 : 0.85) && Contact.Y >= Bottom - 0.26;
        if (!Mover.bWet || !bContinuous) return;

        const FVector2f Lateral(float(Mover.Velocity.X), float(Mover.Velocity.Z));
        const float Speed = Lateral.Size();
        const float DownSpeed = FMath::Max3(float(-Instant.Y), float(-Previous.Velocity.Y), Previous.PeakFallSpeed);
        const float LateralSpeed = FMath::Max(Speed, float(FVector2f(float(Instant.X), float(Instant.Z)).Size()));
        const bool bCrossedFromAbove = Previous.Position.Y > Top + (bPlayer ? 0.01 : 0.05);
        const bool bEntry = Mover.EntryCooldown <= 0.0f &&
            (!Previous.bWet || FMath::Abs(Previous.Surface - Top) > 0.52f) &&
            ((bCrossedFromAbove && DownSpeed > 0.45f) || (bPlayer && LateralSpeed > 1.8f));
        if (bEntry)
        {
            const float Impact = bPlayer
                ? FMath::Clamp(DownSpeed * 0.65f + LateralSpeed * 0.24f, 1.25f, 6.2f)
                : FMath::Clamp(DownSpeed * 0.42f + LateralSpeed * 0.15f, 0.35f, 4.2f);
            const FVector At(Contact.X, Top, Contact.Z);
            if (bPlayer)
            {
                const float T = FMath::Clamp((Impact - 1.25f) / 4.75f, 0.0f, 1.0f);
                const float BroadRadius = Radius * FMath::Lerp(2.65f, 3.35f, T);
                const float InnerRadius = Radius * FMath::Lerp(1.55f, 1.85f, T);
                const float EntryFoam = FMath::Clamp(0.45f + Impact * 0.18f, 0.45f, 1.0f);
                AddImpulse(At, BroadRadius, -Impact * 0.78f, EntryFoam * 0.55f, FVector2f::ZeroVector);
                AddImpulse(At, InnerRadius, -Impact * 0.52f, EntryFoam, FVector2f::ZeroVector);
            }
            else
            {
                AddImpulse(At, Radius * 1.95f, -Impact,
                    FMath::Clamp(Impact * (0.28f + Radius * 0.30f), 0.15f, 0.95f), FVector2f::ZeroVector);
            }
            if (WaterParticles && (Impact > 0.4f || bPlayer))
            {
                const float SplashScale = bPlayer
                    ? FMath::Clamp(0.85f + Impact * 0.20f, 0.90f, 2.05f)
                    : FMath::Clamp(0.65f + Impact * Radius * 0.55f, 0.65f, 1.65f);
                WaterParticles->SpawnSurfaceSplash(ToUnreal(At) + FVector(0, 0, bPlayer ? 4.0 : 2.0), false, SplashScale);
            }
            Mover.PeakFallSpeed = 0.0f;
            Mover.EntryCooldown = bPlayer ? 0.22f : 0.15f;
            UE_CLOG(bTraceP3, LogTemp, Display, TEXT("WaterP3 %s entry impulse=%.2f radius=%.2f"),
                bPlayer ? TEXT("player") : TEXT("entity"), Impact, Radius);
            return;
        }
        Mover.PeakFallSpeed = 0.0f;
        const float VerticalStir = bPlayer && Depth < 1.35 ? FMath::Abs(float(Mover.Velocity.Y)) * 0.75f : 0.0f;
        const float StirSpeed = FMath::Max(Speed, VerticalStir);
        if (StirSpeed < (bHull ? 0.65f : 0.22f)) return;
        if (bHull)
        {
            const float Wake = FMath::Clamp((Speed - 0.6f) / 4.2f, 0.10f, 1.45f);
            AddImpulse(FVector(Contact.X, Top, Contact.Z), FMath::Clamp(Radius * 4.6f, 2.7f, 4.2f),
                -16.5f * Dt * Wake, 6.2f * Dt * Wake, Lateral.GetSafeNormal());
        }
        else if (bPlayer || Radius > 0.23f)
        {
            const float Strength = bPlayer
                ? FMath::Clamp(StirSpeed / 2.6f, 0.14f, 2.1f)
                : FMath::Clamp(Speed / 3.1f, 0.10f, 1.7f);
            const float MoveRadius = bPlayer ? Radius * 1.42f : Radius * 1.15f;
            const float MoveGain = bPlayer ? -15.5f : -9.5f;
            const float FoamGain = bPlayer ? 2.1f : 1.1f;
            const int32 Segments = FMath::Clamp(FMath::CeilToInt32(Travel.Size() / 0.32), 1, 4);
            for (int32 I = 1; I <= Segments; ++I)
            {
                const FVector Point = FMath::Lerp(Previous.Position, Contact, double(I) / Segments);
                const uint32 AtCell = WaterColumn(WaterBlock(Point));
                if ((AtCell & SurfaceFlags) != SurfaceFlags || FMath::Abs(SurfaceY(AtCell, MinY) - Top) > 0.52f) continue;
                AddImpulse(FVector(Point.X, SurfaceY(AtCell, MinY), Point.Z), MoveRadius,
                    MoveGain * Dt * Strength / Segments, FoamGain * Dt * FMath::Square(Strength) / Segments, FVector2f::ZeroVector);
            }
        }
    };

    int32 Hulls = 0;
    if (bRippleHasWater)
    {
        const FVector Feet = LocalPlayerView != BridgeWire::ELocalPlayerView::None
            ? Eye + PlayerEyeOffsetMC : Eye - FVector(0.0, 1.62, 0.0);
        MoveThroughWater(PlayerWaterMover, Feet, 0.52f, true, false);

        const auto& World = DynamicSpaces[BridgeWire::DynamicWorld];
        const double Now = FPlatformTime::Seconds();
        struct FBody
        {
            FBox3f Bounds{ForceInit};
            uint32 Part = MAX_uint32;
            float Scale = 1.0f;
        };
        TMap<FVector, FBody> Bodies;
        if (Now - World.LastBatchAt < 0.6)
        {
            for (const auto& Pair : World.Rigid)
            {
                const BridgeDynamic::FInstance Pose = Pair.Value.Sample(Now);
                const FVector MC = ToMinecraft(Pose.Position);
                if (FMath::Abs(MC.Y - Eye.Y) > 14.0 || FMath::Abs(MC.X - Eye.X) > Width * 0.55 ||
                    FMath::Abs(MC.Z - Eye.Z) > Width * 0.55) continue;
                const BridgeDynamic::FMesh* Mesh = World.Meshes.Find(Pose.Mesh);
                if (!Mesh || !Mesh->Bounds.IsValid) continue;
                FBody& Body = Bodies.FindOrAdd(MC);
                Body.Part = FMath::Min(Body.Part, Pair.Key);
                Body.Scale = Pose.Scale;
                Body.Bounds += Mesh->Bounds;
            }
        }
        TSet<uint32> PresentBodies;
        for (const auto& Pair : Bodies)
        {
            const FBody& Body = Pair.Value;
            const FVector3f Size = Body.Bounds.GetSize() * Body.Scale * 0.01f;
            // The stream provides shape, transforms and stable part IDs. Classify floating
            // hulls by their wide, shallow bounds and require contact with the free surface.
            const float Long = FMath::Max(Size.X, Size.Y);
            const float Wide = FMath::Min(Size.X, Size.Y);
            const bool bHull = Long >= 1.22f && Long <= 3.35f && Wide >= 0.82f && Size.Z >= 0.22f && Size.Z <= 1.24f;
            const float Radius = FMath::Clamp(FMath::Sqrt(Size.X * Size.Y) * 0.54f, 0.23f, 0.97f);
            const float Bottom = bHull ? 0.0f : FMath::Clamp(Body.Bounds.Min.Z * Body.Scale * 0.01f, -0.58f, 0.65f);
            // Rigid item poses use the render-time bob as part of their position.
            const float ItemBias = (Body.Part & BridgeDynamic::BakedPoseFlag) == 0 ? 0.23f : 0.0f;
            FWaterMover& Mover = EntityWaterMovers.FindOrAdd(Body.Part);
            MoveThroughWater(Mover, Pair.Key + FVector(0.0, Bottom - ItemBias, 0.0), Radius, false, bHull);
            Hulls += bHull && Mover.bWet && Mover.Velocity.SizeSquared() > 0.5;
            PresentBodies.Add(Body.Part);
        }
        for (auto It = EntityWaterMovers.CreateIterator(); It; ++It)
        {
            if (!PresentBodies.Contains(It.Key())) It.RemoveCurrent();
        }
    }
    else
    {
        PlayerWaterMover = {};
        EntityWaterMovers.Reset();
    }

    const int32 BodyImpulses = Frame.Impulses.Num();
    if ((WaterfallScanIn -= Dt) <= 0.0f || LastWaterfallGridRevision != WaterGridRevision)
    {
        FindWaterfallImpacts(Eye, Tier >= 3 ? 12 : 8);
        LastWaterfallGridRevision = WaterGridRevision;
        WaterfallScanIn = 0.17f;
    }
    TArray<FBridgeWaterfallFX, TInlineAllocator<12>> ImpactFX;
    for (const auto& Fall : NearbyWaterfalls)
    {
        const FVector& P = Fall.PositionMC;
        if (FVector::DistSquared(Eye, P) > FMath::Square(48.0)) continue;
        const float Force = FMath::Clamp(FMath::Sqrt(FMath::Max(3.0f, Fall.Drop)) * 0.34f, 0.62f, 1.75f);
        ImpactFX.Add({ToUnreal(P), Force});
        const uint32 Cell = WaterColumn(WaterBlock(P));
        if ((Cell & SurfaceFlags) != SurfaceFlags || FMath::Abs(SurfaceY(Cell, MinY) - P.Y) > 0.54) continue;
        const float Phase = WeatherSim.Clock * 22.0f + float(FMath::Fmod(P.X * 7.51 + P.Z * -3.58, double(UE_TWO_PI)));
        const FVector At = P + FVector(FMath::Sin(Phase) * 0.23f, 0.0, FMath::Cos(Phase * 1.21f) * 0.23f);
        const float Pressure = (FMath::Cos(Phase) + 0.62f * FMath::Sin(Phase * 1.83f)) * 17.5f;
        AddImpulse(At, 0.78f * Force, Pressure * Dt * Force, Dt * 3.1f * Force, FVector2f::ZeroVector);
    }
    if (WaterParticles) WaterParticles->TickSurfaceEffects(Dt, true, ImpactFX);

    const int32 FallImpulses = Frame.Impulses.Num() - BodyImpulses;
    RainCrownIn -= Dt;
    const float Rain = FMath::SmoothStep(0.61f, 0.92f, StormRain);
    if (Rain > 0.0f && bRippleHasWater && bPrecipGridReady)
    {
        RainRippleCarry += 0.80f * Rain * Width * Width * Dt;
        const int32 Count = FMath::Min(84, FMath::FloorToInt32(RainRippleCarry));
        RainRippleCarry -= Count;
        for (int32 I = 0; I < Count; ++I)
        {
            FVector At(Origin.X + RippleRainRandom.FRand() * Width, 0.0, Origin.Y + RippleRainRandom.FRand() * Width);
            const FIntPoint Block = WaterBlock(At);
            const uint32 Cell = WaterColumn(Block);
            if ((Cell & (SurfaceFlags | OpenSky)) != (SurfaceFlags | OpenSky)) continue;
            At.Y = SurfaceY(Cell, MinY);
            int32 PrecipTop = 0;
            uint8 PrecipType = 0;
            if (!SamplePrecipColumn(Block.X, Block.Y, PrecipTop, PrecipType) || PrecipType != 1 ||
                PrecipTop > FMath::CeilToInt32(At.Y)) continue;
            const float Size = RippleRainRandom.FRandRange(0.22f, 0.34f);
            AddImpulse(At, Size, -RippleRainRandom.FRandRange(0.27f, 0.64f), 0.0f, FVector2f::ZeroVector);
            if (RainCrownIn <= 0.0f && WaterParticles && FVector::DistSquared(Eye, At) <= FMath::Square(18.0))
            {
                WaterParticles->SpawnSurfaceSplash(ToUnreal(At) + FVector(0, 0, 1.5), true,
                    RippleRainRandom.FRandRange(0.72f, 1.23f));
                RainCrownIn = Tier >= 3 ? 0.042f : 0.065f;
            }
        }
    }
    else
    {
        RainRippleCarry = 0.0f;
    }
    Material.SetScalarParameterValue(TEXT("WaterRippleRain"), Rain);
    if (!Frame.Impulses.IsEmpty()) RippleActivity = 5.5f;
    else RippleActivity -= Dt;
    Frame.bActive = bRippleHasWater && RippleActivity > 0.0f;
    if (bTraceP3 && FMath::FloorToInt32(WeatherSim.Clock * 2.0f) != FMath::FloorToInt32((WeatherSim.Clock - Dt) * 2.0f))
    {
        UE_LOG(LogTemp, Display, TEXT("WaterP3 %dx%d domain=%.1fm active=%d bodies=%d hulls=%d falls=%d fallImpulses=%d rain=%d"),
            Frame.Domain.Size, Frame.Domain.Size, Width, Frame.bActive, BodyImpulses, Hulls, ImpactFX.Num(), FallImpulses,
            Frame.Impulses.Num() - BodyImpulses - FallImpulses);
    }
    WaterSim->UpdateRipples(Material, MoveTemp(Frame));
}
