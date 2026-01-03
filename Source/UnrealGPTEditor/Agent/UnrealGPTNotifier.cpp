#include "UnrealGPTNotifier.h"
#include "UnrealGPTAgentClient.h"
#include "Async/Async.h"

void UnrealGPTNotifier::BroadcastAgentMessage(UUnrealGPTAgentClient* Client, const FString& Content, const TArray<FString>& ToolCallIds)
{
	if (!Client)
	{
		return;
	}

	if (IsInGameThread())
	{
		Client->OnAgentMessage.Broadcast(TEXT("assistant"), Content, ToolCallIds);
		return;
	}

	TArray<FString> ToolCallIdsCopy = ToolCallIds;
	FString ContentCopy = Content;
	AsyncTask(ENamedThreads::GameThread, [Client, ContentCopy, ToolCallIdsCopy]()
	{
		Client->OnAgentMessage.Broadcast(TEXT("assistant"), ContentCopy, ToolCallIdsCopy);
	});
}

void UnrealGPTNotifier::BroadcastAgentReasoning(UUnrealGPTAgentClient* Client, const FString& Content)
{
	if (!Client)
	{
		return;
	}

	if (IsInGameThread())
	{
		Client->OnAgentReasoning.Broadcast(Content);
		return;
	}

	FString ContentCopy = Content;
	AsyncTask(ENamedThreads::GameThread, [Client, ContentCopy]()
	{
		Client->OnAgentReasoning.Broadcast(ContentCopy);
	});
}

void UnrealGPTNotifier::BroadcastToolCall(UUnrealGPTAgentClient* Client, const FString& ToolName, const FString& ArgumentsJson)
{
	if (!Client)
	{
		return;
	}

	if (IsInGameThread())
	{
		Client->OnToolCall.Broadcast(ToolName, ArgumentsJson);
		return;
	}

	FString ToolNameCopy = ToolName;
	FString ArgsCopy = ArgumentsJson;
	AsyncTask(ENamedThreads::GameThread, [Client, ToolNameCopy, ArgsCopy]()
	{
		Client->OnToolCall.Broadcast(ToolNameCopy, ArgsCopy);
	});
}

void UnrealGPTNotifier::BroadcastToolResult(UUnrealGPTAgentClient* Client, const FString& ToolCallId, const FString& Result)
{
	if (!Client)
	{
		return;
	}

	if (IsInGameThread())
	{
		Client->OnToolResult.Broadcast(ToolCallId, Result);
		return;
	}

	FString ToolCallIdCopy = ToolCallId;
	FString ResultCopy = Result;
	AsyncTask(ENamedThreads::GameThread, [Client, ToolCallIdCopy, ResultCopy]()
	{
		Client->OnToolResult.Broadcast(ToolCallIdCopy, ResultCopy);
	});
}
