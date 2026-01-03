#include "UnrealGPTConversationCatalog.h"
#include "UnrealGPTSessionManager.h"

TArray<FSessionInfo> UnrealGPTConversationCatalog::GetSessionList(UUnrealGPTSessionManager* SessionManager)
{
	if (SessionManager)
	{
		return SessionManager->GetSessionList();
	}

	return TArray<FSessionInfo>();
}
