#include "VeilNetAntiCheatComponent.h"

#include "EasyHWIDBlueprintLibrary.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VeilNetRuntime.h"

namespace
{
	bool IsValidFingerprint(const FString& Fingerprint)
	{
		if (Fingerprint.Len() != 64)
		{
			return false;
		}

		for (const TCHAR Character : Fingerprint)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}

		return true;
	}
}

UVeilNetAntiCheatComponent::UVeilNetAntiCheatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UVeilNetAntiCheatComponent::SubmitDeviceForValidation()
{
	const APlayerController* PlayerController =
		Cast<APlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		UE_LOG(
			LogVeilNet,
			Warning,
			TEXT("Device validation must be requested by the owning player controller."));
		return;
	}

	FString Fingerprint;
	if (!UEasyHWIDBlueprintLibrary::GetHardwareFingerprint(Fingerprint))
	{
		Server_SubmitFingerprint(FString());
		return;
	}

	Server_SubmitFingerprint(Fingerprint);
}

void UVeilNetAntiCheatComponent::Server_SubmitFingerprint_Implementation(
	const FString& Fingerprint)
{
	if (bValidationStarted)
	{
		return;
	}
	bValidationStarted = true;

	if (!IsValidFingerprint(Fingerprint))
	{
		RejectPlayer(TEXT("Device validation could not read a supported hardware fingerprint."));
		return;
	}

	CheckDeviceOnServer(Fingerprint);
}

void UVeilNetAntiCheatComponent::CheckDeviceOnServer(
	const FString& Fingerprint)
{
	if (ServiceBaseUrl.IsEmpty())
	{
		UE_LOG(LogVeilNet, Error, TEXT("VeilNet device validation has no service URL."));
		RejectPlayer(TEXT("Device validation is unavailable. Please try again later."));
		return;
	}

	const FString ServerToken =
		FPlatformMisc::GetEnvironmentVariable(TEXT("VEILNET_GAME_TOKEN"));
	if (ServerToken.IsEmpty())
	{
		UE_LOG(
			LogVeilNet,
			Error,
			TEXT("VEILNET_GAME_TOKEN is not configured on the game server."));
		RejectPlayer(TEXT("Device validation is unavailable. Please try again later."));
		return;
	}

	FString BaseUrl = ServiceBaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	TSharedRef<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("deviceFingerprint"), Fingerprint);

	FString SerializedBody;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&SerializedBody);
	if (!FJsonSerializer::Serialize(RequestBody, Writer))
	{
		RejectPlayer(TEXT("Device validation could not be started."));
		return;
	}

	const TWeakObjectPtr<UVeilNetAntiCheatComponent> WeakThis(this);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	Request->SetURL(BaseUrl + TEXT("/api/v1/game/check"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(
		TEXT("Authorization"),
		FString::Printf(TEXT("Bearer %s"), *ServerToken));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(SerializedBody);
	Request->SetTimeout(10.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis](
			FHttpRequestPtr,
			FHttpResponsePtr Response,
			const bool bWasSuccessful)
		{
			UVeilNetAntiCheatComponent* Component = WeakThis.Get();
			if (!Component)
			{
				return;
			}

			bool bBanned = false;
			bool bValidResponse = bWasSuccessful
				&& Response.IsValid()
				&& EHttpResponseCodes::IsOk(Response->GetResponseCode());
			if (bValidResponse)
			{
				TSharedPtr<FJsonObject> ResponseObject;
				const TSharedRef<TJsonReader<>> Reader =
					TJsonReaderFactory<>::Create(Response->GetContentAsString());
				bValidResponse = FJsonSerializer::Deserialize(
					Reader,
					ResponseObject)
					&& ResponseObject.IsValid()
					&& ResponseObject->TryGetBoolField(
						TEXT("banned"),
						bBanned);
			}

			if (!bValidResponse)
			{
				UE_LOG(
					LogVeilNet,
					Error,
					TEXT("VeilNet device validation service did not return a valid response."));
				Component->RejectPlayer(
					TEXT("Device validation is unavailable. Please try again later."));
				return;
			}

			if (bBanned)
			{
				Component->RejectPlayer(
					TEXT("This device is not permitted to join."));
				return;
			}

			UE_LOG(
				LogVeilNet,
				Log,
				TEXT("VeilNet device validation succeeded."));
			Component->bDeviceValidated = true;
		});

	if (!Request->ProcessRequest())
	{
		RejectPlayer(TEXT("Device validation could not be started."));
	}
}

void UVeilNetAntiCheatComponent::RejectPlayer(const FString& Message)
{
	if (APlayerController* PlayerController =
		Cast<APlayerController>(GetOwner()))
	{
		PlayerController->ClientReturnToMainMenuWithTextReason(
			FText::FromString(Message));
	}
}
