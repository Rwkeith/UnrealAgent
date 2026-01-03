#include "UnrealGPTHttpClient.h"
#include "UnrealGPTAgentClient.h"
#include "Http.h"

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
