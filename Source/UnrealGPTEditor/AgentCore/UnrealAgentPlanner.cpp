// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentPlanner.h"
#include "UnrealAgentLLMInterface.h"

FAgentPlanner::FAgentPlanner()
{
}

// ==================== PLAN CREATION ====================

FAgentPlan FAgentPlanner::CreatePlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	// First, try to recognize the goal pattern
	EGoalPattern Pattern = DetectGoalPattern(Goal);

	FAgentPlan Plan;

	if (Pattern != EGoalPattern::Unknown)
	{
		// Use programmatic planning for known patterns
		Plan = CreatePlanProgrammatically(Goal, WorldModel);
	}
	else if (bUseLLMForPlanning && LLMInterface)
	{
		// Use LLM for unknown patterns
		Plan = CreatePlanFromLLMSuggestion(Goal, WorldModel);
	}
	else
	{
		// Fallback: create a minimal plan
		Plan.GoalId = Goal.GoalId;
		Plan.Summary = TEXT("Default plan - manual execution required");

		// Add a scene query step to gather context
		TMap<FString, FString> QueryArgs;
		QueryArgs.Add(TEXT("max_results"), TEXT("50"));
		Plan.AddObservation(TEXT("Query current scene state"), TEXT("scene_query"), QueryArgs);
	}

	// Inject additional steps as needed
	if (bAutoAddVerification && Goal.SuccessCriteria.Num() > 0)
	{
		InjectVerificationSteps(Plan, Goal);
	}

	// Validate the plan
	FPlanValidation Validation = ValidatePlan(Plan);
	if (Validation.IsValid())
	{
		Plan.MarkValidated();
	}

	return Plan;
}

FAgentPlan FAgentPlanner::CreatePlanFromLLMSuggestion(const FAgentGoal& Goal,
	const FAgentWorldModel& WorldModel)
{
	FAgentPlan Plan;
	Plan.GoalId = Goal.GoalId;

	if (!LLMInterface)
	{
		Plan.Summary = TEXT("LLM interface not available");
		return Plan;
	}

	// Build prompt for LLM
	FString Prompt = BuildPlanPrompt(Goal, WorldModel);

	// Get LLM suggestion
	FString LLMResponse = LLMInterface->SuggestPlan(Goal, WorldModel);

	// Parse steps from response
	TArray<FPlanStep> Steps = ParseLLMPlanResponse(LLMResponse);

	// Validate and add each step
	for (FPlanStep& Step : Steps)
	{
		Step = SanitizeStep(Step);
		FPlanValidation StepValidation = FAgentPlanValidator::ValidateStep(Step);

		if (StepValidation.IsValid())
		{
			Plan.AddStep(Step);
		}
		// Invalid steps are dropped with a warning
	}

	Plan.Summary = FString::Printf(TEXT("LLM-suggested plan: %d steps"), Plan.Steps.Num());
	Plan.Rationale = TEXT("Generated from LLM suggestion based on goal description");

	return Plan;
}

FAgentPlan FAgentPlanner::CreatePlanProgrammatically(const FAgentGoal& Goal,
	const FAgentWorldModel& WorldModel)
{
	EGoalPattern Pattern = DetectGoalPattern(Goal);

	switch (Pattern)
	{
	case EGoalPattern::SpawnObjects:
		return GenerateSpawnPlan(Goal, WorldModel);

	case EGoalPattern::TransformObjects:
		return GenerateTransformPlan(Goal, WorldModel);

	case EGoalPattern::QueryScene:
		return GenerateInspectionPlan(Goal, WorldModel);

	case EGoalPattern::ArrangeObjects:
		return GenerateArrangementPlan(Goal, WorldModel);

	default:
		{
			FAgentPlan Plan;
			Plan.GoalId = Goal.GoalId;
			Plan.Summary = TEXT("Unknown pattern - generic plan");
			return Plan;
		}
	}
}

// ==================== PLAN VALIDATION ====================

FPlanValidation FAgentPlanner::ValidatePlan(const FAgentPlan& Plan)
{
	return FAgentPlanValidator::ValidatePlan(Plan);
}

FAgentPlan FAgentPlanner::ValidateAndFixPlan(const FAgentPlan& Plan, const FAgentWorldModel& WorldModel)
{
	FAgentPlan FixedPlan = Plan;

	for (int32 i = 0; i < FixedPlan.Steps.Num(); i++)
	{
		FPlanStep& Step = FixedPlan.Steps[i];

		// Sanitize the step
		Step = SanitizeStep(Step);

		// Add preconditions based on world model
		AddPreconditionsFromWorldModel(Step, WorldModel);
	}

	// Re-validate
	FPlanValidation Validation = ValidatePlan(FixedPlan);
	if (Validation.IsValid())
	{
		FixedPlan.MarkValidated();
	}

	return FixedPlan;
}

// ==================== REPLANNING ====================

FAgentPlan FAgentPlanner::ReplanFromFailure(const FAgentPlan& FailedPlan, int32 FailedStepIndex,
	const FStepResult& FailedResult, const FAgentWorldModel& WorldModel)
{
	FAgentPlan NewPlan;
	NewPlan.GoalId = FailedPlan.GoalId;
	NewPlan.Rationale = FString::Printf(TEXT("Replan after step %d failed: %s"),
		FailedStepIndex + 1, *FailedResult.Summary);

	// Option 1: Skip the failed step and continue with remaining
	// Option 2: Generate alternative approach
	// Option 3: Add recovery steps before retrying

	// For now, implement option 1: skip and continue
	for (int32 i = FailedStepIndex + 1; i < FailedPlan.Steps.Num(); i++)
	{
		NewPlan.AddStep(FailedPlan.Steps[i]);
	}

	// If we have an LLM, ask for alternative approach
	if (bUseLLMForPlanning && LLMInterface && NewPlan.Steps.Num() == 0)
	{
		// Need completely new approach
		FAgentGoal TempGoal;
		TempGoal.GoalId = FailedPlan.GoalId;
		TempGoal.Description = FString::Printf(TEXT("Alternative approach needed. Previous attempt failed: %s"),
			*FailedResult.Summary);

		FString Response = LLMInterface->SuggestRecoveryPlan(FailedPlan, FailedResult, WorldModel);
		TArray<FPlanStep> RecoverySteps = ParseLLMPlanResponse(Response);

		for (FPlanStep& Step : RecoverySteps)
		{
			Step = SanitizeStep(Step);
			NewPlan.AddStep(Step);
		}
	}

	NewPlan.Summary = FString::Printf(TEXT("Recovery plan: %d steps"), NewPlan.Steps.Num());

	FPlanValidation Validation = ValidatePlan(NewPlan);
	if (Validation.IsValid())
	{
		NewPlan.MarkValidated();
	}

	return NewPlan;
}

FAgentPlan FAgentPlanner::AdaptPlan(const FAgentPlan& OriginalPlan, const FAgentWorldModel& WorldModel)
{
	FAgentPlan AdaptedPlan = OriginalPlan;

	// Check preconditions for remaining steps against current world state
	for (int32 i = OriginalPlan.CurrentStepIndex; i < AdaptedPlan.Steps.Num(); i++)
	{
		FPlanStep& Step = AdaptedPlan.Steps[i];

		// Update preconditions based on current world model
		AddPreconditionsFromWorldModel(Step, WorldModel);

		// Check if step's target actors still exist
		const FString* ActorId = Step.ToolArguments.Find(TEXT("actor_id"));
		if (ActorId && !WorldModel.FindActor(*ActorId))
		{
			// Actor no longer exists - mark step for skip or update
			Step.AddPrecondition(TEXT("Target actor exists"),
				FString::Printf(TEXT("WorldModel.FindActor('%s') != null"), **ActorId), true);
		}
	}

	return AdaptedPlan;
}

// ==================== STEP GENERATION ====================

FPlanStep FAgentPlanner::CreateObservationStep(const FString& Description, const FActorQuery& Query)
{
	TMap<FString, FString> Args;

	if (!Query.ClassFilter.IsEmpty())
	{
		Args.Add(TEXT("class"), Query.ClassFilter);
	}
	if (!Query.LabelFilter.IsEmpty())
	{
		Args.Add(TEXT("label"), Query.LabelFilter);
	}
	if (!Query.TagFilter.IsEmpty())
	{
		Args.Add(TEXT("tag"), Query.TagFilter);
	}
	Args.Add(TEXT("max_results"), FString::FromInt(Query.MaxResults));

	return FPlanStep::CreateObservation(Description, TEXT("scene_query"), Args);
}

FPlanStep FAgentPlanner::CreateVerificationStep(const FString& Description, const FSuccessCriterion& Criterion)
{
	return FPlanStep::CreateVerification(Description, Criterion.ValidationQuery);
}

FPlanStep FAgentPlanner::CreateToolCallStep(const FString& Description, const FString& ToolName,
	const TMap<FString, FString>& Args)
{
	FPlanStep Step = FPlanStep::CreateToolCall(Description, ToolName, Args);

	// Validate immediately
	FPlanValidation Validation = FAgentPlanValidator::ValidateStep(Step);
	if (!Validation.IsValid())
	{
		// Log validation errors but still return the step
		for (const FString& Error : Validation.Errors)
		{
			UE_LOG(LogTemp, Warning, TEXT("Step validation warning: %s"), *Error);
		}
	}

	return Step;
}

// ==================== PRIVATE: LLM PLAN PARSING ====================

TArray<FPlanStep> FAgentPlanner::ParseLLMPlanResponse(const FString& Response)
{
	TArray<FPlanStep> Steps;

	// Parse numbered list format:
	// 1. scene_query: Query current actors
	// 2. python_execute: Spawn trees at positions
	// 3. scene_query: Verify spawned actors

	TArray<FString> Lines;
	Response.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();

		// Skip empty lines
		if (TrimmedLine.IsEmpty())
		{
			continue;
		}

		// Look for numbered steps (1. 2. etc) or bullet points (- *)
		int32 ContentStart = 0;
		if (TrimmedLine.Len() > 2)
		{
			if (FChar::IsDigit(TrimmedLine[0]) && TrimmedLine[1] == '.')
			{
				ContentStart = 2;
			}
			else if (TrimmedLine[0] == '-' || TrimmedLine[0] == '*')
			{
				ContentStart = 1;
			}
		}

		if (ContentStart == 0)
		{
			continue;  // Not a step line
		}

		FString StepContent = TrimmedLine.Mid(ContentStart).TrimStart();

		// Parse tool name and description
		// Format: "tool_name: description" or "tool_name(args): description"
		int32 ColonPos = StepContent.Find(TEXT(":"));
		if (ColonPos == INDEX_NONE)
		{
			continue;
		}

		FString ToolPart = StepContent.Left(ColonPos).TrimStartAndEnd();
		FString Description = StepContent.Mid(ColonPos + 1).TrimStart();

		// Extract tool name (might have args in parens)
		FString ToolName = ToolPart;
		TMap<FString, FString> Args;

		int32 ParenStart = ToolPart.Find(TEXT("("));
		if (ParenStart != INDEX_NONE)
		{
			ToolName = ToolPart.Left(ParenStart);
			// Would parse args from parens here
		}

		// Create the step
		FPlanStep Step;
		Step.Description = Description;
		Step.ToolName = ToolName.ToLower();  // Normalize to lowercase

		// Determine step type
		if (ToolName.ToLower() == TEXT("scene_query") || ToolName.ToLower() == TEXT("get_actor"))
		{
			Step.Type = EPlanStepType::Observation;
		}
		else
		{
			Step.Type = EPlanStepType::ToolCall;
		}

		Steps.Add(Step);
	}

	return Steps;
}

FPlanStep FAgentPlanner::SanitizeStep(const FPlanStep& Step)
{
	FPlanStep Sanitized = Step;

	// Normalize tool name to lowercase
	Sanitized.ToolName = Step.ToolName.ToLower();

	// Fix common tool name typos/aliases
	if (Sanitized.ToolName == TEXT("query") || Sanitized.ToolName == TEXT("scene query"))
	{
		Sanitized.ToolName = TEXT("scene_query");
	}
	else if (Sanitized.ToolName == TEXT("python") || Sanitized.ToolName == TEXT("execute"))
	{
		Sanitized.ToolName = TEXT("python_execute");
	}
	else if (Sanitized.ToolName == TEXT("screenshot") || Sanitized.ToolName == TEXT("capture"))
	{
		Sanitized.ToolName = TEXT("viewport_screenshot");
	}
	else if (Sanitized.ToolName == TEXT("transform") || Sanitized.ToolName == TEXT("move"))
	{
		Sanitized.ToolName = TEXT("set_actor_transform");
	}
	else if (Sanitized.ToolName == TEXT("duplicate") || Sanitized.ToolName == TEXT("clone"))
	{
		Sanitized.ToolName = TEXT("duplicate_actor");
	}

	// Ensure step has a description
	if (Sanitized.Description.IsEmpty())
	{
		Sanitized.Description = FString::Printf(TEXT("Execute %s"), *Sanitized.ToolName);
	}

	return Sanitized;
}

FString FAgentPlanner::BuildPlanPrompt(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FString Prompt;

	Prompt += TEXT("Create a step-by-step plan to achieve this goal:\n");
	Prompt += FString::Printf(TEXT("Goal: %s\n\n"), *Goal.Description);

	// Add success criteria
	if (Goal.SuccessCriteria.Num() > 0)
	{
		Prompt += TEXT("Success criteria:\n");
		for (const FSuccessCriterion& Criterion : Goal.SuccessCriteria)
		{
			Prompt += FString::Printf(TEXT("- %s\n"), *Criterion.Description);
		}
		Prompt += TEXT("\n");
	}

	// Add world context
	Prompt += FString::Printf(TEXT("Current scene has %d known actors.\n\n"), WorldModel.GetActorCount());

	// Add available tools
	Prompt += TEXT("Available tools:\n");
	Prompt += TEXT("- scene_query: Query actors by class, label, or tag\n");
	Prompt += TEXT("- get_actor: Get detailed info about a specific actor\n");
	Prompt += TEXT("- set_actor_transform: Move/rotate/scale an actor\n");
	Prompt += TEXT("- duplicate_actor: Clone an actor\n");
	Prompt += TEXT("- python_execute: Run Python code in Unreal\n");
	Prompt += TEXT("- viewport_screenshot: Capture the viewport\n");
	Prompt += TEXT("\n");

	Prompt += TEXT("Return a numbered list of steps in format:\n");
	Prompt += TEXT("1. tool_name: description of what this step does\n");

	return Prompt;
}

// ==================== PRIVATE: PROGRAMMATIC PLAN GENERATION ====================

FAgentPlan FAgentPlanner::GenerateSpawnPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FAgentPlan Plan;
	Plan.GoalId = Goal.GoalId;
	Plan.Summary = TEXT("Spawn objects plan");
	Plan.Rationale = TEXT("Programmatic plan for object spawning");

	// Step 1: Query current scene to find a good spawn location
	TMap<FString, FString> QueryArgs;
	QueryArgs.Add(TEXT("max_results"), TEXT("20"));
	Plan.AddObservation(TEXT("Query current scene state"), TEXT("scene_query"), QueryArgs);

	// Step 2: Execute Python code to spawn objects
	TMap<FString, FString> PythonArgs;

	// Extract count from goal parameters
	FString CountStr = Goal.GetParameter(TEXT("count"), TEXT("1"));
	int32 Count = FCString::Atoi(*CountStr);
	if (Count <= 0) Count = 1;

	FString ObjectType = Goal.GetParameter(TEXT("object_type"), TEXT("cube"));

	// Generate actual Python code to spawn objects
	FString PythonCode = TEXT("import unreal\n");
	PythonCode += TEXT("import math\n\n");
	PythonCode += TEXT("# Spawn actors at the center of the level\n");
	PythonCode += TEXT("center = unreal.Vector(0, 0, 100)\n");
	PythonCode += TEXT("spacing = 200.0\n\n");
	PythonCode += FString::Printf(TEXT("count = %d\n"), Count);
	PythonCode += TEXT("spawned_actors = []\n\n");
	PythonCode += TEXT("for i in range(count):\n");
	PythonCode += TEXT("    # Calculate position in a grid\n");
	PythonCode += TEXT("    row = i // 5\n");
	PythonCode += TEXT("    col = i % 5\n");
	PythonCode += TEXT("    x = center.x + col * spacing\n");
	PythonCode += TEXT("    y = center.y + row * spacing\n");
	PythonCode += TEXT("    z = center.z\n");
	PythonCode += TEXT("    location = unreal.Vector(x, y, z)\n\n");
	PythonCode += TEXT("    # Spawn a cube static mesh actor\n");
	PythonCode += TEXT("    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(\n");
	PythonCode += TEXT("        unreal.StaticMeshActor,\n");
	PythonCode += TEXT("        location,\n");
	PythonCode += TEXT("        unreal.Rotator(0, 0, 0)\n");
	PythonCode += TEXT("    )\n");
	PythonCode += TEXT("    if actor:\n");
	PythonCode += TEXT("        # Set the cube mesh\n");
	PythonCode += TEXT("        mesh = unreal.EditorAssetLibrary.load_asset('/Engine/BasicShapes/Cube')\n");
	PythonCode += TEXT("        if mesh:\n");
	PythonCode += TEXT("            actor.static_mesh_component.set_static_mesh(mesh)\n");
	PythonCode += FString::Printf(TEXT("        actor.set_actor_label('Spawned_%s_{}'.format(i))\n"), *ObjectType);
	PythonCode += TEXT("        spawned_actors.append(actor.get_name())\n\n");
	PythonCode += TEXT("print(f'Spawned {len(spawned_actors)} actors')\n");

	PythonArgs.Add(TEXT("code"), PythonCode);
	Plan.AddToolCall(TEXT("Spawn objects via Python"), TEXT("python_execute"), PythonArgs);

	// Step 3: Verify the spawn
	Plan.AddVerification(TEXT("Verify objects were spawned"), TEXT("count >= 1"));

	return Plan;
}

FAgentPlan FAgentPlanner::GenerateTransformPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FAgentPlan Plan;
	Plan.GoalId = Goal.GoalId;
	Plan.Summary = TEXT("Transform objects plan");

	// Step 1: Find the target actor(s)
	TMap<FString, FString> QueryArgs;
	FString TargetLabel = Goal.GetParameter(TEXT("target"), TEXT(""));
	if (!TargetLabel.IsEmpty())
	{
		QueryArgs.Add(TEXT("label"), TargetLabel);
	}
	Plan.AddObservation(TEXT("Find target actor(s)"), TEXT("scene_query"), QueryArgs);

	// Step 2: Apply transform
	TMap<FString, FString> TransformArgs;
	TransformArgs.Add(TEXT("actor_id"), TEXT("<from_query>"));  // Placeholder

	FString Location = Goal.GetParameter(TEXT("location"), TEXT(""));
	if (!Location.IsEmpty())
	{
		TransformArgs.Add(TEXT("location"), Location);
	}

	FString Rotation = Goal.GetParameter(TEXT("rotation"), TEXT(""));
	if (!Rotation.IsEmpty())
	{
		TransformArgs.Add(TEXT("rotation"), Rotation);
	}

	Plan.AddToolCall(TEXT("Apply transform to actor"), TEXT("set_actor_transform"), TransformArgs);

	// Step 3: Verify
	Plan.AddVerification(TEXT("Verify transform was applied"), TEXT(""));

	return Plan;
}

FAgentPlan FAgentPlanner::GenerateInspectionPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FAgentPlan Plan;
	Plan.GoalId = Goal.GoalId;
	Plan.Summary = TEXT("Inspect scene plan");

	// Single step: query the scene
	TMap<FString, FString> QueryArgs;
	QueryArgs.Add(TEXT("max_results"), TEXT("100"));

	FString ClassFilter = Goal.GetParameter(TEXT("class"), TEXT(""));
	if (!ClassFilter.IsEmpty())
	{
		QueryArgs.Add(TEXT("class"), ClassFilter);
	}

	FString LabelFilter = Goal.GetParameter(TEXT("label"), TEXT(""));
	if (!LabelFilter.IsEmpty())
	{
		QueryArgs.Add(TEXT("label"), LabelFilter);
	}

	Plan.AddObservation(TEXT("Query scene"), TEXT("scene_query"), QueryArgs);

	return Plan;
}

FAgentPlan FAgentPlanner::GenerateArrangementPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FAgentPlan Plan;
	Plan.GoalId = Goal.GoalId;
	Plan.Summary = TEXT("Arrange objects plan");

	// Step 1: Query current scene
	TMap<FString, FString> QueryArgs;
	QueryArgs.Add(TEXT("max_results"), TEXT("20"));
	Plan.AddObservation(TEXT("Query current scene state"), TEXT("scene_query"), QueryArgs);

	// Step 2: Execute Python to create arrangement
	TMap<FString, FString> PythonArgs;

	// Parse goal description for arrangement parameters
	FString Desc = Goal.Description.ToLower();

	// Detect pattern type
	FString Pattern = TEXT("circle");
	if (Desc.Contains(TEXT("grid"))) Pattern = TEXT("grid");
	else if (Desc.Contains(TEXT("line"))) Pattern = TEXT("line");
	else if (Desc.Contains(TEXT("circle")) || Desc.Contains(TEXT("circular"))) Pattern = TEXT("circle");

	// Detect object type
	FString ObjectType = TEXT("cube");
	if (Desc.Contains(TEXT("pillar"))) ObjectType = TEXT("cylinder");
	else if (Desc.Contains(TEXT("sphere"))) ObjectType = TEXT("sphere");
	else if (Desc.Contains(TEXT("cone"))) ObjectType = TEXT("cone");
	else if (Desc.Contains(TEXT("cube"))) ObjectType = TEXT("cube");

	// Detect count - look for numbers in description
	int32 Count = 8;  // Default
	FString CountStr = Goal.GetParameter(TEXT("count"), TEXT(""));
	if (!CountStr.IsEmpty())
	{
		Count = FCString::Atoi(*CountStr);
	}
	else
	{
		// Try to extract number from description
		for (int32 i = 1; i <= 100; i++)
		{
			if (Desc.Contains(FString::FromInt(i)))
			{
				Count = i;
				break;
			}
		}
	}

	// Detect radius
	float Radius = 500.0f;  // Default radius in units

	// Map object type to mesh path
	FString MeshPath = TEXT("/Engine/BasicShapes/Cube");
	if (ObjectType == TEXT("cylinder")) MeshPath = TEXT("/Engine/BasicShapes/Cylinder");
	else if (ObjectType == TEXT("sphere")) MeshPath = TEXT("/Engine/BasicShapes/Sphere");
	else if (ObjectType == TEXT("cone")) MeshPath = TEXT("/Engine/BasicShapes/Cone");

	// Generate actual Python code for the arrangement
	FString PythonCode = TEXT("import unreal\n");
	PythonCode += TEXT("import math\n\n");

	if (Pattern == TEXT("circle"))
	{
		PythonCode += TEXT("# Create circular arrangement\n");
		PythonCode += TEXT("center = unreal.Vector(0, 0, 100)  # Center of the level, slightly above ground\n");
		PythonCode += FString::Printf(TEXT("radius = %.1f\n"), Radius);
		PythonCode += FString::Printf(TEXT("count = %d\n"), Count);
		PythonCode += TEXT("spawned_actors = []\n\n");
		PythonCode += TEXT("for i in range(count):\n");
		PythonCode += TEXT("    # Calculate position on circle\n");
		PythonCode += TEXT("    angle = (2 * math.pi * i) / count\n");
		PythonCode += TEXT("    x = center.x + radius * math.cos(angle)\n");
		PythonCode += TEXT("    y = center.y + radius * math.sin(angle)\n");
		PythonCode += TEXT("    z = center.z\n");
		PythonCode += TEXT("    location = unreal.Vector(x, y, z)\n\n");
		PythonCode += TEXT("    # Calculate rotation to face center\n");
		PythonCode += TEXT("    yaw = math.degrees(angle) + 90  # Face inward\n");
		PythonCode += TEXT("    rotation = unreal.Rotator(0, yaw, 0)\n\n");
		PythonCode += TEXT("    # Spawn the actor\n");
		PythonCode += TEXT("    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(\n");
		PythonCode += TEXT("        unreal.StaticMeshActor,\n");
		PythonCode += TEXT("        location,\n");
		PythonCode += TEXT("        rotation\n");
		PythonCode += TEXT("    )\n");
		PythonCode += TEXT("    if actor:\n");
		PythonCode += FString::Printf(TEXT("        mesh = unreal.EditorAssetLibrary.load_asset('%s')\n"), *MeshPath);
		PythonCode += TEXT("        if mesh:\n");
		PythonCode += TEXT("            actor.static_mesh_component.set_static_mesh(mesh)\n");
		PythonCode += FString::Printf(TEXT("        actor.set_actor_label('%s_{}'.format(i))\n"), *ObjectType.ToUpper());
		PythonCode += TEXT("        spawned_actors.append(actor.get_name())\n\n");
		PythonCode += TEXT("print(f'Created circular arrangement with {len(spawned_actors)} objects')\n");
	}
	else if (Pattern == TEXT("grid"))
	{
		int32 Cols = FMath::CeilToInt(FMath::Sqrt((float)Count));
		PythonCode += TEXT("# Create grid arrangement\n");
		PythonCode += TEXT("center = unreal.Vector(0, 0, 100)\n");
		PythonCode += TEXT("spacing = 200.0\n");
		PythonCode += FString::Printf(TEXT("count = %d\n"), Count);
		PythonCode += FString::Printf(TEXT("cols = %d\n"), Cols);
		PythonCode += TEXT("spawned_actors = []\n\n");
		PythonCode += TEXT("for i in range(count):\n");
		PythonCode += TEXT("    row = i // cols\n");
		PythonCode += TEXT("    col = i % cols\n");
		PythonCode += TEXT("    x = center.x + (col - cols/2) * spacing\n");
		PythonCode += TEXT("    y = center.y + (row - count/cols/2) * spacing\n");
		PythonCode += TEXT("    z = center.z\n");
		PythonCode += TEXT("    location = unreal.Vector(x, y, z)\n");
		PythonCode += TEXT("    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(\n");
		PythonCode += TEXT("        unreal.StaticMeshActor,\n");
		PythonCode += TEXT("        location,\n");
		PythonCode += TEXT("        unreal.Rotator(0, 0, 0)\n");
		PythonCode += TEXT("    )\n");
		PythonCode += TEXT("    if actor:\n");
		PythonCode += FString::Printf(TEXT("        mesh = unreal.EditorAssetLibrary.load_asset('%s')\n"), *MeshPath);
		PythonCode += TEXT("        if mesh:\n");
		PythonCode += TEXT("            actor.static_mesh_component.set_static_mesh(mesh)\n");
		PythonCode += FString::Printf(TEXT("        actor.set_actor_label('%s_{}'.format(i))\n"), *ObjectType.ToUpper());
		PythonCode += TEXT("        spawned_actors.append(actor.get_name())\n\n");
		PythonCode += TEXT("print(f'Created grid arrangement with {len(spawned_actors)} objects')\n");
	}
	else  // line
	{
		PythonCode += TEXT("# Create line arrangement\n");
		PythonCode += TEXT("start = unreal.Vector(-500, 0, 100)\n");
		PythonCode += TEXT("spacing = 200.0\n");
		PythonCode += FString::Printf(TEXT("count = %d\n"), Count);
		PythonCode += TEXT("spawned_actors = []\n\n");
		PythonCode += TEXT("for i in range(count):\n");
		PythonCode += TEXT("    x = start.x + i * spacing\n");
		PythonCode += TEXT("    location = unreal.Vector(x, start.y, start.z)\n");
		PythonCode += TEXT("    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(\n");
		PythonCode += TEXT("        unreal.StaticMeshActor,\n");
		PythonCode += TEXT("        location,\n");
		PythonCode += TEXT("        unreal.Rotator(0, 0, 0)\n");
		PythonCode += TEXT("    )\n");
		PythonCode += TEXT("    if actor:\n");
		PythonCode += FString::Printf(TEXT("        mesh = unreal.EditorAssetLibrary.load_asset('%s')\n"), *MeshPath);
		PythonCode += TEXT("        if mesh:\n");
		PythonCode += TEXT("            actor.static_mesh_component.set_static_mesh(mesh)\n");
		PythonCode += FString::Printf(TEXT("        actor.set_actor_label('%s_{}'.format(i))\n"), *ObjectType.ToUpper());
		PythonCode += TEXT("        spawned_actors.append(actor.get_name())\n\n");
		PythonCode += TEXT("print(f'Created line arrangement with {len(spawned_actors)} objects')\n");
	}

	PythonArgs.Add(TEXT("code"), PythonCode);
	Plan.AddToolCall(TEXT("Arrange objects via Python"), TEXT("python_execute"), PythonArgs);

	// Step 3: Verify
	TMap<FString, FString> VerifyQueryArgs;
	VerifyQueryArgs.Add(TEXT("label_contains"), ObjectType.ToUpper());
	Plan.AddObservation(TEXT("Verify objects were spawned"), TEXT("scene_query"), VerifyQueryArgs);

	return Plan;
}

// ==================== PRIVATE: STEP INJECTION ====================

void FAgentPlanner::InjectObservationSteps(FAgentPlan& Plan, const FAgentWorldModel& WorldModel)
{
	// If world model is stale, add observation at start
	if (WorldModel.NeedsRefresh())
	{
		TMap<FString, FString> QueryArgs;
		QueryArgs.Add(TEXT("max_results"), TEXT("50"));
		FPlanStep ObsStep = FPlanStep::CreateObservation(TEXT("Refresh scene state"), TEXT("scene_query"), QueryArgs);

		// Insert at beginning
		Plan.Steps.Insert(ObsStep, 0);
	}
}

void FAgentPlanner::InjectVerificationSteps(FAgentPlan& Plan, const FAgentGoal& Goal)
{
	// Add verification step at end for each success criterion
	for (const FSuccessCriterion& Criterion : Goal.SuccessCriteria)
	{
		if (Criterion.bRequired)
		{
			FPlanStep VerifyStep = CreateVerificationStep(
				FString::Printf(TEXT("Verify: %s"), *Criterion.Description),
				Criterion);
			Plan.AddStep(VerifyStep);
		}
	}
}

void FAgentPlanner::AddPreconditionsFromWorldModel(FPlanStep& Step, const FAgentWorldModel& WorldModel)
{
	// If step operates on an actor, add precondition that actor exists
	const FString* ActorId = Step.ToolArguments.Find(TEXT("actor_id"));
	if (ActorId && !ActorId->IsEmpty() && *ActorId != TEXT("<from_query>"))
	{
		Step.AddPrecondition(
			TEXT("Target actor exists"),
			FString::Printf(TEXT("WorldModel.FindActor('%s') != null"), **ActorId),
			true);

		// Also check if we can modify it
		Step.AddPrecondition(
			TEXT("Target actor can be modified"),
			FString::Printf(TEXT("WorldModel.CanModifyActor('%s')"), **ActorId),
			true);
	}
}

// ==================== PRIVATE: PATTERN RECOGNITION ====================

FAgentPlanner::EGoalPattern FAgentPlanner::DetectGoalPattern(const FAgentGoal& Goal)
{
	FString Desc = Goal.Description.ToLower();

	// IMPORTANT: Check more specific patterns BEFORE generic ones
	// "create a circular arrangement" should match ArrangeObjects, not SpawnObjects

	// Arrangement patterns - check FIRST because they may contain "create" or "spawn"
	if (Desc.Contains(TEXT("arrange")) || Desc.Contains(TEXT("arrangement")) ||
		Desc.Contains(TEXT("layout")) || Desc.Contains(TEXT("organize")) ||
		Desc.Contains(TEXT("circular")) ||
		// Pattern + action combinations
		(Desc.Contains(TEXT("circle")) && (Desc.Contains(TEXT("create")) || Desc.Contains(TEXT("make")) || Desc.Contains(TEXT("spawn")))) ||
		(Desc.Contains(TEXT("grid")) && (Desc.Contains(TEXT("create")) || Desc.Contains(TEXT("make")) || Desc.Contains(TEXT("spawn")))) ||
		(Desc.Contains(TEXT("line")) && (Desc.Contains(TEXT("create")) || Desc.Contains(TEXT("make")) || Desc.Contains(TEXT("spawn")))))
	{
		return EGoalPattern::ArrangeObjects;
	}

	// Delete patterns - check before spawn (in case of "delete and create")
	if (Desc.Contains(TEXT("delete")) || Desc.Contains(TEXT("remove")) ||
		Desc.Contains(TEXT("clear")))
	{
		return EGoalPattern::DeleteObjects;
	}

	// Transform patterns
	if (Desc.Contains(TEXT("move")) || Desc.Contains(TEXT("rotate")) ||
		Desc.Contains(TEXT("scale")) || Desc.Contains(TEXT("transform")))
	{
		return EGoalPattern::TransformObjects;
	}

	// Query patterns
	if (Desc.Contains(TEXT("find")) || Desc.Contains(TEXT("list")) ||
		Desc.Contains(TEXT("show")) || Desc.Contains(TEXT("count")) ||
		Desc.Contains(TEXT("what")))
	{
		return EGoalPattern::QueryScene;
	}

	// Modify patterns
	if (Desc.Contains(TEXT("change")) || Desc.Contains(TEXT("set")) ||
		Desc.Contains(TEXT("update")) || Desc.Contains(TEXT("modify")))
	{
		return EGoalPattern::ModifyProperties;
	}

	// Spawn/create patterns - check LAST as a fallback for generic creation
	if (Desc.Contains(TEXT("spawn")) || Desc.Contains(TEXT("place")) ||
		Desc.Contains(TEXT("create")) || Desc.Contains(TEXT("add")) ||
		Desc.Contains(TEXT("make")))
	{
		return EGoalPattern::SpawnObjects;
	}

	return EGoalPattern::Unknown;
}

FString FAgentPlanner::GoalPatternToString(EGoalPattern Pattern)
{
	switch (Pattern)
	{
	case EGoalPattern::SpawnObjects: return TEXT("SpawnObjects");
	case EGoalPattern::TransformObjects: return TEXT("TransformObjects");
	case EGoalPattern::QueryScene: return TEXT("QueryScene");
	case EGoalPattern::ArrangeObjects: return TEXT("ArrangeObjects");
	case EGoalPattern::DeleteObjects: return TEXT("DeleteObjects");
	case EGoalPattern::ModifyProperties: return TEXT("ModifyProperties");
	default: return TEXT("Unknown");
	}
}

FString FAgentPlanner::RecognizeGoalPattern(const FAgentGoal& Goal)
{
	return GoalPatternToString(DetectGoalPattern(Goal));
}

FAgentPlan FAgentPlanner::GetPlanTemplate(const FString& TaskType)
{
	FAgentPlan Template;

	if (TaskType == TEXT("SpawnObjects"))
	{
		Template.Summary = TEXT("Spawn objects template");
		// Add template steps...
	}
	else if (TaskType == TEXT("ArrangeObjects"))
	{
		Template.Summary = TEXT("Arrange objects template");
		// Add template steps...
	}

	return Template;
}
