#pragma once

#include "CoreMinimal.h"

class IHttpRequest;
class UUnrealGPTAgentClient;

class UnrealGPTHttpClient
{
public:
	static TSharedRef<IHttpRequest> CreateRequest();

	static TSharedRef<IHttpRequest> BuildJsonPost(
		TSharedRef<IHttpRequest> Request,
		const FString& Url,
		const FString& ApiKey,
		const FString& Body,
		UUnrealGPTAgentClient* Client);
};
