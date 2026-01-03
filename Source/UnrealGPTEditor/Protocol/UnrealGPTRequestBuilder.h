#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FAgentMessage;

class UnrealGPTRequestBuilder
{
public:
	static FString GetImageMimeType(const FString& ImageData);
	static void SetUserMessageWithImages(TSharedPtr<FJsonObject> MsgObj, const FString& MessageText, const TArray<FString>& ImageBase64, bool bUseResponsesApi);
	static void AppendResponsesApiFunctionCallOutputs(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FAgentMessage>& ToolResultsToInclude, int32 MaxToolResultSize);
	static void AppendResponsesApiImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText);
	static void AppendLegacyImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText);
	static bool TryAddToolCallsToAssistantMessage(const FAgentMessage& Msg, TSharedPtr<FJsonObject> MsgObj);
	static bool CanAppendToolMessage(const TArray<TSharedPtr<FJsonValue>>& MessagesArray, int32 MessageIndex);
};
