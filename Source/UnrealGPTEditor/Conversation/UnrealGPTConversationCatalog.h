#pragma once

#include "CoreMinimal.h"
#include "UnrealGPTSessionTypes.h"

class UUnrealGPTSessionManager;

class UnrealGPTConversationCatalog
{
public:
	static TArray<FSessionInfo> GetSessionList(UUnrealGPTSessionManager* SessionManager);
};
