#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentInstructions.h"
#include "UnrealGPTReplicateClient.h"
#include "UnrealGPTToolSchemas.h"
#include "UnrealGPTToolExecutor.h"
#include "UnrealGPTSessionManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Editor/EditorEngine.h"
#include "UnrealGPTSceneContext.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Async/Async.h"
#include "Misc/EngineVersion.h"

namespace
{
	/** Helper: get engine version string like "5.7" */
	FString GetEngineVersionString()
	{
		return FString::Printf(TEXT("%u.%u"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
	}

	/** Helper: Get the path to the conversation log file for a given session */
	FString GetConversationLogPath(const FString& SessionId)
	{
		// Save to project's Saved/Logs folder with session ID in filename
		const FString Filename = FString::Printf(TEXT("UnrealGPT_Conversation_%s.jsonl"), *SessionId);
		return FPaths::ProjectSavedDir() / TEXT("Logs") / Filename;
	}

	/** Helper: Log API request/response to conversation history file
	 *  Format: JSON Lines (one JSON object per line) for easy parsing
	 *  Each entry contains: timestamp, direction (request/response), and the full JSON body
	 */
	void LogApiConversation(const FString& SessionId, const FString& Direction, const FString& JsonBody, int32 ResponseCode = 0)
	{
		if (SessionId.IsEmpty())
		{
			return; // No session, skip logging
		}
		const FString ConversationLogPath = GetConversationLogPath(SessionId);

		// Create a JSON object for this log entry
		TSharedPtr<FJsonObject> LogEntry = MakeShareable(new FJsonObject);
		LogEntry->SetStringField(TEXT("timestamp"), FDateTime::Now().ToIso8601());
		LogEntry->SetStringField(TEXT("direction"), Direction);

		if (ResponseCode > 0)
		{
			LogEntry->SetNumberField(TEXT("status_code"), ResponseCode);
		}

		// Parse the JSON body to include it as a nested object (not escaped string)
		TSharedPtr<FJsonObject> BodyJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
		if (FJsonSerializer::Deserialize(Reader, BodyJson) && BodyJson.IsValid())
		{
			LogEntry->SetObjectField(TEXT("body"), BodyJson);
		}
		else
		{
			// If parsing fails, store as raw string (might be truncated or malformed)
			LogEntry->SetStringField(TEXT("body_raw"), JsonBody.Left(50000)); // Limit raw string size
		}

		// Serialize to a single line of JSON
		FString LogLine;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&LogLine);
		FJsonSerializer::Serialize(LogEntry.ToSharedRef(), Writer);
		LogLine += TEXT("\n");

		// Append to file (create if doesn't exist)
		FFileHelper::SaveStringToFile(
			LogLine,
			*ConversationLogPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			EFileWrite::FILEWRITE_Append
		);

		UE_LOG(LogTemp, Verbose, TEXT("UnrealGPT: Logged %s to conversation history"), *Direction);
	}

	/** Helper: convert an FProperty into a compact JSON description */
	TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property)
	{
		if (!Property)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> PropJson = MakeShareable(new FJsonObject);
		PropJson->SetStringField(TEXT("name"), Property->GetName());
		PropJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType(nullptr, 0));
		PropJson->SetStringField(TEXT("ue_type"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT("Unknown"));

		// Basic, high-signal property flags that are relevant for Python/Blueprint use.
		TArray<FString> Flags;
		if (Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Flags.Add(TEXT("Edit"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Flags.Add(TEXT("BlueprintVisible"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient))
		{
			Flags.Add(TEXT("Transient"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Config))
		{
			Flags.Add(TEXT("Config"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			PropJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		return PropJson;
	}

	/** Helper: convert a UFunction into a compact JSON description */
	TSharedPtr<FJsonObject> BuildFunctionJson(UFunction* Function)
	{
		if (!Function)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> FuncJson = MakeShareable(new FJsonObject);
		FuncJson->SetStringField(TEXT("name"), Function->GetName());

		// Function flags: only expose the ones that matter for scripting.
		TArray<FString> Flags;
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			Flags.Add(TEXT("BlueprintCallable"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			Flags.Add(TEXT("BlueprintPure"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			Flags.Add(TEXT("BlueprintEvent"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Net))
		{
			Flags.Add(TEXT("Net"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Flags.Add(TEXT("Static"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			FuncJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		// Parameters and return type.
		TArray<TSharedPtr<FJsonValue>> ParamsJson;
		TSharedPtr<FJsonObject> ReturnJson;

		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			FProperty* ParamProp = *ParamIt;
			if (!ParamProp)
			{
				continue;
			}

			const bool bIsReturn = ParamProp->HasAnyPropertyFlags(CPF_ReturnParm);
			if (bIsReturn)
			{
				ReturnJson = MakeShareable(new FJsonObject);
				ReturnJson->SetStringField(TEXT("name"), ParamProp->GetName());
				ReturnJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
				ReturnJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
				continue;
			}

			if (!ParamProp->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("name"), ParamProp->GetName());
			ParamJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
			ParamJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
			ParamJson->SetBoolField(TEXT("is_out"), ParamProp->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm));

			ParamsJson.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}

		if (ParamsJson.Num() > 0)
		{
			FuncJson->SetArrayField(TEXT("parameters"), ParamsJson);
		}
		if (ReturnJson.IsValid())
		{
			FuncJson->SetObjectField(TEXT("return"), ReturnJson);
		}

		return FuncJson;
	}

	/** Helper: build a reflection "schema" JSON object for a class */
	FString BuildReflectionSchemaJson(UClass* Class)
	{
		if (!Class)
		{
			TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
			ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
			ErrorObj->SetStringField(TEXT("message"), TEXT("Class not found"));

			FString ErrorJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ErrorJson);
			FJsonSerializer::Serialize(ErrorObj.ToSharedRef(), Writer);
			return ErrorJson;
		}

		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("class_name"), Class->GetName());
		Root->SetStringField(TEXT("path_name"), Class->GetPathName());
		Root->SetStringField(TEXT("cpp_type"), FString::Printf(TEXT("%s*"), *Class->GetName()));

		// Properties
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			TSharedPtr<FJsonObject> PropJson = BuildPropertyJson(Property);
			if (PropJson.IsValid())
			{
				PropertiesJson.Add(MakeShareable(new FJsonValueObject(PropJson)));
			}
		}
		Root->SetArrayField(TEXT("properties"), PropertiesJson);

		// Functions
		TArray<TSharedPtr<FJsonValue>> FunctionsJson;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			TSharedPtr<FJsonObject> FuncJson = BuildFunctionJson(Function);
			if (FuncJson.IsValid())
			{
				FunctionsJson.Add(MakeShareable(new FJsonValueObject(FuncJson)));
			}
		}
		Root->SetArrayField(TEXT("functions"), FunctionsJson);

		FString OutJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutJson;
	}

	// ==================== JSON HELPER FUNCTIONS ====================
	// These reduce repetition when building JSON for tool results and transforms

	/** Build a JSON object from an FVector */
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	/** Build a JSON object from an FRotator */
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	/** Build a standard tool result JSON string with status, message, and optional details */
	FString MakeToolResult(const FString& Status, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
	{
		TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
		ResultObj->SetStringField(TEXT("status"), Status);
		ResultObj->SetStringField(TEXT("message"), Message);
		if (Details.IsValid())
		{
			ResultObj->SetObjectField(TEXT("details"), Details);
		}

		FString ResultString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
		FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
		return ResultString;
	}

	/** Shorthand for error results */
	FString MakeErrorResult(const FString& Message)
	{
		return MakeToolResult(TEXT("error"), Message);
	}

	/** Shorthand for success results */
	FString MakeSuccessResult(const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
	{
		return MakeToolResult(TEXT("ok"), Message, Details);
	}
}

UUnrealGPTAgentClient::UUnrealGPTAgentClient()
	: ToolCallIterationCount(0)
	, bRequestInProgress(false)
{
	Settings = GetMutableDefault<UUnrealGPTSettings>();
	ExecutedToolCallSignatures.Reset();
	bLastToolWasPythonExecute = false;
	bLastSceneQueryFoundResults = false;
}

void UUnrealGPTAgentClient::Initialize()
{
	// Prevent this agent client from being garbage collected while the widget is alive.
	// The Slate widget holds a raw pointer (not a UPROPERTY), so we must root this object
	// to ensure it stays valid across level loads and GC runs.
	if (!IsRooted())
	{
		AddToRoot();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: AgentClient added to root to prevent GC"));
	}

	// Ensure settings are loaded
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
	}

	// Initialize session manager for conversation persistence
	if (!SessionManager)
	{
		SessionManager = NewObject<UUnrealGPTSessionManager>();
		SessionManager->AddToRoot();
		SessionManager->Initialize();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: SessionManager initialized"));
	}

	// Initialize conversation session ID for logging
	if (ConversationSessionId.IsEmpty())
	{
		ConversationSessionId = GenerateSessionId();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Initialized conversation session: %s"), *ConversationSessionId);

		// Begin auto-save for the initial session
		if (SessionManager)
		{
			SessionManager->BeginAutoSave(ConversationSessionId);
		}
	}
}

void UUnrealGPTAgentClient::SendMessage(const FString& UserMessage, const TArray<FString>& ImageBase64)
{
	if (bRequestInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Request already in progress"));
		return;
	}

	// Ensure Settings is valid
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved"));
			return;
		}
	}

	if (Settings->ApiKey.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: API Key not set in settings"));
		return;
	}

	// Check message size and warn/truncate if too large
	// Large messages (especially from Capture Context) can cause API timeouts or 503 errors
	const int32 MaxMessageSize = 100000; // 100KB limit for user message text
	const int32 MaxImageDataSize = 2000000; // 2MB limit for total image data
	FString ProcessedMessage = UserMessage;

	if (!UserMessage.IsEmpty())
	{
		const int32 MessageSize = UserMessage.Len();
		if (MessageSize > MaxMessageSize)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: User message is very large (%d chars). Truncating to %d chars to prevent API issues."),
				MessageSize, MaxMessageSize);
			ProcessedMessage = UserMessage.Left(MaxMessageSize) + TEXT("\n\n[Message truncated due to size limits]");
		}

		// Log size for debugging
		if (MessageSize > 10000)
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: User message size: %d characters"), MessageSize);
		}
	}

	// Check total image data size
	if (ImageBase64.Num() > 0)
	{
		int32 TotalImageSize = 0;
		for (const FString& ImageData : ImageBase64)
		{
			TotalImageSize += ImageData.Len();
		}

		if (TotalImageSize > MaxImageDataSize)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Total image data is very large (%d bytes). This may cause API timeouts. Consider using smaller images."), TotalImageSize);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Including %d image(s) with total size: %d bytes"), ImageBase64.Num(), TotalImageSize);
		}
	}

	// Reset tool call iteration counter for new user messages
	const bool bIsNewUserMessage = !UserMessage.IsEmpty();
	if (bIsNewUserMessage)
	{
		ToolCallIterationCount = 0;
		// If history is empty, clear PreviousResponseId as it's a fresh conversation
		if (ConversationHistory.Num() == 0)
		{
			PreviousResponseId.Empty();
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message with empty history - clearing previous_response_id"));
		}
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: New user message - resetting tool call iteration counter"));
	}
		else
		{
			// Increment counter for tool call continuation
			ToolCallIterationCount++;

			// Use configurable max iterations from settings. 0 = unlimited.
			const int32 MaxIterations = Settings->MaxToolCallIterations;

			if (MaxIterations > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d/%d"), ToolCallIterationCount, MaxIterations);
				if (ToolCallIterationCount >= MaxIterations)
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Maximum tool call iterations (%d) reached. Stopping to prevent infinite loop."), MaxIterations);

					// Add a system message to history so the model knows it was interrupted
					FAgentMessage LimitEntry;
					LimitEntry.Role = TEXT("assistant");
					LimitEntry.Content = FString::Printf(TEXT("[Tool call limit reached after %d iterations. The conversation will continue from here. You can adjust the limit in Project Settings > UnrealGPT > Safety > Max Tool Call Iterations, or set to 0 for unlimited.]"), MaxIterations);
					ConversationHistory.Add(LimitEntry);

					// Notify UI that we stopped
					OnAgentMessage.Broadcast(TEXT("assistant"), LimitEntry.Content, TArray<FString>());

					ToolCallIterationCount = 0;
					bRequestInProgress = false;
					return;
				}
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call continuation - iteration %d (unlimited)"), ToolCallIterationCount);
			}
		}

	// Add user message to history only if not empty (empty means continuing after tool call)
	// CRITICAL: Do NOT add empty user messages - this breaks Responses API tool continuation
	// Use ProcessedMessage which may have been truncated for size limits
	if (!ProcessedMessage.IsEmpty())
	{
		FAgentMessage UserMsg;
		UserMsg.Role = TEXT("user");
		UserMsg.Content = ProcessedMessage;
		ConversationHistory.Add(UserMsg);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added user message to history: %s"), *ProcessedMessage.Left(100));

		// Save to session for persistence
		SaveUserMessageToSession(ProcessedMessage, ImageBase64);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Empty user message - this is a tool continuation, NOT adding to history"));
	}

	// Build request JSON
	// Note: Using Responses API (/v1/responses) for better agentic tool calling support
	TSharedPtr<FJsonObject> RequestJson = MakeShareable(new FJsonObject);
	RequestJson->SetStringField(TEXT("model"), Settings->DefaultModel);
	const bool bUseResponsesApi = IsUsingResponsesApi();

	// Configure reasoning effort dynamically based on task complexity
	// (Responses API + gpt-5/o-series models)
	if (bUseResponsesApi)
	{
		// Simple check for models that likely support reasoning
		const FString ModelName = Settings->DefaultModel.ToLower();
		const bool bSupportsReasoning = ModelName.Contains(TEXT("gpt-5")) || ModelName.Contains(TEXT("o1")) || ModelName.Contains(TEXT("o3"));

		if (bSupportsReasoning)
		{
			// Determine appropriate effort level based on message complexity
			const FString ReasoningEffort = DetermineReasoningEffort(UserMessage, ImageBase64);

			TSharedPtr<FJsonObject> ReasoningObj = MakeShareable(new FJsonObject);
			ReasoningObj->SetStringField(TEXT("effort"), ReasoningEffort);
			if (bAllowReasoningSummary)
			{
				ReasoningObj->SetStringField(TEXT("summary"), TEXT("auto"));
			}
			RequestJson->SetObjectField(TEXT("reasoning"), ReasoningObj);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Enabled reasoning (effort: %s%s) for model %s"),
				*ReasoningEffort,
				bAllowReasoningSummary ? TEXT(", summary: auto") : TEXT(""),
				*Settings->DefaultModel);
		}
	}

	// High-level behavior instructions for the agent (extracted to separate file for readability)
	const FString EngineVersion = GetEngineVersionString();
	const FString AgentInstructions = UnrealGPTAgentInstructions::GetInstructions(EngineVersion);
	if (bUseResponsesApi)
	{
		RequestJson->SetStringField(TEXT("instructions"), AgentInstructions);

		TSharedPtr<FJsonObject> TextObj = MakeShareable(new FJsonObject);
		TextObj->SetStringField(TEXT("verbosity"), TEXT("low"));
		RequestJson->SetObjectField(TEXT("text"), TextObj);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Set Responses API verbosity to low for concise outputs"));
	}
	// Temporarily disable streaming for Responses API until SSE parser fully supports new event schema
	RequestJson->SetBoolField(TEXT("stream"), !bUseResponsesApi);
	
	if (bUseResponsesApi)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using Responses API for agentic tool calling"));
		
		// For Responses API, use previous_response_id if available
		if (!PreviousResponseId.IsEmpty())
		{
			RequestJson->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Using previous_response_id: %s"), *PreviousResponseId);
		}
	}

	// Build messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Building messages array from history. History size: %d"), ConversationHistory.Num());
	
	// For Responses API, handle input differently:
	// - Use previous_response_id to maintain state
	// - Only include new user messages in input (or tool results when continuing after tool execution)
	// - Function results are provided as function_call_output items when continuing after tool execution
	// For legacy API, include full conversation history
	int32 StartIndex = 0;
	TArray<FAgentMessage> ToolResultsToInclude; // For Responses API, we'll add function results as input items
	
	if (bUseResponsesApi && !PreviousResponseId.IsEmpty())
	{
		if (bIsNewUserMessage)
		{
			// For new user messages, only include the new message (it's already added to history)
			// Don't look for tool results - those are only relevant when continuing after tool execution
			// The new user message was just added, so it's the last item in history
			const int32 HistorySize = ConversationHistory.Num();
			if (HistorySize > 0)
			{
				StartIndex = HistorySize - 1; // Only include the last message (the new user message)
				// Double-check that StartIndex is valid
				if (StartIndex < 0 || StartIndex >= HistorySize)
				{
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Calculated invalid StartIndex %d for history size %d, resetting to 0"), StartIndex, HistorySize);
					StartIndex = 0;
				}
			}
			else
			{
				StartIndex = 0; // Safety fallback - should not happen as we just added a message
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: History is empty after adding user message, this should not happen"));
			}
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Responses API - new user message, starting from index %d (history size: %d)"), StartIndex, ConversationHistory.Num());
		}
		else
		{
			// For tool call continuation, find tool results that need to be included
			// Look for tool messages after the last assistant message with tool_calls
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool continuation - searching for tool results in history (size: %d)"), ConversationHistory.Num());
			
			for (int32 i = ConversationHistory.Num() - 1; i >= 0; --i)
			{
				if (ConversationHistory[i].Role == TEXT("assistant") && 
					(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found assistant message with tool_calls at index %d (tool_call_ids: %d)"), 
						i, ConversationHistory[i].ToolCallIds.Num());
					
					// Found the assistant message with tool_calls
					// Collect tool results that follow it
					for (int32 j = i + 1; j < ConversationHistory.Num(); ++j)
					{
						if (ConversationHistory[j].Role == TEXT("tool"))
						{
							ToolResultsToInclude.Add(ConversationHistory[j]);
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added tool result to include: call_id=%s, content_length=%d"), 
								*ConversationHistory[j].ToolCallId, ConversationHistory[j].Content.Len());
						}
						else if (ConversationHistory[j].Role == TEXT("user"))
						{
							StartIndex = j; // Start from this user message
							UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found user message at index %d, stopping tool result collection"), j);
							break;
						}
					}
					break;
				}
			}
			
			// If no user message found, start from after tool results
			if (StartIndex == 0 && ToolResultsToInclude.Num() > 0)
			{
				StartIndex = ConversationHistory.Num(); // Don't include any history messages, only tool results
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: No user message found, starting from end of history (index %d)"), StartIndex);
			}
			
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Responses API - tool continuation, starting from index %d, will include %d tool results"), StartIndex, ToolResultsToInclude.Num());
			
			// Log all tool call IDs we're looking for vs what we found
			if (ConversationHistory.Num() > 0)
			{
				const FAgentMessage& LastAssistantMsg = ConversationHistory[ConversationHistory.Num() - 1];
				if (LastAssistantMsg.Role == TEXT("assistant") && LastAssistantMsg.ToolCallIds.Num() > 0)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Last assistant message has %d tool_call_ids"), LastAssistantMsg.ToolCallIds.Num());
					for (const FString& ExpectedCallId : LastAssistantMsg.ToolCallIds)
					{
						bool bFound = false;
						for (const FAgentMessage& ToolResult : ToolResultsToInclude)
						{
							if (ToolResult.ToolCallId == ExpectedCallId)
							{
								bFound = true;
								break;
							}
						}
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Tool call %s: %s"), *ExpectedCallId, bFound ? TEXT("FOUND") : TEXT("MISSING"));
					}
				}
			}
		}
	}
	
	// For Responses API, add function results as input items with type "function_call_output"
	// IMPORTANT: Only include tool results that are reasonably sized to prevent context overflow
	// CRITICAL: For tool continuation (empty UserMessage), we MUST include tool results
	if (bUseResponsesApi)
	{
		// If this is a tool continuation (empty UserMessage), ensure we have tool results
		if (UserMessage.IsEmpty() && ToolResultsToInclude.Num() == 0)
		{
			// Try to find tool results from the most recent assistant message with tool_calls
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Tool continuation but no tool results found - searching history for recent tool results"));
			for (int32 i = ConversationHistory.Num() - 1; i >= 0 && i >= ConversationHistory.Num() - 10; --i)
			{
				if (ConversationHistory[i].Role == TEXT("tool"))
				{
					ToolResultsToInclude.Add(ConversationHistory[i]);
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found tool result in history at index %d: call_id=%s"), 
						i, *ConversationHistory[i].ToolCallId);
				}
				else if (ConversationHistory[i].Role == TEXT("assistant") && 
					(ConversationHistory[i].ToolCallIds.Num() > 0 || !ConversationHistory[i].ToolCallsJson.IsEmpty()))
				{
					// Found assistant message with tool_calls, stop searching backwards
					break;
				}
			}
		}
		
		if (ToolResultsToInclude.Num() > 0)
		{
			int32 TotalSize = 0;
			for (const FAgentMessage& ToolResult : ToolResultsToInclude)
			{
				// Skip tool results that are already truncated/summarized (they're already safe)
				// But also check total size to be safe
				int32 ResultSize = ToolResult.Content.Len();
				if (TotalSize + ResultSize > MaxToolResultSize * 5) // Allow up to 5x max size total
				{
					UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Skipping tool result (size: %d) to prevent context overflow. Total size: %d"), 
						ResultSize, TotalSize);
					continue;
				}
				
				// Create function_call_output input item
				TSharedPtr<FJsonObject> FunctionResultObj = MakeShareable(new FJsonObject);
				FunctionResultObj->SetStringField(TEXT("type"), TEXT("function_call_output"));
				FunctionResultObj->SetStringField(TEXT("call_id"), ToolResult.ToolCallId);
				FunctionResultObj->SetStringField(TEXT("output"), ToolResult.Content);
				
				MessagesArray.Add(MakeShareable(new FJsonValueObject(FunctionResultObj)));
				TotalSize += ResultSize;
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added function_call_output input for call_id: %s (size: %d, total: %d)"), 
					*ToolResult.ToolCallId, ResultSize, TotalSize);
			}
		}
		else if (UserMessage.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool continuation with empty message but no tool results found! This will cause API error."));
		}

		// CRITICAL: Add viewport screenshot images to the input when continuing after tool calls
		// This allows the model to actually SEE the screenshots it requested
		// For Responses API, images must be wrapped in a "message" type input item with role "user"
		if (ImageBase64.Num() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Adding %d screenshot image(s) to Responses API input for visual analysis"), ImageBase64.Num());

			// Create a message input item containing the images
			TSharedPtr<FJsonObject> MessageInputObj = MakeShareable(new FJsonObject);
			MessageInputObj->SetStringField(TEXT("type"), TEXT("message"));
			MessageInputObj->SetStringField(TEXT("role"), TEXT("user"));

			// Build content array with text prompt and images
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			// Add a text prompt to guide the model to analyze the image
			TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
			TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
			TextContent->SetStringField(TEXT("text"), TEXT("Here is the viewport screenshot you requested. Analyze what you see and describe the scene state."));
			ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

			for (const FString& ImageData : ImageBase64)
			{
				TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);

				// Detect image format from base64 header
				const FString MimeType = ImageData.StartsWith(TEXT("/9j/")) ? TEXT("image/jpeg") : TEXT("image/png");

				ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
				ImageContent->SetStringField(
					TEXT("image_url"),
					FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));

				ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added input_image (%s, %d chars) to message content"), *MimeType, ImageData.Len());
			}

			MessageInputObj->SetArrayField(TEXT("content"), ContentArray);
			MessagesArray.Add(MakeShareable(new FJsonValueObject(MessageInputObj)));
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added message with %d image(s) to Responses API input"), ImageBase64.Num());
		}
	}
	
	// Add conversation history (or subset for Responses API)
	// Ensure StartIndex is valid (non-negative and within bounds)
	const int32 HistorySize = ConversationHistory.Num();
	if (StartIndex < 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Invalid StartIndex (%d), resetting to 0"), StartIndex);
		StartIndex = 0;
	}
	// Note: StartIndex == HistorySize is valid for tool continuations (means skip all history, only send tool results)
	if (StartIndex > HistorySize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: StartIndex (%d) > history size (%d), resetting to 0"), StartIndex, HistorySize);
		StartIndex = 0;
	}
	
	// Final safety check before accessing array
	if (HistorySize == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Conversation history is empty, skipping message processing"));
	}
	else
	{
		for (int32 i = StartIndex; i < HistorySize; ++i)
		{
			// Additional bounds check inside loop for extra safety
			if (i < 0 || i >= HistorySize)
			{
				UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Array index %d out of bounds (size: %d), breaking loop"), i, HistorySize);
				break;
			}
			
			const FAgentMessage& Msg = ConversationHistory[i];
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing message %d: role=%s, hasToolCallsJson=%d, ToolCallIds.Num()=%d"), 
				i, *Msg.Role, !Msg.ToolCallsJson.IsEmpty(), Msg.ToolCallIds.Num());
			
			TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject);
			MsgObj->SetStringField(TEXT("role"), Msg.Role);
			
			if (Msg.Role == TEXT("user") && ImageBase64.Num() > 0)
			{
				// Multimodal content
				TArray<TSharedPtr<FJsonValue>> ContentArray;
				
				TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
				
				// For Responses API, use input_text; for legacy Chat Completions, use text
				if (bUseResponsesApi)
				{
					TextContent->SetStringField(TEXT("type"), TEXT("input_text"));
					TextContent->SetStringField(TEXT("text"), Msg.Content);
				}
				else
				{
					TextContent->SetStringField(TEXT("type"), TEXT("text"));
					TextContent->SetStringField(TEXT("text"), Msg.Content);
				}
				
				ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));
				
				for (const FString& ImageData : ImageBase64)
				{
					TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);

					// Detect image format from base64 header
					// PNG starts with iVBORw0KGgo, JPEG starts with /9j/
					const FString MimeType = ImageData.StartsWith(TEXT("/9j/")) ? TEXT("image/jpeg") : TEXT("image/png");

					if (bUseResponsesApi)
					{
						// OpenAI Responses API multimodal schema:
						// { "type": "input_image", "image_url": "data:image/<type>;base64,..." }
						ImageContent->SetStringField(TEXT("type"), TEXT("input_image"));
						ImageContent->SetStringField(
							TEXT("image_url"),
							FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
					}
					else
					{
						// Legacy Chat Completions multimodal schema:
						// { "type": "image_url", "image_url": { "url": "data:image/<type>;base64,..." } }
						ImageContent->SetStringField(TEXT("type"), TEXT("image_url"));
						TSharedPtr<FJsonObject> ImageUrl = MakeShareable(new FJsonObject);
						ImageUrl->SetStringField(
							TEXT("url"),
							FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
						ImageContent->SetObjectField(TEXT("image_url"), ImageUrl);
					}

					ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
				}
				
				MsgObj->SetArrayField(TEXT("content"), ContentArray);
			}
			else if (Msg.Role == TEXT("assistant") && (Msg.ToolCallIds.Num() > 0 || !Msg.ToolCallsJson.IsEmpty()))
			{
				// For Responses API, skip assistant messages with tool_calls - the API maintains tool call state internally
				if (IsUsingResponsesApi())
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping assistant message with tool_calls for Responses API (state maintained by API)"));
					continue;
				}
				
				// For legacy Chat Completions API, include assistant messages with tool_calls
				// Content can be null or empty when tool_calls are present
				if (!Msg.Content.IsEmpty())
				{
					MsgObj->SetStringField(TEXT("content"), Msg.Content);
				}
				else
				{
					// Set content to null (empty string should work, but null is more correct)
					MsgObj->SetStringField(TEXT("content"), TEXT(""));
				}
				
				// Parse and add tool_calls array - CRITICAL: must succeed
				bool bToolCallsAdded = false;
				if (!Msg.ToolCallsJson.IsEmpty())
				{
					TSharedRef<TJsonReader<>> ToolCallsReader = TJsonReaderFactory<>::Create(Msg.ToolCallsJson);
					TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
					if (FJsonSerializer::Deserialize(ToolCallsReader, ToolCallsArray) && ToolCallsArray.Num() > 0)
					{
						MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
						bToolCallsAdded = true;
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Successfully added tool_calls to assistant message. ToolCalls count: %d"), ToolCallsArray.Num());
					}
					else
					{
						// If deserialization failed, try to reconstruct from stored data
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to deserialize tool_calls JSON: %s. Attempting reconstruction."), *Msg.ToolCallsJson);
					}
				}
				
				// If tool_calls weren't added and we have ToolCallIds, try to reconstruct
				if (!bToolCallsAdded && Msg.ToolCallIds.Num() > 0)
				{
					// Reconstruct minimal tool_calls array from ToolCallIds
					// Note: This is a fallback - we don't have the full tool call info, but we can create a minimal structure
					TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
					for (const FString& ToolCallId : Msg.ToolCallIds)
					{
						TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
						ToolCallObj->SetStringField(TEXT("id"), ToolCallId);
						ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));
						
						// We don't have the function name/args, so create empty function object
						TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
						FunctionObj->SetStringField(TEXT("name"), TEXT("unknown"));
						FunctionObj->SetStringField(TEXT("arguments"), TEXT("{}"));
						ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);
						
						ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));
					}
					
					if (ToolCallsArray.Num() > 0)
					{
						MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
						bToolCallsAdded = true;
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Reconstructed tool_calls from ToolCallIds. Count: %d"), ToolCallsArray.Num());
					}
				}
				
				// If we still couldn't add tool_calls, this is a critical error
				if (!bToolCallsAdded)
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: CRITICAL: Cannot add tool_calls to assistant message. ToolCallsJson empty: %d, ToolCallIds.Num(): %d"), 
						Msg.ToolCallsJson.IsEmpty(), Msg.ToolCallIds.Num());
					// Skip this message to avoid API error - but this will cause the tool message to be orphaned
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Skipping assistant message without valid tool_calls to prevent API error"));
					continue;
				}
			}
			else if (Msg.Role == TEXT("tool"))
			{
				// Tool messages are NOT supported in Responses API input array
				// The API maintains tool call state internally via previous_response_id
				if (bUseResponsesApi)
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Skipping tool message for Responses API (state maintained via previous_response_id)"));
					continue;
				}
				
				// For legacy API, tool messages must follow an assistant message with tool_calls
				bool bCanAddToolMessage = false;
				if (MessagesArray.Num() > 0)
				{
					const TSharedPtr<FJsonValue>& LastMsgValue = MessagesArray.Last();
					if (LastMsgValue.IsValid() && LastMsgValue->Type == EJson::Object)
					{
						TSharedPtr<FJsonObject> LastMsgObj = LastMsgValue->AsObject();
						if (LastMsgObj.IsValid())
						{
							FString LastRole;
							if (LastMsgObj->TryGetStringField(TEXT("role"), LastRole))
							{
								if (LastRole == TEXT("assistant") && LastMsgObj->HasField(TEXT("tool_calls")))
								{
									bCanAddToolMessage = true;
								}
								else
								{
									UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d does not follow assistant message with tool_calls. Previous role: %s, has tool_calls: %d"), 
										i, *LastRole, LastMsgObj->HasField(TEXT("tool_calls")));
								}
							}
						}
					}
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Tool message at index %d has no preceding messages"), i);
				}
				
				// Only add tool message if it follows a valid assistant message with tool_calls
				if (!bCanAddToolMessage)
				{
					UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Skipping tool message at index %d to prevent API error"), i);
					continue;
				}
				
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
				MsgObj->SetStringField(TEXT("tool_call_id"), Msg.ToolCallId);
			}
			else
			{
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
			}
			
			MessagesArray.Add(MakeShareable(new FJsonValueObject(MsgObj)));
		}
	}

	// For legacy Chat Completions API: Add screenshot images as a user message when continuing after tool calls
	// (The Responses API handles this earlier in the function)
	if (!bUseResponsesApi && ImageBase64.Num() > 0 && UserMessage.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Adding %d screenshot image(s) as user message for visual analysis (legacy API)"), ImageBase64.Num());

		TSharedPtr<FJsonObject> ImageMsgObj = MakeShareable(new FJsonObject);
		ImageMsgObj->SetStringField(TEXT("role"), TEXT("user"));

		TArray<TSharedPtr<FJsonValue>> ContentArray;

		// Add a text prompt to guide the model to analyze the image
		TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), TEXT("Here is the viewport screenshot you requested. Please analyze what you see and describe the scene."));
		ContentArray.Add(MakeShareable(new FJsonValueObject(TextContent)));

		for (const FString& ImageData : ImageBase64)
		{
			TSharedPtr<FJsonObject> ImageContent = MakeShareable(new FJsonObject);
			const FString MimeType = ImageData.StartsWith(TEXT("/9j/")) ? TEXT("image/jpeg") : TEXT("image/png");

			ImageContent->SetStringField(TEXT("type"), TEXT("image_url"));
			TSharedPtr<FJsonObject> ImageUrl = MakeShareable(new FJsonObject);
			ImageUrl->SetStringField(TEXT("url"), FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *ImageData));
			ImageContent->SetObjectField(TEXT("image_url"), ImageUrl);

			ContentArray.Add(MakeShareable(new FJsonValueObject(ImageContent)));
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added image_url (%s, %d chars) to user message"), *MimeType, ImageData.Len());
		}

		ImageMsgObj->SetArrayField(TEXT("content"), ContentArray);
		MessagesArray.Add(MakeShareable(new FJsonValueObject(ImageMsgObj)));
	}

	const FString ConversationFieldName = IsUsingResponsesApi() ? TEXT("input") : TEXT("messages");
	
	// For Responses API, if we have function results, they're already added as input items
	// Otherwise, add the messages array
	if (MessagesArray.Num() > 0 || !bUseResponsesApi)
	{
		RequestJson->SetArrayField(ConversationFieldName, MessagesArray);
	}
	else if (bUseResponsesApi && MessagesArray.Num() == 0)
	{
		// For Responses API, if we have previous_response_id but no new input, 
		// we still need to provide an empty array or the API might error
		RequestJson->SetArrayField(ConversationFieldName, MessagesArray);
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Responses API request with previous_response_id but empty input array"));
	}

	// Add tools
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& ToolDef : BuildToolDefinitions())
	{
		ToolsArray.Add(MakeShareable(new FJsonValueObject(ToolDef)));
	}
	if (ToolsArray.Num() > 0)
	{
		RequestJson->SetArrayField(TEXT("tools"), ToolsArray);
	}

	// Serialize to string
	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestJson.ToSharedRef(), Writer);

	// Log the request body for debugging (truncated to avoid excessive log spam)
	const int32 MaxLogLength = 2000;
	if (RequestBody.Len() <= MaxLogLength)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body: %s"), *RequestBody);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body (truncated): %s..."), *RequestBody.Left(MaxLogLength));
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body length: %d characters"), RequestBody.Len());
	}

	// Cache the body so we can safely retry with small modifications (e.g., stripping reasoning.summary)
	LastRequestBody = RequestBody;

	// Create HTTP request
	CurrentRequest = CreateHttpRequest();
	CurrentRequest->SetURL(GetEffectiveApiUrl());
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));
	CurrentRequest->SetContentAsString(RequestBody);
	CurrentRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

	bRequestInProgress = true;
	RequestStartTime = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Starting HTTP request (timeout: %.1f seconds)"), Settings->ExecutionTimeoutSeconds);
	CurrentRequest->ProcessRequest();
}

void UUnrealGPTAgentClient::CancelRequest()
{
	if (CurrentRequest.IsValid() && bRequestInProgress)
	{
		CurrentRequest->CancelRequest();
		bRequestInProgress = false;
	}
}

void UUnrealGPTAgentClient::ClearHistory()
{
	ConversationHistory.Empty();
	PreviousResponseId.Empty();
	ToolCallIterationCount = 0;
	ExecutedToolCallSignatures.Reset();
	bLastToolWasPythonExecute = false;
	bLastSceneQueryFoundResults = false;
}

FString UUnrealGPTAgentClient::GenerateSessionId()
{
	// Format: YYYYMMDD_HHMMSS (e.g., 20260101_143052)
	return FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
}

void UUnrealGPTAgentClient::StartNewConversation()
{
	// Save current session before starting new one
	if (SessionManager && !ConversationSessionId.IsEmpty())
	{
		SessionManager->SaveCurrentSession();
		SessionManager->EndAutoSave();
	}

	// Generate a new session ID for the new conversation log file
	ConversationSessionId = GenerateSessionId();

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Starting new conversation with session ID: %s"), *ConversationSessionId);

	// Clear the conversation history (same as ClearHistory)
	ClearHistory();

	// Begin auto-save for the new session
	if (SessionManager)
	{
		SessionManager->BeginAutoSave(ConversationSessionId);
		SessionManager->RefreshSessionList();
	}
}

TArray<TSharedPtr<FJsonObject>> UUnrealGPTAgentClient::BuildToolDefinitions()
{
	TArray<TSharedPtr<FJsonObject>> Tools;
	const bool bUseResponsesApi = IsUsingResponsesApi();

	// Determine which tools to enable based on settings
	const bool bEnablePython = Settings && Settings->bEnablePythonExecution;
	const bool bEnableViewport = Settings && Settings->bEnableViewportScreenshot;
	const bool bEnableReplicate = Settings && Settings->bEnableReplicateTool && !Settings->ReplicateApiToken.IsEmpty();

	// Get all standard tool schemas and convert to JSON
	TArray<FToolSchema> Schemas = UnrealGPTToolSchemas::GetStandardToolSchemas(bEnablePython, bEnableViewport, bEnableReplicate);
	for (const FToolSchema& Schema : Schemas)
	{
		Tools.Add(UnrealGPTToolSchemas::BuildToolJson(Schema, bUseResponsesApi));
	}

	// OpenAI-hosted web_search and file_search tools (Responses API only).
	// These use a different JSON format and aren't function tools.
	if (bUseResponsesApi)
	{
		// web_search
		TSharedPtr<FJsonObject> WebSearchTool = MakeShareable(new FJsonObject);
		WebSearchTool->SetStringField(TEXT("type"), TEXT("web_search"));
		Tools.Add(WebSearchTool);

		// file_search over UE Python API docs (only if VectorStoreId is configured)
		if (Settings && !Settings->VectorStoreId.IsEmpty())
		{
			TSharedPtr<FJsonObject> FileSearchTool = MakeShareable(new FJsonObject);
			FileSearchTool->SetStringField(TEXT("type"), TEXT("file_search"));

			TArray<TSharedPtr<FJsonValue>> VectorStores;
			VectorStores.Add(MakeShareable(new FJsonValueString(Settings->VectorStoreId)));
			FileSearchTool->SetArrayField(TEXT("vector_store_ids"), VectorStores);
			FileSearchTool->SetNumberField(TEXT("max_num_results"), 20);

			Tools.Add(FileSearchTool);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: file_search enabled with vector store: %s"), *Settings->VectorStoreId);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: file_search disabled (no Vector Store ID configured)"));
		}
	}

	return Tools;
}

void UUnrealGPTAgentClient::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bRequestInProgress = false;

	// Calculate elapsed time for this request
	const double ElapsedTime = FPlatformTime::Seconds() - RequestStartTime;

	// Ensure Settings is valid before proceeding
	if (!Settings)
	{
		Settings = GetMutableDefault<UUnrealGPTSettings>();
		if (!Settings)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null and could not be retrieved in OnResponseReceived"));
			return;
		}
	}

	if (!bWasSuccessful || !Response.IsValid())
	{
		// Log more details about the failure including timing
		EHttpRequestStatus::Type RequestStatus = EHttpRequestStatus::NotStarted;
		if (Request.IsValid())
		{
			RequestStatus = Request->GetStatus();
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP request FAILED after %.2f seconds (timeout setting: %.1f seconds) - URL: %s, Status: %s"),
				ElapsedTime,
				Settings->ExecutionTimeoutSeconds,
				*Request->GetURL(),
				EHttpRequestStatus::ToString(RequestStatus));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP request FAILED after %.2f seconds - Request object is invalid"), ElapsedTime);
		}

		if (Response.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Response code: %d, Content: %s"),
				Response->GetResponseCode(),
				*Response->GetContentAsString().Left(500));
		}

		// Retry once for transient failures (connection reset, etc.)
		// Only retry if we have a cached request body and haven't already retried
		const int32 MaxRetries = 1;
		if (HttpRetryCount < MaxRetries && !LastRequestBody.IsEmpty() &&
			RequestStatus == EHttpRequestStatus::Failed)
		{
			HttpRetryCount++;
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Retrying request (attempt %d/%d) after transient failure..."), HttpRetryCount, MaxRetries + 1);

			// Wait a moment before retrying (using a timer to avoid blocking)
			FTimerHandle RetryTimerHandle;
			GEditor->GetTimerManager()->SetTimer(RetryTimerHandle, [this]()
			{
				if (!LastRequestBody.IsEmpty())
				{
					TSharedRef<IHttpRequest> RetryRequest = CreateHttpRequest();
					RetryRequest->SetURL(GetEffectiveApiUrl());
					RetryRequest->SetVerb(TEXT("POST"));
					RetryRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					if (SafeSettings && !SafeSettings->ApiKey.IsEmpty())
					{
						RetryRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *SafeSettings->ApiKey));
					}

					RetryRequest->SetContentAsString(LastRequestBody);
					RetryRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

					bRequestInProgress = true;
					RequestStartTime = FPlatformTime::Seconds();

						RetryRequest->ProcessRequest();
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Retry request sent (timeout: %.1f seconds)"), SafeSettings ? SafeSettings->ExecutionTimeoutSeconds : 90.0f);
				}
			}, 1.0f, false); // Wait 1 second before retry

			return;
		}

		// Reset retry count on final failure
		HttpRetryCount = 0;
		return;
	}

	// Reset retry count on success
	HttpRetryCount = 0;

	int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: HTTP response received in %.2f seconds - Status: %d"), ElapsedTime, ResponseCode);

	if (ResponseCode != 200)
	{
		const FString& ErrorBody = ResponseBody;
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: HTTP error %d: %s"), ResponseCode, *ErrorBody);

		// Handle 429 rate limit errors by retrying after a delay
		if (ResponseCode == 429 && !LastRequestBody.IsEmpty() && RateLimitRetryCount < MaxRateLimitRetries)
		{
			RateLimitRetryCount++;

			// Parse the error to extract retry delay if provided
			float RetryDelaySeconds = 1.0f; // Default 1 second
			TSharedPtr<FJsonObject> ErrorRoot;
			TSharedRef<TJsonReader<>> ErrorReader = TJsonReaderFactory<>::Create(ErrorBody);
			if (FJsonSerializer::Deserialize(ErrorReader, ErrorRoot) && ErrorRoot.IsValid())
			{
				const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
				if (ErrorRoot->TryGetObjectField(TEXT("error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
				{
					FString Message;
					(*ErrorObjPtr)->TryGetStringField(TEXT("message"), Message);

					// Try to extract delay from message like "Please try again in 589ms"
					int32 MsIndex = Message.Find(TEXT("in "));
					if (MsIndex != INDEX_NONE)
					{
						FString DelayPart = Message.Mid(MsIndex + 3);
						if (DelayPart.Contains(TEXT("ms")))
						{
							int32 DelayMs = FCString::Atoi(*DelayPart);
							if (DelayMs > 0)
							{
								RetryDelaySeconds = FMath::Max(0.5f, DelayMs / 1000.0f + 0.1f); // Add 100ms buffer
							}
						}
						else if (DelayPart.Contains(TEXT("s")))
						{
							float DelayS = FCString::Atof(*DelayPart);
							if (DelayS > 0)
							{
								RetryDelaySeconds = DelayS + 0.1f; // Add 100ms buffer
							}
						}
					}
				}
			}

			// Cap retry delay to prevent excessively long waits
			RetryDelaySeconds = FMath::Clamp(RetryDelaySeconds, 0.5f, 30.0f);

			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Rate limited (429). Retry %d/%d in %.2f seconds..."), RateLimitRetryCount, MaxRateLimitRetries, RetryDelaySeconds);

			FTimerHandle RateLimitRetryHandle;
			GEditor->GetTimerManager()->SetTimer(RateLimitRetryHandle, [this]()
			{
				if (!LastRequestBody.IsEmpty())
				{
					TSharedRef<IHttpRequest> RetryRequest = CreateHttpRequest();
					RetryRequest->SetURL(GetEffectiveApiUrl());
					RetryRequest->SetVerb(TEXT("POST"));
					RetryRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					if (SafeSettings && !SafeSettings->ApiKey.IsEmpty())
					{
						RetryRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *SafeSettings->ApiKey));
					}

					RetryRequest->SetContentAsString(LastRequestBody);
					RetryRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

					bRequestInProgress = true;
					RequestStartTime = FPlatformTime::Seconds();
					RetryRequest->ProcessRequest();
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Rate limit retry request sent"));
				}
			}, RetryDelaySeconds, false);

			return;
		}

		// Gracefully handle organizations that are not yet allowed to use reasoning summaries.
		// In that case, we disable reasoning.summary for this session and let the user continue
		// using the agent without having to change any settings.
		if (ResponseCode == 400 && bAllowReasoningSummary && IsUsingResponsesApi())
		{
			TSharedPtr<FJsonObject> ErrorRoot;
			TSharedRef<TJsonReader<>> ErrorReader = TJsonReaderFactory<>::Create(ErrorBody);
			if (FJsonSerializer::Deserialize(ErrorReader, ErrorRoot) && ErrorRoot.IsValid())
			{
				const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
				if (ErrorRoot->TryGetObjectField(TEXT("error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
				{
					FString Param;
					FString Code;
					FString Message;
					(*ErrorObjPtr)->TryGetStringField(TEXT("param"), Param);
					(*ErrorObjPtr)->TryGetStringField(TEXT("code"), Code);
					(*ErrorObjPtr)->TryGetStringField(TEXT("message"), Message);

					if (Param == TEXT("reasoning.summary") && Code == TEXT("unsupported_value"))
					{
						UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Disabling reasoning.summary — org is not verified (%s)"), *Message);
						bAllowReasoningSummary = false;

						// Retry the last request once, without reasoning.summary.
						if (!bRequestInProgress && !LastRequestBody.IsEmpty())
						{
							const FString OriginalBody = LastRequestBody;
							TSharedPtr<FJsonObject> OriginalJson;
							TSharedRef<TJsonReader<>> OriginalReader = TJsonReaderFactory<>::Create(OriginalBody);
							if (FJsonSerializer::Deserialize(OriginalReader, OriginalJson) && OriginalJson.IsValid())
							{
								const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
								if (OriginalJson->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
								{
									TSharedPtr<FJsonObject> ReasoningObj = *ReasoningObjPtr;
									ReasoningObj->RemoveField(TEXT("summary"));
									OriginalJson->SetObjectField(TEXT("reasoning"), ReasoningObj);

									FString NewBody;
									TSharedRef<TJsonWriter<>> NewWriter = TJsonWriterFactory<>::Create(&NewBody);
									if (FJsonSerializer::Serialize(OriginalJson.ToSharedRef(), NewWriter))
									{
										TSharedRef<IHttpRequest> RetryRequest = CreateHttpRequest();
										RetryRequest->SetURL(GetEffectiveApiUrl());
										RetryRequest->SetVerb(TEXT("POST"));
										RetryRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
										RetryRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));
										RetryRequest->SetContentAsString(NewBody);
										RetryRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealGPTAgentClient::OnResponseReceived);

										bRequestInProgress = true;
										RequestStartTime = FPlatformTime::Seconds();
										UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Retrying request without reasoning.summary (timeout: %.1f seconds)"), Settings->ExecutionTimeoutSeconds);
										RetryRequest->ProcessRequest();
										return;
									}
								}
							}
						}
					}
				}
			}
		}

		return;
	}

	// Reset rate limit retry counter on successful response
	RateLimitRetryCount = 0;

	// Log successful request/response pair to conversation history file
	LogApiConversation(ConversationSessionId, TEXT("request"), LastRequestBody);
	LogApiConversation(ConversationSessionId, TEXT("response"), ResponseBody, ResponseCode);

	// Use ResponseBody which was already retrieved above
	const FString& ResponseContent = ResponseBody;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received response (length: %d)"), ResponseContent.Len());
	if (ResponseContent.Len() < 500)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response content: %s"), *ResponseContent);
	}

	if (IsUsingResponsesApi())
	{
		ProcessResponsesApiResponse(ResponseContent);
	}
	else
	{
		ProcessStreamingResponse(ResponseContent);
	}
}

void UUnrealGPTAgentClient::ProcessStreamingResponse(const FString& ResponseContent)
{
	// Parse streaming response (SSE format)
	TArray<FString> Lines;
	ResponseContent.ParseIntoArrayLines(Lines);

	FString AccumulatedContent;
	FString CurrentToolCallId;
	FString CurrentToolName;
	FString CurrentToolArguments;

	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("data: ")))
		{
			FString Data = Line.Mid(6); // Remove "data: "
			if (Data == TEXT("[DONE]"))
			{
				break;
			}

			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
			if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
			{
				// Check for choices array
				const TArray<TSharedPtr<FJsonValue>>* ChoicesArray;
				if (JsonObject->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* ChoiceObj;
					if ((*ChoicesArray)[0]->TryGetObject(ChoiceObj))
					{
						const TSharedPtr<FJsonObject>* DeltaObj;
						if ((*ChoiceObj)->TryGetObjectField(TEXT("delta"), DeltaObj))
						{
							// Content delta
							FString ContentDelta;
							if ((*DeltaObj)->TryGetStringField(TEXT("content"), ContentDelta))
							{
								AccumulatedContent += ContentDelta;
							}

							// Tool calls delta
							const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray;
							if ((*DeltaObj)->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
							{
								for (const TSharedPtr<FJsonValue>& ToolCallValue : *ToolCallsArray)
								{
									const TSharedPtr<FJsonObject>* ToolCallObj;
									if (ToolCallValue->TryGetObject(ToolCallObj))
									{
										(*ToolCallObj)->TryGetStringField(TEXT("id"), CurrentToolCallId);
										
										const TSharedPtr<FJsonObject>* FunctionObj;
										if ((*ToolCallObj)->TryGetObjectField(TEXT("function"), FunctionObj))
										{
											(*FunctionObj)->TryGetStringField(TEXT("name"), CurrentToolName);
											
											FString ArgumentsDelta;
											if ((*FunctionObj)->TryGetStringField(TEXT("arguments"), ArgumentsDelta))
											{
												CurrentToolArguments += ArgumentsDelta;
											}
										}
									}
								}
							}
						}

						// Check if finished
						FString FinishReason;
						if ((*ChoiceObj)->TryGetStringField(TEXT("finish_reason"), FinishReason))
						{
							if (FinishReason == TEXT("tool_calls") && !CurrentToolCallId.IsEmpty())
							{
								// Build tool_calls JSON for assistant message
								TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
								ToolCallObj->SetStringField(TEXT("id"), CurrentToolCallId);
								ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));
								
								TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
								FunctionObj->SetStringField(TEXT("name"), CurrentToolName);
								FunctionObj->SetStringField(TEXT("arguments"), CurrentToolArguments);
								ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);

								TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
								ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));

								FString ToolCallsJsonString;
								TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
								bool bSerialized = FJsonSerializer::Serialize(ToolCallsArray, ToolCallsWriter);
								
								if (!bSerialized || ToolCallsJsonString.IsEmpty())
								{
									UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to serialize tool_calls array"));
								}
								else
								{
									UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Serialized tool_calls: %s"), *ToolCallsJsonString);
								}

								// Add assistant message with tool_calls to history FIRST
								FAgentMessage AssistantMsg;
								AssistantMsg.Role = TEXT("assistant");
								AssistantMsg.Content = AccumulatedContent;
								AssistantMsg.ToolCallIds.Add(CurrentToolCallId);
								AssistantMsg.ToolCallsJson = ToolCallsJsonString;
								ConversationHistory.Add(AssistantMsg);
								
								UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message with tool_calls to history. History size: %d"), ConversationHistory.Num());

								OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedContent, TArray<FString>{CurrentToolCallId});

								// Execute tool call
								FString ToolResult = ExecuteToolCall(CurrentToolName, CurrentToolArguments);
								
								// Truncate or summarize large tool results to prevent context window overflow
								FString ToolResultForHistory = ToolResult;
								const bool bIsScreenshot = (CurrentToolName == TEXT("viewport_screenshot"));
								if (ToolResultForHistory.Len() > MaxToolResultSize)
								{
									// Check for both PNG (iVBORw0KGgo) and JPEG (/9j/) base64 headers
									const bool bIsBase64Image = ToolResultForHistory.StartsWith(TEXT("iVBORw0KGgo")) || ToolResultForHistory.StartsWith(TEXT("/9j/"));
									if (bIsScreenshot && bIsBase64Image)
									{
										ToolResultForHistory = TEXT("Screenshot captured successfully. [Base64 image data omitted from history to prevent context overflow - ")
											TEXT("the image was captured and can be viewed in the UI. Length: ") + FString::FromInt(ToolResult.Len()) + TEXT(" characters]");
										UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large screenshot result (%d chars) to prevent context overflow"), ToolResult.Len());
									}
									else
									{
										ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSize) + 
											TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) + 
											TEXT(" characters. Full result available in tool output.]");
										UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large tool result (%d chars) to prevent context overflow"), ToolResult.Len());
									}
								}
								
								// Add tool result to conversation (truncated version)
								FAgentMessage ToolMsg;
								ToolMsg.Role = TEXT("tool");
								ToolMsg.Content = ToolResultForHistory;
								ToolMsg.ToolCallId = CurrentToolCallId;
								ConversationHistory.Add(ToolMsg);

								OnToolResult.Broadcast(CurrentToolCallId, ToolResult);

								// Continue conversation with tool result.
								// If this was a viewport_screenshot call, also forward the image as multimodal input
								// so the model can analyze the actual viewport image (not just a text summary).
								TArray<FString> ScreenshotImages;
								if (bIsScreenshot && !ToolResult.IsEmpty())
								{
									ScreenshotImages.Add(ToolResult);
								}

								SendMessage(TEXT(""), ScreenshotImages);
							}
							else if (!AccumulatedContent.IsEmpty())
							{
								// Add assistant message to history
								FAgentMessage AssistantMsg;
								AssistantMsg.Role = TEXT("assistant");
								AssistantMsg.Content = AccumulatedContent;
								ConversationHistory.Add(AssistantMsg);

								OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedContent, TArray<FString>());
							}
						}
					}
				}
			}
		}
	}
}

void UUnrealGPTAgentClient::ProcessResponsesApiResponse(const FString& ResponseContent)
{
	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to parse Responses API JSON response"));
		return;
	}

	// Log all top-level fields to understand the response structure
	TArray<FString> FieldNames;
	RootObject->Values.GetKeys(FieldNames);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response root fields: %s"), *FString::Join(FieldNames, TEXT(", ")));

	// Store the response ID for subsequent requests
	FString ResponseId;
	if (RootObject->TryGetStringField(TEXT("id"), ResponseId))
	{
		PreviousResponseId = ResponseId;
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Stored PreviousResponseId: %s"), *PreviousResponseId);
	}
	
	// Check response status
	FString Status;
	if (RootObject->TryGetStringField(TEXT("status"), Status))
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response status: %s"), *Status);
		if (Status == TEXT("failed") || Status == TEXT("cancelled"))
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Response status indicates failure: %s"), *Status);
			ToolCallIterationCount = 0; // Reset on failure
			bRequestInProgress = false;
			return;
		}
	}

	// If the model provided a reasoning summary, surface it immediately for the UI.
	const TSharedPtr<FJsonObject>* ReasoningObjPtr = nullptr;
	if (RootObject->TryGetObjectField(TEXT("reasoning"), ReasoningObjPtr) && ReasoningObjPtr && (*ReasoningObjPtr).IsValid())
	{
		FString ReasoningSummary;
		if ((*ReasoningObjPtr)->TryGetStringField(TEXT("summary"), ReasoningSummary) && !ReasoningSummary.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Received reasoning summary (length: %d)"), ReasoningSummary.Len());
			OnAgentReasoning.Broadcast(ReasoningSummary);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("output"), OutputArray) || !OutputArray)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Responses API response missing 'output' array. Checking for streaming format..."));
		
		// Check if it's actually a streaming response (SSE format)
		if (ResponseContent.Contains(TEXT("data: ")))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Response appears to be streaming format, processing as SSE"));
			ProcessStreamingResponse(ResponseContent);
			return;
		}
		
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found output array with %d items"), OutputArray->Num());

	// Extract tool calls and text from output array using helper method
	TArray<FToolCallInfo> ToolCalls;
	FString AccumulatedText;
	ExtractFromResponseOutput(*OutputArray, ToolCalls, AccumulatedText);

	// Process the extracted tool calls and/or text
	if (ToolCalls.Num() > 0)
	{
		ProcessExtractedToolCalls(ToolCalls, AccumulatedText);
		return;
	}

	// No tool calls - just process the text response
	if (!AccumulatedText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing regular assistant message (no tool calls)"));
		FAgentMessage AssistantMsg;
		AssistantMsg.Role = TEXT("assistant");
		AssistantMsg.Content = AccumulatedText;
		ConversationHistory.Add(AssistantMsg);

		OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedText, TArray<FString>());
		ToolCallIterationCount = 0;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Message output had no text content and no tool calls"));
		ToolCallIterationCount = 0;
	}
}

void UUnrealGPTAgentClient::ExtractFromResponseOutput(const TArray<TSharedPtr<FJsonValue>>& OutputArray, TArray<FToolCallInfo>& OutToolCalls, FString& OutAccumulatedText)
{
	for (int32 i = 0; i < OutputArray.Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& OutputValue = OutputArray[i];
		TSharedPtr<FJsonObject> OutputObj = OutputValue.IsValid() ? OutputValue->AsObject() : nullptr;
		if (!OutputObj.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Output item %d is not a valid object"), i);
			continue;
		}

		FString OutputType;
		OutputObj->TryGetStringField(TEXT("type"), OutputType);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Output item %d type: %s"), i, *OutputType);

		if (OutputType == TEXT("function_call"))
		{
			// Direct function_call output item
			FToolCallInfo Info;

			// Try different possible field names for the function call ID
			if (!OutputObj->TryGetStringField(TEXT("call_id"), Info.Id))
			{
				if (!OutputObj->TryGetStringField(TEXT("id"), Info.Id))
				{
					OutputObj->TryGetStringField(TEXT("function_call_id"), Info.Id);
				}
			}

			// Try to get function information
			const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
			if (OutputObj->TryGetObjectField(TEXT("function"), FunctionObjPtr) && FunctionObjPtr && FunctionObjPtr->IsValid())
			{
				(*FunctionObjPtr)->TryGetStringField(TEXT("name"), Info.Name);
				(*FunctionObjPtr)->TryGetStringField(TEXT("arguments"), Info.Arguments);
			}
			else
			{
				// Try direct fields on the output object
				OutputObj->TryGetStringField(TEXT("name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("function_name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("arguments"), Info.Arguments);
				OutputObj->TryGetStringField(TEXT("function_arguments"), Info.Arguments);
			}

			if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
			{
				OutToolCalls.Add(Info);
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found function call - id: %s, name: %s"), *Info.Id, *Info.Name);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: function_call output missing required fields - id: '%s', name: '%s'"), *Info.Id, *Info.Name);
			}
		}
		else if (OutputType == TEXT("file_search_call") || OutputType == TEXT("web_search_call"))
		{
			// Handle specialized OpenAI-hosted search tools (server-side only)
			const bool bIsFileSearch = (OutputType == TEXT("file_search_call"));
			const FString ToolName = bIsFileSearch ? TEXT("file_search") : TEXT("web_search");

			FString CallId;
			if (!OutputObj->TryGetStringField(TEXT("call_id"), CallId))
			{
				OutputObj->TryGetStringField(TEXT("id"), CallId);
			}

			// Build a compact arguments JSON object for the UI (queries, status, etc.)
			// Note: In the Responses API, queries/results are at the TOP LEVEL of the output object,
			// not nested inside a "file_search" or "web_search" sub-object.
			TSharedPtr<FJsonObject> ArgsJson = MakeShareable(new FJsonObject);

			// Also extract results for UI display
			FString ResultSummary;
			int32 ResultCount = 0;

			// Copy relevant fields to ArgsJson for UI display (skip results, we summarize separately)
			for (const auto& Pair : OutputObj->Values)
			{
				if (Pair.Key != TEXT("results") && Pair.Key != TEXT("type") && Pair.Key != TEXT("id"))
				{
					ArgsJson->SetField(Pair.Key, Pair.Value);
				}
			}

			// Extract results array (at top level of OutputObj)
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			if (OutputObj->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray)
			{
				ResultCount = ResultsArray->Num();
				if (ResultCount > 0)
				{
					ResultSummary = FString::Printf(TEXT("Found %d result(s):\n\n"), ResultCount);

					// Show up to 3 results with snippets
					const int32 MaxToShow = FMath::Min(3, ResultCount);
					for (int32 ResultIdx = 0; ResultIdx < MaxToShow; ++ResultIdx)
					{
						const TSharedPtr<FJsonObject> ResultObj = (*ResultsArray)[ResultIdx]->AsObject();
						if (!ResultObj.IsValid()) continue;

						FString FileName;
						ResultObj->TryGetStringField(TEXT("filename"), FileName);
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("name"), FileName);
						}
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("file_id"), FileName);
						}
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("id"), FileName);
						}

						// Try to get text snippet
						FString Snippet;
						const TArray<TSharedPtr<FJsonValue>>* TextArray = nullptr;
						if (ResultObj->TryGetArrayField(TEXT("text"), TextArray) && TextArray && TextArray->Num() > 0)
						{
							// Text is often an array of content blocks
							for (const auto& TextVal : *TextArray)
							{
								const TSharedPtr<FJsonObject> TextObj = TextVal->AsObject();
								if (TextObj.IsValid())
								{
									FString TextContent;
									if (TextObj->TryGetStringField(TEXT("text"), TextContent) && !TextContent.IsEmpty())
									{
										Snippet = TextContent.Left(150);
										if (TextContent.Len() > 150) Snippet += TEXT("...");
										break;
									}
								}
							}
						}
						// Also try "content" arrays (Responses API blocks), then direct fields
						if (Snippet.IsEmpty())
						{
							const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
							if (ResultObj->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray && ContentArray->Num() > 0)
							{
								for (const auto& ContentVal : *ContentArray)
								{
									if (!ContentVal.IsValid())
									{
										continue;
									}

									FString ContentText;
									if (ContentVal->Type == EJson::String && ContentVal->TryGetString(ContentText) && !ContentText.IsEmpty())
									{
										Snippet = ContentText;
									}
									else
									{
										const TSharedPtr<FJsonObject> ContentObj = ContentVal->AsObject();
										if (ContentObj.IsValid())
										{
											if (ContentObj->TryGetStringField(TEXT("text"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
											else if (ContentObj->TryGetStringField(TEXT("content"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
											else if (ContentObj->TryGetStringField(TEXT("value"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
										}
									}

									if (!Snippet.IsEmpty())
									{
										break;
									}
								}
							}

							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("text"), Snippet);
							}
							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("content"), Snippet);
							}
							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("snippet"), Snippet);
							}
							if (!Snippet.IsEmpty() && Snippet.Len() > 150)
							{
								Snippet = Snippet.Left(150) + TEXT("...");
							}
						}

						// Optional relevance score if present
						double ScoreValue = 0.0;
						const bool bHasScore = ResultObj->TryGetNumberField(TEXT("score"), ScoreValue);
						const FString ScoreSuffix = bHasScore
							? FString::Printf(TEXT(" (score %.3f)"), ScoreValue)
							: TEXT("");

						ResultSummary += FString::Printf(TEXT("%d. %s%s\n"), ResultIdx + 1,
							FileName.IsEmpty() ? TEXT("(unnamed)") : *FileName,
							*ScoreSuffix);
						if (!Snippet.IsEmpty())
						{
							ResultSummary += FString::Printf(TEXT("   %s\n"), *Snippet);
						}
						ResultSummary += TEXT("\n");
					}

					if (ResultCount > MaxToShow)
					{
						ResultSummary += FString::Printf(TEXT("... and %d more result(s)"), ResultCount - MaxToShow);
					}
				}
				else
				{
					ResultSummary = TEXT("No results found.");
				}
			}

			// Check status field (at top level)
			FString Status;
			OutputObj->TryGetStringField(TEXT("status"), Status);

			FString ArgsString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsString);
			FJsonSerializer::Serialize(ArgsJson.ToSharedRef(), Writer);

			// Broadcast tool call to UI
			if (IsInGameThread())
			{
				OnToolCall.Broadcast(ToolName, ArgsString);
				// Also broadcast results if we have them
				if (!ResultSummary.IsEmpty())
				{
					OnToolResult.Broadcast(CallId, ResultSummary);
				}
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [this, ToolName, ArgsString, CallId, ResultSummary]()
				{
					OnToolCall.Broadcast(ToolName, ArgsString);
					if (!ResultSummary.IsEmpty())
					{
						OnToolResult.Broadcast(CallId, ResultSummary);
					}
				});
			}

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Server-side %s - status: %s, results: %d"), *ToolName, *Status, ResultCount);
			// NOTE: NOT adding to OutToolCalls - these are server-side only
		}
		else if (OutputType == TEXT("message"))
		{
			// Message output with content array
			const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
			if (!OutputObj->TryGetArrayField(TEXT("content"), ContentArray) || !ContentArray)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
			{
				TSharedPtr<FJsonObject> ContentObj = ContentValue.IsValid() ? ContentValue->AsObject() : nullptr;
				if (!ContentObj.IsValid())
				{
					continue;
				}

				FString ContentType;
				ContentObj->TryGetStringField(TEXT("type"), ContentType);

				if (ContentType == TEXT("output_text") || ContentType == TEXT("text"))
				{
					FString TextChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), TextChunk))
					{
						OutAccumulatedText += TextChunk;
					}
				}
				else if (ContentType == TEXT("reasoning") || ContentType == TEXT("thought"))
				{
					FString ReasoningChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), ReasoningChunk))
					{
						OnAgentReasoning.Broadcast(ReasoningChunk);
					}
				}
				else if (ContentType == TEXT("tool_call"))
				{
					const TSharedPtr<FJsonObject>* ToolCallObjPtr = nullptr;
					if (ContentObj->TryGetObjectField(TEXT("tool_call"), ToolCallObjPtr) && ToolCallObjPtr && ToolCallObjPtr->IsValid())
					{
						FToolCallInfo Info;
						(*ToolCallObjPtr)->TryGetStringField(TEXT("id"), Info.Id);

						const TSharedPtr<FJsonObject>* FuncObjPtr = nullptr;
						if ((*ToolCallObjPtr)->TryGetObjectField(TEXT("function"), FuncObjPtr) && FuncObjPtr && FuncObjPtr->IsValid())
						{
							(*FuncObjPtr)->TryGetStringField(TEXT("name"), Info.Name);
							(*FuncObjPtr)->TryGetStringField(TEXT("arguments"), Info.Arguments);
						}

						if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
						{
							OutToolCalls.Add(MoveTemp(Info));
						}
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Extracted %d tool calls, %d chars of text"), OutToolCalls.Num(), OutAccumulatedText.Len());
}

void UUnrealGPTAgentClient::ProcessExtractedToolCalls(const TArray<FToolCallInfo>& ToolCalls, const FString& AccumulatedText)
{
	// Serialize tool_calls array for history
	TArray<TSharedPtr<FJsonValue>> ToolCallsJsonArray;
	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		TSharedPtr<FJsonObject> ToolCallJson = MakeShareable(new FJsonObject);
		ToolCallJson->SetStringField(TEXT("id"), CallInfo.Id);
		ToolCallJson->SetStringField(TEXT("type"), TEXT("function"));

		TSharedPtr<FJsonObject> FunctionJson = MakeShareable(new FJsonObject);
		FunctionJson->SetStringField(TEXT("name"), CallInfo.Name);
		FunctionJson->SetStringField(TEXT("arguments"), CallInfo.Arguments);
		ToolCallJson->SetObjectField(TEXT("function"), FunctionJson);

		ToolCallsJsonArray.Add(MakeShareable(new FJsonValueObject(ToolCallJson)));
	}

	FString ToolCallsJsonString;
	TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
	FJsonSerializer::Serialize(ToolCallsJsonArray, ToolCallsWriter);

	// Add assistant message to history
	FAgentMessage AssistantMsg;
	AssistantMsg.Role = TEXT("assistant");
	AssistantMsg.Content = AccumulatedText;
	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		AssistantMsg.ToolCallIds.Add(CallInfo.Id);
	}
	AssistantMsg.ToolCallsJson = ToolCallsJsonString;
	ConversationHistory.Add(AssistantMsg);

	// Save assistant message to session for persistence
	SaveAssistantMessageToSession(AccumulatedText, AssistantMsg.ToolCallIds, ToolCallsJsonString);

	// Broadcast to UI
	if (!AccumulatedText.IsEmpty())
	{
		OnAgentMessage.Broadcast(TEXT("assistant"), AccumulatedText, AssistantMsg.ToolCallIds);
	}
	else
	{
		OnAgentMessage.Broadcast(TEXT("assistant"), TEXT("Executing tools..."), AssistantMsg.ToolCallIds);
	}

	// Helper lambda for server-side tools
	auto IsServerSideTool = [](const FString& Name) -> bool
	{
		return Name == TEXT("file_search") || Name == TEXT("web_search");
	};

	bool bHasClientSideTools = false;
	bool bHasAsyncReplicateTools = false;
	TArray<FString> ScreenshotImages;

	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		const bool bIsScreenshot = (CallInfo.Name == TEXT("viewport_screenshot"));
		const bool bIsServerSideTool = IsServerSideTool(CallInfo.Name);
		const bool bIsAsyncReplicateTool = (CallInfo.Name == TEXT("replicate_generate"));

		if (!bIsServerSideTool)
		{
			bHasClientSideTools = true;
		}

		// Async Replicate execution
		if (bIsAsyncReplicateTool)
		{
			bHasAsyncReplicateTools = true;
			const FString ToolNameCopy = CallInfo.Name;
			const FString ArgsCopy = CallInfo.Arguments;
			const FString CallIdCopy = CallInfo.Id;
			const int32 MaxToolResultSizeLocal = MaxToolResultSize;

			Async(EAsyncExecution::ThreadPool, [this, ToolNameCopy, ArgsCopy, CallIdCopy, MaxToolResultSizeLocal]()
			{
				const FString ToolResult = ExecuteToolCall(ToolNameCopy, ArgsCopy);

				FString ToolResultForHistory = ToolResult;
				if (ToolResultForHistory.Len() > MaxToolResultSizeLocal)
				{
					ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSizeLocal) +
						TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) +
						TEXT(" characters.]");
				}

				AsyncTask(ENamedThreads::GameThread, [this, ToolNameCopy, ArgsCopy, CallIdCopy, ToolResult, ToolResultForHistory]()
				{
					FAgentMessage ToolMsg;
					ToolMsg.Role = TEXT("tool");
					ToolMsg.ToolCallId = CallIdCopy;
					ToolMsg.Content = ToolResultForHistory;
					ConversationHistory.Add(ToolMsg);

					// Save tool message and tool call to session for persistence
					TArray<FString> ToolImages = ExtractImagesFromToolResult(ToolResult);
					SaveToolMessageToSession(CallIdCopy, ToolResultForHistory, ToolImages);
					SaveToolCallToSession(ToolNameCopy, ArgsCopy, ToolResult);

					OnToolResult.Broadcast(CallIdCopy, ToolResult);
					SendMessage(TEXT(""), TArray<FString>());
				});
			});
			continue;
		}

		// Synchronous execution
		FString ToolResult = ExecuteToolCall(CallInfo.Name, CallInfo.Arguments);
		FString ToolResultForHistory = ToolResult;

		// Handle screenshot image extraction
		if (bIsScreenshot && !ToolResult.IsEmpty())
		{
			const FString ImageSeparator = TEXT("\n__IMAGE_BASE64__\n");
			int32 SeparatorIndex = ToolResult.Find(ImageSeparator);
			if (SeparatorIndex != INDEX_NONE)
			{
				FString MetadataJson = ToolResult.Left(SeparatorIndex);
				FString ImageBase64 = ToolResult.Mid(SeparatorIndex + ImageSeparator.Len());
				if (!ImageBase64.IsEmpty())
				{
					ScreenshotImages.Add(ImageBase64);
				}
				ToolResultForHistory = MetadataJson;
			}
			else
			{
				ScreenshotImages.Add(ToolResult);
			}
		}
		else if (ToolResultForHistory.Len() > MaxToolResultSize)
		{
			ToolResultForHistory = ToolResultForHistory.Left(MaxToolResultSize) +
				TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) +
				TEXT(" characters.]");
		}

		FAgentMessage ToolMsg;
		ToolMsg.Role = TEXT("tool");
		ToolMsg.ToolCallId = CallInfo.Id;
		ToolMsg.Content = ToolResultForHistory;
		ConversationHistory.Add(ToolMsg);

		// Save tool message and tool call to session for persistence
		TArray<FString> ToolImages = ExtractImagesFromToolResult(ToolResult);
		SaveToolMessageToSession(CallInfo.Id, ToolResultForHistory, ToolImages);
		SaveToolCallToSession(CallInfo.Name, CallInfo.Arguments, ToolResult);

		OnToolResult.Broadcast(CallInfo.Id, ToolResult);
	}

	if (!bHasClientSideTools)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: All tools were server-side. Waiting for continuation."));
		ToolCallIterationCount = 0;
		return;
	}

	if (bHasAsyncReplicateTools)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Async tools scheduled; waiting for completion."));
		return;
	}

	// Check max iterations
	const int32 MaxIterations = Settings ? Settings->MaxToolCallIterations : 100;
	if (MaxIterations > 0 && ToolCallIterationCount >= MaxIterations - 1)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Reached max tool call iterations (%d)."), MaxIterations);

		FAgentMessage LimitEntry;
		LimitEntry.Role = TEXT("assistant");
		LimitEntry.Content = FString::Printf(TEXT("[Tool call limit reached after %d iterations.]"), MaxIterations);
		ConversationHistory.Add(LimitEntry);
		OnAgentMessage.Broadcast(TEXT("assistant"), LimitEntry.Content, TArray<FString>());

		ToolCallIterationCount = 0;
		bRequestInProgress = false;
		return;
	}

	// Continue conversation with tool results
	SendMessage(TEXT(""), ScreenshotImages);
}

FString UUnrealGPTAgentClient::ExecuteToolCall(const FString& ToolName, const FString& ArgumentsJson)
{
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: ExecuteToolCall ENTRY - Tool: %s, Args length: %d"), *ToolName, ArgumentsJson.Len());

	FString Result;

	const bool bIsPythonExecute = (ToolName == TEXT("python_execute"));
	const bool bIsSceneQuery = (ToolName == TEXT("scene_query"));

	if (bIsPythonExecute)
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			FString Code;
			if (ArgsObj->TryGetStringField(TEXT("code"), Code))
			{
				Result = UUnrealGPTToolExecutor::ExecutePythonCode(Code);
			}
		}
	}
	else if (ToolName == TEXT("viewport_screenshot"))
	{
		FString MetadataJson;
		FString ImageBase64 = UUnrealGPTToolExecutor::GetViewportScreenshot(ArgumentsJson, MetadataJson);

		// The image will be sent as multimodal input separately.
		// Return the metadata JSON as the tool result so the model has context.
		// The image base64 is stored separately for multimodal handling.
		if (!ImageBase64.IsEmpty())
		{
			// Store the base64 image for later multimodal use (handled in caller)
			// Return metadata + a marker that indicates the image was captured
			Result = MetadataJson;
			// Append the base64 image after a separator so caller can extract it
			Result += TEXT("\n__IMAGE_BASE64__\n") + ImageBase64;
		}
		else
		{
			Result = MetadataJson; // Will contain error info
		}
	}
	else if (ToolName == TEXT("scene_query"))
	{
		// Pass the raw JSON arguments through to the scene query helper.
		// The JSON can specify filters like class_contains, label_contains,
		// name_contains, component_class_contains, and max_results.
		Result = UUnrealGPTSceneContext::QueryScene(ArgumentsJson);

		// Check if scene_query returned results (non-empty JSON array).
		// If it did, mark that we found results so we can block subsequent python_execute calls.
		// scene_query returns a JSON array string, so check if it's not just "[]"
		bLastSceneQueryFoundResults = !Result.IsEmpty() && Result != TEXT("[]") && Result.StartsWith(TEXT("["));
		if (bLastSceneQueryFoundResults)
		{
			// Try to parse and verify it's actually a non-empty array
			TSharedPtr<FJsonValue> JsonValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
			if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
				if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray))
				{
					bLastSceneQueryFoundResults = (JsonArray->Num() > 0);
					if (bLastSceneQueryFoundResults)
					{
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: scene_query found %d results - will block subsequent python_execute"), JsonArray->Num());
					}
				}
				else
				{
					bLastSceneQueryFoundResults = false;
				}
			}
			else
			{
				bLastSceneQueryFoundResults = false;
			}
		}
	}
	else if (ToolName == TEXT("reflection_query"))
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse reflection_query arguments\"}");
		}

		FString ClassName;
		if (!ArgsObj->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: class_name\"}");
		}

		// Try to resolve the class by short name first, then by treating the
		// input as a fully-qualified object path.
		UClass* TargetClass = FindObject<UClass>(nullptr, *ClassName);
		if (!TargetClass)
		{
			TargetClass = LoadObject<UClass>(nullptr, *ClassName);
		}

		Result = BuildReflectionSchemaJson(TargetClass);
	}
	else if (ToolName == TEXT("replicate_generate"))
	{
		// Delegate to dedicated Replicate client (extracted for code clarity)
		Result = UUnrealGPTReplicateClient::Generate(ArgumentsJson);
	}
	else if (ToolName == TEXT("file_search") || ToolName == TEXT("web_search"))
	{
		// These are server-side tools executed by the model/platform.
		// We just acknowledge them to avoid "Unknown tool" errors and allow the loop to continue.
		// The actual results are typically incorporated into the model's context or subsequent messages.
		Result = FString::Printf(TEXT("Tool '%s' executed successfully by server."), *ToolName);
	}
	// ==================== ATOMIC EDITOR TOOLS (delegated to ToolExecutor) ====================
	else if (ToolName == TEXT("get_actor"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteGetActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSetActorTransform(ArgumentsJson);
	}
	else if (ToolName == TEXT("select_actors"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSelectActors(ArgumentsJson);
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteDuplicateActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("snap_actor_to_ground"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSnapActorToGround(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actors_rotation"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSetActorsRotation(ArgumentsJson);
	}
	else
	{
		Result = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
	}

	// Track last tool type so we can avoid repeated python_execute runs.
	bLastToolWasPythonExecute = bIsPythonExecute;

	// Reset scene_query results flag if we're running a different tool (not scene_query)
	if (!bIsSceneQuery)
	{
		bLastSceneQueryFoundResults = false;
	}

	// Ensure OnToolCall delegate (which can touch Slate/UI) is always broadcast on the game thread.
	if (IsInGameThread())
	{
		OnToolCall.Broadcast(ToolName, ArgumentsJson);
	}
	else
	{
		const FString ToolNameCopy = ToolName;
		const FString ArgsCopy = ArgumentsJson;
		AsyncTask(ENamedThreads::GameThread, [this, ToolNameCopy, ArgsCopy]()
		{
			OnToolCall.Broadcast(ToolNameCopy, ArgsCopy);
		});
	}

	return Result;
}

TSharedRef<IHttpRequest> UUnrealGPTAgentClient::CreateHttpRequest()
{
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();

	// Apply per-request timeout from settings if configured
	if (UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>())
	{
		if (SafeSettings->ExecutionTimeoutSeconds > 0.0f)
		{
			Request->SetTimeout(SafeSettings->ExecutionTimeoutSeconds);
			Request->SetActivityTimeout(SafeSettings->ExecutionTimeoutSeconds);
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: CreateHttpRequest - Set timeout to %.1f seconds (request + activity)"), SafeSettings->ExecutionTimeoutSeconds);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: CreateHttpRequest - ExecutionTimeoutSeconds is <= 0 (%.1f), using default timeout"), SafeSettings->ExecutionTimeoutSeconds);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: CreateHttpRequest - Could not get settings, using default timeout"));
	}

	return Request;
}

FString UUnrealGPTAgentClient::GetEffectiveApiUrl() const
{
	// Always get a fresh reference to settings to avoid accessing invalid cached pointers
	// Settings can become invalid if the object is garbage collected
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || !IsValid(SafeSettings))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null or invalid and could not be retrieved"));
		return TEXT("https://api.openai.com/v1/responses"); // Default fallback
	}
	
	// Build effective URL based on BaseUrlOverride and ApiEndpoint
	FString BaseUrl = SafeSettings->BaseUrlOverride;
	FString ApiEndpoint = SafeSettings->ApiEndpoint;

	// If no override is set, use ApiEndpoint as-is (caller should provide full URL)
	if (BaseUrl.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (no override): %s"), *ApiEndpoint);
		return ApiEndpoint;
	}

	// Normalize base URL (remove trailing slash)
	if (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.RemoveAt(BaseUrl.Len() - 1);
	}

	// If ApiEndpoint is empty, just use the base URL
	if (ApiEndpoint.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override only): %s"), *BaseUrl);
		return BaseUrl;
	}

	// If ApiEndpoint is a full URL, extract its path portion and append to BaseUrl
	int32 ProtocolIndex = ApiEndpoint.Find(TEXT("://"));
	if (ProtocolIndex != INDEX_NONE)
	{
		int32 PathStartIndex = ApiEndpoint.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ProtocolIndex + 3);
		if (PathStartIndex != INDEX_NONE && PathStartIndex < ApiEndpoint.Len())
		{
			FString Path = ApiEndpoint.Mid(PathStartIndex);
			if (!Path.StartsWith(TEXT("/")))
			{
				Path = TEXT("/") + Path;
			}

			const FString EffectiveUrl = BaseUrl + Path;
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + parsed path): %s"), *EffectiveUrl);
			return EffectiveUrl;
		}
	}

	// Otherwise treat ApiEndpoint as a path relative to BaseUrl
	FString Path = ApiEndpoint;
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/") + Path;
	}

	const FString EffectiveUrl = BaseUrl + Path;
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + relative path): %s"), *EffectiveUrl);
	return EffectiveUrl;
}

bool UUnrealGPTAgentClient::IsUsingResponsesApi() const
{
	FString ApiUrl = GetEffectiveApiUrl();
	return ApiUrl.Contains(TEXT("/v1/responses"));
}

FString UUnrealGPTAgentClient::DetermineReasoningEffort(const FString& UserMessage, const TArray<FString>& ImagePaths) const
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

	// LOW effort: simple, direct operations
	// Single object manipulation, clear instructions, straightforward tool calls
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning effort = LOW (straightforward operation)"));
	return TEXT("low");
}

bool UUnrealGPTAgentClient::DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults) const
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

// ==================== SESSION PERSISTENCE ====================

bool UUnrealGPTAgentClient::LoadConversation(const FString& SessionId)
{
	if (!SessionManager)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: SessionManager is null, cannot load conversation"));
		return false;
	}

	FSessionData LoadedSession;
	if (!SessionManager->LoadSession(SessionId, LoadedSession))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to load session: %s"), *SessionId);
		return false;
	}

	// Save current session before switching
	if (!ConversationSessionId.IsEmpty())
	{
		SessionManager->SaveCurrentSession();
		SessionManager->EndAutoSave();
	}

	// Clear current state
	ClearHistory();

	// Restore state from loaded session
	ConversationSessionId = SessionId;
	PreviousResponseId = LoadedSession.PreviousResponseId;

	// Rebuild conversation history from persisted messages
	for (const FPersistedMessage& Msg : LoadedSession.Messages)
	{
		FAgentMessage AgentMsg;
		AgentMsg.Role = Msg.Role;
		AgentMsg.Content = Msg.Content;
		AgentMsg.ToolCallIds = Msg.ToolCallIds;
		AgentMsg.ToolCallId = Msg.ToolCallId;
		AgentMsg.ToolCallsJson = Msg.ToolCallsJson;
		ConversationHistory.Add(AgentMsg);
	}

	// Set up session manager to track this session
	// IMPORTANT: BeginAutoSave must be called BEFORE SetCurrentSessionData because
	// BeginAutoSave initializes CurrentSessionData to empty. We then overwrite with loaded data.
	SessionManager->BeginAutoSave(SessionId);
	SessionManager->SetCurrentSessionData(LoadedSession);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Loaded conversation %s with %d messages"), *SessionId, ConversationHistory.Num());

	return true;
}

TArray<FSessionInfo> UUnrealGPTAgentClient::GetSessionList() const
{
	if (SessionManager)
	{
		return SessionManager->GetSessionList();
	}
	return TArray<FSessionInfo>();
}

void UUnrealGPTAgentClient::SaveUserMessageToSession(const FString& UserMessage, const TArray<FString>& Images)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("user");
	Msg.Content = UserMessage;
	Msg.ImageBase64 = Images;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);
}

void UUnrealGPTAgentClient::SaveAssistantMessageToSession(const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("assistant");
	Msg.Content = Content;
	Msg.ToolCallIds = ToolCallIds;
	Msg.ToolCallsJson = ToolCallsJson;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);

	// Trigger save after assistant response
	SessionManager->SaveCurrentSession();
}

void UUnrealGPTAgentClient::SaveToolMessageToSession(const FString& ToolCallId, const FString& Result, const TArray<FString>& Images)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedMessage Msg;
	Msg.Role = TEXT("tool");
	Msg.ToolCallId = ToolCallId;
	Msg.Content = Result;
	Msg.ImageBase64 = Images;
	Msg.Timestamp = FDateTime::Now();

	SessionManager->AppendMessage(Msg);
}

void UUnrealGPTAgentClient::SaveToolCallToSession(const FString& ToolName, const FString& Arguments, const FString& Result)
{
	if (!SessionManager || !SessionManager->IsAutoSaveActive())
	{
		return;
	}

	FPersistedToolCall ToolCall;
	ToolCall.ToolName = ToolName;
	ToolCall.Arguments = Arguments;
	ToolCall.Result = Result;
	ToolCall.Timestamp = FDateTime::Now();

	SessionManager->AppendToolCall(ToolCall);
}

TArray<FString> UUnrealGPTAgentClient::ExtractImagesFromToolResult(const FString& ToolResult) const
{
	TArray<FString> Images;

	// Check for the image separator pattern used by viewport_screenshot
	const FString ImageSeparator = TEXT("\n__IMAGE_BASE64__\n");
	int32 SeparatorIndex = ToolResult.Find(ImageSeparator);

	if (SeparatorIndex != INDEX_NONE)
	{
		FString ImageBase64 = ToolResult.Mid(SeparatorIndex + ImageSeparator.Len());
		if (!ImageBase64.IsEmpty())
		{
			Images.Add(ImageBase64);
		}
	}
	else
	{
		// Check if the entire result is a base64 image (PNG or JPEG header)
		if (ToolResult.StartsWith(TEXT("iVBORw0KGgo")) || ToolResult.StartsWith(TEXT("/9j/")))
		{
			Images.Add(ToolResult);
		}
	}

	return Images;
}

