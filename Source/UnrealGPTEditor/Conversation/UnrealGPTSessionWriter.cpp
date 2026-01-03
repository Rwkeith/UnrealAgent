#include "UnrealGPTSessionWriter.h"
#include "UnrealGPTSessionManager.h"

void UnrealGPTSessionWriter::SaveUserMessage(UUnrealGPTSessionManager* SessionManager, const FString& UserMessage, const TArray<FString>& Images)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("user");
	Msg.Content = UserMessage;
	Msg.ImageBase64 = Images;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);
}

void UnrealGPTSessionWriter::SaveAssistantMessage(UUnrealGPTSessionManager* SessionManager, const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("assistant");
	Msg.Content = Content;
	Msg.ToolCallIds = ToolCallIds;
	Msg.ToolCallsJson = ToolCallsJson;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);
}

void UnrealGPTSessionWriter::SaveAssistantMessageAndFlush(UUnrealGPTSessionManager* SessionManager, const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson)
{
	SaveAssistantMessage(SessionManager, Content, ToolCallIds, ToolCallsJson);

	if (SessionManager && SessionManager->IsAutoSaveActive())
	{
		SessionManager->SaveCurrentSession();
	}
}

void UnrealGPTSessionWriter::SaveToolMessage(UUnrealGPTSessionManager* SessionManager, const FString& ToolCallId, const FString& Result, const TArray<FString>& Images)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("tool");
	Msg.ToolCallId = ToolCallId;
	Msg.Content = Result;
	Msg.ImageBase64 = Images;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);
}

void UnrealGPTSessionWriter::SaveToolCall(UUnrealGPTSessionManager* SessionManager, const FString& ToolName, const FString& Arguments, const FString& Result)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedToolCall ToolCall;
	ToolCall.ToolName = ToolName;
	ToolCall.Arguments = Arguments;
	ToolCall.Result = Result;
	ToolCall.Timestamp = FDateTime::Now();

	SessionManager->AppendToolCall(ToolCall);
}
