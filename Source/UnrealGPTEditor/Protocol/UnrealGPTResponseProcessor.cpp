#include "UnrealGPTResponseProcessor.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTNotifier.h"
#include "UnrealGPTResponseParser.h"
#include "UnrealGPTToolCallProcessor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void UnrealGPTResponseProcessor::ProcessResponse(UUnrealGPTAgentClient* Client, const FString& ResponseContent)
{
	if (!Client)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received response (length: %d)"), ResponseContent.Len());
	if (ResponseContent.Len() < 500)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response content: %s"), *ResponseContent);
	}

	Client->HandleResponsePayload(ResponseContent);
}

void UnrealGPTResponseProcessor::HandleResponsePayload(UUnrealGPTAgentClient* Client, const FString& ResponseContent)
{
	if (!Client)
	{
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to parse response JSON"));
		return;
	}

	// Log all top-level fields to understand the response structure
	TArray<FString> FieldNames;
	RootObject->Values.GetKeys(FieldNames);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response root fields: %s"), *FString::Join(FieldNames, TEXT(", ")));

	// Store the response ID for subsequent requests
	FString ResponseId;
	if (RootObject->TryGetStringField(TEXT("id"), ResponseId))
	{
		Client->PreviousResponseId = ResponseId;
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Stored PreviousResponseId: %s"), *Client->PreviousResponseId);
	}
	
	// Check response status
	FString Status;
	if (RootObject->TryGetStringField(TEXT("status"), Status))
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response status: %s"), *Status);
		if (Status == TEXT("failed") || Status == TEXT("cancelled"))
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Response status indicates failure: %s"), *Status);
			Client->ToolCallIterationCount = 0; // Reset on failure
			Client->bRequestInProgress = false;
			return;
		}
	}

	// If the model provided a reasoning summary, surface it immediately for the UI.
	const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
	if (RootObject->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
	{
		FString ReasoningSummary;
		if ((*ReasoningObjPtr)->TryGetStringField(TEXT("summary"), ReasoningSummary) && !ReasoningSummary.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received reasoning summary (length: %d)"), ReasoningSummary.Len());
			UnrealGPTNotifier::BroadcastAgentReasoning(Client, ReasoningSummary);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("output"), OutputArray) || !OutputArray)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Response missing 'output' array"));
		
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found output array with %d items"), OutputArray->Num());

	// Extract tool calls and text from output array using helper method
	FResponseParseResult ParseResult;
	UnrealGPTResponseParser::ExtractFromResponseOutput(*OutputArray, ParseResult);

	if (ParseResult.ReasoningChunks.Num() > 0)
	{
		for (const FString& ReasoningChunk : ParseResult.ReasoningChunks)
		{
			if (!ReasoningChunk.IsEmpty())
			{
				UnrealGPTNotifier::BroadcastAgentReasoning(Client, ReasoningChunk);
			}
		}
	}

	if (ParseResult.ServerSideToolCalls.Num() > 0)
	{
		for (const FServerSideToolCall& ServerSideCall : ParseResult.ServerSideToolCalls)
		{
			UnrealGPTNotifier::BroadcastToolCall(Client, ServerSideCall.ToolName, ServerSideCall.ArgsJson);
			if (!ServerSideCall.ResultSummary.IsEmpty())
			{
				UnrealGPTNotifier::BroadcastToolResult(Client, ServerSideCall.CallId, ServerSideCall.ResultSummary);
			}
		}
	}

	// Process the extracted tool calls and/or text
	if (ParseResult.ToolCalls.Num() > 0)
	{
		UnrealGPTToolCallProcessor::ProcessToolCalls(Client, ParseResult.ToolCalls, ParseResult.AccumulatedText);
		return;
	}

	// No tool calls - just process the text response
	if (!ParseResult.AccumulatedText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing regular assistant message (no tool calls)"));
		FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantMessage(ParseResult.AccumulatedText);
		UnrealGPTConversationState::AppendMessage(Client->ConversationHistory, AssistantMsg);

		UnrealGPTNotifier::BroadcastAgentMessage(Client, ParseResult.AccumulatedText, TArray<FString>());
		Client->ToolCallIterationCount = 0;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Message output had no text content and no tool calls"));
		Client->ToolCallIterationCount = 0;
	}
}
