#pragma once

#include "CoreMinimal.h"
#include "UnrealGPTToolCallTypes.h"

struct FServerSideToolCall
{
	FString ToolName;
	FString CallId;
	FString ArgsJson;
	FString ResultSummary;
	FString Status;
	int32 ResultCount = 0;
};

struct FResponseParseResult
{
	TArray<FToolCallInfo> ToolCalls;
	FString AccumulatedText;
	TArray<FString> ReasoningChunks;
	TArray<FServerSideToolCall> ServerSideToolCalls;
};

class UnrealGPTResponseParser
{
public:
	static void ExtractFromResponseOutput(const TArray<TSharedPtr<FJsonValue>>& OutputArray, FResponseParseResult& OutResult);
};
