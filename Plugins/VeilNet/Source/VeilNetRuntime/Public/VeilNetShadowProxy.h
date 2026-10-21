#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "VeilNetShadowProxy.generated.h"

class USkeletalMesh;
class USkeletalMeshComponent;
class UPoseableMeshComponent;

USTRUCT()
struct VEILNETRUNTIME_API FVeilNetShadowBonePose
{
    GENERATED_BODY()

    UPROPERTY()
    float SampleTimeSeconds = 0.0f;

    UPROPERTY()
    TArray<FVector_NetQuantize10> Translations;

    UPROPERTY()
    TArray<FRotator> Rotations;

    UPROPERTY()
    TArray<FVector_NetQuantize10> Scales;

    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
    {
        constexpr uint32 MaxBoneCount = 512;
        bOutSuccess = true;
        Ar << SampleTimeSeconds;
        uint32 BoneCount = Ar.IsSaving()
            ? static_cast<uint32>(Translations.Num())
            : 0;

        if (Ar.IsSaving()
            && (Rotations.Num() != Translations.Num()
                || Scales.Num() != Translations.Num()))
        {
            Ar.SetError();
            bOutSuccess = false;
            return false;
        }

        Ar.SerializeIntPacked(BoneCount);
        if (BoneCount > MaxBoneCount)
        {
            Ar.SetError();
            bOutSuccess = false;
            return false;
        }

        if (Ar.IsLoading())
        {
            Translations.SetNumUninitialized(BoneCount);
            Rotations.SetNumUninitialized(BoneCount);
            Scales.SetNumUninitialized(BoneCount);
        }

        for (uint32 Index = 0; Index < BoneCount; ++Index)
        {
            bool bVectorSuccess = true;
            Translations[Index].NetSerialize(Ar, Map, bVectorSuccess);
            bOutSuccess &= bVectorSuccess;
            Rotations[Index].SerializeCompressedShort(Ar);
            Scales[Index].NetSerialize(Ar, Map, bVectorSuccess);
            bOutSuccess &= bVectorSuccess;
        }

        return true;
    }
};

template<>
struct TStructOpsTypeTraits<FVeilNetShadowBonePose>
    : public TStructOpsTypeTraitsBase2<FVeilNetShadowBonePose>
{
    enum
    {
        WithNetSerializer = true
    };
};

USTRUCT()
struct VEILNETRUNTIME_API FVeilNetShadowProxyState
{
    GENERATED_BODY()

    UPROPERTY()
    FVector_NetQuantize10 Location = FVector::ZeroVector;

    UPROPERTY()
    FRotator Rotation = FRotator::ZeroRotator;

    UPROPERTY()
    FVector_NetQuantize10 Velocity = FVector::ZeroVector;

    UPROPERTY()
    float ServerTimeSeconds = 0.0f;

};

UCLASS(Blueprintable)
class VEILNETRUNTIME_API AVeilNetShadowProxy : public AActor
{
    GENERATED_BODY()

public:
    AVeilNetShadowProxy();

    bool InitializeFromSource(
        AActor* SourceActor,
        USkeletalMeshComponent* SourceMesh);

protected:
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;
    virtual bool IsNetRelevantFor(
        const AActor* RealViewer,
        const AActor* ViewTarget,
        const FVector& SrcLocation) const override;
    virtual void GetLifetimeReplicatedProps(
        TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
    UPROPERTY(ReplicatedUsing=OnRep_ShadowMesh)
    TObjectPtr<USkeletalMesh> ShadowMesh;

    UPROPERTY(ReplicatedUsing=OnRep_ShadowMeshTransform)
    FTransform ShadowMeshTransform = FTransform::Identity;

    UPROPERTY(ReplicatedUsing=OnRep_ShadowState)
    FVeilNetShadowProxyState ShadowState;

    UPROPERTY(VisibleAnywhere, Category="VeilNet|Shadow Proxy")
    TObjectPtr<UPoseableMeshComponent> ShadowPoseMesh;

    UPROPERTY(ReplicatedUsing=OnRep_ShadowBoneTransforms)
    FVeilNetShadowBonePose ShadowBonePose;

    FVeilNetShadowBonePose PreviousShadowBonePose;
    FVeilNetShadowBonePose LastReceivedShadowBonePose;

    TWeakObjectPtr<AActor> SourceActor;
    TWeakObjectPtr<USkeletalMeshComponent> SourceMeshComponent;
    EVisibilityBasedAnimTickOption SourceAnimTickOption =
        EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    bool bSourceTickSettingsCaptured = false;
    bool bSourceUpdateRateOptimizations = true;
    float PoseUpdateAccumulator = 0.0f;

    UFUNCTION()
    void OnRep_ShadowMesh();

    UFUNCTION()
    void OnRep_ShadowMeshTransform();

    UFUNCTION()
    void OnRep_ShadowState();

    UFUNCTION()
    void OnRep_ShadowBoneTransforms();

    void ApplyShadowMeshSettings();
    void UpdateShadowPose();
    void ApplyShadowPose();
    void ApplyPredictedTransform();
};
