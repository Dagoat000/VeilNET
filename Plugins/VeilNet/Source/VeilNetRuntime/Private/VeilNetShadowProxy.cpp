#include "VeilNetShadowProxy.h"

#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "VeilNetVisibilityComponent.h"

namespace
{
    constexpr float ShadowPoseUpdateInterval = 1.0f / 60.0f;
    constexpr float MaxShadowPoseExtrapolationSeconds = 0.05f;
    constexpr float MaxShadowPosePredictionIntervals = 2.0f;
}

AVeilNetShadowProxy::AVeilNetShadowProxy()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    bReplicates = true;
    bAlwaysRelevant = false;
    SetReplicateMovement(false);

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

    ShadowPoseMesh = CreateDefaultSubobject<UPoseableMeshComponent>(
        TEXT("Shadow Pose Mesh"));
    ShadowPoseMesh->SetupAttachment(RootComponent);
    ShadowPoseMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ShadowPoseMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::None);
    ShadowPoseMesh->SetOwnerNoSee(false);
    ShadowPoseMesh->SetCastShadow(true);
    ShadowPoseMesh->SetCastHiddenShadow(true);
    ShadowPoseMesh->SetHiddenInGame(true);

    SetNetUpdateFrequency(60.0f);
    SetMinNetUpdateFrequency(60.0f);
    NetPriority = 3.0f;
}

bool AVeilNetShadowProxy::IsNetRelevantFor(
    const AActor* RealViewer,
    const AActor* ViewTarget,
    const FVector& SrcLocation) const
{
    const AActor* Source = SourceActor.Get();
    if (!IsValid(Source)
        || !Source->IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation))
    {
        return false;
    }

    if (ViewTarget == Source)
    {
        return true;
    }

    const APawn* ViewerPawn = Cast<APawn>(ViewTarget);
    if (!ViewerPawn)
    {
        if (const APlayerController* PlayerController =
            Cast<APlayerController>(RealViewer))
        {
            ViewerPawn = PlayerController->GetPawn();
        }
    }

    if (ViewerPawn == Source)
    {
        return true;
    }

    const UVeilNetVisibilityComponent* Visibility =
        ViewerPawn
            ? ViewerPawn->FindComponentByClass<UVeilNetVisibilityComponent>()
            : nullptr;

    return Visibility
        && Visibility->IsActorVisibleForReplication(Source);
}

bool AVeilNetShadowProxy::InitializeFromSource(
    AActor* InSourceActor,
    USkeletalMeshComponent* InSourceMesh)
{
    if (!HasAuthority() || !IsValid(InSourceActor) || !IsValid(InSourceMesh))
    {
        return false;
    }

    if (SourceMeshComponent.Get() != InSourceMesh)
    {
        if (bSourceTickSettingsCaptured)
        {
            if (USkeletalMeshComponent* PreviousSourceMesh =
                SourceMeshComponent.Get())
            {
                PreviousSourceMesh->VisibilityBasedAnimTickOption =
                    SourceAnimTickOption;
                PreviousSourceMesh->bEnableUpdateRateOptimizations =
                    bSourceUpdateRateOptimizations;
            }
        }
        bSourceTickSettingsCaptured = false;
    }
    SourceActor = InSourceActor;
    SourceMeshComponent = InSourceMesh;
    if (!bSourceTickSettingsCaptured)
    {
        SourceAnimTickOption = InSourceMesh->VisibilityBasedAnimTickOption;
        bSourceUpdateRateOptimizations =
            InSourceMesh->bEnableUpdateRateOptimizations;
        bSourceTickSettingsCaptured = true;
    }
    InSourceMesh->VisibilityBasedAnimTickOption =
        EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    InSourceMesh->bEnableUpdateRateOptimizations = false;

    ShadowMesh = InSourceMesh->GetSkeletalMeshAsset();
    ShadowMeshTransform = InSourceMesh->GetComponentTransform()
        .GetRelativeTransform(InSourceActor->GetActorTransform());
    ShadowState.Location = InSourceActor->GetActorLocation();
    ShadowState.Rotation = InSourceActor->GetActorRotation();
    ShadowState.Velocity = InSourceActor->GetVelocity();
    ShadowState.ServerTimeSeconds = GetWorld()->GetTimeSeconds();
    SetActorTransform(InSourceActor->GetActorTransform());
    ApplyShadowMeshSettings();
    UpdateShadowPose();
    ForceNetUpdate();
    return true;
}

void AVeilNetShadowProxy::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (!HasAuthority())
    {
        ApplyPredictedTransform();
        ApplyShadowPose();
        return;
    }

    AActor* Source = SourceActor.Get();
    if (!Source)
    {
        return;
    }

    ShadowState.Location = Source->GetActorLocation();
    ShadowState.Rotation = Source->GetActorRotation();
    ShadowState.Velocity = Source->GetVelocity();
    ShadowState.ServerTimeSeconds = GetWorld()->GetTimeSeconds();
    SetActorTransform(Source->GetActorTransform());

    PoseUpdateAccumulator += DeltaSeconds;
    if (PoseUpdateAccumulator >= ShadowPoseUpdateInterval)
    {
        PoseUpdateAccumulator = FMath::Fmod(
            PoseUpdateAccumulator,
            ShadowPoseUpdateInterval);
        UpdateShadowPose();
        ForceNetUpdate();
    }
}

void AVeilNetShadowProxy::EndPlay(EEndPlayReason::Type EndPlayReason)
{
    if (bSourceTickSettingsCaptured)
    {
        if (USkeletalMeshComponent* SourceMesh = SourceMeshComponent.Get())
        {
            SourceMesh->VisibilityBasedAnimTickOption = SourceAnimTickOption;
            SourceMesh->bEnableUpdateRateOptimizations =
                bSourceUpdateRateOptimizations;
        }
    }

    Super::EndPlay(EndPlayReason);
}

void AVeilNetShadowProxy::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AVeilNetShadowProxy, ShadowMesh);
    DOREPLIFETIME(AVeilNetShadowProxy, ShadowMeshTransform);
    DOREPLIFETIME(AVeilNetShadowProxy, ShadowState);
    DOREPLIFETIME(AVeilNetShadowProxy, ShadowBonePose);
}

void AVeilNetShadowProxy::OnRep_ShadowMesh()
{
    ApplyShadowMeshSettings();
    ApplyShadowPose();
}

void AVeilNetShadowProxy::OnRep_ShadowMeshTransform()
{
    ApplyShadowMeshSettings();
}

void AVeilNetShadowProxy::OnRep_ShadowState()
{
    ApplyPredictedTransform();
}

void AVeilNetShadowProxy::OnRep_ShadowBoneTransforms()
{
    PreviousShadowBonePose =
        LastReceivedShadowBonePose.Translations.IsEmpty()
            ? ShadowBonePose
            : LastReceivedShadowBonePose;
    LastReceivedShadowBonePose = ShadowBonePose;
    ApplyShadowPose();
}

void AVeilNetShadowProxy::UpdateShadowPose()
{
    USkeletalMeshComponent* SourceMesh = SourceMeshComponent.Get();
    if (!SourceMesh || !ShadowPoseMesh)
    {
        return;
    }

    const TArray<FTransform> BoneTransforms =
        SourceMesh->GetBoneSpaceTransforms();
    if (BoneTransforms.IsEmpty())
    {
        return;
    }

    PreviousShadowBonePose = ShadowBonePose;
    ShadowBonePose.SampleTimeSeconds = GetWorld()->GetTimeSeconds();
    ShadowBonePose.Translations.SetNumUninitialized(BoneTransforms.Num());
    ShadowBonePose.Rotations.SetNumUninitialized(BoneTransforms.Num());
    ShadowBonePose.Scales.SetNumUninitialized(BoneTransforms.Num());
    for (int32 Index = 0; Index < BoneTransforms.Num(); ++Index)
    {
        ShadowBonePose.Translations[Index] = BoneTransforms[Index].GetLocation();
        ShadowBonePose.Rotations[Index] =
            BoneTransforms[Index].GetRotation().Rotator();
        ShadowBonePose.Scales[Index] = BoneTransforms[Index].GetScale3D();
    }
    ApplyShadowPose();
}

void AVeilNetShadowProxy::ApplyShadowPose()
{
    if (!ShadowPoseMesh
        || !ShadowPoseMesh->GetSkinnedAsset()
        || ShadowBonePose.Translations.Num() != ShadowPoseMesh->GetNumBones()
        || ShadowBonePose.Rotations.Num() != ShadowPoseMesh->GetNumBones()
        || ShadowBonePose.Scales.Num() != ShadowPoseMesh->GetNumBones())
    {
        return;
    }

    float PredictionAlpha = 0.0f;
    const bool bCanPredictPose =
        !HasAuthority()
        && PreviousShadowBonePose.Translations.Num()
            == ShadowBonePose.Translations.Num()
        && PreviousShadowBonePose.Rotations.Num()
            == ShadowBonePose.Rotations.Num();
    if (bCanPredictPose)
    {
        const UWorld* World = GetWorld();
        const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        const float SampleInterval =
            ShadowBonePose.SampleTimeSeconds
            - PreviousShadowBonePose.SampleTimeSeconds;
        if (GameState && SampleInterval > KINDA_SMALL_NUMBER)
        {
            const float PoseAge = FMath::Clamp(
                GameState->GetServerWorldTimeSeconds()
                    - ShadowBonePose.SampleTimeSeconds,
                0.0f,
                MaxShadowPoseExtrapolationSeconds);
            PredictionAlpha = FMath::Min(
                PoseAge / SampleInterval,
                MaxShadowPosePredictionIntervals);
        }
    }

    TArray<FTransform>& BoneTransforms = ShadowPoseMesh->BoneSpaceTransforms;
    BoneTransforms.SetNumUninitialized(ShadowBonePose.Translations.Num());
    for (int32 Index = 0; Index < BoneTransforms.Num(); ++Index)
    {
        FVector Translation = ShadowBonePose.Translations[Index];
        FQuat Rotation = ShadowBonePose.Rotations[Index].Quaternion();
        if (PredictionAlpha > 0.0f)
        {
            Translation +=
                (Translation - PreviousShadowBonePose.Translations[Index])
                * PredictionAlpha;
            const FQuat PreviousRotation =
                PreviousShadowBonePose.Rotations[Index].Quaternion();
            const FQuat RotationDelta =
                Rotation * PreviousRotation.Inverse();
            Rotation = FQuat::Slerp(
                FQuat::Identity,
                RotationDelta,
                PredictionAlpha) * Rotation;
            Rotation.Normalize();
        }

        BoneTransforms[Index] = FTransform(
            Rotation,
            Translation,
            ShadowBonePose.Scales[Index]);
    }
    ShadowPoseMesh->MarkRefreshTransformDirty();
    ShadowPoseMesh->RefreshBoneTransforms();
}

void AVeilNetShadowProxy::ApplyPredictedTransform()
{
    UWorld* World = GetWorld();
    const AGameStateBase* GameState =
        World ? World->GetGameState() : nullptr;
    if (!GameState)
    {
        return;
    }

    const float StateAge = FMath::Clamp(
        GameState->GetServerWorldTimeSeconds()
            - ShadowState.ServerTimeSeconds,
        0.0f,
        0.25f);
    const FVector PredictedLocation =
        FVector(ShadowState.Location)
        + FVector(ShadowState.Velocity) * StateAge;

    SetActorLocationAndRotation(
        PredictedLocation,
        ShadowState.Rotation,
        false,
        nullptr,
        ETeleportType::TeleportPhysics);
}

void AVeilNetShadowProxy::ApplyShadowMeshSettings()
{
    if (!ShadowPoseMesh)
    {
        return;
    }

    UPoseableMeshComponent* ShadowMeshComponent = ShadowPoseMesh;
    ShadowMeshComponent->SetSkinnedAssetAndUpdate(ShadowMesh.Get());
    ShadowMeshComponent->SetRelativeTransform(ShadowMeshTransform);

    ShadowMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ShadowMeshComponent->SetFirstPersonPrimitiveType(
        EFirstPersonPrimitiveType::None);
    ShadowMeshComponent->SetOwnerNoSee(false);
    ShadowMeshComponent->SetCastShadow(true);
    ShadowMeshComponent->SetCastHiddenShadow(true);
    ShadowMeshComponent->SetHiddenInGame(true);
    ShadowMeshComponent->bEnableUpdateRateOptimizations = false;
}
