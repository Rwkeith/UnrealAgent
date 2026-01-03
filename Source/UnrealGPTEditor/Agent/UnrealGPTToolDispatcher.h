#pragma once

#include "CoreMinimal.h"

class UnrealGPTToolDispatcher
{
public:
	static FString ExecuteToolCall(
		const FString& ToolName,
		const FString& ArgumentsJson,
		bool& bLastToolWasPythonExecute,
		bool& bLastSceneQueryFoundResults,
		TFunction<void(const FString&, const FString&)> BroadcastToolCall);
};
