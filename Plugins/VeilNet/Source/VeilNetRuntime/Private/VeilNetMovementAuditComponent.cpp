#include "VeilNetMovementAuditComponent.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PhysicsVolume.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VeilNetRuntime.h"

void UVeilNetMovementAuditComponent::SubmitAuditEvent(
	const TCHAR* EventKind,
	double Observed,
	double Allowed,
	EMovementMode MovementMode,
	double ServerTimeSeconds)
{
	const FString ServiceBaseUrl =
		FPlatformMisc::GetEnvironmentVariable(TEXT("VEILNET_AUDIT_URL"));
	const FString GameToken =
		FPlatformMisc::GetEnvironmentVariable(TEXT("VEILNET_GAME_TOKEN"));
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	const APlayerController* PlayerController = Character
		? Cast<APlayerController>(Character->GetController())
		: nullptr;
	const APlayerState* PlayerState = PlayerController
		? PlayerController->PlayerState
		: nullptr;

	if (ServiceBaseUrl.IsEmpty() || GameToken.IsEmpty() || !PlayerState)
	{
		return;
	}
	if (ServerTimeSeconds - LastAuditSubmissionTime < 1.0)
	{
		return;
	}

	const int32 PlayerId = PlayerState->GetPlayerId();
	if (PlayerId < 0)
	{
		return;
	}

	FString BaseUrl = ServiceBaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	TSharedRef<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("kind"), EventKind);
	RequestBody->SetNumberField(TEXT("playerId"), PlayerId);
	RequestBody->SetNumberField(TEXT("observed"), Observed);
	RequestBody->SetNumberField(TEXT("allowed"), Allowed);
	RequestBody->SetNumberField(
		TEXT("movementMode"),
		static_cast<int32>(MovementMode));
	RequestBody->SetNumberField(TEXT("serverTime"), ServerTimeSeconds);

	FString SerializedBody;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&SerializedBody);
	if (!FJsonSerializer::Serialize(RequestBody, Writer))
	{
		UE_LOG(
			LogVeilNet,
			Error,
			TEXT("Failed to serialize a VeilNet movement audit event."));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	Request->SetURL(BaseUrl + TEXT("/api/v1/game/events"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(
		TEXT("Authorization"),
		FString::Printf(TEXT("Bearer %s"), *GameToken));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(SerializedBody);
	Request->SetTimeout(5.0f);
	LastAuditSubmissionTime = ServerTimeSeconds;
	Request->OnProcessRequestComplete().BindLambda(
		[](FHttpRequestPtr,
			FHttpResponsePtr Response,
			const bool bWasSuccessful)
		{
			if (!bWasSuccessful
				|| !Response.IsValid()
				|| !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
			{
				UE_LOG(
					LogVeilNet,
					Warning,
					TEXT("VeilNet could not persist a movement audit event."));
			}
		});

	if (!Request->ProcessRequest())
	{
		UE_LOG(
			LogVeilNet,
			Warning,
			TEXT("VeilNet could not start a movement audit event request."));
	}
}

UVeilNetMovementAuditComponent::UVeilNetMovementAuditComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	SetIsReplicatedByDefault(false);
}

void UVeilNetMovementAuditComponent::BeginPlay()
{
	Super::BeginPlay();
	SetComponentTickInterval(FMath::Clamp(SampleInterval, 0.05f, 1.0f));
}

void UVeilNetMovementAuditComponent::ResetPositionBaseline()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	bHasPositionBaseline = false;
	bLoggedPositionDiscontinuity = false;
}

void UVeilNetMovementAuditComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!Character || !Character->HasAuthority())
	{
		return;
	}

	APlayerController* PlayerController =
		Cast<APlayerController>(Character->GetController());
	UCharacterMovementComponent* Movement =
		Character->GetCharacterMovement();
	if (!PlayerController || !Movement)
	{
		ConsecutiveExcessiveSpeedSamples = 0;
		bLoggedCurrentIncident = false;
		bHasPositionBaseline = false;
		bLoggedPositionDiscontinuity = false;
		LastMovementMode = MOVE_None;
		return;
	}

	const EMovementMode MovementMode = Movement->MovementMode;
	if (MovementMode == MOVE_None)
	{
		ConsecutiveExcessiveSpeedSamples = 0;
		bLoggedCurrentIncident = false;
		LastMovementMode = MovementMode;
		return;
	}
	if (MovementMode != LastMovementMode)
	{
		ConsecutiveExcessiveSpeedSamples = 0;
		bLoggedCurrentIncident = false;
		LastMovementMode = MovementMode;
	}

	SampleAccumulator += DeltaTime;
	const float Interval = FMath::Clamp(SampleInterval, 0.05f, 1.0f);
	if (SampleAccumulator < Interval)
	{
		return;
	}
	SampleAccumulator = FMath::Fmod(SampleAccumulator, Interval);

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const FVector CurrentLocation = Character->GetActorLocation();
	if (!bHasPositionBaseline)
	{
		LastSampledLocation = CurrentLocation;
		LastPositionSampleTime = Now;
		bHasPositionBaseline = true;
	}
	else
	{
		const double ElapsedSeconds = Now - LastPositionSampleTime;
		if (ElapsedSeconds <= 0.0 || ElapsedSeconds > 2.0)
		{
			LastSampledLocation = CurrentLocation;
			LastPositionSampleTime = Now;
			bLoggedPositionDiscontinuity = false;
		}
		else
		{
			const float MaximumAllowedSpeed =
				FMath::Max(Movement->GetMaxSpeed(), 0.0f)
					* FMath::Clamp(MaxSpeedMultiplier, 1.0f, 5.0f)
				+ FMath::Clamp(SpeedAllowance, 0.0f, 1000.0f);
			float MaximumAllowedVerticalSpeed = MaximumAllowedSpeed;
			if (MovementMode == MOVE_Falling)
			{
				if (CurrentLocation.Z >= LastSampledLocation.Z)
				{
					MaximumAllowedVerticalSpeed =
						FMath::Max(Movement->JumpZVelocity, 0.0f)
							* FMath::Clamp(
								VerticalSpeedMultiplier,
								1.0f,
								5.0f)
						+ FMath::Clamp(
							VerticalSpeedAllowance,
							0.0f,
							1000.0f);
				}
				else
				{
					const APhysicsVolume* PhysicsVolume =
						Character->GetPhysicsVolume();
					const float TerminalVelocity = PhysicsVolume
						? PhysicsVolume->TerminalVelocity
						: 4000.0f;
					MaximumAllowedVerticalSpeed =
						FMath::Max(TerminalVelocity, 0.0f)
							* FMath::Clamp(
								VerticalSpeedMultiplier,
								1.0f,
								5.0f)
						+ FMath::Clamp(
							VerticalSpeedAllowance,
							0.0f,
							1000.0f);
				}
			}

			const float ObservedHorizontalDisplacement = FVector::Dist2D(
				LastSampledLocation,
				CurrentLocation);
			const float ObservedVerticalDisplacement = FMath::Abs(
				CurrentLocation.Z - LastSampledLocation.Z);
			const float MaximumAllowedHorizontalDisplacement =
				MaximumAllowedSpeed * static_cast<float>(ElapsedSeconds)
				+ FMath::Clamp(PositionDisplacementAllowance, 0.0f, 1000.0f);
			const float MaximumAllowedVerticalDisplacement =
				MaximumAllowedVerticalSpeed
					* static_cast<float>(ElapsedSeconds)
				+ FMath::Clamp(PositionDisplacementAllowance, 0.0f, 1000.0f);

			if ((ObservedHorizontalDisplacement
					> MaximumAllowedHorizontalDisplacement
				|| ObservedVerticalDisplacement
					> MaximumAllowedVerticalDisplacement)
				&& !bLoggedPositionDiscontinuity)
			{
				const APlayerState* PlayerState = PlayerController->PlayerState;
				const FString PlayerName = PlayerState
					? PlayerState->GetPlayerName()
					: PlayerController->GetName();
				UE_LOG(
					LogVeilNet,
					Warning,
					TEXT("Movement position discontinuity: player=%s controller=%s horizontal=%.1f/%.1f vertical=%.1f/%.1f mode=%d elapsed=%.3f server_time=%.2f"),
					*PlayerName,
					*GetNameSafe(PlayerController),
					ObservedHorizontalDisplacement,
					MaximumAllowedHorizontalDisplacement,
					ObservedVerticalDisplacement,
					MaximumAllowedVerticalDisplacement,
					static_cast<int32>(MovementMode),
					static_cast<float>(ElapsedSeconds),
					Now);
				SubmitAuditEvent(
					TEXT("movement_discontinuity"),
					FMath::Max(
						ObservedHorizontalDisplacement,
						ObservedVerticalDisplacement),
					FMath::Max(
						MaximumAllowedHorizontalDisplacement,
						MaximumAllowedVerticalDisplacement),
					MovementMode,
					Now);
				bLoggedPositionDiscontinuity = true;
			}
			else if (ObservedHorizontalDisplacement
					<= MaximumAllowedHorizontalDisplacement
				&& ObservedVerticalDisplacement
					<= MaximumAllowedVerticalDisplacement)
			{
				bLoggedPositionDiscontinuity = false;
			}

			LastSampledLocation = CurrentLocation;
			LastPositionSampleTime = Now;
		}
	}

	const float MaximumAllowedSpeed =
		FMath::Max(Movement->GetMaxSpeed(), 0.0f)
			* FMath::Clamp(MaxSpeedMultiplier, 1.0f, 5.0f)
		+ FMath::Clamp(SpeedAllowance, 0.0f, 1000.0f);
	const bool bGroundedMovement =
		MovementMode == MOVE_Walking
		|| MovementMode == MOVE_NavWalking;
	const bool bFallingMovement = MovementMode == MOVE_Falling;
	const float ObservedHorizontalSpeed = Movement->Velocity.Size2D();
	const float ObservedFullSpeed = Movement->Velocity.Size();
	const float MaximumAllowedVerticalSpeed = bFallingMovement
		? (Movement->Velocity.Z >= 0.0f
			? FMath::Max(Movement->JumpZVelocity, 0.0f)
				* FMath::Clamp(VerticalSpeedMultiplier, 1.0f, 5.0f)
				+ FMath::Clamp(VerticalSpeedAllowance, 0.0f, 1000.0f)
			: FMath::Max(
				Character->GetPhysicsVolume()
					? Character->GetPhysicsVolume()->TerminalVelocity
					: 4000.0f,
				0.0f)
				* FMath::Clamp(VerticalSpeedMultiplier, 1.0f, 5.0f)
				+ FMath::Clamp(VerticalSpeedAllowance, 0.0f, 1000.0f))
		: MaximumAllowedSpeed;
	const bool bExcessiveHorizontalSpeed =
		ObservedHorizontalSpeed > MaximumAllowedSpeed;
	const bool bExcessiveVerticalSpeed =
		FMath::Abs(Movement->Velocity.Z) > MaximumAllowedVerticalSpeed;
	const bool bExcessiveFullSpeed =
		!bGroundedMovement
		&& !bFallingMovement
		&& ObservedFullSpeed > MaximumAllowedSpeed;
	const bool bExcessiveSpeed = bExcessiveHorizontalSpeed
		|| bExcessiveVerticalSpeed
		|| bExcessiveFullSpeed;

	if (!bExcessiveSpeed)
	{
		ConsecutiveExcessiveSpeedSamples = 0;
		bLoggedCurrentIncident = false;
		return;
	}

	++ConsecutiveExcessiveSpeedSamples;
	if (bLoggedCurrentIncident
		|| ConsecutiveExcessiveSpeedSamples
			< FMath::Clamp(ConsecutiveSamplesBeforeLog, 1, 20))
	{
		return;
	}

	const APlayerState* PlayerState = PlayerController->PlayerState;
	const FString PlayerName = PlayerState
		? PlayerState->GetPlayerName()
		: PlayerController->GetName();
	UE_LOG(
		LogVeilNet,
		Warning,
		TEXT("Movement speed anomaly: player=%s controller=%s horizontal=%.1f/%.1f vertical=%.1f/%.1f mode=%d server_time=%.2f"),
		*PlayerName,
		*GetNameSafe(PlayerController),
		ObservedHorizontalSpeed,
		MaximumAllowedSpeed,
		FMath::Abs(Movement->Velocity.Z),
		MaximumAllowedVerticalSpeed,
		static_cast<int32>(Movement->MovementMode),
		Now);
	SubmitAuditEvent(
		TEXT("movement_speed"),
		FMath::Max(
			ObservedHorizontalSpeed,
			FMath::Abs(Movement->Velocity.Z)),
		FMath::Max(
			MaximumAllowedSpeed,
			MaximumAllowedVerticalSpeed),
		Movement->MovementMode,
		Now);
	bLoggedCurrentIncident = true;
}
