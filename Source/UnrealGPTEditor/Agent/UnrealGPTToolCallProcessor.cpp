#include "UnrealGPTToolCallProcessor.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTNotifier.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTToolDispatcher.h"
#include "UnrealGPTToolResultProcessor.h"
#include "Async/Async.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

void UnrealGPTToolCallProcessor::ProcessToolCalls(
	UUnrealGPTAgentClient* Client,
	const TArray<FToolCallInfo>& ToolCalls,
	const FString& AccumulatedText)
{
	if (!Client)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> ToolCallsJsonArray;
	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		TSharedPtr<FJsonObject> ToolCallJson = MakeShareable(new FJsonObject);
		ToolCallJson->SetStringField(TEXT("id"), CallInfo.Id);
		ToolCallJson->SetStringField(TEXT("type"), TEXT("function"));

		TSharedPtr<FJsonObject> FunctionJson = MakeShareable(new FJsonObject);
		FunctionJson->SetStringField(TEXT("name"), CallInfo.Name);
		FunctionJson->SetStringField(TEXT("arguments"), CallInfo.Arguments);
		ToolCallJson->SetObjectField(TEXT("function"), FunctionJson);

		ToolCallsJsonArray.Add(MakeShareable(new FJsonValueObject(ToolCallJson)));
	}

	FString ToolCallsJsonString;
	TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
	FJsonSerializer::Serialize(ToolCallsJsonArray, ToolCallsWriter);

	TArray<FString> ToolCallIds;
	ToolCallIds.Reserve(ToolCalls.Num());
	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		ToolCallIds.Add(CallInfo.Id);
	}

	FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantToolCallMessage(
		AccumulatedText,
		ToolCallIds,
		ToolCallsJsonString);
	UnrealGPTConversationState::AppendMessage(Client->ConversationHistory, AssistantMsg);

	Client->SaveAssistantMessageToSession(AccumulatedText, ToolCallIds, ToolCallsJsonString);

	if (!AccumulatedText.IsEmpty())
	{
		UnrealGPTNotifier::BroadcastAgentMessage(Client, AccumulatedText, ToolCallIds);
	}
	else
	{
		UnrealGPTNotifier::BroadcastAgentMessage(Client, TEXT("Executing tools..."), ToolCallIds);
	}

	auto IsServerSideTool = [](const FString& Name) -> bool
	{
		return Name == TEXT("file_search") || Name == TEXT("web_search");
	};

	auto IsAsyncTool = [](const FString& Name) -> bool
	{
		// replicate_generate is async to avoid blocking the editor thread.
		return Name == TEXT("replicate_generate");
	};

	auto ExecuteToolCall = [Client](const FString& ToolName, const FString& ArgumentsJson) -> FString
	{
		return UnrealGPTToolDispatcher::ExecuteToolCall(
			ToolName,
			ArgumentsJson,
			Client->bLastToolWasPythonExecute,
			Client->bLastSceneQueryFoundResults,
			[Client](const FString& ToolNameInner, const FString& ArgumentsJsonInner)
			{
				UnrealGPTNotifier::BroadcastToolCall(Client, ToolNameInner, ArgumentsJsonInner);
			});
	};

	bool bHasClientSideTools = false;
	bool bHasAsyncTools = false;
	TArray<FString> ScreenshotImages;

	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		if (IsServerSideTool(CallInfo.Name))
		{
			UE_LOG(LogTemp, Verbose, TEXT("UnrealGPT: Skipping server-side tool call '%s' (handled by API)"), *CallInfo.Name);
			continue;
		}

		const bool bIsAsyncTool = IsAsyncTool(CallInfo.Name);

		bHasClientSideTools = true;

		// Async tool execution
		if (bIsAsyncTool)
		{
			bHasAsyncTools = true;
			const FString ToolNameCopy = CallInfo.Name;
			const FString ArgsCopy = CallInfo.Arguments;
			const FString CallIdCopy = CallInfo.Id;
			const int32 MaxToolResultSizeLocal = Client->MaxToolResultSize;

			Async(EAsyncExecution::ThreadPool, [Client, ToolNameCopy, ArgsCopy, CallIdCopy, MaxToolResultSizeLocal, ExecuteToolCall]()
			{
				const FString ToolResult = ExecuteToolCall(ToolNameCopy, ArgsCopy);

				FProcessedToolResult ProcessedToolResult =
					UnrealGPTToolResultProcessor::ProcessResult(ToolNameCopy, ToolResult, MaxToolResultSizeLocal);

				AsyncTask(ENamedThreads::GameThread, [Client, ToolNameCopy, ArgsCopy, CallIdCopy, ToolResult, ProcessedToolResult]()
				{
					FAgentMessage ToolMsg = UnrealGPTConversationState::CreateToolMessage(ProcessedToolResult.ResultForHistory, CallIdCopy);
					UnrealGPTConversationState::AppendMessage(Client->ConversationHistory, ToolMsg);

					Client->SaveToolMessageToSession(CallIdCopy, ProcessedToolResult.ResultForHistory, ProcessedToolResult.Images);
					Client->SaveToolCallToSession(ToolNameCopy, ArgsCopy, ToolResult);

					UnrealGPTNotifier::BroadcastToolResult(Client, CallIdCopy, ToolResult);
					Client->SendMessage(TEXT(""), TArray<FString>());
				});
			});
			continue;
		}

		// Synchronous execution
		FString ToolResult = ExecuteToolCall(CallInfo.Name, CallInfo.Arguments);
		FProcessedToolResult ProcessedToolResult =
			UnrealGPTToolResultProcessor::ProcessResult(CallInfo.Name, ToolResult, Client->MaxToolResultSize);
		ScreenshotImages.Append(ProcessedToolResult.Images);

		FAgentMessage ToolMsg = UnrealGPTConversationState::CreateToolMessage(ProcessedToolResult.ResultForHistory, CallInfo.Id);
		UnrealGPTConversationState::AppendMessage(Client->ConversationHistory, ToolMsg);

		Client->SaveToolMessageToSession(CallInfo.Id, ProcessedToolResult.ResultForHistory, ProcessedToolResult.Images);
		Client->SaveToolCallToSession(CallInfo.Name, CallInfo.Arguments, ToolResult);

		UnrealGPTNotifier::BroadcastToolResult(Client, CallInfo.Id, ToolResult);
	}

	if (!bHasClientSideTools)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: All tools were server-side. Waiting for continuation."));
		Client->ToolCallIterationCount = 0;
		return;
	}

	if (bHasAsyncTools)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Async tools scheduled; waiting for completion."));
		return;
	}

	const int32 MaxIterations = Client->Settings ? Client->Settings->MaxToolCallIterations : 100;
	if (MaxIterations > 0 && Client->ToolCallIterationCount >= MaxIterations - 1)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Reached max tool call iterations (%d)."), MaxIterations);

		FAgentMessage LimitEntry = UnrealGPTConversationState::CreateAssistantMessage(
			FString::Printf(TEXT("[Tool call limit reached after %d iterations.]"), MaxIterations));
		UnrealGPTConversationState::AppendMessage(Client->ConversationHistory, LimitEntry);
		UnrealGPTNotifier::BroadcastAgentMessage(Client, LimitEntry.Content, TArray<FString>());

		Client->ToolCallIterationCount = 0;
		Client->bRequestInProgress = false;
		return;
	}

	Client->SendMessage(TEXT(""), ScreenshotImages);
}
