#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FAgentMessage;

class UnrealGPTRequestBuilder
{
public:
	static FString GetImageMimeType(const FString& ImageData);
	static void SetUserMessageWithImages(TSharedPtr<FJsonObject> MsgObj, const FString& MessageText, const TArray<FString>& ImageBase64);
	static TArray<TSharedPtr<FJsonValue>> BuildInputItems(
		const TArray<FAgentMessage>& ConversationHistory,
		const TArray<FString>& ImageBase64,
		bool bIsNewUserMessage,
		const FString& PreviousResponseId,
		int32 MaxToolResultSize);
	static void AppendFunctionCallOutputs(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FAgentMessage>& ToolResultsToInclude, int32 MaxToolResultSize);
	static void AppendImageMessage(TArray<TSharedPtr<FJsonValue>>& MessagesArray, const TArray<FString>& ImageBase64, const FString& PromptText);
};
