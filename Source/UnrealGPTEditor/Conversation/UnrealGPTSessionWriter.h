#pragma once

#include "CoreMinimal.h"

class UUnrealGPTSessionManager;

class UnrealGPTSessionWriter
{
public:
	static void SaveUserMessage(UUnrealGPTSessionManager* SessionManager, const FString& UserMessage, const TArray<FString>& Images);
	static void SaveAssistantMessage(UUnrealGPTSessionManager* SessionManager, const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson);
	static void SaveToolMessage(UUnrealGPTSessionManager* SessionManager, const FString& ToolCallId, const FString& Result, const TArray<FString>& Images);
	static void SaveToolCall(UUnrealGPTSessionManager* SessionManager, const FString& ToolName, const FString& Arguments, const FString& Result);
	static void SaveAssistantMessageAndFlush(UUnrealGPTSessionManager* SessionManager, const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson);
};
