
#include "VeilNetVisibilityComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ReferenceSkeleton.h"
#include "TimerManager.h"

namespace
{
    struct FVisibilitySample
    {
        FVector Location;
        float DistanceSquared;
    };
}

UVeilNetVisibilityComponent::UVeilNetVisibilityComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UVeilNetVisibilityComponent::BeginPlay()
{
    Super::BeginPlay();

    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return;
    }

    if (Owner->HasAuthority())
    {
        World->GetTimerManager().SetTimer(
            UpdateTimer,
            this,
            &UVeilNetVisibilityComponent::UpdateFogOfWar,
            FMath::Max(UpdateInterval, 0.033f),
            true
        );

        UpdateFogOfWar();
    }
}

void UVeilNetVisibilityComponent::EndPlay(
    const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(UpdateTimer);
    }

    LastVisibleTimes.Empty();

    Super::EndPlay(EndPlayReason);
}

bool UVeilNetVisibilityComponent::HasTargetTag(
    const AActor* Actor) const
{
    if (!IsValid(Actor))
    {
        return false;
    }

    bool bHasMatchingTag = false;

    for (const FName& Tag : TargetTags)
    {
        if (!Tag.IsNone() && Actor->ActorHasTag(Tag))
        {
            bHasMatchingTag = true;
            break;
        }
    }

    if (!bHasMatchingTag)
    {
        const APawn* TargetPawn = Cast<APawn>(Actor);
        const AController* TargetController =
            TargetPawn ? TargetPawn->GetController() : nullptr;
        bHasMatchingTag =
            bIncludePlayerControlledPawns
            && TargetController
            && TargetController->IsPlayerController();
    }

    if (!bHasMatchingTag)
    {
        return false;
    }

    return RequiredTargetTag.IsNone()
        || Actor->ActorHasTag(RequiredTargetTag);
}

bool UVeilNetVisibilityComponent::IsPointInView(
    const FVector& ViewLocation,
    const FVector& ViewForward,
    const FVector& Point) const
{
    const FVector ToPoint =
        Point - ViewLocation;

    if (ToPoint.IsNearlyZero())
    {
        return true;
    }

    const FVector Direction =
        ToPoint.GetSafeNormal();

    const float HalfAngle =
        FMath::Clamp(
            HorizontalFOV * 0.5f +
            FOVPaddingDegrees,
            1.f,
            179.f);

    const float MinDot =
        FMath::Cos(
            FMath::DegreesToRadians(
                HalfAngle));

    return FVector::DotProduct(
        ViewForward.GetSafeNormal(),
        Direction) >= MinDot;
}

bool UVeilNetVisibilityComponent::IsPointVisible(
    const FVector& ViewLocation,
    const FVector& Point,
    const FCollisionQueryParams& Params) const
{
    UWorld* World = GetWorld();

    if (!World)
    {
        return false;
    }

    FHitResult Hit;

    const bool bBlocked =
        World->LineTraceSingleByChannel(
            Hit,
            ViewLocation,
            Point,
            VisibilityTraceChannel,
            Params);

    return !bBlocked;
}

void UVeilNetVisibilityComponent::GatherBodySamples(
    AActor* Target,
    TArray<FVector>& OutSamples) const
{
    OutSamples.Reset();
    if (!IsValid(Target))
    {
        return;
    }

    const float SampleSpacing = FMath::Max(VisibilitySampleSpacing, 1.0f);

    if (USkeletalMeshComponent* Mesh =
        Target->FindComponentByClass<USkeletalMeshComponent>())
    {
        if (const USkeletalMesh* SkeletalMesh = Mesh->GetSkeletalMeshAsset())
        {
            const FReferenceSkeleton& Skeleton =
                SkeletalMesh->GetRefSkeleton();
            const int32 BoneCount =
                FMath::Min(Mesh->GetNumBones(), Skeleton.GetNum());

            TArray<FVector> BoneLocations;
            BoneLocations.SetNum(BoneCount);

            for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
            {
                BoneLocations[BoneIndex] =
                    Mesh->GetBoneLocation(Mesh->GetBoneName(BoneIndex));
                OutSamples.Add(BoneLocations[BoneIndex]);
            }

            for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
            {
                const int32 ParentIndex =
                    Skeleton.GetParentIndex(BoneIndex);
                if (ParentIndex < 0 || ParentIndex >= BoneCount)
                {
                    continue;
                }

                const FVector& Start = BoneLocations[ParentIndex];
                const FVector& End = BoneLocations[BoneIndex];
                const float SegmentLength = FVector::Distance(Start, End);
                const int32 SegmentCount = FMath::Clamp(
                    FMath::CeilToInt(SegmentLength / SampleSpacing),
                    1,
                    16);

                for (int32 SegmentIndex = 1;
                    SegmentIndex < SegmentCount;
                    ++SegmentIndex)
                {
                    const float Alpha =
                        static_cast<float>(SegmentIndex) / SegmentCount;
                    OutSamples.Add(FMath::Lerp(Start, End, Alpha));
                }
            }
        }
    }

    if (OutSamples.IsEmpty())
    {
        OutSamples.Add(Target->GetActorLocation());
    }
}

bool UVeilNetVisibilityComponent::CanSeeTarget(
    AActor* Target,
    const FVector& ViewLocation,
    const FRotator& ViewRotation) const
{
    if (!IsValid(Target) || Target == GetOwner())
    {
        return false;
    }

    const FVector Forward =
        ViewRotation.Vector();

    FCollisionQueryParams QueryParams(
        SCENE_QUERY_STAT(TagFogOfWar),
        true);
    QueryParams.AddIgnoredActor(GetOwner());
    QueryParams.AddIgnoredActor(Target);
    QueryParams.bTraceComplex = false;

    const float MaxVisionRadiusSquared =
        FMath::Square(FMath::Max(VisionRadius, 0.0f));
    TArray<FVisibilitySample> CandidateSamples;
    TArray<FVector> BodySamples;
    GatherBodySamples(Target, BodySamples);

    for (const FVector& Point : BodySamples)
    {
        const float DistanceSquared =
            FVector::DistSquared(ViewLocation, Point);

        if (DistanceSquared <= MaxVisionRadiusSquared
            && IsPointInView(ViewLocation, Forward, Point))
        {
            CandidateSamples.Add({ Point, DistanceSquared });
        }
    }

    if (CandidateSamples.IsEmpty())
    {
        const FVector Point = Target->GetActorLocation();
        const float DistanceSquared =
            FVector::DistSquared(ViewLocation, Point);
        if (DistanceSquared <= MaxVisionRadiusSquared
            && IsPointInView(ViewLocation, Forward, Point))
        {
            CandidateSamples.Add({ Point, DistanceSquared });
        }
    }

    CandidateSamples.Sort(
        [](const FVisibilitySample& Left, const FVisibilitySample& Right)
        {
            return Left.DistanceSquared < Right.DistanceSquared;
        });

    const int32 TraceCount = FMath::Min(
        CandidateSamples.Num(),
        FMath::Clamp(MaxVisibilityTraceCount, 8, 128));

    constexpr int32 NearestSampleCount = 8;
    for (int32 TraceIndex = 0; TraceIndex < TraceCount; ++TraceIndex)
    {
        int32 SampleIndex = TraceIndex;
        if (TraceCount > NearestSampleCount
            && CandidateSamples.Num() > TraceCount
            && TraceIndex >= NearestSampleCount)
        {
            const int32 DistributedTraceIndex =
                TraceIndex - NearestSampleCount;
            const int32 DistributedTraceCount =
                TraceCount - NearestSampleCount;
            const int32 RemainingSampleCount =
                CandidateSamples.Num() - NearestSampleCount;

            SampleIndex = NearestSampleCount
                + DistributedTraceIndex * RemainingSampleCount
                    / DistributedTraceCount;
        }

        if (IsPointVisible(
                ViewLocation,
                CandidateSamples[SampleIndex].Location,
                QueryParams))
        {
            return true;
        }
    }

    return false;
}

bool UVeilNetVisibilityComponent::IsActorVisibleForReplication(
    const AActor* Target) const
{
    if (!GetOwner() || !GetOwner()->HasAuthority())
    {
        return false;
    }

    if (!IsValid(Target) || !HasTargetTag(Target))
    {
        return false;
    }

    const double* LastSeen =
        LastVisibleTimes.Find(
            TWeakObjectPtr<AActor>(
                const_cast<AActor*>(Target)
            )
        );

    if (!LastSeen)
    {
        return false;
    }

    const UWorld* World = GetWorld();

    if (!World)
    {
        return false;
    }

    return World->GetTimeSeconds() - *LastSeen
        <= VisibilityGracePeriod;
}

void UVeilNetVisibilityComponent::UpdateFogOfWar()
{
    UWorld* World = GetWorld();
    APawn* Observer = Cast<APawn>(GetOwner());

    if (!World || !Observer || !Observer->HasAuthority())
    {
        return;
    }

    APlayerController* PC =
        Cast<APlayerController>(Observer->GetController());

    // Do not calculate visibility until the pawn has a controller.
    if (!PC || !PC->IsPlayerController())
    {
        return;
    }

    // Use the authoritative pawn eye location and control rotation.
    // This does not depend on a client-supplied visibility result.
    FVector ViewLocation;
    FRotator ViewRotation;

    PC->GetPlayerViewPoint(
        ViewLocation,
        ViewRotation);
    const double Now = World->GetTimeSeconds();

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Target = *It;

        if (!IsValid(Target) || Target == Observer)
        {
            continue;
        }

        if (!HasTargetTag(Target))
        {
            continue;
        }

        const bool bVisible = CanSeeTarget(
            Target,
            ViewLocation,
            ViewRotation
        );

        const TWeakObjectPtr<AActor> Key(Target);

        if (bVisible)
        {
            const double* PreviousSeen = LastVisibleTimes.Find(Key);
            if (!PreviousSeen
                || Now - *PreviousSeen
                    > UpdateInterval + VisibilityGracePeriod)
            {
                Target->ForceNetUpdate();
            }

            LastVisibleTimes.Add(Key, Now);
        }

        if (bDebugDraw)
        {
            FVector Origin;
            FVector Extent;
            Target->GetActorBounds(false, Origin, Extent);

            DrawDebugLine(
                World,
                ViewLocation,
                Origin,
                bVisible ? FColor::Green : FColor::Red,
                false,
                UpdateInterval,
                0,
                1.0f
            );
        }
    }

    // Remove destroyed actors and stale visibility records.
    for (auto It = LastVisibleTimes.CreateIterator(); It; ++It)
    {
        AActor* Target = It.Key().Get();

        if (!IsValid(Target)
            || Now - It.Value() > VisibilityGracePeriod + 5.0)
        {
            It.RemoveCurrent();
        }
    }
}
