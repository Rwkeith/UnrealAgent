#pragma once

#include "CoreMinimal.h"

struct FToolCallInfo;

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

struct FStreamingParseResult
{
	FString AccumulatedContent;
	FString ToolCallId;
	FString ToolName;
	FString ToolArguments;
	FString FinishReason;
};

class UnrealGPTResponseParser
{
public:
	static void ExtractFromResponseOutput(const TArray<TSharedPtr<FJsonValue>>& OutputArray, FResponseParseResult& OutResult);
	static void ParseChatCompletionsSse(const FString& ResponseContent, FStreamingParseResult& OutResult);
};
