#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Http.h"
#include "Dom/JsonObject.h"
#include "UnrealGPTSessionTypes.h"
#include "UnrealGPTToolCallTypes.h"
#include "UnrealGPTAgentClient.generated.h"

// Forward declarations
class UUnrealGPTSessionManager;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnAgentMessage, const FString&, Role, const FString&, Content, const TArray<FString>&, ToolCalls);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAgentReasoning, const FString&, ReasoningContent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnToolCall, const FString&, ToolName, const FString&, Arguments);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnToolResult, const FString&, ToolCallId, const FString&, Result);

USTRUCT()
struct FAgentMessage
{
	GENERATED_BODY()

	UPROPERTY()
	FString Role; // "user", "assistant", "system", "tool"

	UPROPERTY()
	FString Content;

	UPROPERTY()
	TArray<FString> ToolCallIds; // For assistant messages with tool_calls, stores the tool call IDs.

	UPROPERTY()
	FString ToolCallId; // For tool messages, the specific tool_call_id

	UPROPERTY()
	FString ToolCallsJson; // For assistant messages, stores the tool_calls array as JSON string
};

USTRUCT()
struct FToolDefinition
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString ParametersSchema; // JSON schema as string
};

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTAgentClient : public UObject
{
	GENERATED_BODY()

	friend class UnrealGPTHttpClient;

public:
	UUnrealGPTAgentClient();

	/** Initialize the agent client with settings */
	void Initialize();

	/** Send a message to the agent and get response */
	void SendMessage(const FString& UserMessage, const TArray<FString>& ImageBase64 = TArray<FString>());

	/** Cancel current request */
	void CancelRequest();

	/** Get conversation history */
	TArray<FAgentMessage> GetConversationHistory() const { return ConversationHistory; }

	/** Clear conversation history */
	void ClearHistory();

	/** Start a new conversation - closes current log file and creates a new session */
	void StartNewConversation();

	// ==================== SESSION MANAGEMENT ====================

	/** Load a previous conversation session */
	bool LoadConversation(const FString& SessionId);

	/** Get list of available sessions */
	TArray<FSessionInfo> GetSessionList() const;

	/** Get the session manager */
	UUnrealGPTSessionManager* GetSessionManager() const { return SessionManager; }

	/** Get current session ID */
	FString GetCurrentSessionId() const { return ConversationSessionId; }

	/** Delegate for agent messages */
	UPROPERTY(BlueprintAssignable)
	FOnAgentMessage OnAgentMessage;

	/** Delegate for reasoning updates */
	UPROPERTY(BlueprintAssignable)
	FOnAgentReasoning OnAgentReasoning;

	/** Delegate for tool calls */
	UPROPERTY(BlueprintAssignable)
	FOnToolCall OnToolCall;

	/** Delegate for tool results */
	UPROPERTY(BlueprintAssignable)
	FOnToolResult OnToolResult;

private:
	/** Build tool definitions array */
	TArray<TSharedPtr<FJsonObject>> BuildToolDefinitions();

	/** Handle HTTP response */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	/** Process streaming response */
	void ProcessStreamingResponse(const FString& ResponseContent);

	/** Process standard JSON response from Responses API (non-streaming) */
	void ProcessResponsesApiResponse(const FString& ResponseContent);

	/** Process extracted tool calls: execute them and continue conversation */
	void ProcessExtractedToolCalls(const TArray<FToolCallInfo>& ToolCalls, const FString& AccumulatedText);

	/** Execute a tool call (delegates to UUnrealGPTToolExecutor) */
	FString ExecuteToolCall(const FString& ToolName, const FString& ArgumentsJson);

	/** Create HTTP request with proper headers */
	TSharedRef<IHttpRequest> CreateHttpRequest();

	/** Get the effective API URL, applying base URL override if set */
	FString GetEffectiveApiUrl() const;

	/** Check if we're using the Responses API endpoint */
	bool IsUsingResponsesApi() const;

	/**
	 * Determine appropriate reasoning effort level based on message complexity.
	 * Returns "low", "medium", or "high".
	 * - low: Simple tool calls, single-step operations
	 * - medium: Multi-step tasks, scene building, some ambiguity
	 * - high: Complex planning, reference image interpretation, architectural decisions
	 */
	FString DetermineReasoningEffort(const FString& UserMessage, const TArray<FString>& ImagePaths) const;

	/** Detect if task completion can be inferred from recent tool results */
	bool DetectTaskCompletion(const TArray<FString>& ToolNames, const TArray<FString>& ToolResults) const;

	/** Current HTTP request */
	TSharedPtr<IHttpRequest> CurrentRequest;

	/** Conversation history */
	TArray<FAgentMessage> ConversationHistory;

	/** Previous response ID for Responses API state management */
	FString PreviousResponseId;

	/** Tool call iteration counter to prevent infinite loops */
	int32 ToolCallIterationCount;

	// MaxToolCallIterations is now configurable via UUnrealGPTSettings

	/** Maximum size (in characters) for tool results to include in conversation history and API requests.
	 *  Large results (like base64 screenshots) will be truncated or summarized to prevent
	 *  context window overflow. This is critical for cost control.
	 */
	static constexpr int32 MaxToolResultSize = 10000; // ~10KB

	/** Signatures of tool calls that have already been executed in this conversation.
	 *  Used to avoid re-running identical python_execute calls in a loop.
	 */
	TSet<FString> ExecutedToolCallSignatures;

	/** Tracks whether the last executed tool was python_execute.
	 *  Used to avoid blindly running python_execute multiple times in a row;
	 *  the agent should instead inspect the scene with scene_query or
	 *  viewport_screenshot before deciding on further code changes.
	 */
	bool bLastToolWasPythonExecute;

	/** Tracks whether the last tool was scene_query and it returned results (non-empty array).
	 *  If true, the next python_execute call should be blocked to prevent loops after
	 *  verification confirms the task is complete.
	 */
	bool bLastSceneQueryFoundResults;

	/** Settings reference */
	class UUnrealGPTSettings* Settings;

	/** Is request in progress */
	bool bRequestInProgress;

	/** Whether we should include reasoning.summary in requests (disabled if the org is not verified). */
	bool bAllowReasoningSummary = true;

	/** Cached copy of the last JSON request body, used for safe retry on specific API errors. */
	FString LastRequestBody;

	/** Retry counter for transient HTTP failures */
	int32 HttpRetryCount = 0;

	/** Retry counter for 429 rate limit errors (resets on success) */
	int32 RateLimitRetryCount = 0;

	/** Maximum rate limit retries before giving up */
	static constexpr int32 MaxRateLimitRetries = 10;

	/** Timestamp when the current HTTP request started (for timing/timeout diagnostics) */
	double RequestStartTime = 0.0;

	/** Current conversation session ID - used for naming log files */
	FString ConversationSessionId;

	/** Generate a new session ID based on current timestamp */
	static FString GenerateSessionId();

	// ==================== SESSION PERSISTENCE ====================

	/** Session manager for conversation persistence */
	UPROPERTY()
	UUnrealGPTSessionManager* SessionManager;

	/** Images attached to the current message being sent */
	TArray<FString> CurrentMessageImages;

	/** Helper to save user message to session */
	void SaveUserMessageToSession(const FString& UserMessage, const TArray<FString>& Images);

	/** Helper to save assistant message to session */
	void SaveAssistantMessageToSession(const FString& Content, const TArray<FString>& ToolCallIds, const FString& ToolCallsJson);

	/** Helper to save tool message to session */
	void SaveToolMessageToSession(const FString& ToolCallId, const FString& Result, const TArray<FString>& Images);

	/** Helper to save tool call for UI reconstruction */
	void SaveToolCallToSession(const FString& ToolName, const FString& Arguments, const FString& Result);

	/** Extract base64 images from tool result string */
};

