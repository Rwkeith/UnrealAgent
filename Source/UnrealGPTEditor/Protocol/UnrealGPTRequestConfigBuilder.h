#pragma once

#include "CoreMinimal.h"

class UUnrealGPTSettings;
class FJsonObject;

class UnrealGPTRequestConfigBuilder
{
public:
	static void ConfigureRequest(
		TSharedPtr<FJsonObject> RequestJson,
		const UUnrealGPTSettings* Settings,
		bool bUseResponsesApi,
		bool bAllowReasoningSummary,
		const FString& AgentInstructions,
		const FString& ReasoningEffort,
		const FString& PreviousResponseId);
};
