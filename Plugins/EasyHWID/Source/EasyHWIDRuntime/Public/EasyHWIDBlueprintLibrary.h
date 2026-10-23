#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EasyHWIDBlueprintLibrary.generated.h"

UCLASS()
class EASYHWIDRUNTIME_API UEasyHWIDBlueprintLibrary
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="EasyHWID",
		meta=(DisplayName="Get Hardware Fingerprint"))
	static bool GetHardwareFingerprint(FString& OutFingerprint);
};
