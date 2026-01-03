#pragma once

#include "CoreMinimal.h"

class UUnrealGPTAgentClient;

class UnrealGPTResponseProcessor
{
public:
	static void ProcessResponse(UUnrealGPTAgentClient* Client, const FString& ResponseContent);
	static void HandleResponsePayload(UUnrealGPTAgentClient* Client, const FString& ResponseContent);
};
