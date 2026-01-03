#pragma once

#include "CoreMinimal.h"

class UUnrealGPTAgentClient;

class UnrealGPTRequestSender
{
public:
	static void SendRequest(UUnrealGPTAgentClient* Client, const FString& RequestBody);
};
