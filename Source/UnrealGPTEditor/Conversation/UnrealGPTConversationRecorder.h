#pragma once

#include "CoreMinimal.h"

class UUnrealGPTSessionManager;
struct FAgentMessage;

class UnrealGPTConversationRecorder
{
public:
	static void RecordUserMessage(
		TArray<FAgentMessage>& ConversationHistory,
		UUnrealGPTSessionManager* SessionManager,
		const FString& Message,
		const TArray<FString>& Images);
};
