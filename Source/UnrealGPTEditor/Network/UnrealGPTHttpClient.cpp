#include "UnrealGPTHttpClient.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "Http.h"

TSharedRef<IHttpRequest> UnrealGPTHttpClient::CreateRequest()
{
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();

	// Apply per-request timeout from settings if configured
	if (UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>())
	{
		if (SafeSettings->ExecutionTimeoutSeconds > 0.0f)
		{
			Request->SetTimeout(SafeSettings->ExecutionTimeoutSeconds);
			Request->SetActivityTimeout(SafeSettings->ExecutionTimeoutSeconds);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: CreateRequest - Set timeout to %.1f seconds (request + activity)"), SafeSettings->ExecutionTimeoutSeconds);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: CreateRequest - ExecutionTimeoutSeconds is <= 0 (%.1f), using default timeout"), SafeSettings->ExecutionTimeoutSeconds);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: CreateRequest - Could not get settings, using default timeout"));
	}

	return Request;
}

TSharedRef<IHttpRequest> UnrealGPTHttpClient::BuildJsonPost(
	TSharedRef<IHttpRequest> Request,
	const FString& Url,
	const FString& ApiKey,
	const FString& Body,
	UUnrealGPTAgentClient* Client)
{
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->SetContentAsString(Body);

	if (Client)
	{
		Request->OnProcessRequestComplete().BindUObject(Client, &UUnrealGPTAgentClient::OnResponseReceived);
	}

	return Request;
}
