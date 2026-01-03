#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTAgentInstructions.h"
#include "UnrealGPTAgentPolicy.h"
#include "UnrealGPTRequestConfigBuilder.h"
#include "UnrealGPTRequestPayloadBuilder.h"
#include "UnrealGPTRequestValidator.h"
#include "UnrealGPTResponseProcessor.h"
#include "UnrealGPTConversationRecorder.h"
#include "UnrealGPTConversationCatalog.h"
#include "UnrealGPTConversationLoader.h"
#include "UnrealGPTTelemetry.h"
#include "UnrealGPTSessionWriter.h"
#include "UnrealGPTResponseHandler.h"
#include "UnrealGPTRequestSender.h"
#include "UnrealGPTSessionManager.h"
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

	const FString ProcessedMessage = UnrealGPTRequestValidator::SanitizeUserMessage(UserMessage);
	UnrealGPTRequestValidator::LogImagePayloadSize(ImageBase64);

	const bool bIsNewUserMessage = !UserMessage.IsEmpty();
	if (!UnrealGPTAgentPolicy::HandleToolCallIteration(
		this,
		Settings,
		bIsNewUserMessage,
		ConversationHistory,
		PreviousResponseId,
		ToolCallIterationCount,
		bRequestInProgress))
	{
		return;
	}

	// Add user message to history only if not empty (empty means continuing after tool call)
	// CRITICAL: Do NOT add empty user messages - this breaks tool continuation
	// Use ProcessedMessage which may have been truncated for size limits
	if (!ProcessedMessage.IsEmpty())
	{
		UnrealGPTConversationRecorder::RecordUserMessage(
			ConversationHistory,
			SessionManager,
			ProcessedMessage,
			ImageBase64);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Empty user message - this is a tool continuation, NOT adding to history"));
	}

	// Configure reasoning effort dynamically based on task complexity
	const FString ReasoningEffort = UnrealGPTRequestConfigBuilder::DetermineReasoningEffort(UserMessage, ImageBase64);

	// High-level behavior instructions for the agent (extracted to separate file for readability)
	const FString EngineVersion = GetEngineVersionString();
	const FString AgentInstructions = UnrealGPTAgentInstructions::GetInstructions(EngineVersion);
	const FString RequestBody = UnrealGPTRequestPayloadBuilder::BuildRequestBody(
		Settings,
		bAllowReasoningSummary,
		AgentInstructions,
		ReasoningEffort,
		PreviousResponseId,
		ConversationHistory,
		ImageBase64,
		bIsNewUserMessage,
		MaxToolResultSize);

	UnrealGPTTelemetry::LogRequestBodySummary(RequestBody);

	UnrealGPTRequestSender::SendRequest(this, RequestBody);
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

void UUnrealGPTAgentClient::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	UnrealGPTResponseHandler::HandleResponse(this, Request, Response, bWasSuccessful);
}

void UUnrealGPTAgentClient::HandleResponsePayload(const FString& ResponseContent)
{
	UnrealGPTResponseProcessor::HandleResponsePayload(this, ResponseContent);
}

// ==================== SESSION PERSISTENCE ====================

bool UUnrealGPTAgentClient::LoadConversation(const FString& SessionId)
{
	ClearHistory();
	return UnrealGPTConversationLoader::LoadConversation(
		SessionManager,
		SessionId,
		ConversationSessionId,
		PreviousResponseId,
		ConversationHistory);
}

TArray<FSessionInfo> UUnrealGPTAgentClient::GetSessionList() const
{
	return UnrealGPTConversationCatalog::GetSessionList(SessionManager);
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

