#pragma once

#include "CoreMinimal.h"

class UUnrealGPTAgentClient;
class IHttpRequest;
class IHttpResponse;

class UnrealGPTResponseHandler
{
public:
	static void HandleResponse(
		UUnrealGPTAgentClient* Client,
		TSharedPtr<IHttpRequest> Request,
		TSharedPtr<IHttpResponse> Response,
		bool bWasSuccessful);
};
