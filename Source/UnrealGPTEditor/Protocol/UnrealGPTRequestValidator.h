#pragma once

#include "CoreMinimal.h"

class UnrealGPTRequestValidator
{
public:
	static FString SanitizeUserMessage(const FString& UserMessage);
	static void LogImagePayloadSize(const TArray<FString>& ImageBase64);
};
