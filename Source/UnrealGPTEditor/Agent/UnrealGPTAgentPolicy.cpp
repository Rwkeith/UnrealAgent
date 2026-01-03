#include "UnrealGPTAgentPolicy.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTNotifier.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool UnrealGPTAgentPolicy::DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults)
{
	if (ToolNames.Num() != ToolResults.Num() || ToolNames.Num() == 0)
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: DetectTaskCompletion - invalid input (names: %d, results: %d)"), ToolNames.Num(), ToolResults.Num());
		return false;
	}

	bool bFoundSuccessfulPythonExecute = false;
	bool bFoundSuccessfulSceneQuery = false;
	bool bFoundScreenshot = false;
	bool bFoundSuccessfulReplicateCall = false;
	bool bFoundReplicateImport = false;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: DetectTaskCompletion - analyzing %d tools"), ToolNames.Num());

	// Analyze tool results to detect completion signals
	for (int32 i = 0; i < ToolNames.Num(); ++i)
	{
		const FString& ToolName = ToolNames[i];
		const FString& ToolResult = ToolResults[i];
		
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Checking tool %d: %s (result length: %d)"), i, *ToolName, ToolResult.Len());

		if (ToolName == TEXT("python_execute"))
		{
			// Check if Python execution succeeded
			TSharedPtr<FJsonObject> ResultObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
			if (FJsonSerializer::Deserialize(Reader, ResultObj) && ResultObj.IsValid())
			{
				FString Status;
				if (ResultObj->TryGetStringField(TEXT("status"), Status) && Status == TEXT("ok"))
				{
					bFoundSuccessfulPythonExecute = true;
					
					// Check if this is an import of generated content (import_mcp_* helpers, etc.)
					FString Message;
					if (ResultObj->TryGetStringField(TEXT("message"), Message))
					{
						FString LowerMessage = Message.ToLower();
						if (LowerMessage.Contains(TEXT("imported")) && 
							(LowerMessage.Contains(TEXT("texture")) || 
							 LowerMessage.Contains(TEXT("mesh")) || 
							 LowerMessage.Contains(TEXT("audio"))))
						{
							bFoundReplicateImport = true;
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Detected content import in python_execute: %s"), *Message);
						}
						
						// Look for completion keywords in the message
						if (LowerMessage.Contains(TEXT("success")) || 
							LowerMessage.Contains(TEXT("created")) ||
							LowerMessage.Contains(TEXT("added")) ||
							LowerMessage.Contains(TEXT("completed")) ||
							LowerMessage.Contains(TEXT("done")))
						{
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - python_execute succeeded with completion keywords: %s"), *Message);
						}
					}
				}
			}
		}
		else if (ToolName == TEXT("replicate_generate"))
		{
			// Check if Replicate call succeeded and produced files
			TSharedPtr<FJsonObject> ResultObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
			if (FJsonSerializer::Deserialize(Reader, ResultObj) && ResultObj.IsValid())
			{
				FString Status;
				if (ResultObj->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success"))
				{
					// Check if files were downloaded
					const TSharedPtr<FJsonObject>* DetailsObj = nullptr;
					if (ResultObj->TryGetObjectField(TEXT("details"), DetailsObj) && DetailsObj && DetailsObj->IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
						if ((*DetailsObj)->TryGetArrayField(TEXT("files"), FilesArray) && FilesArray && FilesArray->Num() > 0)
						{
							bFoundSuccessfulReplicateCall = true;
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - replicate_generate succeeded with %d file(s)"), FilesArray->Num());
						}
					}
				}
			}
		}
		else if (ToolName == TEXT("scene_query"))
		{
			// Check if scene_query found matching objects
			if (!ToolResult.IsEmpty() && ToolResult != TEXT("[]") && ToolResult.StartsWith(TEXT("[")))
			{
				TSharedPtr<FJsonValue> JsonValue;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
				if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
					if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray) && JsonArray->Num() > 0)
					{
						bFoundSuccessfulSceneQuery = true;
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Completion detected - scene_query found %d matching objects"), JsonArray->Num());
					}
				}
			}
		}
		else if (ToolName == TEXT("viewport_screenshot"))
		{
			// Screenshot capture is a verification step
			// Check for both PNG (iVBORw0KGgo) and JPEG (/9j/) base64 headers
			const bool bIsBase64Image = ToolResult.StartsWith(TEXT("iVBORw0KGgo")) || ToolResult.StartsWith(TEXT("/9j/"));
			if (!ToolResult.IsEmpty() && bIsBase64Image)
			{
				bFoundScreenshot = true;
			}
		}
	}

	// Completion is detected if:
	// 1. Python execution succeeded AND scene_query found matching objects (strong signal)
	//    This pattern indicates: creation succeeded + verification confirmed = task complete
	// 2. Replicate call succeeded AND import succeeded AND scene_query found objects (content creation workflow)
	//    This pattern indicates: content generated + imported + verified = task complete
	// We require BOTH creation/import AND verification signals to avoid false positives
	bool bCompletionDetected = false;
	if (bFoundSuccessfulPythonExecute && bFoundSuccessfulSceneQuery)
	{
		bCompletionDetected = true;
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Task completion detected: python_execute succeeded + scene_query found objects"));
	}
	else if (bFoundSuccessfulReplicateCall && bFoundReplicateImport && bFoundSuccessfulSceneQuery)
	{
		bCompletionDetected = true;
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Task completion detected: replicate_generate succeeded + import succeeded + scene_query found objects"));
	}
	else
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("UnrealGPT: Completion not detected - python_execute: %d, replicate_generate: %d, content_import: %d, scene_query: %d"), 
			bFoundSuccessfulPythonExecute ? 1 : 0, 
			bFoundSuccessfulReplicateCall ? 1 : 0,
			bFoundReplicateImport ? 1 : 0,
			bFoundSuccessfulSceneQuery ? 1 : 0);
	}

	return bCompletionDetected;
}

bool UnrealGPTAgentPolicy::HandleToolCallIteration(
	UUnrealGPTAgentClient* Client,
	const UUnrealGPTSettings* Settings,
	bool bIsNewUserMessage,
	TArray<FAgentMessage>& ConversationHistory,
	FString& PreviousResponseId,
	int32& ToolCallIterationCount,
	bool& bRequestInProgress)
{
	if (bIsNewUserMessage)
	{
		ToolCallIterationCount = 0;
		if (ConversationHistory.Num() == 0)
		{
			PreviousResponseId.Empty();
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message with empty history - clearing previous_response_id"));
		}
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message - resetting tool call iteration counter"));
		return true;
	}

	ToolCallIterationCount++;

	const int32 MaxIterations = Settings ? Settings->MaxToolCallIterations : 0;
	if (MaxIterations > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d/%d"), ToolCallIterationCount, MaxIterations);
		if (ToolCallIterationCount >= MaxIterations)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Maximum tool call iterations (%d) reached. Stopping to prevent infinite loop."), MaxIterations);

			const FAgentMessage LimitEntry = UnrealGPTConversationState::CreateAssistantMessage(
				FString::Printf(TEXT("[Tool call limit reached after %d iterations. The conversation will continue from here. You can adjust the limit in Project Settings > UnrealGPT > Safety > Max Tool Call Iterations, or set to 0 for unlimited.]"), MaxIterations));
			UnrealGPTConversationState::AppendMessage(ConversationHistory, LimitEntry);

			if (Client)
			{
				UnrealGPTNotifier::BroadcastAgentMessage(Client, LimitEntry.Content, TArray<FString>());
			}

			ToolCallIterationCount = 0;
			bRequestInProgress = false;
			return false;
		}
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d (unlimited)"), ToolCallIterationCount);
	}

	return true;
}
