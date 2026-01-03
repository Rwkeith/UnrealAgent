#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentInstructions.h"
#include "UnrealGPTReplicateClient.h"
#include "UnrealGPTToolSchemas.h"
#include "UnrealGPTToolExecutor.h"
#include "UnrealGPTJsonHelpers.h"
#include "UnrealGPTRequestBuilder.h"
#include "UnrealGPTRequestConfigBuilder.h"
#include "UnrealGPTResponseParser.h"
#include "UnrealGPTConversationState.h"
#include "UnrealGPTToolDispatcher.h"
#include "UnrealGPTToolResultProcessor.h"
#include "UnrealGPTTelemetry.h"
#include "UnrealGPTSessionWriter.h"
#include "UnrealGPTNotifier.h"
#include "UnrealGPTHttpClient.h"
#include "UnrealGPTRetryPolicy.h"
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
					FAgentMessage LimitEntry = UnrealGPTConversationState::CreateAssistantMessage(
						FString::Printf(TEXT("[Tool call limit reached after %d iterations. The conversation will continue from here. You can adjust the limit in Project Settings > UnrealGPT > Safety > Max Tool Call Iterations, or set to 0 for unlimited.]"), MaxIterations));
					UnrealGPTConversationState::AppendMessage(ConversationHistory, LimitEntry);

					// Notify UI that we stopped
					UnrealGPTNotifier::BroadcastAgentMessage(this, LimitEntry.Content, TArray<FString>());

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
		FAgentMessage UserMsg = UnrealGPTConversationState::CreateUserMessage(ProcessedMessage);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, UserMsg);
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
	const bool bUseResponsesApi = IsUsingResponsesApi();

	// Configure reasoning effort dynamically based on task complexity
	// (Responses API + gpt-5/o-series models)
	const FString ReasoningEffort = bUseResponsesApi
		? DetermineReasoningEffort(UserMessage, ImageBase64)
		: FString();

	// High-level behavior instructions for the agent (extracted to separate file for readability)
	const FString EngineVersion = GetEngineVersionString();
	const FString AgentInstructions = UnrealGPTAgentInstructions::GetInstructions(EngineVersion);
	UnrealGPTRequestConfigBuilder::ConfigureRequest(
		RequestJson,
		Settings,
		bUseResponsesApi,
		bAllowReasoningSummary,
		AgentInstructions,
		ReasoningEffort,
		PreviousResponseId);

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
		FConversationContinuation Continuation = UnrealGPTConversationState::BuildResponsesApiContinuation(ConversationHistory, bIsNewUserMessage);
		StartIndex = Continuation.StartIndex;
		ToolResultsToInclude = MoveTemp(Continuation.ToolResultsToInclude);
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
			UnrealGPTRequestBuilder::AppendResponsesApiFunctionCallOutputs(MessagesArray, ToolResultsToInclude, MaxToolResultSize);
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
			UnrealGPTRequestBuilder::AppendResponsesApiImageMessage(
				MessagesArray,
				ImageBase64,
				TEXT("Here is the viewport screenshot you requested. Analyze what you see and describe the scene state."));
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
				UnrealGPTRequestBuilder::SetUserMessageWithImages(MsgObj, Msg.Content, ImageBase64, bUseResponsesApi);
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
				const bool bToolCallsAdded = UnrealGPTRequestBuilder::TryAddToolCallsToAssistantMessage(Msg, MsgObj);
				
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
				if (!UnrealGPTRequestBuilder::CanAppendToolMessage(MessagesArray, i))
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
		UnrealGPTRequestBuilder::AppendLegacyImageMessage(
			MessagesArray,
			ImageBase64,
			TEXT("Here is the viewport screenshot you requested. Please analyze what you see and describe the scene."));
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
	CurrentRequest = UnrealGPTHttpClient::BuildJsonPost(
		CreateHttpRequest(),
		GetEffectiveApiUrl(),
		Settings->ApiKey,
		RequestBody,
		this);

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
					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					const FString ApiKey = SafeSettings ? SafeSettings->ApiKey : FString();

					TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
						CreateHttpRequest(),
						GetEffectiveApiUrl(),
						ApiKey,
						LastRequestBody,
						this);

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

			float RetryDelaySeconds = UnrealGPTRetryPolicy::ParseRetryDelaySeconds(ErrorBody);

			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Rate limited (429). Retry %d/%d in %.2f seconds..."), RateLimitRetryCount, MaxRateLimitRetries, RetryDelaySeconds);

			FTimerHandle RateLimitRetryHandle;
			GEditor->GetTimerManager()->SetTimer(RateLimitRetryHandle, [this]()
			{
				if (!LastRequestBody.IsEmpty())
				{
					UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
					const FString ApiKey = SafeSettings ? SafeSettings->ApiKey : FString();

					TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
						CreateHttpRequest(),
						GetEffectiveApiUrl(),
						ApiKey,
						LastRequestBody,
						this);

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
										TSharedRef<IHttpRequest> RetryRequest = UnrealGPTHttpClient::BuildJsonPost(
											CreateHttpRequest(),
											GetEffectiveApiUrl(),
											Settings->ApiKey,
											NewBody,
											this);

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
	UnrealGPTTelemetry::LogApiConversation(ConversationSessionId, TEXT("request"), LastRequestBody);
	UnrealGPTTelemetry::LogApiConversation(ConversationSessionId, TEXT("response"), ResponseBody, ResponseCode);

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
	FStreamingParseResult ParseResult;
	UnrealGPTResponseParser::ParseChatCompletionsSse(ResponseContent, ParseResult);

	if (ParseResult.FinishReason == TEXT("tool_calls") && !ParseResult.ToolCallId.IsEmpty())
	{
		// Build tool_calls JSON for assistant message
		TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject);
		ToolCallObj->SetStringField(TEXT("id"), ParseResult.ToolCallId);
		ToolCallObj->SetStringField(TEXT("type"), TEXT("function"));

		TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
		FunctionObj->SetStringField(TEXT("name"), ParseResult.ToolName);
		FunctionObj->SetStringField(TEXT("arguments"), ParseResult.ToolArguments);
		ToolCallObj->SetObjectField(TEXT("function"), FunctionObj);

		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));

		FString ToolCallsJsonString;
		TSharedRef<TJsonWriter<>> ToolCallsWriter = TJsonWriterFactory<>::Create(&ToolCallsJsonString);
		const bool bSerialized = FJsonSerializer::Serialize(ToolCallsArray, ToolCallsWriter);

		if (!bSerialized || ToolCallsJsonString.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to serialize tool_calls array"));
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Serialized tool_calls: %s"), *ToolCallsJsonString);
		}

		// Add assistant message with tool_calls to history FIRST
		FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantToolCallMessage(
			ParseResult.AccumulatedContent,
			TArray<FString>{ParseResult.ToolCallId},
			ToolCallsJsonString);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, AssistantMsg);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Added assistant message with tool_calls to history. History size: %d"), ConversationHistory.Num());

		UnrealGPTNotifier::BroadcastAgentMessage(this, ParseResult.AccumulatedContent, TArray<FString>{ParseResult.ToolCallId});

		// Execute tool call
		FString ToolResult = ExecuteToolCall(ParseResult.ToolName, ParseResult.ToolArguments);

		FProcessedToolResult ProcessedToolResult =
			UnrealGPTToolResultProcessor::ProcessResult(ParseResult.ToolName, ToolResult, MaxToolResultSize);

		// Add tool result to conversation (truncated version)
		FAgentMessage ToolMsg = UnrealGPTConversationState::CreateToolMessage(ProcessedToolResult.ResultForHistory, ParseResult.ToolCallId);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, ToolMsg);

		UnrealGPTNotifier::BroadcastToolResult(this, ParseResult.ToolCallId, ToolResult);

		// Continue conversation with tool result.
		// If this was a viewport_screenshot call, also forward the image as multimodal input
		// so the model can analyze the actual viewport image (not just a text summary).
		SendMessage(TEXT(""), ProcessedToolResult.Images);
	}
	else if (!ParseResult.AccumulatedContent.IsEmpty())
	{
		// Add assistant message to history
		FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantMessage(ParseResult.AccumulatedContent);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, AssistantMsg);

		UnrealGPTNotifier::BroadcastAgentMessage(this, ParseResult.AccumulatedContent, TArray<FString>());
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
			UnrealGPTNotifier::BroadcastAgentReasoning(this, ReasoningSummary);
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
	FResponseParseResult ParseResult;
	UnrealGPTResponseParser::ExtractFromResponseOutput(*OutputArray, ParseResult);

	if (ParseResult.ReasoningChunks.Num() > 0)
	{
		for (const FString& ReasoningChunk : ParseResult.ReasoningChunks)
		{
			if (!ReasoningChunk.IsEmpty())
			{
				UnrealGPTNotifier::BroadcastAgentReasoning(this, ReasoningChunk);
			}
		}
	}

	if (ParseResult.ServerSideToolCalls.Num() > 0)
	{
		for (const FServerSideToolCall& ServerSideCall : ParseResult.ServerSideToolCalls)
		{
			UnrealGPTNotifier::BroadcastToolCall(this, ServerSideCall.ToolName, ServerSideCall.ArgsJson);
			if (!ServerSideCall.ResultSummary.IsEmpty())
			{
				UnrealGPTNotifier::BroadcastToolResult(this, ServerSideCall.CallId, ServerSideCall.ResultSummary);
			}
		}
	}

	// Process the extracted tool calls and/or text
	if (ParseResult.ToolCalls.Num() > 0)
	{
		ProcessExtractedToolCalls(ParseResult.ToolCalls, ParseResult.AccumulatedText);
		return;
	}

	// No tool calls - just process the text response
	if (!ParseResult.AccumulatedText.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Processing regular assistant message (no tool calls)"));
		FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantMessage(ParseResult.AccumulatedText);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, AssistantMsg);

		UnrealGPTNotifier::BroadcastAgentMessage(this, ParseResult.AccumulatedText, TArray<FString>());
		ToolCallIterationCount = 0;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Message output had no text content and no tool calls"));
		ToolCallIterationCount = 0;
	}
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
	TArray<FString> ToolCallIds;
	ToolCallIds.Reserve(ToolCalls.Num());
	for (const FToolCallInfo& CallInfo : ToolCalls)
	{
		ToolCallIds.Add(CallInfo.Id);
	}

	FAgentMessage AssistantMsg = UnrealGPTConversationState::CreateAssistantToolCallMessage(
		AccumulatedText,
		ToolCallIds,
		ToolCallsJsonString);
	UnrealGPTConversationState::AppendMessage(ConversationHistory, AssistantMsg);

	// Save assistant message to session for persistence
	SaveAssistantMessageToSession(AccumulatedText, ToolCallIds, ToolCallsJsonString);

	// Broadcast to UI
	if (!AccumulatedText.IsEmpty())
	{
		UnrealGPTNotifier::BroadcastAgentMessage(this, AccumulatedText, ToolCallIds);
	}
	else
	{
		UnrealGPTNotifier::BroadcastAgentMessage(this, TEXT("Executing tools..."), ToolCallIds);
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

				FProcessedToolResult ProcessedToolResult =
					UnrealGPTToolResultProcessor::ProcessResult(ToolNameCopy, ToolResult, MaxToolResultSizeLocal);

				AsyncTask(ENamedThreads::GameThread, [this, ToolNameCopy, ArgsCopy, CallIdCopy, ToolResult, ProcessedToolResult]()
				{
					FAgentMessage ToolMsg = UnrealGPTConversationState::CreateToolMessage(ProcessedToolResult.ResultForHistory, CallIdCopy);
					UnrealGPTConversationState::AppendMessage(ConversationHistory, ToolMsg);

					// Save tool message and tool call to session for persistence
					SaveToolMessageToSession(CallIdCopy, ProcessedToolResult.ResultForHistory, ProcessedToolResult.Images);
					SaveToolCallToSession(ToolNameCopy, ArgsCopy, ToolResult);

					UnrealGPTNotifier::BroadcastToolResult(this, CallIdCopy, ToolResult);
					SendMessage(TEXT(""), TArray<FString>());
				});
			});
			continue;
		}

		// Synchronous execution
		FString ToolResult = ExecuteToolCall(CallInfo.Name, CallInfo.Arguments);
		FProcessedToolResult ProcessedToolResult =
			UnrealGPTToolResultProcessor::ProcessResult(CallInfo.Name, ToolResult, MaxToolResultSize);
		ScreenshotImages.Append(ProcessedToolResult.Images);

		FAgentMessage ToolMsg = UnrealGPTConversationState::CreateToolMessage(ProcessedToolResult.ResultForHistory, CallInfo.Id);
		UnrealGPTConversationState::AppendMessage(ConversationHistory, ToolMsg);

		// Save tool message and tool call to session for persistence
		SaveToolMessageToSession(CallInfo.Id, ProcessedToolResult.ResultForHistory, ProcessedToolResult.Images);
		SaveToolCallToSession(CallInfo.Name, CallInfo.Arguments, ToolResult);

		UnrealGPTNotifier::BroadcastToolResult(this, CallInfo.Id, ToolResult);
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

		FAgentMessage LimitEntry = UnrealGPTConversationState::CreateAssistantMessage(
			FString::Printf(TEXT("[Tool call limit reached after %d iterations.]"), MaxIterations));
		UnrealGPTConversationState::AppendMessage(ConversationHistory, LimitEntry);
		UnrealGPTNotifier::BroadcastAgentMessage(this, LimitEntry.Content, TArray<FString>());

		ToolCallIterationCount = 0;
		bRequestInProgress = false;
		return;
	}

	// Continue conversation with tool results
	SendMessage(TEXT(""), ScreenshotImages);
}

FString UUnrealGPTAgentClient::ExecuteToolCall(const FString& ToolName, const FString& ArgumentsJson)
{
	return UnrealGPTToolDispatcher::ExecuteToolCall(
		ToolName,
		ArgumentsJson,
		bLastToolWasPythonExecute,
		bLastSceneQueryFoundResults,
		[this](const FString& ToolNameInner, const FString& ArgumentsJsonInner)
		{
			UnrealGPTNotifier::BroadcastToolCall(this, ToolNameInner, ArgumentsJsonInner);
		});
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

	// MEDIUM as baseline: this agent always involves tool execution (Python code, atomic tools)
	// which benefits from additional reasoning to avoid mistakes in code generation.
	// LOW would only be appropriate for pure Q&A without tool use, which this agent doesn't do.
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Reasoning effort = MEDIUM (baseline for tool-using agent)"));
	return TEXT("medium");
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
		UnrealGPTConversationState::AppendMessage(ConversationHistory, AgentMsg);
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
	UnrealGPTSessionWriter::SaveUserMessage(SessionManager, UserMessage, Images);
}

void UUnrealGPTAgentClient::SaveAssistantMessageToSession(const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson)
{
	UnrealGPTSessionWriter::SaveAssistantMessageAndFlush(SessionManager, Content, ToolCallIds, ToolCallsJson);
}

void UUnrealGPTAgentClient::SaveToolMessageToSession(const FString& ToolCallId, const FString& Result, const TArray<FString>& Images)
{
	UnrealGPTSessionWriter::SaveToolMessage(SessionManager, ToolCallId, Result, Images);
}

void UUnrealGPTAgentClient::SaveToolCallToSession(const FString& ToolName, const FString& Arguments, const FString& Result)
{
	UnrealGPTSessionWriter::SaveToolCall(SessionManager, ToolName, Arguments, Result);
}

