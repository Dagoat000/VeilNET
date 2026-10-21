// Copyright Epic Games, Inc. All Rights Reserved.

#include "TestingPlaceCharacter.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "VeilNetVisibilityComponent.h"
#include "TestingPlace.h"
#include "VeilNetShadowProxyComponent.h"

ATestingPlaceCharacter::ATestingPlaceCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
	
	// Create the first person mesh that will be viewed only by this character's owner
	FirstPersonMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("First Person Mesh"));
	VisibilityComponent =
		CreateDefaultSubobject<UVeilNetVisibilityComponent>(TEXT("VeilNet Visibility"));
	VisibilityComponent->TargetTags.Add(FName(TEXT("Enemy")));

	ShadowProxyComponent =
		CreateDefaultSubobject<UVeilNetShadowProxyComponent>(TEXT("VeilNet Shadow Proxy"));
	ShadowProxyComponent->SourceMesh = GetMesh();

	FirstPersonMesh->SetupAttachment(GetMesh());
	FirstPersonMesh->SetOnlyOwnerSee(true);
	FirstPersonMesh->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;
	FirstPersonMesh->SetCollisionProfileName(FName("NoCollision"));

	// Create the Camera Component	
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("First Person Camera"));
	FirstPersonCameraComponent->SetupAttachment(FirstPersonMesh, FName("head"));
	FirstPersonCameraComponent->SetRelativeLocationAndRotation(FVector(-2.8f, 5.89f, 0.0f), FRotator(0.0f, 90.0f, -90.0f));
	FirstPersonCameraComponent->bUsePawnControlRotation = true;
	FirstPersonCameraComponent->bEnableFirstPersonFieldOfView = true;
	FirstPersonCameraComponent->bEnableFirstPersonScale = true;
	FirstPersonCameraComponent->FirstPersonFieldOfView = 70.0f;
	FirstPersonCameraComponent->FirstPersonScale = 0.6f;

	// configure the character comps
	GetMesh()->SetOwnerNoSee(true);
	GetMesh()->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::WorldSpaceRepresentation;

	GetCapsuleComponent()->SetCapsuleSize(34.0f, 96.0f);

	// Configure character movement
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;
	GetCharacterMovement()->AirControl = 0.5f;
}

bool ATestingPlaceCharacter::IsNetRelevantFor(
	const AActor* RealViewer,
	const AActor* ViewTarget,
	const FVector& SrcLocation) const
{
	if (!Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation))
	{
		return false;
	}

	if (ViewTarget == this)
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

	if (ViewerPawn == this)
	{
		return true;
	}

	const UVeilNetVisibilityComponent* FogOfWar =
		ViewerPawn
			? ViewerPawn->FindComponentByClass<UVeilNetVisibilityComponent>()
			: nullptr;

	return FogOfWar
		&& FogOfWar->IsActorVisibleForReplication(this);
}

void ATestingPlaceCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{	
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ATestingPlaceCharacter::DoJumpStart);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ATestingPlaceCharacter::DoJumpEnd);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ATestingPlaceCharacter::MoveInput);

		// Looking/Aiming
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ATestingPlaceCharacter::LookInput);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ATestingPlaceCharacter::LookInput);
	}
	else
	{
		UE_LOG(LogTestingPlace, Error, TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}


void ATestingPlaceCharacter::MoveInput(const FInputActionValue& Value)
{
	// get the Vector2D move axis
	FVector2D MovementVector = Value.Get<FVector2D>();

	// pass the axis values to the move input
	DoMove(MovementVector.X, MovementVector.Y);

}

void ATestingPlaceCharacter::LookInput(const FInputActionValue& Value)
{
	// get the Vector2D look axis
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	// pass the axis values to the aim input
	DoAim(LookAxisVector.X, LookAxisVector.Y);

}

void ATestingPlaceCharacter::DoAim(float Yaw, float Pitch)
{
	if (GetController())
	{
		// pass the rotation inputs
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void ATestingPlaceCharacter::DoMove(float Right, float Forward)
{
	if (GetController())
	{
		// pass the move inputs
		AddMovementInput(GetActorRightVector(), Right);
		AddMovementInput(GetActorForwardVector(), Forward);
	}
}

void ATestingPlaceCharacter::DoJumpStart()
{
	// pass Jump to the character
	Jump();
}

void ATestingPlaceCharacter::DoJumpEnd()
{
	// pass StopJumping to the character
	StopJumping();
}
