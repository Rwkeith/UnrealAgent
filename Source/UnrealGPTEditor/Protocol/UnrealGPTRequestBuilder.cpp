#include "UnrealGPTRequestBuilder.h"
#include "UnrealGPTAgentClient.h"
#include "Serialization/JsonSerializer.h"

FString UnrealGPTRequestBuilder::GetImageMimeType(const FString& ImageData)
{
	return ImageData.StartsWith(TEXT("/9j/")) ? TEXT("image/jpeg") : TEXT("image/png");
}

void UnrealGPTRequestBuilder::SetUserMessageWithImages(TSharedPtr<FJsonObject> MsgObj, const FString& MessageText, const TArray<FString>& ImageBase64, bool bUseResponsesApi)
{
	TArray<TSharedPtr<FJsonValue>> ContentArray;

	TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
	if (bUseResponsesApi)
	{
		TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
	}
	else
	{
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
	}
	TextContent->SetStringField(TEXT("text"), MessageText);
	ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

	for (const FString& ImageData : ImageBase64)
	{
		TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
		const FString MimeType = GetImageMimeType(ImageData);

		if (bUseResponsesApi)
		{
			ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
			ImageContent->SetStringField(
				TEXT("image_url"),
				FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
		}
		else
		{
			ImageContent->SetStringField(TEXT("type"), TEXT("image_url"));
			TSharedPtr<FJsonObject> ImageUrl = MakeShareable(new FJsonObject);
			ImageUrl->SetStringField(
				TEXT("url"),
				FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
			ImageContent->SetObjectField(TEXT("image_url"), ImageUrl);
		}

		ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
	}

	MsgObj->SetArrayField(TEXT("content"), ContentArray);
}

void UnrealGPTRequestBuilder::AppendResponsesApiFunctionCallOutputs(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FAgentMessage>& ToolResultsToInclude, int32 MaxToolResultSize)
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

void UnrealGPTRequestBuilder::AppendResponsesApiImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText)
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
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added message with %d image(s) to Responses API input"), ImageBase64.Num());
}

void UnrealGPTRequestBuilder::AppendLegacyImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText)
{
	TSharedPtr<FJsonObject> ImageMsgObj = MakeShareable(new FJsonObject);
	ImageMsgObj->SetStringField(TEXT("role"), TEXT("user"));

	TArray<TSharedPtr<FJsonValue>> ContentArray;

	TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), PromptText);
	ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

	for (const FString& ImageData : ImageBase64)
	{
		TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
		const FString MimeType = GetImageMimeType(ImageData);

		ImageContent->SetStringField(TEXT("type"), TEXT("image_url"));
		TSharedPtr<FJsonObject> ImageUrl = MakeShareable(new FJsonObject);
		ImageUrl->SetStringField(TEXT("url"), FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
		ImageContent->SetObjectField(TEXT("image_url"), ImageUrl);

		ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added image_url (%s, %d chars) to user message"), *MimeType, ImageData.Len());
	}

	ImageMsgObj->SetArrayField(TEXT("content"), ContentArray);
	MessagesArray.Add(MakeShareable(new FJsonValueObject(ImageMsgObj)));
}

bool UnrealGPTRequestBuilder::TryAddToolCallsToAssistantMessage(const FAgentMessage& Msg, TSharedPtr<FJsonObject> MsgObj)
{
	bool bToolCallsAdded = false;
	if (!Msg.ToolCallsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> ToolCallsReader = TJsonReaderFactory<>::Create(Msg.ToolCallsJson);
		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		if (FJsonSerializer::Deserialize(ToolCallsReader, ToolCallsArray) && ToolCallsArray.Num() > 0)
		{
			MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
			bToolCallsAdded = true;
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Successfully added tool_calls to assistant message. ToolCalls count: %d"), ToolCallsArray.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to deserialize tool_calls JSON: %s. Attempting reconstruction."), *Msg.ToolCallsJson);
		}
	}

	if (!bToolCallsAdded && Msg.ToolCallIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		for (const FString& ToolCallId : Msg.ToolCallIds)
		{
			TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
			ToolCallObj->SetStringField(TEXT("id"), ToolCallId);
			ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));

			TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
			FunctionObj->SetStringField(TEXT("name"), TEXT("unknown"));
			FunctionObj->SetStringField(TEXT("arguments"), TEXT("{}"));
			ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);

			ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));
		}

		if (ToolCallsArray.Num() > 0)
		{
			MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
			bToolCallsAdded = true;
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Reconstructed tool_calls from ToolCallIds. Count: %d"), ToolCallsArray.Num());
		}
	}

	return bToolCallsAdded;
}

bool UnrealGPTRequestBuilder::CanAppendToolMessage(const TArray<TSharedPtr<FJsonValue>>& MessagesArray, int32 MessageIndex)
{
	if (MessagesArray.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d has no preceding messages"), MessageIndex);
		return false;
	}

	const TSharedPtr<FJsonValue>& LastMsgValue = MessagesArray.Last();
	if (LastMsgValue.IsValid() && LastMsgValue->Type == EJson::Object)
	{
		TSharedPtr<FJsonObject> LastMsgObj = LastMsgValue->AsObject();
		if (LastMsgObj.IsValid())
		{
			FString LastRole;
			if (LastMsgObj->TryGetStringField(TEXT("role"), LastRole))
			{
				if (LastRole == TEXT("assistant") && LastMsgObj->HasField(TEXT("tool_calls")))
				{
					return true;
				}

				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d does not follow assistant message with tool_calls. Previous role: %s, has tool_calls: %d"),
					MessageIndex, *LastRole, LastMsgObj->HasField(TEXT("tool_calls")));
			}
		}
	}

	return false;
}
