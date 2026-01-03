#include "UnrealGPTRequestConfigBuilder.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"

void UnrealGPTRequestConfigBuilder::ConfigureRequest(
	TSharedPtr<FJsonObject> RequestJson,
	const UUnrealGPTSettings* Settings,
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

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Set verbosity to low for concise outputs"));

	RequestJson->SetBoolField(TEXT("stream"), false);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using agentic tool calling endpoint"));
	RequestJson->SetStringField(TEXT("truncation"), TEXT("auto"));

	if (!PreviousResponseId.IsEmpty())
	{
		RequestJson->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using previous_response_id: %s"), *PreviousResponseId);
	}
}

FString UnrealGPTRequestConfigBuilder::DetermineReasoningEffort(const FString& UserMessage, const TArray<FString>& ImagePaths)
{
	const FString MessageLower = UserMessage.ToLower();

	// HIGH effort indicators: complex planning, reference images, architectural decisions
	// These require the model to think deeply about multiple steps and trade-offs
	const bool bHasReferenceImage = ImagePaths.Num() > 0;
	const bool bIsSceneBuilding = MessageLower.Contains(TEXT("build this scene")) ||
		MessageLower.Contains(TEXT("recreate")) ||
		MessageLower.Contains(TEXT("match this")) ||
		MessageLower.Contains(TEXT("like this image")) ||
		MessageLower.Contains(TEXT("from this reference"));
	const bool bIsArchitectural = MessageLower.Contains(TEXT("design")) ||
		MessageLower.Contains(TEXT("architecture")) ||
		MessageLower.Contains(TEXT("layout")) ||
		MessageLower.Contains(TEXT("plan out")) ||
		MessageLower.Contains(TEXT("structure"));
	const bool bIsComplex = MessageLower.Contains(TEXT("and then")) ||
		MessageLower.Contains(TEXT("after that")) ||
		MessageLower.Contains(TEXT("multiple")) ||
		MessageLower.Contains(TEXT("several")) ||
		MessageLower.Contains(TEXT("complete")) ||
		MessageLower.Contains(TEXT("entire")) ||
		MessageLower.Contains(TEXT("whole"));

	if ((bHasReferenceImage && bIsSceneBuilding) || bIsArchitectural)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning effort = HIGH (reference image scene building or architectural)"));
		return TEXT("high");
	}

	// MEDIUM effort indicators: multi-step tasks, some ambiguity, environment setup
	const bool bIsEnvironmentSetup = MessageLower.Contains(TEXT("lighting")) ||
		MessageLower.Contains(TEXT("environment")) ||
		MessageLower.Contains(TEXT("atmosphere")) ||
		MessageLower.Contains(TEXT("outdoor")) ||
		MessageLower.Contains(TEXT("indoor")) ||
		MessageLower.Contains(TEXT("setup"));
	const bool bHasQuantity = MessageLower.Contains(TEXT("few")) ||
		MessageLower.Contains(TEXT("some")) ||
		MessageLower.Contains(TEXT("arrange")) ||
		MessageLower.Contains(TEXT("distribute")) ||
		MessageLower.Contains(TEXT("place around"));
	const bool bNeedsPlanning = MessageLower.Contains(TEXT("organize")) ||
		MessageLower.Contains(TEXT("rearrange")) ||
		MessageLower.Contains(TEXT("adjust")) ||
		MessageLower.Contains(TEXT("fix")) ||
		MessageLower.Contains(TEXT("improve"));
	const bool bHasAmbiguity = MessageLower.Contains(TEXT("something like")) ||
		MessageLower.Contains(TEXT("maybe")) ||
		MessageLower.Contains(TEXT("kind of")) ||
		MessageLower.Contains(TEXT("similar to")) ||
		MessageLower.Contains(TEXT("approximately"));

	if (bIsEnvironmentSetup || bHasQuantity || bNeedsPlanning || bHasAmbiguity || bIsComplex || bHasReferenceImage)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning effort = MEDIUM (multi-step or ambiguous task)"));
		return TEXT("medium");
	}

	// MEDIUM as baseline: this agent always involves tool execution (Python code, atomic tools)
	// which benefits from additional reasoning to avoid mistakes in code generation.
	// LOW would only be appropriate for pure Q&A without tool use, which this agent doesn't do.
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning effort = MEDIUM (baseline for tool-using agent)"));
	return TEXT("medium");
}
