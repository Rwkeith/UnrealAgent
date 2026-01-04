#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "Containers/Ticker.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTSessionTypes.h"
#include "UnrealAgentController.h"

// Forward declarations
struct FSlateBrush;
class UTexture2D;
struct FAgentGoal;
struct FGoalEvaluation;
struct FPlanStep;
struct FStepResult;

class SUnrealGPTWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealGPTWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SUnrealGPTWidget();

	friend class UUnrealGPTWidgetDelegateHandler;

private:
	/** Create chat message widget */
	TSharedRef<SWidget> CreateMessageWidget(const FString& Role, const FString& Content);

	/** Create tool call widget */
	TSharedRef<SWidget> CreateToolCallWidget(const FString& ToolName, const FString& Arguments, const FString& Result);

	/** Parse markdown to rich text for better message display */
	TSharedRef<SWidget> CreateMarkdownWidget(const FString& Content);

	/** Create specialized widget for specific tool types */
	TSharedRef<SWidget> CreateToolSpecificWidget(const FString& ToolName, const FString& Arguments, const FString& Result);

	/** Get color scheme for message role */
	FLinearColor GetRoleColor(const FString& Role) const;

	/** Get icon brush for tool name */
	const FSlateBrush* GetToolIcon(const FString& ToolName) const;

	/** Handle send button clicked */
	FReply OnSendClicked();

	/** Handle request context button clicked */
	FReply OnRequestContextClicked();

	/** Handle clear history button clicked */
	FReply OnClearHistoryClicked();

	/** Handle new conversation button clicked */
	FReply OnNewConversationClicked();

	// ==================== SESSION MANAGEMENT ====================

	/** Refresh the session dropdown options */
	void RefreshSessionDropdown();

	/** Handle session selection from dropdown */
	void OnSessionSelected(TSharedPtr<FSessionInfo> NewSelection, ESelectInfo::Type SelectInfo);

	/** Generate widget for session dropdown item */
	TSharedRef<SWidget> GenerateSessionComboItem(TSharedPtr<FSessionInfo> InItem);

	/** Get the text to display for current session */
	FText GetCurrentSessionText() const;

	/** Rebuild chat history UI from a loaded session */
	void RebuildChatHistoryFromSession();

	/** Display a base64 image in chat history */
	void DisplayImageFromBase64(const FString& ImageBase64);

	/** Handle settings button clicked */
	FReply OnSettingsClicked();

	/** Handle voice input button clicked */
	FReply OnVoiceInputClicked();

	/** Handle attach-image button clicked */
	FReply OnAttachImageClicked();

	/** Handle transcription complete */
	void OnTranscriptionComplete(const FString& TranscribedText);

	/** Handle recording started */
	void OnRecordingStarted();

	/** Handle recording stopped */
	void OnRecordingStopped();

	/** Check if send button should be enabled */
	bool IsSendEnabled() const;

	/** Handle agent message delegate - called from agent client */
	void HandleAgentMessage(const FString& Role, const FString& Content, const TArray<FString>& ToolCalls);

	/** Handle agent reasoning delegate - called from agent client */
	void HandleAgentReasoning(const FString& ReasoningContent);

	/** Handle tool call delegate - called from agent client */
	void HandleToolCall(const FString& ToolName, const FString& Arguments);

	/** Handle tool result delegate - called from agent client */
	void HandleToolResult(const FString& ToolCallId, const FString& Result);

	// ==================== AGENT CONTROLLER HANDLERS ====================

	/** Handle agent state changes - updates status display */
	void HandleAgentStateChanged(EAgentState OldState, EAgentState NewState);

	/** Handle goal completion - shows success message */
	void HandleAgentGoalCompleted(const FAgentGoal& Goal, const FGoalEvaluation& Evaluation);

	/** Handle goal failure - shows failure message */
	void HandleAgentGoalFailed(const FAgentGoal& Goal, const FString& Reason);

	/** Handle step completion - updates progress display */
	void HandleAgentStepCompleted(const FPlanStep& Step, const FStepResult& Result);

	/** Handle agent needing user input - prompts user */
	void HandleAgentNeedUserInput(const FString& Question);

	/** Handle agent progress updates - updates progress bar */
	void HandleAgentProgress(const FString& Message, float Percent);

	/** Create a widget to display agent state */
	TSharedRef<SWidget> CreateAgentStateWidget(EAgentState State, const FString& Message);

	/** Get display name for agent state */
	FString GetAgentStateDisplayName(EAgentState State) const;

	/** Get color for agent state */
	FLinearColor GetAgentStateColor(EAgentState State) const;

	/** Agent client instance.
	 *  NOTE: This is a raw pointer in a Slate widget (not a UObject), so UPROPERTY() cannot be used.
	 *  AddToRoot() is called immediately after NewObject() in Construct() to prevent garbage collection.
	 */
	UUnrealGPTAgentClient* AgentClient;

	/** Delegate handler for dynamic delegates.
	 *  NOTE: This is a raw pointer in a Slate widget (not a UObject), so UPROPERTY() cannot be used.
	 *  AddToRoot() is called immediately after NewObject() in Construct() to prevent garbage collection.
	 */
	class UUnrealGPTWidgetDelegateHandler* DelegateHandler;

	/** Chat history scroll box */
	TSharedPtr<class SScrollBox> ChatHistoryBox;

	/** Input text box */
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;

	/** Send button */
	TSharedPtr<SButton> SendButton;

	/** Request context button */
	TSharedPtr<SButton> RequestContextButton;

	/** Clear history button */
	TSharedPtr<SButton> ClearHistoryButton;

	/** New conversation button */
	TSharedPtr<SButton> NewConversationButton;

	/** Settings button */
	TSharedPtr<SButton> SettingsButton;

	/** Voice input button */
	TSharedPtr<SButton> VoiceInputButton;

	/** Screenshot preview image */
	TSharedPtr<class SImage> ScreenshotPreview;

	/** Voice input instance.
	 *  NOTE: This is a raw pointer in a Slate widget (not a UObject), so UPROPERTY() cannot be used.
	 *  AddToRoot() is called immediately after NewObject() in Construct() to prevent garbage collection.
	 */
	class UUnrealGPTVoiceInput* VoiceInput;

	/** Tool call list */
	TArray<FString> ToolCallHistory;

	/** Pending images attached by the user (base64-encoded) to be sent with the next message */
	TArray<FString> PendingAttachedImages;

	/** Persistent brushes for screenshots to ensure Slate does not reference freed memory */
	TArray<TSharedPtr<FSlateBrush>> ScreenshotBrushes;

	/** Persistent textures for screenshots that have been rooted to prevent GC.
	 *  Each texture has AddToRoot() called when created to prevent garbage collection.
	 */
	TArray<UTexture2D*> ScreenshotTextures;

	/** Compact, dynamic area that shows when the agent is reasoning and its reasoning summary */
	TSharedPtr<class SBorder> ReasoningStatusBorder;

	/** Text block used to display the latest reasoning summary from the agent */
	TSharedPtr<class STextBlock> ReasoningSummaryText;

	// ==================== SESSION MANAGEMENT ====================

	/** Session dropdown combobox */
	TSharedPtr<SComboBox<TSharedPtr<FSessionInfo>>> SessionComboBox;

	/** Session dropdown options (shared pointers for Slate) */
	TArray<TSharedPtr<FSessionInfo>> SessionComboOptions;

	/** Currently selected session in dropdown */
	TSharedPtr<FSessionInfo> CurrentSelectedSession;

	// ==================== AGENT CONTROLLER ====================

	/** The true agent controller - orchestrates goal-plan-execute-evaluate cycle.
	 *  This is a heap-allocated non-UObject, so we manage its lifecycle manually.
	 */
	TUniquePtr<FAgentController> AgentController;

	/** Ticker handle for driving the agent state machine */
	FTSTicker::FDelegateHandle AgentTickerHandle;

	/** Called by the ticker to drive the agent state machine */
	bool OnAgentTick(float DeltaTime);

	/** Agent status display border */
	TSharedPtr<class SBorder> AgentStatusBorder;

	/** Agent status text */
	TSharedPtr<class STextBlock> AgentStatusText;

	/** Agent progress bar */
	TSharedPtr<class SProgressBar> AgentProgressBar;

	/** Agent progress message */
	TSharedPtr<class STextBlock> AgentProgressText;

	/** Cancel button for aborting agent operations */
	TSharedPtr<class SButton> AgentCancelButton;

	/** Handle cancel button click */
	FReply OnAgentCancelClicked();

	/** Current agent state for display */
	EAgentState CurrentDisplayedAgentState = EAgentState::Idle;

	// ==================== AGENT MODE ====================

	/** Whether we're in Agent Mode (true) or LLM Chat Mode (false) */
	bool bAgentModeEnabled = false;

	/** Toggle button for switching between modes */
	TSharedPtr<class SButton> AgentModeToggleButton;

	/** Text showing current mode */
	TSharedPtr<class STextBlock> AgentModeText;

	/** Handle agent mode toggle button click */
	FReply OnAgentModeToggleClicked();

	/** Get the display text for current mode */
	FText GetAgentModeText() const;

	/** Get the tooltip for the mode toggle */
	FText GetAgentModeTooltip() const;

	/** Get the button color based on current mode */
	FSlateColor GetAgentModeButtonColor() const;
};

