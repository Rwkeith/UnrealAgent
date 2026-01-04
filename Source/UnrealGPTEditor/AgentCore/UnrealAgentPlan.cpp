// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentPlan.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ==================== FPlanStep ====================

FString FPlanStep::GetArgumentsJson() const
{
	TSharedRef<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

	for (const auto& Pair : ToolArguments)
	{
		JsonObject->SetStringField(Pair.Key, Pair.Value);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject, Writer);

	return OutputString;
}

int32 FPlanStep::GetNextStepIndex(EStepResultStatus ResultStatus) const
{
	// Convert status to string for lookup
	FString StatusKey;
	switch (ResultStatus)
	{
	case EStepResultStatus::Success:
		StatusKey = TEXT("Success");
		break;
	case EStepResultStatus::PartialSuccess:
		StatusKey = TEXT("PartialSuccess");
		break;
	case EStepResultStatus::Failed:
		StatusKey = TEXT("Failed");
		break;
	case EStepResultStatus::Blocked:
		StatusKey = TEXT("Blocked");
		break;
	case EStepResultStatus::Skipped:
		StatusKey = TEXT("Skipped");
		break;
	}

	// Look up in branch map
	const int32* NextIndex = NextStepOnResult.Find(StatusKey);
	if (NextIndex)
	{
		return *NextIndex;
	}

	// Default: return -1 to indicate "proceed sequentially"
	return -1;
}

// ==================== FAgentPlan ====================

FString FAgentPlan::GetDebugSummary() const
{
	FString DebugSummary;

	FString StatusStr;
	switch (Status)
	{
	case EPlanStatus::Draft: StatusStr = TEXT("DRAFT"); break;
	case EPlanStatus::Validated: StatusStr = TEXT("VALIDATED"); break;
	case EPlanStatus::Executing: StatusStr = TEXT("EXECUTING"); break;
	case EPlanStatus::Completed: StatusStr = TEXT("COMPLETED"); break;
	case EPlanStatus::Failed: StatusStr = TEXT("FAILED"); break;
	case EPlanStatus::Replanning: StatusStr = TEXT("REPLANNING"); break;
	}

	DebugSummary += FString::Printf(TEXT("Plan [%s]: %s\n"), *StatusStr, *this->Summary);
	DebugSummary += FString::Printf(TEXT("  Goal: %s\n"), *GoalId);
	DebugSummary += FString::Printf(TEXT("  Progress: %.0f%% (%d/%d steps)\n"),
		CalculateProgress(), CurrentStepIndex, Steps.Num());

	if (Rationale.Len() > 0)
	{
		DebugSummary += FString::Printf(TEXT("  Rationale: %s\n"), *Rationale);
	}

	DebugSummary += TEXT("  Steps:\n");
	for (int32 i = 0; i < Steps.Num(); i++)
	{
		const FPlanStep& Step = Steps[i];
		FString Marker = (i == CurrentStepIndex) ? TEXT(">>") : TEXT("  ");
		FString ExecutedStr = Step.bExecuted ? TEXT("[X]") : TEXT("[ ]");

		FString TypeStr;
		switch (Step.Type)
		{
		case EPlanStepType::ToolCall: TypeStr = TEXT("TOOL"); break;
		case EPlanStepType::Observation: TypeStr = TEXT("OBS"); break;
		case EPlanStepType::Verification: TypeStr = TEXT("VERIFY"); break;
		case EPlanStepType::Decision: TypeStr = TEXT("DECIDE"); break;
		case EPlanStepType::SubPlan: TypeStr = TEXT("SUBPLAN"); break;
		}

		DebugSummary += FString::Printf(TEXT("  %s %s %d. [%s] %s"),
			*Marker, *ExecutedStr, i + 1, *TypeStr, *Step.Description);

		if (Step.Type == EPlanStepType::ToolCall || Step.Type == EPlanStepType::Observation)
		{
			DebugSummary += FString::Printf(TEXT(" -> %s()"), *Step.ToolName);
		}

		DebugSummary += TEXT("\n");
	}

	if (Assumptions.Num() > 0)
	{
		DebugSummary += TEXT("  Assumptions:\n");
		for (const FString& Assumption : Assumptions)
		{
			DebugSummary += FString::Printf(TEXT("    - %s\n"), *Assumption);
		}
	}

	if (Risks.Num() > 0)
	{
		DebugSummary += TEXT("  Risks:\n");
		for (const FString& Risk : Risks)
		{
			DebugSummary += FString::Printf(TEXT("    - %s\n"), *Risk);
		}
	}

	return DebugSummary;
}

// ==================== FAgentPlanValidator ====================

FPlanValidation FAgentPlanValidator::ValidatePlan(const FAgentPlan& Plan)
{
	FPlanValidation Validation;
	Validation.bValid = true;

	// Check for empty plan
	if (Plan.Steps.Num() == 0)
	{
		Validation.bValid = false;
		Validation.Errors.Add(TEXT("Plan has no steps"));
		return Validation;
	}

	// Check for goal ID
	if (Plan.GoalId.IsEmpty())
	{
		Validation.Warnings.Add(TEXT("Plan has no associated goal ID"));
	}

	// Validate each step
	for (int32 i = 0; i < Plan.Steps.Num(); i++)
	{
		const FPlanStep& Step = Plan.Steps[i];
		FPlanValidation StepValidation = ValidateStep(Step);

		if (!StepValidation.bValid)
		{
			Validation.bValid = false;
			for (const FString& Error : StepValidation.Errors)
			{
				Validation.Errors.Add(FString::Printf(TEXT("Step %d: %s"), i + 1, *Error));
			}
		}

		for (const FString& Warning : StepValidation.Warnings)
		{
			Validation.Warnings.Add(FString::Printf(TEXT("Step %d: %s"), i + 1, *Warning));
		}

		// Check branching references
		for (const auto& Branch : Step.NextStepOnResult)
		{
			int32 TargetIndex = Branch.Value;
			if (TargetIndex >= 0 && TargetIndex >= Plan.Steps.Num())
			{
				Validation.bValid = false;
				Validation.Errors.Add(FString::Printf(
					TEXT("Step %d: Branch target %d is out of range (max %d)"),
					i + 1, TargetIndex, Plan.Steps.Num() - 1));
			}
		}
	}

	// Check for potential infinite loops in branching
	// (Simple check: step can't branch back to itself)
	for (int32 i = 0; i < Plan.Steps.Num(); i++)
	{
		const FPlanStep& Step = Plan.Steps[i];
		for (const auto& Branch : Step.NextStepOnResult)
		{
			if (Branch.Value == i)
			{
				Validation.Warnings.Add(FString::Printf(
					TEXT("Step %d branches to itself - potential infinite loop"), i + 1));
			}
		}
	}

	return Validation;
}

FPlanValidation FAgentPlanValidator::ValidateStep(const FPlanStep& Step)
{
	FPlanValidation Validation;
	Validation.bValid = true;

	// Check for description
	if (Step.Description.IsEmpty())
	{
		Validation.Warnings.Add(TEXT("Step has no description"));
	}

	// Validate based on type
	switch (Step.Type)
	{
	case EPlanStepType::ToolCall:
	case EPlanStepType::Observation:
		if (Step.ToolName.IsEmpty())
		{
			Validation.bValid = false;
			Validation.Errors.Add(TEXT("Tool call step has no tool name"));
		}
		else if (!IsValidTool(Step.ToolName))
		{
			Validation.bValid = false;
			Validation.Errors.Add(FString::Printf(TEXT("Unknown tool: %s"), *Step.ToolName));
		}
		else
		{
			// Validate arguments
			TArray<FString> ArgErrors;
			if (!ValidateToolArguments(Step.ToolName, Step.ToolArguments, ArgErrors))
			{
				for (const FString& Error : ArgErrors)
				{
					Validation.Errors.Add(Error);
				}
				Validation.bValid = false;
			}
		}
		break;

	case EPlanStepType::Decision:
		if (Step.Condition.IsEmpty())
		{
			Validation.bValid = false;
			Validation.Errors.Add(TEXT("Decision step has no condition"));
		}
		break;

	case EPlanStepType::SubPlan:
		if (Step.SubPlanId.IsEmpty())
		{
			Validation.bValid = false;
			Validation.Errors.Add(TEXT("SubPlan step has no sub-plan ID"));
		}
		break;

	case EPlanStepType::Verification:
		// Verification steps need either a tool or outcomes to check
		if (Step.ToolName.IsEmpty() && Step.ExpectedOutcomes.Num() == 0)
		{
			Validation.Warnings.Add(TEXT("Verification step has no verification method"));
		}
		break;
	}

	return Validation;
}

bool FAgentPlanValidator::IsValidTool(const FString& ToolName)
{
	TArray<FString> ValidTools = GetValidToolNames();
	return ValidTools.Contains(ToolName);
}

bool FAgentPlanValidator::ValidateToolArguments(const FString& ToolName, const TMap<FString, FString>& Args, TArray<FString>& OutErrors)
{
	// Basic validation - more specific validation would check against tool schemas
	// For now, just ensure required arguments are present for known tools

	if (ToolName == TEXT("python_execute"))
	{
		if (!Args.Contains(TEXT("code")) || Args[TEXT("code")].IsEmpty())
		{
			OutErrors.Add(TEXT("python_execute requires 'code' argument"));
			return false;
		}
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		if (!Args.Contains(TEXT("actor_id")) || Args[TEXT("actor_id")].IsEmpty())
		{
			OutErrors.Add(TEXT("set_actor_transform requires 'actor_id' argument"));
			return false;
		}
	}
	else if (ToolName == TEXT("get_actor"))
	{
		if (!Args.Contains(TEXT("actor_id")) || Args[TEXT("actor_id")].IsEmpty())
		{
			OutErrors.Add(TEXT("get_actor requires 'actor_id' argument"));
			return false;
		}
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		if (!Args.Contains(TEXT("actor_id")) || Args[TEXT("actor_id")].IsEmpty())
		{
			OutErrors.Add(TEXT("duplicate_actor requires 'actor_id' argument"));
			return false;
		}
	}

	return true;
}

TArray<FString> FAgentPlanValidator::GetValidToolNames()
{
	// These should match the tools defined in UnrealGPTToolSchemas
	return TArray<FString>{
		TEXT("scene_query"),
		TEXT("get_actor"),
		TEXT("set_actor_transform"),
		TEXT("set_actors_rotation"),
		TEXT("select_actors"),
		TEXT("duplicate_actor"),
		TEXT("snap_actor_to_ground"),
		TEXT("python_execute"),
		TEXT("viewport_screenshot"),
		TEXT("reflection_query"),
		TEXT("replicate_generate")
	};
}
