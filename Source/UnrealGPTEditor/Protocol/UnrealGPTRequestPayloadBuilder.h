#pragma once

#include "CoreMinimal.h"

class UUnrealGPTSettings;
struct FAgentMessage;

class UnrealGPTRequestPayloadBuilder
{
public:
	static FString BuildRequestBody(
		const UUnrealGPTSettings* Settings,
		bool bAllowReasoningSummary,
		const FString& AgentInstructions,
		const FString& ReasoningEffort,
		const FString& PreviousResponseId,
		const TArray<FAgentMessage>& ConversationHistory,
		const TArray<FString>& ImageBase64,
		bool bIsNewUserMessage,
		int32 MaxToolResultSize);
};
