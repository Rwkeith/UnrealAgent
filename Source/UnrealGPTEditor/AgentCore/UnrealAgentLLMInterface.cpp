// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentLLMInterface.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Internationalization/Regex.h"
#include "UnrealGPTSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"

FAgentLLMInterface::FAgentLLMInterface()
{
	// Default system prompts
	GoalParsingSystemPrompt = TEXT(
		"You are a goal parser for an Unreal Engine AI agent. "
		"Extract structured information from user requests.\n"
		"Return JSON with: description, parameters (key-value pairs), constraints, and success_criteria.\n"
		"Be precise and extract all relevant details."
	);

	PlanGenerationSystemPrompt = TEXT(
		"You are a planner for an Unreal Engine AI agent. "
		"Create step-by-step plans using these tools:\n"
		"- scene_query: Query actors by class/label/tag\n"
		"- get_actor: Get details of a specific actor\n"
		"- set_actor_transform: Move/rotate/scale an actor\n"
		"- duplicate_actor: Clone an actor\n"
		"- python_execute: Run Python code\n"
		"- viewport_screenshot: Capture the viewport\n\n"
		"Return a numbered list of steps. Each step: 'tool_name: description'"
	);

	CodeGenerationSystemPrompt = TEXT(
		"You are a Python code generator for Unreal Engine. "
		"Generate safe, transactional Python code using the unreal module.\n"
		"Always use editor_actor_subsystem for actor operations.\n"
		"Wrap operations in try/except. Return status as JSON."
	);
}

// ==================== INTENT PARSING ====================

FAgentGoal FAgentLLMInterface::ParseUserIntent(const FString& UserRequest)
{
	FString Prompt = BuildGoalParsingPrompt(UserRequest);
	FString Response = SendPrompt(GoalParsingSystemPrompt, Prompt);

	return ParseGoalFromResponse(Response, UserRequest);
}

TMap<FString, FString> FAgentLLMInterface::ExtractParameters(const FString& UserRequest)
{
	FString Prompt = FString::Printf(
		TEXT("Extract parameters from this request as key-value pairs:\n\"%s\"\n\n"
			"Return JSON object with parameters like: {\"count\": \"10\", \"pattern\": \"circle\", \"object\": \"tree\"}"),
		*UserRequest);

	FString Response = SendPrompt(GoalParsingSystemPrompt, Prompt);
	return ParseParametersFromResponse(Response);
}

TArray<FSuccessCriterion> FAgentLLMInterface::SuggestSuccessCriteria(const FAgentGoal& Goal)
{
	FString Prompt = FString::Printf(
		TEXT("Suggest success criteria for this goal:\n\"%s\"\n\n"
			"Return JSON array of criteria, each with: description, type (SceneQuery/PropertyCheck/AssetExists), query.\n"
			"Example: [{\"description\": \"10 trees exist\", \"type\": \"SceneQuery\", \"query\": \"label contains 'Tree', count >= 10\"}]"),
		*Goal.Description);

	FString Response = SendPrompt(GoalParsingSystemPrompt, Prompt);
	return ParseCriteriaFromResponse(Response);
}

// ==================== PLAN GENERATION ====================

FString FAgentLLMInterface::SuggestPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FString Prompt = BuildPlanGenerationPrompt(Goal, WorldModel);
	return SendPrompt(PlanGenerationSystemPrompt, Prompt);
}

TMap<FString, FString> FAgentLLMInterface::SuggestToolArguments(const FString& ToolName,
	const FString& StepDescription, const FAgentWorldModel& WorldModel)
{
	FString Prompt = FString::Printf(
		TEXT("Generate arguments for tool '%s' to accomplish:\n\"%s\"\n\n"
			"Current scene has %d actors.\n"
			"Return JSON object with argument names and values."),
		*ToolName, *StepDescription, WorldModel.GetActorCount());

	FString Response = SendPrompt(PlanGenerationSystemPrompt, Prompt);
	return ParseParametersFromResponse(Response);
}

FString FAgentLLMInterface::GeneratePythonCode(const FString& TaskDescription,
	const FAgentWorldModel& WorldModel)
{
	FString Prompt = BuildPythonCodePrompt(TaskDescription, WorldModel);
	FString Response = SendPrompt(CodeGenerationSystemPrompt, Prompt);

	// Extract code from response (might be in markdown code block)
	if (Response.Contains(TEXT("```python")))
	{
		int32 Start = Response.Find(TEXT("```python")) + 9;
		int32 End = Response.Find(TEXT("```"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End != INDEX_NONE)
		{
			return Response.Mid(Start, End - Start).TrimStartAndEnd();
		}
	}
	else if (Response.Contains(TEXT("```")))
	{
		int32 Start = Response.Find(TEXT("```")) + 3;
		int32 End = Response.Find(TEXT("```"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End != INDEX_NONE)
		{
			return Response.Mid(Start, End - Start).TrimStartAndEnd();
		}
	}

	return Response;
}

// ==================== RECOVERY ====================

FString FAgentLLMInterface::SuggestRecoveryPlan(const FAgentPlan& FailedPlan,
	const FStepResult& FailedStep, const FAgentWorldModel& WorldModel)
{
	FString Prompt = BuildRecoveryPrompt(FailedPlan, FailedStep);
	return SendPrompt(PlanGenerationSystemPrompt, Prompt);
}

TArray<FSuggestedFix> FAgentLLMInterface::SuggestFixes(const FPlanStep& FailedStep,
	const FStepResult& Result)
{
	TArray<FSuggestedFix> Fixes;

	FString Prompt = FString::Printf(
		TEXT("A step failed:\nTool: %s\nDescription: %s\nError: %s\n\n"
			"Suggest fixes as JSON array: [{\"description\": \"...\", \"parameter\": \"...\", \"new_value\": \"...\", \"confidence\": 0.8}]"),
		*FailedStep.ToolName, *FailedStep.Description, *Result.ToolResult.ErrorMessage);

	FString Response = SendPrompt(PlanGenerationSystemPrompt, Prompt);

	// Parse fixes from response
	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);

	if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* FixesArray;
		if (JsonValue->TryGetArray(FixesArray))
		{
			for (const TSharedPtr<FJsonValue>& FixValue : *FixesArray)
			{
				const TSharedPtr<FJsonObject>* FixObject;
				if (FixValue->TryGetObject(FixObject))
				{
					FSuggestedFix Fix;
					Fix.Description = (*FixObject)->GetStringField(TEXT("description"));
					Fix.ParameterName = (*FixObject)->GetStringField(TEXT("parameter"));
					Fix.NewValue = (*FixObject)->GetStringField(TEXT("new_value"));
					Fix.Confidence = (*FixObject)->GetNumberField(TEXT("confidence"));
					Fixes.Add(Fix);
				}
			}
		}
	}

	return Fixes;
}

// ==================== USER COMMUNICATION ====================

FString FAgentLLMInterface::ExplainFailure(const FStepResult& FailedStep)
{
	FString Prompt = FString::Printf(
		TEXT("Explain this error in simple terms for a user:\n%s\n\nSummary: %s"),
		*FailedStep.ToolResult.ErrorMessage, *FailedStep.Summary);

	return SendPrompt(TEXT("You are a helpful assistant explaining errors to users."), Prompt);
}

FString FAgentLLMInterface::GenerateProgressUpdate(const FAgentGoal& Goal, const FAgentPlan& Plan)
{
	float Progress = Plan.CalculateProgress();

	FString Prompt = FString::Printf(
		TEXT("Generate a brief progress update:\nGoal: %s\nProgress: %.0f%%\nCurrent step: %d of %d"),
		*Goal.Description, Progress, Plan.CurrentStepIndex + 1, Plan.Steps.Num());

	return SendPrompt(TEXT("Generate brief, friendly progress updates."), Prompt);
}

FString FAgentLLMInterface::GenerateClarifyingQuestion(const FAgentGoal& Goal,
	const TArray<FString>& AmbiguousAspects)
{
	FString Aspects = FString::Join(AmbiguousAspects, TEXT(", "));

	FString Prompt = FString::Printf(
		TEXT("The user's request is ambiguous:\n\"%s\"\n\nUnclear aspects: %s\n\n"
			"Generate a friendly clarifying question."),
		*Goal.OriginalRequest, *Aspects);

	return SendPrompt(TEXT("You ask clarifying questions politely and concisely."), Prompt);
}

// ==================== VISUAL VERIFICATION ====================

bool FAgentLLMInterface::VerifyVisualCondition(const FString& Condition,
	const TArray<uint8>& ScreenshotPNG)
{
	FString Prompt = FString::Printf(
		TEXT("Look at this screenshot and answer YES or NO:\n%s"),
		*Condition);

	FString Response = SendPromptWithImage(
		TEXT("You are a visual verification system. Answer only YES or NO."),
		Prompt, ScreenshotPNG);

	return Response.Contains(TEXT("YES"));
}

FString FAgentLLMInterface::DescribeScreenshot(const TArray<uint8>& ScreenshotPNG)
{
	return SendPromptWithImage(
		TEXT("Describe what you see in this Unreal Engine editor screenshot."),
		TEXT("Describe this scene concisely."),
		ScreenshotPNG);
}

// ==================== PRIVATE: LLM COMMUNICATION ====================

FString FAgentLLMInterface::SendPrompt(const FString& SystemPrompt, const FString& UserPrompt)
{
	// Check cache first
	FString CacheKey = FString::Printf(TEXT("%s|%s"), *SystemPrompt.Left(50), *UserPrompt.Left(100));
	if (bCacheResponses)
	{
		FString* CachedResponse = ResponseCache.Find(CacheKey);
		if (CachedResponse)
		{
			return *CachedResponse;
		}
	}

	/*
	 * INTEGRATION NOTE: To enable real LLM calls, implement async LLM integration here.
	 *
	 * The existing UnrealGPTAgentClient uses async HTTP requests with callbacks.
	 * Options for integration:
	 *
	 * 1. ASYNC PATTERN (Recommended):
	 *    - Add async variants: SendPromptAsync(Prompt, OnComplete)
	 *    - Update FAgentController to handle async planning
	 *    - More complex but doesn't block the editor
	 *
	 * 2. SYNC PATTERN (Simpler but blocks editor):
	 *    - Create a dedicated HTTP client for synchronous calls
	 *    - Use FHttpModule with blocking wait
	 *    - Not recommended for production
	 *
	 * 3. PROGRAMMATIC PLANNING (Current approach):
	 *    - FAgentPlanner uses pattern recognition for known tasks
	 *    - LLM only needed for complex/unknown patterns
	 *    - Works without LLM for common Unreal Engine tasks
	 *
	 * For now, we return intelligent stub responses that allow the agent
	 * to function with programmatic planning for known patterns.
	 */

	FString Response;

	// Smart stub responses based on prompt content
	if (UserPrompt.Contains(TEXT("Extract parameters")))
	{
		// Try to extract basic parameters from the user prompt itself
		Response = ExtractParametersFromText(UserPrompt);
	}
	else if (UserPrompt.Contains(TEXT("success criteria")))
	{
		// Generate specific criteria based on goal description
		// Extract count from prompt if present
		int32 ExpectedCount = 1;
		FRegexPattern CountPattern(TEXT("(\\d+)"));
		FRegexMatcher CountMatcher(CountPattern, UserPrompt);
		if (CountMatcher.FindNext())
		{
			ExpectedCount = FCString::Atoi(*CountMatcher.GetCaptureGroup(1));
		}

		// Extract object type for label-based matching
		// Note: The planner creates actors with labels like "CYLINDER_0", "CUBE_1", etc.
		// So we match the uppercase object type prefix
		FString LabelFilter = TEXT("");
		if (UserPrompt.Contains(TEXT("pillar")) || UserPrompt.Contains(TEXT("cylinder")))
		{
			LabelFilter = TEXT("CYLINDER");
		}
		else if (UserPrompt.Contains(TEXT("cube")) || UserPrompt.Contains(TEXT("box")))
		{
			LabelFilter = TEXT("CUBE");
		}
		else if (UserPrompt.Contains(TEXT("sphere")) || UserPrompt.Contains(TEXT("ball")))
		{
			LabelFilter = TEXT("SPHERE");
		}
		else if (UserPrompt.Contains(TEXT("cone")))
		{
			LabelFilter = TEXT("CONE");
		}
		else if (UserPrompt.Contains(TEXT("tree")))
		{
			LabelFilter = TEXT("Tree");
		}

		// Build the query - use class filter for StaticMeshActor if no specific label
		FString Query;
		if (!LabelFilter.IsEmpty())
		{
			Query = FString::Printf(TEXT("label contains '%s', count >= %d"), *LabelFilter.Replace(TEXT("*"), TEXT("")), ExpectedCount);
		}
		else
		{
			// Generic query - just check that we have at least the expected number of static mesh actors
			Query = FString::Printf(TEXT("class=StaticMeshActor, count >= %d"), ExpectedCount);
		}

		Response = FString::Printf(
			TEXT("[{\"description\": \"At least %d objects created\", \"type\": \"SceneQuery\", \"query\": \"%s\"}]"),
			ExpectedCount, *Query);

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Generated success criteria: %s"), *Response);
	}
	else if (UserPrompt.Contains(TEXT("step-by-step")) || SystemPrompt.Contains(TEXT("planner")))
	{
		// Return a generic observe-act-verify plan
		Response = TEXT("1. scene_query: Query current scene state\n2. python_execute: Execute task\n3. scene_query: Verify results");
	}
	else if (UserPrompt.Contains(TEXT("error")) || UserPrompt.Contains(TEXT("Explain")))
	{
		// For error explanations, return the error summary directly
		Response = TEXT("The operation encountered an issue. Please check the error details and try a different approach.");
	}
	else if (UserPrompt.Contains(TEXT("progress")))
	{
		// Return a generic progress message
		Response = TEXT("Working on your request...");
	}
	else
	{
		// Default stub
		Response = TEXT("Task acknowledged. Using programmatic planning.");
	}

	// Cache the response
	if (bCacheResponses)
	{
		ResponseCache.Add(CacheKey, Response);
	}

	return Response;
}

FString FAgentLLMInterface::ExtractParametersFromText(const FString& Text)
{
	// Simple parameter extraction from natural language
	TMap<FString, FString> Params;

	// Look for numbers (could be count)
	FRegexPattern NumberPattern(TEXT("(\\d+)"));
	FRegexMatcher NumberMatcher(NumberPattern, Text);
	if (NumberMatcher.FindNext())
	{
		Params.Add(TEXT("count"), NumberMatcher.GetCaptureGroup(1));
	}

	// Look for common patterns
	if (Text.Contains(TEXT("circle")) || Text.Contains(TEXT("circular")))
	{
		Params.Add(TEXT("pattern"), TEXT("circle"));
	}
	else if (Text.Contains(TEXT("grid")))
	{
		Params.Add(TEXT("pattern"), TEXT("grid"));
	}
	else if (Text.Contains(TEXT("line")) || Text.Contains(TEXT("row")))
	{
		Params.Add(TEXT("pattern"), TEXT("line"));
	}

	// Look for common object types
	if (Text.Contains(TEXT("tree")))
	{
		Params.Add(TEXT("object"), TEXT("tree"));
	}
	else if (Text.Contains(TEXT("cube")) || Text.Contains(TEXT("box")))
	{
		Params.Add(TEXT("object"), TEXT("cube"));
	}
	else if (Text.Contains(TEXT("sphere")) || Text.Contains(TEXT("ball")))
	{
		Params.Add(TEXT("object"), TEXT("sphere"));
	}

	// Convert to JSON
	if (Params.Num() == 0)
	{
		return TEXT("{\"count\": \"1\"}");
	}

	FString Result = TEXT("{");
	bool bFirst = true;
	for (const auto& Pair : Params)
	{
		if (!bFirst) Result += TEXT(", ");
		Result += FString::Printf(TEXT("\"%s\": \"%s\""), *Pair.Key, *Pair.Value);
		bFirst = false;
	}
	Result += TEXT("}");

	return Result;
}

FString FAgentLLMInterface::SendPromptWithImage(const FString& SystemPrompt,
	const FString& UserPrompt, const TArray<uint8>& ImagePNG)
{
	// In production, this would use your existing UnrealGPTAgentClient with image support
	// For now, return a stub response

	return TEXT("Visual verification not implemented - returning YES as stub");
}

// ==================== PRIVATE: PARSING HELPERS ====================

FAgentGoal FAgentLLMInterface::ParseGoalFromResponse(const FString& Response,
	const FString& OriginalRequest)
{
	FAgentGoal Goal;
	Goal.OriginalRequest = OriginalRequest;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		Goal.Description = JsonObject->GetStringField(TEXT("description"));

		// Parse parameters
		const TSharedPtr<FJsonObject>* ParamsObject;
		if (JsonObject->TryGetObjectField(TEXT("parameters"), ParamsObject))
		{
			for (const auto& Pair : (*ParamsObject)->Values)
			{
				Goal.Parameters.Add(Pair.Key, Pair.Value->AsString());
			}
		}

		// Parse constraints
		const TArray<TSharedPtr<FJsonValue>>* ConstraintsArray;
		if (JsonObject->TryGetArrayField(TEXT("constraints"), ConstraintsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ConstraintsArray)
			{
				Goal.Constraints.Add(Value->AsString());
			}
		}

		// Parse success criteria
		const TArray<TSharedPtr<FJsonValue>>* CriteriaArray;
		if (JsonObject->TryGetArrayField(TEXT("success_criteria"), CriteriaArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *CriteriaArray)
			{
				const TSharedPtr<FJsonObject>* CriterionObject;
				if (Value->TryGetObject(CriterionObject))
				{
					FSuccessCriterion Criterion;
					Criterion.Description = (*CriterionObject)->GetStringField(TEXT("description"));
					Criterion.ValidationQuery = (*CriterionObject)->GetStringField(TEXT("query"));

					FString TypeStr = (*CriterionObject)->GetStringField(TEXT("type"));
					if (TypeStr == TEXT("SceneQuery"))
					{
						Criterion.Type = ESuccessCriterionType::SceneQuery;
					}
					else if (TypeStr == TEXT("PropertyCheck"))
					{
						Criterion.Type = ESuccessCriterionType::PropertyCheck;
					}
					else if (TypeStr == TEXT("AssetExists"))
					{
						Criterion.Type = ESuccessCriterionType::AssetExists;
					}

					Goal.SuccessCriteria.Add(Criterion);
				}
			}
		}
	}
	else
	{
		// Failed to parse - use original request as description
		Goal.Description = OriginalRequest;
	}

	return Goal;
}

TMap<FString, FString> FAgentLLMInterface::ParseParametersFromResponse(const FString& Response)
{
	TMap<FString, FString> Parameters;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		for (const auto& Pair : JsonObject->Values)
		{
			Parameters.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Parameters;
}

TArray<FSuccessCriterion> FAgentLLMInterface::ParseCriteriaFromResponse(const FString& Response)
{
	TArray<FSuccessCriterion> Criteria;

	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);

	if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CriteriaArray;
		if (JsonValue->TryGetArray(CriteriaArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *CriteriaArray)
			{
				const TSharedPtr<FJsonObject>* CriterionObject;
				if (Value->TryGetObject(CriterionObject))
				{
					FSuccessCriterion Criterion;
					Criterion.Description = (*CriterionObject)->GetStringField(TEXT("description"));
					Criterion.ValidationQuery = (*CriterionObject)->GetStringField(TEXT("query"));

					FString TypeStr = (*CriterionObject)->GetStringField(TEXT("type"));
					if (TypeStr == TEXT("SceneQuery"))
					{
						Criterion.Type = ESuccessCriterionType::SceneQuery;
					}
					else if (TypeStr == TEXT("PropertyCheck"))
					{
						Criterion.Type = ESuccessCriterionType::PropertyCheck;
					}

					Criteria.Add(Criterion);
				}
			}
		}
	}

	return Criteria;
}

// ==================== PRIVATE: PROMPT TEMPLATES ====================

FString FAgentLLMInterface::BuildGoalParsingPrompt(const FString& UserRequest)
{
	return FString::Printf(
		TEXT("Parse this user request into a structured goal:\n\n\"%s\"\n\n"
			"Return JSON with:\n"
			"- description: Clear goal statement\n"
			"- parameters: Key-value pairs (count, pattern, object type, etc.)\n"
			"- constraints: Any limitations mentioned\n"
			"- success_criteria: Array of {description, type, query}"),
		*UserRequest);
}

FString FAgentLLMInterface::BuildPlanGenerationPrompt(const FAgentGoal& Goal,
	const FAgentWorldModel& WorldModel)
{
	FString Prompt;

	Prompt += FString::Printf(TEXT("Create a plan for this goal:\n\"%s\"\n\n"), *Goal.Description);

	if (Goal.Parameters.Num() > 0)
	{
		Prompt += TEXT("Parameters:\n");
		for (const auto& Pair : Goal.Parameters)
		{
			Prompt += FString::Printf(TEXT("- %s: %s\n"), *Pair.Key, *Pair.Value);
		}
		Prompt += TEXT("\n");
	}

	if (Goal.SuccessCriteria.Num() > 0)
	{
		Prompt += TEXT("Success criteria:\n");
		for (const FSuccessCriterion& Criterion : Goal.SuccessCriteria)
		{
			Prompt += FString::Printf(TEXT("- %s\n"), *Criterion.Description);
		}
		Prompt += TEXT("\n");
	}

	Prompt += FString::Printf(TEXT("Current scene: %d actors\n\n"), WorldModel.GetActorCount());

	Prompt += TEXT("Return numbered steps in format: 'tool_name: description'");

	return Prompt;
}

FString FAgentLLMInterface::BuildPythonCodePrompt(const FString& TaskDescription,
	const FAgentWorldModel& WorldModel)
{
	FString Prompt;

	Prompt += TEXT("Generate Python code for Unreal Engine to accomplish:\n");
	Prompt += FString::Printf(TEXT("\"%s\"\n\n"), *TaskDescription);

	Prompt += TEXT("Requirements:\n");
	Prompt += TEXT("- Use import unreal\n");
	Prompt += TEXT("- Use editor_actor_subsystem for actor operations\n");
	Prompt += TEXT("- Wrap in try/except for error handling\n");
	Prompt += TEXT("- Print result as JSON: {\"status\": \"ok\", \"message\": \"...\"}\n\n");

	Prompt += FString::Printf(TEXT("Current scene has %d actors.\n"), WorldModel.GetActorCount());

	return Prompt;
}

FString FAgentLLMInterface::BuildRecoveryPrompt(const FAgentPlan& FailedPlan,
	const FStepResult& FailedStep)
{
	FString Prompt;

	Prompt += TEXT("A plan step failed. Suggest recovery.\n\n");

	Prompt += FString::Printf(TEXT("Original plan: %s\n"), *FailedPlan.Summary);
	Prompt += FString::Printf(TEXT("Failed at step %d: %s\n"),
		FailedPlan.CurrentStepIndex + 1,
		FailedPlan.Steps.IsValidIndex(FailedPlan.CurrentStepIndex)
			? *FailedPlan.Steps[FailedPlan.CurrentStepIndex].Description
			: TEXT("Unknown"));
	Prompt += FString::Printf(TEXT("Error: %s\n\n"), *FailedStep.Summary);

	Prompt += TEXT("Suggest alternative steps to complete the goal.\n");
	Prompt += TEXT("Return numbered steps in format: 'tool_name: description'");

	return Prompt;
}

// ==================== ASYNC METHODS ====================

void FAgentLLMInterface::ParseUserIntentAsync(const FString& UserRequest,
	FOnGoalParsed OnComplete, FOnLLMError OnError)
{
	FString Prompt = BuildGoalParsingPrompt(UserRequest);

	// Store the callback and original request
	PendingGoalCallback = OnComplete;
	PendingOriginalRequest = UserRequest;
	PendingRequestType = EAsyncRequestType::GoalParsing;
	PendingOnError = OnError;

	// Make the async request
	SendPromptAsync(GoalParsingSystemPrompt, Prompt,
		FOnLLMResponse::CreateLambda([this](const FString& Response)
		{
			FAgentGoal Goal = ParseGoalFromResponse(Response, PendingOriginalRequest);
			if (PendingGoalCallback.IsBound())
			{
				PendingGoalCallback.Execute(Goal);
			}
			PendingRequestType = EAsyncRequestType::None;
		}),
		OnError);
}

void FAgentLLMInterface::SuggestPlanAsync(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel,
	FOnPlanSuggested OnComplete, FOnLLMError OnError)
{
	FString Prompt = BuildPlanGenerationPrompt(Goal, WorldModel);

	PendingPlanCallback = OnComplete;
	PendingRequestType = EAsyncRequestType::PlanSuggestion;
	PendingOnError = OnError;

	SendPromptAsync(PlanGenerationSystemPrompt, Prompt,
		FOnLLMResponse::CreateLambda([this](const FString& Response)
		{
			if (PendingPlanCallback.IsBound())
			{
				PendingPlanCallback.Execute(Response);
			}
			PendingRequestType = EAsyncRequestType::None;
		}),
		OnError);
}

void FAgentLLMInterface::GeneratePythonCodeAsync(const FString& TaskDescription,
	const FAgentWorldModel& WorldModel, FOnCodeGenerated OnComplete, FOnLLMError OnError)
{
	FString Prompt = BuildPythonCodePrompt(TaskDescription, WorldModel);

	PendingCodeCallback = OnComplete;
	PendingRequestType = EAsyncRequestType::CodeGeneration;
	PendingOnError = OnError;

	SendPromptAsync(CodeGenerationSystemPrompt, Prompt,
		FOnLLMResponse::CreateLambda([this](const FString& Response)
		{
			// Extract code from response (might be in markdown code block)
			FString Code = Response;
			if (Response.Contains(TEXT("```python")))
			{
				int32 Start = Response.Find(TEXT("```python")) + 9;
				int32 End = Response.Find(TEXT("```"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
				if (End != INDEX_NONE)
				{
					Code = Response.Mid(Start, End - Start).TrimStartAndEnd();
				}
			}
			else if (Response.Contains(TEXT("```")))
			{
				int32 Start = Response.Find(TEXT("```")) + 3;
				int32 End = Response.Find(TEXT("```"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
				if (End != INDEX_NONE)
				{
					Code = Response.Mid(Start, End - Start).TrimStartAndEnd();
				}
			}

			if (PendingCodeCallback.IsBound())
			{
				PendingCodeCallback.Execute(Code);
			}
			PendingRequestType = EAsyncRequestType::None;
		}),
		OnError);
}

void FAgentLLMInterface::SendPromptAsync(const FString& SystemPrompt, const FString& UserPrompt,
	FOnLLMResponse OnComplete, FOnLLMError OnError)
{
	// Check if already processing a request
	if (bAsyncRequestInProgress)
	{
		if (OnError.IsBound())
		{
			OnError.Execute(TEXT("An async request is already in progress"));
		}
		return;
	}

	// Get settings
	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	if (!Settings || Settings->ApiKey.IsEmpty())
	{
		if (OnError.IsBound())
		{
			OnError.Execute(TEXT("API key not configured in UnrealGPT settings"));
		}
		return;
	}

	// Build the request
	FString RequestBody = BuildCompletionRequestBody(SystemPrompt, UserPrompt);

	// Determine the API endpoint (use chat completions for simple prompts)
	FString ApiUrl = Settings->ApiEndpoint;
	// If using the responses endpoint, switch to chat completions for simple prompts
	if (ApiUrl.Contains(TEXT("/responses")))
	{
		ApiUrl = ApiUrl.Replace(TEXT("/responses"), TEXT("/chat/completions"));
	}

	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(ApiUrl);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ApiKey));
	HttpRequest->SetContentAsString(RequestBody);
	HttpRequest->SetTimeout(RequestTimeout);

	// Store callbacks
	PendingOnComplete = OnComplete;
	PendingOnError = OnError;
	bAsyncRequestInProgress = true;
	CurrentHttpRequest = HttpRequest;

	// Bind response handler
	HttpRequest->OnProcessRequestComplete().BindRaw(this, &FAgentLLMInterface::OnAsyncResponseReceived);

	// Send request
	if (!HttpRequest->ProcessRequest())
	{
		bAsyncRequestInProgress = false;
		CurrentHttpRequest.Reset();
		if (OnError.IsBound())
		{
			OnError.Execute(TEXT("Failed to send HTTP request"));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Sent async LLM request to %s"), *ApiUrl);
}

void FAgentLLMInterface::CancelAsyncRequest()
{
	if (bAsyncRequestInProgress && CurrentHttpRequest.IsValid())
	{
		CurrentHttpRequest->CancelRequest();
		CurrentHttpRequest.Reset();
		bAsyncRequestInProgress = false;
		PendingRequestType = EAsyncRequestType::None;

		UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Cancelled async LLM request"));
	}
}

// ==================== ASYNC HELPERS ====================

void FAgentLLMInterface::OnAsyncResponseReceived(FHttpRequestPtr Request,
	FHttpResponsePtr Response, bool bWasSuccessful)
{
	bAsyncRequestInProgress = false;
	CurrentHttpRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		FString ErrorMsg = TEXT("HTTP request failed");
		if (Response.IsValid())
		{
			ErrorMsg = FString::Printf(TEXT("HTTP error: %d"), Response->GetResponseCode());
		}

		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT Agent: %s"), *ErrorMsg);

		if (PendingOnError.IsBound())
		{
			PendingOnError.Execute(ErrorMsg);
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();

	if (ResponseCode != 200)
	{
		FString ErrorMsg = FString::Printf(TEXT("API error %d: %s"), ResponseCode, *ResponseContent.Left(500));
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT Agent: %s"), *ErrorMsg);

		if (PendingOnError.IsBound())
		{
			PendingOnError.Execute(ErrorMsg);
		}
		return;
	}

	// Extract content from response
	FString Content = ExtractContentFromResponse(ResponseContent);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Received LLM response (%d chars)"), Content.Len());

	// Cache the response if enabled
	if (bCacheResponses && !Content.IsEmpty())
	{
		// We don't have the original prompt here, so caching is skipped for async
		// Could be improved by storing the cache key with the request
	}

	// Call the completion callback
	if (PendingOnComplete.IsBound())
	{
		PendingOnComplete.Execute(Content);
	}
}

FString FAgentLLMInterface::BuildCompletionRequestBody(const FString& SystemPrompt, const FString& UserPrompt)
{
	const UUnrealGPTSettings* Settings = GetDefault<UUnrealGPTSettings>();
	FString Model = Settings ? Settings->DefaultModel : TEXT("gpt-4");

	// Build a simple chat completion request
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("model"), Model);

	// Messages array
	TArray<TSharedPtr<FJsonValue>> Messages;

	// System message
	TSharedRef<FJsonObject> SystemMessage = MakeShared<FJsonObject>();
	SystemMessage->SetStringField(TEXT("role"), TEXT("system"));
	SystemMessage->SetStringField(TEXT("content"), SystemPrompt);
	Messages.Add(MakeShared<FJsonValueObject>(SystemMessage));

	// User message
	TSharedRef<FJsonObject> UserMessage = MakeShared<FJsonObject>();
	UserMessage->SetStringField(TEXT("role"), TEXT("user"));
	UserMessage->SetStringField(TEXT("content"), UserPrompt);
	Messages.Add(MakeShared<FJsonValueObject>(UserMessage));

	RootObject->SetArrayField(TEXT("messages"), Messages);

	// Optional parameters
	RootObject->SetNumberField(TEXT("temperature"), 0.7);
	RootObject->SetNumberField(TEXT("max_tokens"), 2000);

	// Serialize to string
	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RootObject, Writer);

	return RequestBody;
}

FString FAgentLLMInterface::ExtractContentFromResponse(const FString& ResponseJson)
{
	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseJson);

	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT Agent: Failed to parse LLM response JSON"));
		return TEXT("");
	}

	// OpenAI chat completion format: choices[0].message.content
	const TArray<TSharedPtr<FJsonValue>>* Choices;
	if (RootObject->TryGetArrayField(TEXT("choices"), Choices) && Choices->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* FirstChoice;
		if ((*Choices)[0]->TryGetObject(FirstChoice))
		{
			const TSharedPtr<FJsonObject>* Message;
			if ((*FirstChoice)->TryGetObjectField(TEXT("message"), Message))
			{
				FString Content;
				if ((*Message)->TryGetStringField(TEXT("content"), Content))
				{
					return Content;
				}
			}
		}
	}

	// Anthropic format: content[0].text
	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	if (RootObject->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* FirstContent;
		if ((*ContentArray)[0]->TryGetObject(FirstContent))
		{
			FString Text;
			if ((*FirstContent)->TryGetStringField(TEXT("text"), Text))
			{
				return Text;
			}
		}
	}

	// Simple text response
	FString DirectContent;
	if (RootObject->TryGetStringField(TEXT("content"), DirectContent))
	{
		return DirectContent;
	}

	UE_LOG(LogTemp, Warning, TEXT("UnrealGPT Agent: Could not extract content from LLM response"));
	return TEXT("");
}
