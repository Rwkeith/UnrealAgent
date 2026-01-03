#include "UnrealGPTConversationState.h"
#include "UnrealGPTAgentClient.h"

FConversationContinuation UnrealGPTConversationState::BuildContinuation(const TArray<FAgentMessage>& ConversationHistory, bool bIsNewUserMessage)
{
	FConversationContinuation Result;

	if (bIsNewUserMessage)
	{
		const int32 HistorySize = ConversationHistory.Num();
		if (HistorySize > 0)
		{
			Result.StartIndex = HistorySize - 1;
			if (Result.StartIndex < 0 || Result.StartIndex >= HistorySize)
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Calculated invalid StartIndex %d for history size %d, resetting to 0"), Result.StartIndex, HistorySize);
				Result.StartIndex = 0;
			}
		}
		else
		{
			Result.StartIndex = 0;
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: History is empty after adding user message, this should not happen"));
		}

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message, starting from index %d (history size: %d)"), Result.StartIndex, ConversationHistory.Num());
		return Result;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool continuation - searching for tool results in history (size: %d)"), ConversationHistory.Num());

	for (int32 i = ConversationHistory.Num() - 1; i >= 0; --i)
	{
		if (ConversationHistory[i].Role == TEXT("assistant") &&
			(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found assistant message with tool_calls at index %d (tool_call_ids: %d)"),
				i, ConversationHistory[i].ToolCallIds.Num());

			for (int32 j = i + 1; j < ConversationHistory.Num(); ++j)
			{
				if (ConversationHistory[j].Role == TEXT("tool"))
				{
					Result.ToolResultsToInclude.Add(ConversationHistory[j]);
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added tool result to include: call_id=%s, content_length=%d"),
						*ConversationHistory[j].ToolCallId, ConversationHistory[j].Content.Len());
				}
				else if (ConversationHistory[j].Role == TEXT("user"))
				{
					Result.StartIndex = j;
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found user message at index %d, stopping tool result collection"), j);
					break;
				}
			}
			break;
		}
	}

	if (Result.StartIndex == 0 && Result.ToolResultsToInclude.Num() > 0)
	{
		Result.StartIndex = ConversationHistory.Num();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: No user message found, starting from end of history (index %d)"), Result.StartIndex);
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool continuation, starting from index %d, will include %d tool results"), Result.StartIndex, Result.ToolResultsToInclude.Num());

	LogToolCallCoverage(ConversationHistory, Result.ToolResultsToInclude);

	return Result;
}

void UnrealGPTConversationState::LogToolCallCoverage(const TArray<FAgentMessage>& ConversationHistory, const TArray<FAgentMessage>& ToolResultsToInclude)
{
	if (ConversationHistory.Num() == 0)
	{
		return;
	}

	const FAgentMessage& LastAssistantMsg = ConversationHistory[ConversationHistory.Num() - 1];
	if (LastAssistantMsg.Role != TEXT("assistant") || LastAssistantMsg.ToolCallIds.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Last assistant message has %d tool_call_ids"), LastAssistantMsg.ToolCallIds.Num());
	for (const FString& ExpectedCallId : LastAssistantMsg.ToolCallIds)
	{
		bool bFound = false;
		for (const FAgentMessage& ToolResult : ToolResultsToInclude)
		{
			if (ToolResult.ToolCallId == ExpectedCallId)
			{
				bFound = true;
				break;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call %s: %s"), *ExpectedCallId, bFound ? TEXT("FOUND") : TEXT("MISSING"));
	}
}

FAgentMessage UnrealGPTConversationState::CreateUserMessage(const FString& Content)
{
	FAgentMessage Message;
	Message.Role = TEXT("user");
	Message.Content = Content;
	return Message;
}

FAgentMessage UnrealGPTConversationState::CreateAssistantMessage(const FString& Content)
{
	FAgentMessage Message;
	Message.Role = TEXT("assistant");
	Message.Content = Content;
	return Message;
}

FAgentMessage UnrealGPTConversationState::CreateAssistantToolCallMessage(const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson)
{
	FAgentMessage Message;
	Message.Role = TEXT("assistant");
	Message.Content = Content;
	Message.ToolCallIds = ToolCallIds;
	Message.ToolCallsJson = ToolCallsJson;
	return Message;
}

FAgentMessage UnrealGPTConversationState::CreateToolMessage(const FString& Content, const FString& ToolCallId)
{
	FAgentMessage Message;
	Message.Role = TEXT("tool");
	Message.Content = Content;
	Message.ToolCallId = ToolCallId;
	return Message;
}

void UnrealGPTConversationState::AppendMessage(TArray<FAgentMessage>& ConversationHistory, const FAgentMessage& Message)
{
	ConversationHistory.Add(Message);
}
