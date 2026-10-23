#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VeilNetAntiCheatComponent.generated.h"

UCLASS(ClassGroup=(VeilNet), meta=(BlueprintSpawnableComponent))
class VEILNETRUNTIME_API UVeilNetAntiCheatComponent
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UVeilNetAntiCheatComponent();

	UPROPERTY(EditAnywhere, Category="VeilNet|Anti-Cheat")
	FString ServiceBaseUrl;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="VeilNet|Anti-Cheat")
	bool bDeviceValidated = false;

	UFUNCTION(BlueprintCallable, Category="VeilNet|Anti-Cheat")
	void SubmitDeviceForValidation();

private:
	UFUNCTION(Server, Reliable)
	void Server_SubmitFingerprint(const FString& Fingerprint);

	void CheckDeviceOnServer(const FString& Fingerprint);
	void RejectPlayer(const FString& Message);
	bool bValidationStarted = false;
};
