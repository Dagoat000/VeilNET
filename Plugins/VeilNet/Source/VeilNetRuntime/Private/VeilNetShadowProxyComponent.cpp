#include "VeilNetShadowProxyComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "VeilNetRuntime.h"
#include "VeilNetShadowProxy.h"

UVeilNetShadowProxyComponent::UVeilNetShadowProxyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	ProxyClass = AVeilNetShadowProxy::StaticClass();
}

void UVeilNetShadowProxyComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !Owner->HasAuthority() || !World)
	{
		return;
	}

	if (!IsValid(SourceMesh))
	{
		SourceMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	}

	if (!IsValid(SourceMesh) || !SourceMesh->GetSkeletalMeshAsset())
	{
		UE_LOG(
			LogVeilNet,
			Warning,
			TEXT("VeilNet shadow proxy on %s has no source skeletal mesh."),
			*GetNameSafe(Owner));
		return;
	}

	if (!ProxyClass)
	{
		UE_LOG(
			LogVeilNet,
			Error,
			TEXT("VeilNet shadow proxy on %s has no proxy class configured."),
			*GetNameSafe(Owner));
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = Owner;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ShadowProxy = World->SpawnActor<AVeilNetShadowProxy>(
		ProxyClass,
		Owner->GetActorTransform(),
		SpawnParameters);

	if (!IsValid(ShadowProxy)
		|| !ShadowProxy->InitializeFromSource(Owner, SourceMesh))
	{
		if (IsValid(ShadowProxy))
		{
			ShadowProxy->Destroy();
			ShadowProxy = nullptr;
		}

		UE_LOG(
			LogVeilNet,
			Error,
			TEXT("Failed to initialize VeilNet shadow proxy for %s."),
			*GetNameSafe(Owner));
	}
}

void UVeilNetShadowProxyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (IsValid(ShadowProxy))
	{
		ShadowProxy->Destroy();
		ShadowProxy = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}
