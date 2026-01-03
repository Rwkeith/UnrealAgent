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
		bool bAllowReasoningSummary,
		const FString& AgentInstructions,
		const FString& ReasoningEffort,
		const FString& PreviousResponseId);

	static FString DetermineReasoningEffort(const FString& UserMessage, const TArray<FString>& ImagePaths);
};
