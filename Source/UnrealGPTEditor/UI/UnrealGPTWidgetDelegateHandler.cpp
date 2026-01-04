#include "UnrealGPTWidgetDelegateHandler.h"
#include "UnrealGPTWidget.h"
#include "UnrealGPTVoiceInput.h"

void UUnrealGPTWidgetDelegateHandler::Initialize(SUnrealGPTWidget* InWidget)
{
	// Prevent this handler from being garbage collected while the widget is alive.
	// The Slate widget only holds a raw pointer (not a UPROPERTY), so we must root
	// this object to ensure delegate calls remain valid.
	if (!IsRooted())
	{
		AddToRoot();
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: WidgetDelegateHandler added to root to prevent GC"));
	}

	Widget = InWidget;
}

// ==================== LLM CLIENT DELEGATES ====================

void UUnrealGPTWidgetDelegateHandler::OnAgentMessageReceived(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls)
{
	if (Widget)
	{
		Widget->HandleAgentMessage(Role, Content, ToolCalls);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentReasoningReceived(const FString& ReasoningContent)
{
	if (Widget)
	{
		Widget->HandleAgentReasoning(ReasoningContent);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnToolCallReceived(const FString& ToolName, const FString& Arguments)
{
	if (Widget)
	{
		Widget->HandleToolCall(ToolName, Arguments);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnToolResultReceived(const FString& ToolCallId, const FString& Result)
{
	if (Widget)
	{
		Widget->HandleToolResult(ToolCallId, Result);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnTranscriptionCompleteReceived(const FString& TranscribedText)
{
	if (Widget)
	{
		Widget->OnTranscriptionComplete(TranscribedText);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnRecordingStartedReceived()
{
	if (Widget)
	{
		Widget->OnRecordingStarted();
	}
}

void UUnrealGPTWidgetDelegateHandler::OnRecordingStoppedReceived()
{
	if (Widget)
	{
		Widget->OnRecordingStopped();
	}
}

// ==================== AGENT CONTROLLER DELEGATES ====================

void UUnrealGPTWidgetDelegateHandler::BindToAgentController(FAgentController* Controller)
{
	if (!Controller)
	{
		return;
	}

	// Bind to FAgentController's non-dynamic delegates
	// Use BindUObject since UUnrealGPTWidgetDelegateHandler is a UObject
	Controller->OnStateChanged.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentStateChanged);
	Controller->OnGoalCompleted.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentGoalCompleted);
	Controller->OnGoalFailed.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentGoalFailed);
	Controller->OnStepCompleted.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentStepCompleted);
	Controller->OnNeedUserInput.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentNeedUserInput);
	Controller->OnProgress.BindUObject(this, &UUnrealGPTWidgetDelegateHandler::OnAgentProgress);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Bound delegate handler to agent controller"));
}

void UUnrealGPTWidgetDelegateHandler::UnbindFromAgentController(FAgentController* Controller)
{
	if (!Controller)
	{
		return;
	}

	Controller->OnStateChanged.Unbind();
	Controller->OnGoalCompleted.Unbind();
	Controller->OnGoalFailed.Unbind();
	Controller->OnStepCompleted.Unbind();
	Controller->OnNeedUserInput.Unbind();
	Controller->OnProgress.Unbind();

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Unbound delegate handler from agent controller"));
}

void UUnrealGPTWidgetDelegateHandler::OnAgentStateChanged(EAgentState OldState, EAgentState NewState)
{
	if (Widget)
	{
		Widget->HandleAgentStateChanged(OldState, NewState);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentGoalCompleted(const FAgentGoal& Goal, const FGoalEvaluation& Evaluation)
{
	if (Widget)
	{
		Widget->HandleAgentGoalCompleted(Goal, Evaluation);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentGoalFailed(const FAgentGoal& Goal, const FString& Reason)
{
	if (Widget)
	{
		Widget->HandleAgentGoalFailed(Goal, Reason);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentStepCompleted(const FPlanStep& Step, const FStepResult& Result)
{
	if (Widget)
	{
		Widget->HandleAgentStepCompleted(Step, Result);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentNeedUserInput(const FString& Question)
{
	if (Widget)
	{
		Widget->HandleAgentNeedUserInput(Question);
	}
}

void UUnrealGPTWidgetDelegateHandler::OnAgentProgress(const FString& Message, float Percent)
{
	if (Widget)
	{
		Widget->HandleAgentProgress(Message, Percent);
	}
}

