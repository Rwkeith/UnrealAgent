#pragma once

#include "CoreMinimal.h"
#include "UnrealGPTToolCallTypes.h"

class UUnrealGPTAgentClient;

class UnrealGPTToolCallProcessor
{
public:
	static void ProcessToolCalls(
		UUnrealGPTAgentClient* Client,
		const TArray<FToolCallInfo>& ToolCalls,
		const FString& AccumulatedText);
};
