#pragma once

#include "CoreMinimal.h"
#include "UnrealGPTSessionTypes.h"

class UUnrealGPTSessionManager;
struct FAgentMessage;

class UnrealGPTConversationLoader
{
public:
	static bool LoadConversation(
		UUnrealGPTSessionManager* SessionManager,
		const FString& SessionId,
		FString& ConversationSessionId,
		FString& PreviousResponseId,
		TArray<FAgentMessage>& ConversationHistory);
};
