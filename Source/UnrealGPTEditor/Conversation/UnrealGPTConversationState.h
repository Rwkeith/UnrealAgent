#pragma once

#include "CoreMinimal.h"

struct FAgentMessage;

struct FConversationContinuation
{
	int32 StartIndex = 0;
	TArray<FAgentMessage> ToolResultsToInclude;
};

class UnrealGPTConversationState
{
public:
	static FConversationContinuation BuildContinuation(const TArray<FAgentMessage>& ConversationHistory, bool bIsNewUserMessage);
	static void LogToolCallCoverage(const TArray<FAgentMessage>& ConversationHistory, const TArray<FAgentMessage>& ToolResultsToInclude);

	static FAgentMessage CreateUserMessage(const FString& Content);
	static FAgentMessage CreateAssistantMessage(const FString& Content);
	static FAgentMessage CreateAssistantToolCallMessage(const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson);
	static FAgentMessage CreateToolMessage(const FString& Content, const FString& ToolCallId);
	static void AppendMessage(TArray<FAgentMessage>& ConversationHistory, const FAgentMessage& Message);
};
