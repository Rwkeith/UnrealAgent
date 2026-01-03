#pragma once

#include "CoreMinimal.h"

class UnrealGPTTelemetry
{
public:
	static FString GetConversationLogPath(const FString& SessionId);
	static void LogApiConversation(const FString& SessionId, const FString& Direction, const FString& JsonBody, int32 ResponseCode = 0);
};
