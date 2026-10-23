#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VeilNetMovementAuditComponent.generated.h"

UCLASS(ClassGroup=(VeilNet), meta=(BlueprintSpawnableComponent))
class VEILNETRUNTIME_API UVeilNetMovementAuditComponent
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UVeilNetMovementAuditComponent();

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="1.0", ClampMax="5.0"))
	float MaxSpeedMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="0.0", ClampMax="1000.0"))
	float SpeedAllowance = 100.0f;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="1.0", ClampMax="5.0"))
	float VerticalSpeedMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="0.0", ClampMax="1000.0"))
	float VerticalSpeedAllowance = 150.0f;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="1", ClampMax="20"))
	int32 ConsecutiveSamplesBeforeLog = 3;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="0.05", ClampMax="1.0"))
	float SampleInterval = 0.2f;

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat",
		meta=(ClampMin="0.0", ClampMax="1000.0"))
	float PositionDisplacementAllowance = 150.0f;

	UFUNCTION(BlueprintCallable, Category="VeilNet|Anti-Cheat",
		meta=(BlueprintAuthorityOnly))
	void ResetPositionBaseline();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void SubmitAuditEvent(
		const TCHAR* EventKind,
		double Observed,
		double Allowed,
		EMovementMode MovementMode,
		double ServerTimeSeconds);

	float SampleAccumulator = 0.0f;
	int32 ConsecutiveExcessiveSpeedSamples = 0;
	bool bLoggedCurrentIncident = false;
	bool bHasPositionBaseline = false;
	bool bLoggedPositionDiscontinuity = false;
	FVector LastSampledLocation = FVector::ZeroVector;
	double LastPositionSampleTime = 0.0;
	double LastAuditSubmissionTime = -1.0;
	EMovementMode LastMovementMode = MOVE_None;
};
