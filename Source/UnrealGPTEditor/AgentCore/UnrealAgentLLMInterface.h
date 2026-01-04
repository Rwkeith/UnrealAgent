// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentGoal.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"
#include "Http.h"

// Forward declarations
class UUnrealGPTAgentClient;

// Async completion delegates
DECLARE_DELEGATE_OneParam(FOnGoalParsed, const FAgentGoal& /*ParsedGoal*/);
DECLARE_DELEGATE_OneParam(FOnPlanSuggested, const FString& /*SuggestedPlan*/);
DECLARE_DELEGATE_OneParam(FOnCodeGenerated, const FString& /*GeneratedCode*/);
DECLARE_DELEGATE_OneParam(FOnLLMResponse, const FString& /*Response*/);
DECLARE_DELEGATE_OneParam(FOnLLMError, const FString& /*ErrorMessage*/);

/**
 * Interface for using LLM as an advisor, NOT as the controller.
 *
 * Key principle: The LLM suggests, YOUR CODE validates and decides.
 *
 * This wraps your existing UnrealGPTAgentClient but uses it differently:
 * - For parsing user intent into goals
 * - For suggesting plans (which get validated)
 * - For generating code snippets (which get sanitized)
 * - For explaining failures to users
 *
 * The LLM does NOT:
 * - Decide what to execute next
 * - Judge if a task succeeded
 * - Control the agent loop
 */
class UNREALGPTEDITOR_API FAgentLLMInterface
{
public:
	FAgentLLMInterface();

	/** Set the underlying agent client */
	void SetAgentClient(UUnrealGPTAgentClient* InClient) { AgentClient = InClient; }

	// ==================== INTENT PARSING ====================

	/**
	 * Parse user request into a structured goal.
	 * LLM extracts: description, parameters, constraints, success criteria.
	 */
	FAgentGoal ParseUserIntent(const FString& UserRequest);

	/**
	 * Extract parameters from natural language.
	 * E.g., "10 trees in a circle" -> {count: 10, pattern: circle, object: tree}
	 */
	TMap<FString, FString> ExtractParameters(const FString& UserRequest);

	/**
	 * Suggest success criteria for a goal.
	 * LLM proposes what "done" looks like.
	 */
	TArray<FSuccessCriterion> SuggestSuccessCriteria(const FAgentGoal& Goal);

	// ==================== PLAN GENERATION ====================

	/**
	 * Suggest a plan for achieving a goal.
	 * Returns raw LLM response (caller must parse and validate).
	 */
	FString SuggestPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Suggest tool arguments for a step.
	 * Returns argument map (caller must validate).
	 */
	TMap<FString, FString> SuggestToolArguments(const FString& ToolName,
		const FString& StepDescription, const FAgentWorldModel& WorldModel);

	/**
	 * Generate Python code for a task.
	 * Returns code string (caller must sanitize and validate).
	 */
	FString GeneratePythonCode(const FString& TaskDescription, const FAgentWorldModel& WorldModel);

	// ==================== RECOVERY ====================

	/**
	 * Suggest a recovery plan after failure.
	 */
	FString SuggestRecoveryPlan(const FAgentPlan& FailedPlan, const FStepResult& FailedStep,
		const FAgentWorldModel& WorldModel);

	/**
	 * Suggest fixes for a failed step.
	 */
	TArray<FSuggestedFix> SuggestFixes(const FPlanStep& FailedStep, const FStepResult& Result);

	// ==================== USER COMMUNICATION ====================

	/**
	 * Explain a failure in natural language (for user display).
	 */
	FString ExplainFailure(const FStepResult& FailedStep);

	/**
	 * Generate a progress update for the user.
	 */
	FString GenerateProgressUpdate(const FAgentGoal& Goal, const FAgentPlan& Plan);

	/**
	 * Ask user a clarifying question.
	 */
	FString GenerateClarifyingQuestion(const FAgentGoal& Goal,
		const TArray<FString>& AmbiguousAspects);

	// ==================== VISUAL VERIFICATION ====================

	/**
	 * Verify a visual condition using vision model.
	 * Returns true if the condition appears to be met.
	 */
	bool VerifyVisualCondition(const FString& Condition, const TArray<uint8>& ScreenshotPNG);

	/**
	 * Describe what's visible in a screenshot.
	 */
	FString DescribeScreenshot(const TArray<uint8>& ScreenshotPNG);

	// ==================== ASYNC METHODS ====================
	// Use these when you need real LLM responses for ambiguous requests.
	// The agent controller should pause its state machine while waiting.

	/**
	 * Parse user request into a structured goal (async).
	 * Use for ambiguous requests that pattern recognition can't handle.
	 */
	void ParseUserIntentAsync(const FString& UserRequest, FOnGoalParsed OnComplete, FOnLLMError OnError);

	/**
	 * Suggest a plan for achieving a goal (async).
	 * Use when programmatic planning fails or for complex multi-step tasks.
	 */
	void SuggestPlanAsync(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel,
		FOnPlanSuggested OnComplete, FOnLLMError OnError);

	/**
	 * Generate Python code for a task (async).
	 * Use for complex code generation that can't be templated.
	 */
	void GeneratePythonCodeAsync(const FString& TaskDescription, const FAgentWorldModel& WorldModel,
		FOnCodeGenerated OnComplete, FOnLLMError OnError);

	/**
	 * Send a generic prompt to the LLM (async).
	 * Use for custom queries like recovery suggestions or clarifications.
	 */
	void SendPromptAsync(const FString& SystemPrompt, const FString& UserPrompt,
		FOnLLMResponse OnComplete, FOnLLMError OnError);

	/** Check if an async request is in progress */
	bool IsRequestInProgress() const { return bAsyncRequestInProgress; }

	/** Cancel the current async request */
	void CancelAsyncRequest();

	// ==================== CONFIGURATION ====================

	/** Check if LLM is available for use */
	bool IsLLMAvailable() const { return AgentClient != nullptr; }

	/** Get the agent client (for checking availability) */
	UUnrealGPTAgentClient* GetAgentClient() const { return AgentClient; }

	/** Set timeout for LLM requests */
	void SetTimeout(float TimeoutSeconds) { RequestTimeout = TimeoutSeconds; }

	/** Enable/disable caching of responses */
	void SetCacheEnabled(bool bEnabled) { bCacheResponses = bEnabled; }

	/** Set the system prompt for goal parsing */
	void SetGoalParsingPrompt(const FString& Prompt) { GoalParsingSystemPrompt = Prompt; }

	/** Set the system prompt for plan generation */
	void SetPlanGenerationPrompt(const FString& Prompt) { PlanGenerationSystemPrompt = Prompt; }

private:
	// ==================== LLM COMMUNICATION ====================

	/**
	 * Send a prompt to the LLM and get response.
	 * This is a synchronous wrapper around your existing async client.
	 */
	FString SendPrompt(const FString& SystemPrompt, const FString& UserPrompt);

	/**
	 * Send a prompt with images (for visual verification).
	 */
	FString SendPromptWithImage(const FString& SystemPrompt, const FString& UserPrompt,
		const TArray<uint8>& ImagePNG);

	// ==================== PARSING HELPERS ====================

	/** Parse goal from LLM response */
	FAgentGoal ParseGoalFromResponse(const FString& Response, const FString& OriginalRequest);

	/** Parse parameters from LLM response */
	TMap<FString, FString> ParseParametersFromResponse(const FString& Response);

	/** Parse criteria from LLM response */
	TArray<FSuccessCriterion> ParseCriteriaFromResponse(const FString& Response);

	// ==================== PROMPT TEMPLATES ====================

	FString BuildGoalParsingPrompt(const FString& UserRequest);
	FString BuildPlanGenerationPrompt(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);
	FString BuildPythonCodePrompt(const FString& TaskDescription, const FAgentWorldModel& WorldModel);
	FString BuildRecoveryPrompt(const FAgentPlan& FailedPlan, const FStepResult& FailedStep);

	// ==================== PARAMETER EXTRACTION ====================

	/** Extract parameters from natural language text (fallback when LLM not available) */
	FString ExtractParametersFromText(const FString& Text);

	// ==================== STATE ====================

	UUnrealGPTAgentClient* AgentClient = nullptr;

	float RequestTimeout = 60.0f;
	bool bCacheResponses = true;

	// System prompts
	FString GoalParsingSystemPrompt;
	FString PlanGenerationSystemPrompt;
	FString CodeGenerationSystemPrompt;

	// Response cache
	TMap<FString, FString> ResponseCache;

	// ==================== ASYNC STATE ====================

	/** Whether an async request is currently in progress */
	bool bAsyncRequestInProgress = false;

	/** Current HTTP request handle for cancellation */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> CurrentHttpRequest;

	/** Pending completion callback */
	FOnLLMResponse PendingOnComplete;

	/** Pending error callback */
	FOnLLMError PendingOnError;

	/** Stored original request for goal parsing callback */
	FString PendingOriginalRequest;

	/** Type of pending async request (for proper response handling) */
	enum class EAsyncRequestType
	{
		None,
		GoalParsing,
		PlanSuggestion,
		CodeGeneration,
		Generic
	};
	EAsyncRequestType PendingRequestType = EAsyncRequestType::None;

	/** Goal parsing completion callback (stored separately due to different signature) */
	FOnGoalParsed PendingGoalCallback;

	/** Plan suggestion completion callback */
	FOnPlanSuggested PendingPlanCallback;

	/** Code generation completion callback */
	FOnCodeGenerated PendingCodeCallback;

	// ==================== ASYNC HELPERS ====================

	/** Handle HTTP response from async request */
	void OnAsyncResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	/** Build the HTTP request body for a simple completion */
	FString BuildCompletionRequestBody(const FString& SystemPrompt, const FString& UserPrompt);

	/** Extract content from API response JSON */
	FString ExtractContentFromResponse(const FString& ResponseJson);
};
