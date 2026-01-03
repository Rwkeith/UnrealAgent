#include "UnrealGPTConversationRecorder.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTSessionWriter.h"

void UnrealGPTConversationRecorder::RecordUserMessage(
	TArray<FAgentMessage>& ConversationHistory,
	UUnrealGPTSessionManager* SessionManager,
	const FString& Message,
	const TArray<FString>& Images)
{
	FAgentMessage UserMsg = UnrealGPTConversationState::CreateUserMessage(Message);
	UnrealGPTConversationState::AppendMessage(ConversationHistory, UserMsg);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added user message to history: %s"), *Message.Left(100));

	UnrealGPTSessionWriter::SaveUserMessage(SessionManager, Message, Images);
}
