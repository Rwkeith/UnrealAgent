#include "UnrealGPTRequestConfigBuilder.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"

void UnrealGPTRequestConfigBuilder::ConfigureRequest(
	TSharedPtr<FJsonObject> RequestJson,
	const UUnrealGPTSettings* Settings,
	bool bUseResponsesApi,
	bool bAllowReasoningSummary,
	const FString& AgentInstructions,
	const FString& ReasoningEffort,
	const FString& PreviousResponseId)
{
	if (!RequestJson.IsValid())
	{
		return;
	}

	if (Settings)
	{
		RequestJson->SetStringField(TEXT("model"), Settings->DefaultModel);
	}

	if (bUseResponsesApi)
	{
		const FString ModelName = Settings ? Settings->DefaultModel.ToLower() : FString();
		const bool bSupportsReasoning = ModelName.Contains(TEXT("gpt-5")) || ModelName.Contains(TEXT("o1")) || ModelName.Contains(TEXT("o3"));

		if (bSupportsReasoning && !ReasoningEffort.IsEmpty())
		{
			TSharedPtr<FJsonObject> ReasoningObj = MakeShareable(new FJsonObject);
			ReasoningObj->SetStringField(TEXT("effort"), ReasoningEffort);
			if (bAllowReasoningSummary)
			{
				ReasoningObj->SetStringField(TEXT("summary"), TEXT("auto"));
			}
			RequestJson->SetObjectField(TEXT("reasoning"), ReasoningObj);
			if (Settings)
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Enabled reasoning (effort: %s%s) for model %s"),
					*ReasoningEffort,
					bAllowReasoningSummary ? TEXT(", summary: auto") : TEXT(""),
					*Settings->DefaultModel);
			}
		}

		RequestJson->SetStringField(TEXT("instructions"), AgentInstructions);

		TSharedPtr<FJsonObject> TextObj = MakeShareable(new FJsonObject);
		TextObj->SetStringField(TEXT("verbosity"), TEXT("low"));
		RequestJson->SetObjectField(TEXT("text"), TextObj);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Set Responses API verbosity to low for concise outputs"));
	}

	RequestJson->SetBoolField(TEXT("stream"), !bUseResponsesApi);

	if (bUseResponsesApi)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using Responses API for agentic tool calling"));
		RequestJson->SetStringField(TEXT("truncation"), TEXT("auto"));

		if (!PreviousResponseId.IsEmpty())
		{
			RequestJson->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using previous_response_id: %s"), *PreviousResponseId);
		}
	}
}
