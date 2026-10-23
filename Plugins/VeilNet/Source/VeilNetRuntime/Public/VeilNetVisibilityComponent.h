
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TimerManager.h"
#include "VeilNetVisibilityComponent.generated.h"

struct FCollisionQueryParams;

UCLASS(ClassGroup=(VeilNet), meta=(BlueprintSpawnableComponent))
class VEILNETRUNTIME_API UVeilNetVisibilityComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UVeilNetVisibilityComponent();

    // Query this on the server from the target actor's relevancy policy.
    UFUNCTION(BlueprintCallable, Category="VeilNet")
    bool IsActorVisibleForReplication(const AActor* Target) const;

    static bool IsActorVisibleForViewer(
        const AActor* Target,
        const AActor* RealViewer,
        const AActor* ViewTarget);

    UPROPERTY(EditAnywhere, Category="VeilNet|Targets")
    TArray<FName> TargetTags;

    UPROPERTY(EditAnywhere, Category="VeilNet|Targets")
    bool bIncludePlayerControlledPawns = true;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(
        const EEndPlayReason::Type EndPlayReason) override;

private:
    UPROPERTY(EditAnywhere, Category="VeilNet|Targets")
    FName RequiredTargetTag = NAME_None;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision")
    TEnumAsByte<ECollisionChannel> VisibilityTraceChannel = ECC_Visibility;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="100.0"))
    float VisionRadius = 2000.0f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="1.0", ClampMax="50.0"))
    float VisibilitySampleSpacing = 12.0f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="8", ClampMax="128"))
    int32 MaxVisibilityTraceCount = 64;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="0.033"))
    float UpdateInterval = 0.033f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="1.0", ClampMax="179.0"))
    float HorizontalFOV = 90.0f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="1.0", ClampMax="179.0"))
    float VerticalFOV = 70.0f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="0.0", ClampMax="45.0"))
    float FOVPaddingDegrees = 5.0f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Vision",
        meta=(ClampMin="0.0", ClampMax="2.0"))
    float VisibilityGracePeriod = 0.033f;

    UPROPERTY(EditAnywhere, Category="VeilNet|Debug")
    bool bDebugDraw = false;

    UPROPERTY(EditAnywhere, Category="VeilNet|Debug")
    bool bLogVisibilityTransitions = true;

    FTimerHandle UpdateTimer;

    // Server time at which each target was last confirmed visible.
    TMap<TWeakObjectPtr<AActor>, double> LastVisibleTimes;
    TMap<TWeakObjectPtr<AActor>, bool> LastLoggedVisibility;

    bool HasTargetTag(const AActor* Actor) const;

    bool IsPointInView(
        const FVector& ViewLocation,
        const FRotator& ViewRotation,
        const FVector& Point) const;

    bool IsPointVisible(
        const FVector& ViewLocation,
        const FVector& Point,
        const FCollisionQueryParams& Params) const;

    bool CanSeeTarget(
        AActor* Target,
        const FVector& ViewLocation,
        const FRotator& ViewRotation) const;

    void GatherBodySamples(AActor* Target, TArray<FVector>& OutSamples) const;

    void UpdateFogOfWar();
};
