// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.generated.h"

/**
 * Status of a goal in the agent system.
 */
UENUM(BlueprintType)
enum class EAgentGoalStatus : uint8
{
	Pending,        // Not yet started
	InProgress,     // Currently being worked on
	Completed,      // Successfully finished
	Failed,         // Could not be completed
	Blocked,        // Waiting on external input or dependency
	Cancelled       // User cancelled
};

/**
 * Type of success criterion for goal evaluation.
 */
UENUM(BlueprintType)
enum class ESuccessCriterionType : uint8
{
	SceneQuery,         // Verify via scene_query (actor count, properties)
	PropertyCheck,      // Check specific actor properties
	VisualVerification, // Use viewport screenshot + vision model
	AssetExists,        // Verify an asset was created/imported
	Custom              // Custom evaluation function
};

/**
 * Type of plan step.
 */
UENUM(BlueprintType)
enum class EPlanStepType : uint8
{
	ToolCall,       // Execute a tool
	Observation,    // Gather information (read-only)
	Verification,   // Verify a condition
	Decision,       // Branch based on condition
	SubPlan         // Nested plan for sub-goal
};

/**
 * Result status of executing a plan step.
 */
UENUM(BlueprintType)
enum class EStepResultStatus : uint8
{
	Success,            // Step completed successfully
	PartialSuccess,     // Step completed with warnings
	Failed,             // Step failed
	Blocked,            // Preconditions not met
	Skipped             // Step was skipped (conditional)
};

/**
 * Recovery action when a step fails.
 */
UENUM(BlueprintType)
enum class ERecoveryAction : uint8
{
	Retry,              // Try the same step again
	RetryWithFix,       // Modify parameters and retry
	Replan,             // Generate a new plan
	SkipStep,           // Skip this step and continue
	EscalateToUser,     // Ask user for help
	Abort               // Give up on the goal
};

/**
 * Confidence level for world model data.
 */
UENUM(BlueprintType)
enum class EActorConfidence : uint8
{
	Confirmed,      // Recently verified via scene query
	Assumed,        // Based on our actions, not verified
	Stale           // Data may be outdated
};

/**
 * Status of a plan.
 */
UENUM(BlueprintType)
enum class EPlanStatus : uint8
{
	Draft,          // Plan created but not validated
	Validated,      // Plan passed validation
	Executing,      // Currently being executed
	Completed,      // All steps finished successfully
	Failed,         // Plan failed during execution
	Replanning      // Generating new plan
};

/**
 * A single success criterion for evaluating goal completion.
 * YOUR CODE evaluates these, not the LLM.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FSuccessCriterion
{
	GENERATED_BODY()

	/** Human-readable description of what we're checking */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/** Type of criterion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ESuccessCriterionType Type = ESuccessCriterionType::SceneQuery;

	/**
	 * Query/check to perform. Format depends on Type:
	 * - SceneQuery: "class=StaticMeshActor, label contains 'Tree', count >= 10"
	 * - PropertyCheck: "actor.Transform.Location.Z > 0"
	 * - AssetExists: "/Game/MyAssets/NewTexture"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ValidationQuery;

	/** If true, goal cannot complete without this criterion passing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bRequired = true;

	/** Cached result of last evaluation */
	bool bLastEvaluationPassed = false;

	/** When this was last evaluated */
	FDateTime LastEvaluatedAt;
};

/**
 * A precondition that must be true before a step can execute.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FStepPrecondition
{
	GENERATED_BODY()

	/** Human-readable description */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/**
	 * Check to perform against world model.
	 * Example: "WorldModel.IsAreaClear(TargetBounds)"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString CheckExpression;

	/** If true, step cannot execute without this passing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bBlocksExecution = true;
};

/**
 * An expected outcome after a step executes.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FExpectedOutcome
{
	GENERATED_BODY()

	/** Human-readable description */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/**
	 * Verification to perform after step.
	 * Example: "WorldModel.FindActor(NewActorId) != null"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString VerificationExpression;

	/** If true, step fails if this doesn't pass */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bRequired = true;
};

/**
 * Suggested fix when a step fails.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FSuggestedFix
{
	GENERATED_BODY()

	/** What the fix does */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/** Parameter to modify */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ParameterName;

	/** New value for the parameter */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString NewValue;

	/** Confidence that this fix will work (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Confidence = 0.5f;
};

/**
 * Result of executing a tool.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FToolResult
{
	GENERATED_BODY()

	/** Was the tool execution successful */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bSuccess = false;

	/** Raw output from the tool */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString RawOutput;

	/** Parsed status field if available */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Status;

	/** Error message if failed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ErrorMessage;

	/** Any actor IDs created/modified */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> AffectedActorIds;

	/** Execution time in seconds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ExecutionTime = 0.0f;
};

/**
 * Result of checking preconditions.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FPreconditionResult
{
	GENERATED_BODY()

	/** Did all required preconditions pass */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bAllPassed = true;

	/** Which preconditions failed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> FailedPreconditions;

	/** Suggested actions to satisfy preconditions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> SuggestedActions;

	bool Passed() const { return bAllPassed; }
};

/**
 * Result of verifying outcomes.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FOutcomeResult
{
	GENERATED_BODY()

	/** Did all required outcomes pass */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bAllPassed = true;

	/** Which outcomes passed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> PassedOutcomes;

	/** Which outcomes failed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> FailedOutcomes;
};

/**
 * Complete result of executing a plan step.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FStepResult
{
	GENERATED_BODY()

	/** Overall status */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EStepResultStatus Status = EStepResultStatus::Failed;

	/** Tool execution result */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FToolResult ToolResult;

	/** Precondition check result */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FPreconditionResult PreconditionResult;

	/** Outcome verification result */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FOutcomeResult OutcomeResult;

	/** Human-readable summary */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Summary;

	/** Issues encountered */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Issues;

	/** Possible fixes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FSuggestedFix> SuggestedFixes;

	/** When this step was executed */
	FDateTime ExecutedAt;

	bool IsSuccess() const
	{
		return Status == EStepResultStatus::Success || Status == EStepResultStatus::PartialSuccess;
	}
};

/**
 * Result of evaluating a goal.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FGoalEvaluation
{
	GENERATED_BODY()

	/** Is the goal complete */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bComplete = false;

	/** Progress percentage (0-100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ProgressPercent = 0.0f;

	/** Which criteria passed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> PassedCriteria;

	/** Which criteria failed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> FailedCriteria;

	/** Summary of evaluation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Summary;

	bool IsComplete() const { return bComplete; }
};

/**
 * Validation result for a plan.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FPlanValidation
{
	GENERATED_BODY()

	/** Is the plan valid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bValid = false;

	/** Validation errors */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Errors;

	/** Validation warnings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Warnings;

	bool IsValid() const { return bValid; }
};

/**
 * A modification made to the world.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FWorldModification
{
	GENERATED_BODY()

	/** Type of modification */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ModificationType;  // "Created", "Modified", "Deleted"

	/** Actor ID affected */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ActorId;

	/** Which goal/step caused this */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString SourceGoalId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString SourceStepId;

	/** When this happened */
	FDateTime Timestamp;

	/** Can this be undone */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCanUndo = true;
};
