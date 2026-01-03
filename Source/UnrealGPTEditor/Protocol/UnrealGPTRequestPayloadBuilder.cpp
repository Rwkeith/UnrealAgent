#include "UnrealGPTRequestPayloadBuilder.h"
#include "UnrealGPTRequestBuilder.h"
#include "UnrealGPTRequestConfigBuilder.h"
#include "UnrealGPTToolDefinitionBuilder.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString UnrealGPTRequestPayloadBuilder::BuildRequestBody(
	const UUnrealGPTSettings* Settings,
	bool bAllowReasoningSummary,
	const FString& AgentInstructions,
	const FString& ReasoningEffort,
	const FString& PreviousResponseId,
	const TArray<FAgentMessage>& ConversationHistory,
	const TArray<FString>& ImageBase64,
	bool bIsNewUserMessage,
	int32 MaxToolResultSize)
{
	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);

	UnrealGPTRequestConfigBuilder::ConfigureRequest(
		RequestJson,
		Settings,
		bAllowReasoningSummary,
		AgentInstructions,
		ReasoningEffort,
		PreviousResponseId);

	TArray<TSharedPtr<FJsonValue>> MessagesArray = UnrealGPTRequestBuilder::BuildInputItems(
		ConversationHistory,
		ImageBase64,
		bIsNewUserMessage,
		PreviousResponseId,
		MaxToolResultSize);

	const FString ConversationFieldName = TEXT("input");
	RequestJson->SetArrayField(ConversationFieldName, MessagesArray);
	if (MessagesArray.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Request with previous_response_id but empty input array"));
	}

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& ToolDef : UnrealGPTToolDefinitionBuilder::BuildToolDefinitions(Settings))
	{
		ToolsArray.Add(MakeShareable(new FJsonValueObject(ToolDef)));
	}
	if (ToolsArray.Num() > 0)
	{
		RequestJson->SetArrayField(TEXT("tools"), ToolsArray);
	}

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestJson.ToSharedRef(), Writer);

	return RequestBody;
}
