#pragma once

#include "CoreMinimal.h"

struct FProcessedToolResult
{
	FString ResultForHistory;
	TArray<FString> Images;
};

class UnrealGPTToolResultProcessor
{
public:
	static FProcessedToolResult ProcessResult(
		const FString& ToolName,
		const FString& ToolResult,
		int32 MaxToolResultSize);
};
