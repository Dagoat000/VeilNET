#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VeilNetShadowProxyComponent.generated.h"

class AVeilNetShadowProxy;
class USkeletalMeshComponent;

UCLASS(ClassGroup=(VeilNet), meta=(BlueprintSpawnableComponent))
class VEILNETRUNTIME_API UVeilNetShadowProxyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVeilNetShadowProxyComponent();

	UPROPERTY(EditAnywhere, Category="VeilNet|Shadow Proxy")
	TObjectPtr<USkeletalMeshComponent> SourceMesh;

	UPROPERTY(EditAnywhere, Category="VeilNet|Shadow Proxy")
	TSubclassOf<AVeilNetShadowProxy> ProxyClass;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AVeilNetShadowProxy> ShadowProxy;
};
