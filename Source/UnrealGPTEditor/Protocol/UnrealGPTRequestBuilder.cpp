#include "UnrealGPTRequestBuilder.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "Serialization/JsonSerializer.h"

FString UnrealGPTRequestBuilder::GetImageMimeType(const FString& ImageData)
{
	return ImageData.StartsWith(TEXT("/9j/")) ? TEXT("image/jpeg") : TEXT("image/png");
}

void UnrealGPTRequestBuilder::SetUserMessageWithImages(TSharedPtr<FJsonObject> MsgObj, const FString& MessageText, const TArray<FString>& ImageBase64)
{
	TArray<TSharedPtr<FJsonValue>> ContentArray;

	TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
	TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
	TextContent->SetStringField(TEXT("text"), MessageText);
	ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

	for (const FString& ImageData : ImageBase64)
	{
		TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
		const FString MimeType = GetImageMimeType(ImageData);

		ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
		ImageContent->SetStringField(
			TEXT("image_url"),
			FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));

		ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
	}

	MsgObj->SetArrayField(TEXT("content"), ContentArray);
}

TArray<TSharedPtr<FJsonValue>> UnrealGPTRequestBuilder::BuildInputItems(
	const TArray<FAgentMessage>& ConversationHistory,
	const TArray<FString>& ImageBase64,
	bool bIsNewUserMessage,
	const FString& PreviousResponseId,
	int32 MaxToolResultSize)
{
	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Building messages array from history. History size: %d"), ConversationHistory.Num());

	// Input handling:
	// - Use previous_response_id to maintain state
	// - Only include new user messages in input (or tool results when continuing after tool execution)
	// - Function results are provided as function_call_output items when continuing after tool execution
	int32 StartIndex = 0;
	TArray<FAgentMessage> ToolResultsToInclude; // We'll add function results as input items

	if (!PreviousResponseId.IsEmpty())
	{
		FConversationContinuation Continuation = UnrealGPTConversationState::BuildContinuation(ConversationHistory, bIsNewUserMessage);
		StartIndex = Continuation.StartIndex;
		ToolResultsToInclude = MoveTemp(Continuation.ToolResultsToInclude);
	}

	// Add function results as input items with type "function_call_output"
	// IMPORTANT: Only include tool results that are reasonably sized to prevent context overflow
	// CRITICAL: For tool continuation (empty UserMessage), we MUST include tool results
	if (!bIsNewUserMessage && ToolResultsToInclude.Num() == 0)
	{
		// Try to find tool results from the most recent assistant message with tool_calls
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Tool continuation but no tool results found - searching history for recent tool results"));
		for (int32 i = ConversationHistory.Num() - 1; i >= 0 && i >= ConversationHistory.Num() - 10; --i)
		{
			if (ConversationHistory[i].Role == TEXT("tool"))
			{
				ToolResultsToInclude.Add(ConversationHistory[i]);
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found tool result in history at index %d: call_id=%s"),
					i, *ConversationHistory[i].ToolCallId);
			}
			else if (ConversationHistory[i].Role == TEXT("assistant") &&
				(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
			{
				// Found assistant message with tool_calls, stop searching backwards
				break;
			}
		}
	}

	if (ToolResultsToInclude.Num() > 0)
	{
		AppendFunctionCallOutputs(MessagesArray, ToolResultsToInclude, MaxToolResultSize);
	}
	else if (!bIsNewUserMessage)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool continuation with empty message but no tool results found! This will cause API error."));
	}

	// CRITICAL: Add viewport screenshot images to the input when continuing after tool calls
	// This allows the model to actually SEE the screenshots it requested
	// Images must be wrapped in a "message" type input item with role "user"
	if (ImageBase64.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Adding %d screenshot image(s) to request input for visual analysis"), ImageBase64.Num());
		AppendImageMessage(
			MessagesArray,
			ImageBase64,
			TEXT("Here is the viewport screenshot you requested. Analyze what you see and describe the scene state."));
	}

	// Add conversation history (or subset for continuation)
	// Ensure StartIndex is valid (non-negative and within bounds)
	const int32 HistorySize = ConversationHistory.Num();
	if (StartIndex < 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Invalid StartIndex (%d), resetting to 0"), StartIndex);
		StartIndex = 0;
	}
	// Note: StartIndex == HistorySize is valid for tool continuations (means skip all history, only send tool results)
	if (StartIndex > HistorySize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: StartIndex (%d) > history size (%d), resetting to 0"), StartIndex, HistorySize);
		StartIndex = 0;
	}

	// Final safety check before accessing array
	if (HistorySize == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Conversation history is empty, skipping message processing"));
	}
	else
	{
		for (int32 i = StartIndex; i < HistorySize; ++i)
		{
			// Additional bounds check inside loop for extra safety
			if (i < 0 || i >= HistorySize)
			{
				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Array index %d out of bounds (size: %d), breaking loop"), i, HistorySize);
				break;
			}
			
			const FAgentMessage& Msg = ConversationHistory[i];
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing message %d: role=%s, hasToolCallsJson=%d, ToolCallIds.Num()=%d"), 
				i, *Msg.Role, !Msg.ToolCallsJson.IsEmpty(), Msg.ToolCallIds.Num());
			
			TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject);
			MsgObj->SetStringField(TEXT("role"), Msg.Role);
			
			if (Msg.Role == TEXT("user") && ImageBase64.Num() > 0)
			{
				SetUserMessageWithImages(MsgObj, Msg.Content, ImageBase64);
			}
			else if (Msg.Role == TEXT("assistant") && (Msg.ToolCallIds.Num() > 0 || !Msg.ToolCallsJson.IsEmpty()))
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping assistant message with tool_calls (state maintained by API)"));
				continue;
			}
			else if (Msg.Role == TEXT("tool"))
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping tool message (state maintained via previous_response_id)"));
				continue;
			}
			else
			{
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
			}
			
			MessagesArray.Add(MakeShareable(new FJsonValueObject(MsgObj)));
		}
	}

	return MessagesArray;
}

void UnrealGPTRequestBuilder::AppendFunctionCallOutputs(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FAgentMessage>& ToolResultsToInclude, int32 MaxToolResultSize)
{
	int32 TotalSize = 0;
	for (const FAgentMessage& ToolResult : ToolResultsToInclude)
	{
		const int32 ResultSize = ToolResult.Content.Len();
		if (TotalSize + ResultSize > MaxToolResultSize * 5)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Skipping tool result (size: %d) to prevent context overflow. Total size: %d"),
				ResultSize, TotalSize);
			continue;
		}

		TSharedPtr<FJsonObject> FunctionResultObj = MakeShareable(new FJsonObject);
		FunctionResultObj->SetStringField(TEXT("type"), TEXT("function_call_output"));
		FunctionResultObj->SetStringField(TEXT("call_id"), ToolResult.ToolCallId);
		FunctionResultObj->SetStringField(TEXT("output"), ToolResult.Content);

		MessagesArray.Add(MakeShareable(new FJsonValueObject(FunctionResultObj)));
		TotalSize += ResultSize;
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added function_call_output input for call_id: %s (size: %d, total: %d)"),
			*ToolResult.ToolCallId, ResultSize, TotalSize);
	}
}

void UnrealGPTRequestBuilder::AppendImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText)
{
	TSharedPtr<FJsonObject> MessageInputObj = MakeShareable(new FJsonObject);
	MessageInputObj->SetStringField(TEXT("type"), TEXT("message"));
	MessageInputObj->SetStringField(TEXT("role"), TEXT("user"));

	TArray<TSharedPtr<FJsonValue>> ContentArray;

	TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
	TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
	TextContent->SetStringField(TEXT("text"), PromptText);
	ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

	for (const FString& ImageData : ImageBase64)
	{
		TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
		const FString MimeType = GetImageMimeType(ImageData);

		ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
		ImageContent->SetStringField(
			TEXT("image_url"),
			FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));

		ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added input_image (%s, %d chars) to message content"), *MimeType, ImageData.Len());
	}

	MessageInputObj->SetArrayField(TEXT("content"), ContentArray);
	MessagesArray.Add(MakeShareable(new FJsonValueObject(MessageInputObj)));
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added message with %d image(s) to request input"), ImageBase64.Num());
}
