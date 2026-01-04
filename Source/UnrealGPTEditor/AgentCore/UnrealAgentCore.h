// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

/**
 * UnrealAgentCore - True Agent Architecture for Unreal Engine
 *
 * This module provides a proper agent architecture where YOUR CODE controls
 * the decision-making, not the LLM. The LLM becomes an advisor/tool.
 *
 * Key Components:
 * ---------------
 *
 * 1. Goals (UnrealAgentGoal.h)
 *    - FAgentGoal: Explicit representation of what we're trying to achieve
 *    - FSuccessCriterion: Success conditions YOUR CODE evaluates
 *    - FAgentGoalManager: Lifecycle management for goals
 *
 * 2. Plans (UnrealAgentPlan.h)
 *    - FPlanStep: Individual action with preconditions and expected outcomes
 *    - FAgentPlan: Ordered sequence of steps to achieve a goal
 *    - FAgentPlanValidator: Validates plans before execution
 *
 * 3. World Model (UnrealAgentWorldModel.h)
 *    - FActorState: Cached state of an actor (may be stale/assumed)
 *    - FAgentWorldModel: Agent's understanding of scene state
 *    - FAgentWorldModelManager: Synchronization with actual scene
 *
 * 4. Types (UnrealAgentTypes.h)
 *    - Enums for statuses, step types, recovery actions
 *    - Result structures for steps, tools, evaluations
 *    - Preconditions and expected outcomes
 *
 *
 * Architecture Overview:
 * ----------------------
 *
 *   User Request
 *        │
 *        ▼
 *   ┌─────────────────┐
 *   │  Goal Manager   │  ← Parse request into explicit goal with success criteria
 *   └────────┬────────┘
 *            │
 *            ▼
 *   ┌─────────────────┐
 *   │    Planner      │  ← Generate validated plan (LLM suggests, YOU validate)
 *   └────────┬────────┘
 *            │
 *            ▼
 *   ┌─────────────────┐
 *   │   Executor      │  ← Execute steps with precondition checks
 *   └────────┬────────┘
 *            │
 *            ▼
 *   ┌─────────────────┐
 *   │  World Model    │  ← Update state based on results
 *   └────────┬────────┘
 *            │
 *            ▼
 *   ┌─────────────────┐
 *   │   Evaluator     │  ← YOUR CODE judges success, not LLM
 *   └────────┬────────┘
 *            │
 *            ▼
 *        Complete?
 *        /      \
 *      Yes       No → Replan/Retry
 *       │
 *       ▼
 *     Done
 *
 *
 * Key Differences from LLM-Controlled:
 * ------------------------------------
 *
 * | Aspect          | LLM-Controlled         | True Agent              |
 * |-----------------|------------------------|-------------------------|
 * | Next action     | LLM decides            | Your code decides       |
 * | Success check   | LLM "thinks" it worked | Your code verifies      |
 * | Goal tracking   | Implicit in prompt     | Explicit data structure |
 * | Planning        | LLM "plans" in text    | Validated step sequence |
 * | World state     | Conversation history   | Explicit world model    |
 * | Recovery        | LLM might loop         | Code enforces limits    |
 *
 *
 * Usage Example:
 * --------------
 *
 * ```cpp
 * // 1. Create goal with explicit success criteria
 * FAgentGoal Goal;
 * Goal.Description = "Place 10 trees in a circle";
 * Goal.AddCriterion(
 *     "10 trees exist",
 *     ESuccessCriterionType::SceneQuery,
 *     "class=StaticMeshActor, label contains 'Tree', count >= 10"
 * );
 *
 * // 2. Create plan with validated steps
 * FAgentPlan Plan;
 * Plan.GoalId = Goal.GoalId;
 *
 * TMap<FString, FString> SpawnArgs;
 * SpawnArgs.Add("code", "...python to spawn trees...");
 *
 * FPlanStep& SpawnStep = Plan.AddToolCall("Spawn 10 trees in circle", "python_execute", SpawnArgs);
 * SpawnStep.AddExpectedOutcome("Trees created", "WorldModel.CountActors(ByLabel('Tree')) >= 10");
 *
 * Plan.AddVerification("Verify tree count", "label contains 'Tree', count >= 10");
 *
 * // 3. Validate plan before execution
 * FPlanValidation Validation = FAgentPlanValidator::ValidatePlan(Plan);
 * if (!Validation.IsValid())
 * {
 *     // Handle invalid plan
 * }
 *
 * // 4. Execute with world model updates
 * while (Plan.HasMoreSteps())
 * {
 *     FPlanStep* Step = Plan.GetNextStep();
 *     FStepResult Result = Executor.ExecuteStep(*Step, WorldModel);
 *
 *     if (Result.IsSuccess())
 *     {
 *         Plan.AdvanceToNextStep();
 *     }
 *     else
 *     {
 *         // YOUR CODE decides recovery action
 *         ERecoveryAction Action = Evaluator.DetermineRecoveryAction(Goal, Result);
 *         // ...
 *     }
 * }
 *
 * // 5. Evaluate goal completion with YOUR CODE
 * FGoalEvaluation Eval = Evaluator.EvaluateGoal(Goal, WorldModel);
 * if (Eval.IsComplete())
 * {
 *     Goal.MarkCompleted();
 * }
 * ```
 *
 *
 * Execution Components (Phase 2):
 * --------------------------------
 *
 * 5. Evaluator (UnrealAgentEvaluator.h)
 *    - FAgentEvaluator: Evaluates success criteria programmatically
 *    - Determines recovery actions based on failure patterns
 *    - No LLM hallucination - either condition is met or it isn't
 *
 * 6. Executor (UnrealAgentExecutor.h)
 *    - FAgentExecutor: Executes steps with precondition checks
 *    - Updates world model after tool execution
 *    - Verifies expected outcomes
 *
 * 7. Planner (UnrealAgentPlanner.h)
 *    - FAgentPlanner: Creates and validates plans
 *    - LLM suggests, YOUR CODE validates
 *    - Programmatic planning for known patterns
 *
 * 8. LLM Interface (UnrealAgentLLMInterface.h)
 *    - FAgentLLMInterface: LLM as advisor, not controller
 *    - Parses intent, suggests plans, explains failures
 *    - All outputs are validated before use
 *
 * 9. Controller (UnrealAgentController.h)
 *    - FAgentController: Main orchestrator
 *    - Runs the agent loop: Goal → Plan → Execute → Evaluate
 *    - State machine with explicit transitions
 *
 *
 * Quick Start:
 * ------------
 *
 * ```cpp
 * // Initialize the agent controller
 * FAgentController Agent;
 * Agent.Initialize(AgentClient);  // Your existing client for tool execution
 *
 * // Handle user request
 * Agent.HandleUserRequest("Place 10 trees in a circle");
 *
 * // Run to completion (or use Tick() for async)
 * Agent.RunToCompletion();
 *
 * // Check result
 * if (Agent.GetState() == EAgentState::Completed)
 * {
 *     // Goal achieved!
 * }
 * ```
 *
 */

#pragma once

// Phase 1: Data Structures
#include "UnrealAgentTypes.h"
#include "UnrealAgentGoal.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"

// Phase 2: Execution Components
#include "UnrealAgentEvaluator.h"
#include "UnrealAgentExecutor.h"
#include "UnrealAgentPlanner.h"
#include "UnrealAgentLLMInterface.h"
#include "UnrealAgentController.h"
