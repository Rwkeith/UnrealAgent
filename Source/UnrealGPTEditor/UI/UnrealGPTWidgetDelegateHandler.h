#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealAgentController.h"
#include "UnrealGPTWidgetDelegateHandler.generated.h"

class SUnrealGPTWidget;

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTWidgetDelegateHandler : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(SUnrealGPTWidget* InWidget);

	// ==================== LLM CLIENT DELEGATES ====================

	UFUNCTION()
	void OnAgentMessageReceived(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls);

	UFUNCTION()
	void OnAgentReasoningReceived(const FString& ReasoningContent);

	UFUNCTION()
	void OnToolCallReceived(const FString& ToolName, const FString& Arguments);

	UFUNCTION()
	void OnToolResultReceived(const FString& ToolCallId, const FString& Result);

	UFUNCTION()
	void OnTranscriptionCompleteReceived(const FString& TranscribedText);

	UFUNCTION()
	void OnRecordingStartedReceived();

	UFUNCTION()
	void OnRecordingStoppedReceived();

	// ==================== AGENT CONTROLLER DELEGATES ====================
	// These are bound to FAgentController's non-dynamic delegates

	/** Bind to FAgentController delegates */
	void BindToAgentController(FAgentController* Controller);

	/** Unbind from FAgentController delegates */
	void UnbindFromAgentController(FAgentController* Controller);

	/** Handle agent state changes */
	void OnAgentStateChanged(EAgentState OldState, EAgentState NewState);

	/** Handle goal completion */
	void OnAgentGoalCompleted(const FAgentGoal& Goal, const FGoalEvaluation& Evaluation);

	/** Handle goal failure */
	void OnAgentGoalFailed(const FAgentGoal& Goal, const FString& Reason);

	/** Handle step completion */
	void OnAgentStepCompleted(const FPlanStep& Step, const FStepResult& Result);

	/** Handle agent needing user input */
	void OnAgentNeedUserInput(const FString& Question);

	/** Handle agent progress updates */
	void OnAgentProgress(const FString& Message, float Percent);

private:
	SUnrealGPTWidget* Widget;
};

