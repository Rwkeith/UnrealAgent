#pragma once

#include "CoreMinimal.h"

class UUnrealGPTAgentClient;
class UUnrealGPTSettings;
struct FAgentMessage;

class UnrealGPTAgentPolicy
{
public:
	static bool DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults);

	static bool HandleToolCallIteration(
		UUnrealGPTAgentClient* Client,
		const UUnrealGPTSettings* Settings,
		bool bIsNewUserMessage,
		TArray<FAgentMessage>& ConversationHistory,
		FString& PreviousResponseId,
		int32& ToolCallIterationCount,
		bool& bRequestInProgress);
};
