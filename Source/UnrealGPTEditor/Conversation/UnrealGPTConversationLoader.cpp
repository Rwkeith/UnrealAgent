#include "UnrealGPTConversationLoader.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTSessionManager.h"

bool UnrealGPTConversationLoader::LoadConversation(
	UUnrealGPTSessionManager* SessionManager,
	const FString& SessionId,
	FString& ConversationSessionId,
	FString& PreviousResponseId,
	TArray<FAgentMessage>& ConversationHistory)
{
	if (!SessionManager)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: SessionManager is null, cannot load conversation"));
		return false;
	}

	FSessionData LoadedSession;
	if (!SessionManager->LoadSession(SessionId, LoadedSession))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to load session: %s"), *SessionId);
		return false;
	}

	if (!ConversationSessionId.IsEmpty())
	{
		SessionManager->SaveCurrentSession();
		SessionManager->EndAutoSave();
	}

	ConversationHistory.Empty();

	ConversationSessionId = SessionId;
	PreviousResponseId = LoadedSession.PreviousResponseId;

	for (const FPersistedMessage& Msg : LoadedSession.Messages)
	{
		FAgentMessage AgentMsg;
		AgentMsg.Role = Msg.Role;
		AgentMsg.Content = Msg.Content;
		AgentMsg.ToolCallIds = Msg.ToolCallIds;
		AgentMsg.ToolCallId = Msg.ToolCallId;
		AgentMsg.ToolCallsJson = Msg.ToolCallsJson;
		UnrealGPTConversationState::AppendMessage(ConversationHistory, AgentMsg);
	}

	// IMPORTANT: BeginAutoSave must be called BEFORE SetCurrentSessionData because
	// BeginAutoSave initializes CurrentSessionData to empty. We then overwrite with loaded data.
	SessionManager->BeginAutoSave(SessionId);
	SessionManager->SetCurrentSessionData(LoadedSession);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Loaded conversation %s with %d messages"), *SessionId, ConversationHistory.Num());

	return true;
}
