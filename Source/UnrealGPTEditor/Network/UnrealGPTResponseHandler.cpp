#include "UnrealGPTResponseHandler.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTHttpClient.h"
#include "UnrealGPTResponseProcessor.h"
#include "UnrealGPTRetryPolicy.h"
#include "UnrealGPTTelemetry.h"
#include "UnrealGPTApiUrlResolver.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Editor.h"
#include "Http.h"

void UnrealGPTResponseHandler::HandleResponse(
	UUnrealGPTAgentClient* Client,
	TSharedPtr<IHttpRequest> Request,
	TSharedPtr<IHttpResponse> Response,
	bool bWasSuccessful)
{
	if (!Client)
	{
		return;
	}

	Client->bRequestInProgress = false;

	const double ElapsedTime = FPlatformTime::Seconds() - Client->RequestStartTime;

	if (!Client->Settings)
	{
		Client->Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Client->Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved in OnResponseReceived"));
			return;
		}
	}

	if (!bWasSuccessful || !Response.IsValid())
	{
		EHttpRequestStatus::Type RequestStatus = EHttpRequestStatus::NotStarted;
		if (Request.IsValid())
		{
			RequestStatus = Request->GetStatus();
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP request FAILED after %.2f seconds (timeout setting: %.1f seconds) - URL: %s, Status: %s"),
				ElapsedTime,
				Client->Settings->ExecutionTimeoutSeconds,
				*Request->GetURL(),
				EHttpRequestStatus::ToString(RequestStatus));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP request FAILED after %.2f seconds - Request object is invalid"), ElapsedTime);
		}

		if (Response.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Response code: %d, Content: %s"),
				Response->GetResponseCode(),
				*Response->GetContentAsString().Left(500));
		}

		const int32 MaxRetries = 1;
		if (Client->HttpRetryCount < MaxRetries && !Client->LastRequestBody.IsEmpty() &&
			RequestStatus == EHttpRequestStatus::Failed)
		{
			Client->HttpRetryCount++;
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Retrying request (attempt %d/%d) after transient failure..."), Client->HttpRetryCount, MaxRetries + 1);

			FTimerHandle RetryTimerHandle;
			GEditor->GetTimerManager()->SetTimer(RetryTimerHandle, [Client]()
			{
				if (!Client->LastRequestBody.IsEmpty())
				{
					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					const FString ApiKey = SafeSettings ? SafeSettings->ApiKey : FString();

					TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
						UnrealGPTHttpClient::CreateRequest(),
						UnrealGPTApiUrlResolver::GetEffectiveApiUrl(),
						ApiKey,
						Client->LastRequestBody,
						Client);

					Client->bRequestInProgress = true;
					Client->RequestStartTime = FPlatformTime::Seconds();
					RetryRequest->ProcessRequest();
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Retry request sent (timeout: %.1f seconds)"), SafeSettings ? SafeSettings->ExecutionTimeoutSeconds : 90.0f);
				}
			}, 1.0f, false);

			return;
		}

		Client->HttpRetryCount = 0;
		return;
	}

	Client->HttpRetryCount = 0;

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: HTTP response received in %.2f seconds - Status: %d"), ElapsedTime, ResponseCode);

	if (ResponseCode != 200)
	{
		const FString& ErrorBody = ResponseBody;
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP error %d: %s"), ResponseCode, *ErrorBody);

		if (ResponseCode == 429 && !Client->LastRequestBody.IsEmpty() && Client->RateLimitRetryCount < Client->MaxRateLimitRetries)
		{
			Client->RateLimitRetryCount++;

			const float RetryDelaySeconds = UnrealGPTRetryPolicy::ParseRetryDelaySeconds(ErrorBody);
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Rate limited (429). Retry %d/%d in %.2f seconds..."), Client->RateLimitRetryCount, Client->MaxRateLimitRetries, RetryDelaySeconds);

			FTimerHandle RateLimitRetryHandle;
			GEditor->GetTimerManager()->SetTimer(RateLimitRetryHandle, [Client]()
			{
				if (!Client->LastRequestBody.IsEmpty())
				{
					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					const FString ApiKey = SafeSettings ? SafeSettings->ApiKey : FString();

					TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
						UnrealGPTHttpClient::CreateRequest(),
						UnrealGPTApiUrlResolver::GetEffectiveApiUrl(),
						ApiKey,
						Client->LastRequestBody,
						Client);

					Client->bRequestInProgress = true;
					Client->RequestStartTime = FPlatformTime::Seconds();
					RetryRequest->ProcessRequest();
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Rate limit retry request sent"));
				}
			}, RetryDelaySeconds, false);

			return;
		}

		if (ResponseCode == 400 && Client->bAllowReasoningSummary)
		{
			TSharedPtr<FJsonObject> ErrorRoot;
			TSharedRef<TJsonReader<>> ErrorReader = TJsonReaderFactory<>::Create(ErrorBody);
			if (FJsonSerializer::Deserialize(ErrorReader, ErrorRoot) && ErrorRoot.IsValid())
			{
				const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
				if (ErrorRoot->TryGetObjectField(TEXT("error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
				{
					FString Param;
					FString Code;
					FString Message;
					(*ErrorObjPtr)->TryGetStringField(TEXT("param"), Param);
					(*ErrorObjPtr)->TryGetStringField(TEXT("code"), Code);
					(*ErrorObjPtr)->TryGetStringField(TEXT("message"), Message);

					if (Param == TEXT("reasoning.summary") && Code == TEXT("unsupported_value"))
					{
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Disabling reasoning.summary (org is not verified: %s)"), *Message);
						Client->bAllowReasoningSummary = false;

						if (!Client->bRequestInProgress && !Client->LastRequestBody.IsEmpty())
						{
							const FString OriginalBody = Client->LastRequestBody;
							TSharedPtr<FJsonObject> OriginalJson;
							TSharedRef<TJsonReader<>> OriginalReader = TJsonReaderFactory<>::Create(OriginalBody);
							if (FJsonSerializer::Deserialize(OriginalReader, OriginalJson) && OriginalJson.IsValid())
							{
								const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
								if (OriginalJson->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
								{
									TSharedPtr<FJsonObject> ReasoningObj = *ReasoningObjPtr;
									ReasoningObj->RemoveField(TEXT("summary"));
									OriginalJson->SetObjectField(TEXT("reasoning"), ReasoningObj);

									FString NewBody;
									TSharedRef<TJsonWriter<>> NewWriter = TJsonWriterFactory<>::Create(&NewBody);
									if (FJsonSerializer::Serialize(OriginalJson.ToSharedRef(), NewWriter))
									{
										TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
											UnrealGPTHttpClient::CreateRequest(),
											UnrealGPTApiUrlResolver::GetEffectiveApiUrl(),
											Client->Settings->ApiKey,
											NewBody,
											Client);

										Client->bRequestInProgress = true;
										Client->RequestStartTime = FPlatformTime::Seconds();
										UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Retrying request without reasoning.summary (timeout: %.1f seconds)"), Client->Settings->ExecutionTimeoutSeconds);
										RetryRequest->ProcessRequest();
										return;
									}
								}
							}
						}
					}
				}
			}
		}

		return;
	}

	Client->RateLimitRetryCount = 0;

	UnrealGPTTelemetry::LogApiConversation(Client->ConversationSessionId, TEXT("request"), Client->LastRequestBody);
	UnrealGPTTelemetry::LogApiConversation(Client->ConversationSessionId, TEXT("response"), ResponseBody, ResponseCode);

	UnrealGPTResponseProcessor::ProcessResponse(Client, ResponseBody);
}
