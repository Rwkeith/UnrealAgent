#pragma once

#include "CoreMinimal.h"

class UUnrealGPTAgentClient;

class UnrealGPTNotifier
{
public:
	static void BroadcastAgentMessage(UUnrealGPTAgentClient* Client, const FString& Content, const TArray<FString>& ToolCallIds);
	static void BroadcastAgentReasoning(UUnrealGPTAgentClient* Client, const FString& Content);
	static void BroadcastToolCall(UUnrealGPTAgentClient* Client, const FString& ToolName, const FString& ArgumentsJson);
	static void BroadcastToolResult(UUnrealGPTAgentClient* Client, const FString& ToolCallId, const FString& Result);
};
