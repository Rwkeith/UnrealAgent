#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTToolExecutor.generated.h"

/**
 * Executes agent tools (python_execute, atomic editor tools, etc.)
 * This class centralizes all tool execution logic separate from the agent client.
 */
UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTToolExecutor : public UObject
{
	GENERATED_BODY()

public:
	// ==================== PYTHON EXECUTION ====================

	/** Execute Python code in the Unreal Editor context */
	static FString ExecutePythonCode(const FString& Code);

	// ==================== VIEWPORT / SCENE ====================

	/** Get viewport screenshot with optional focus actor and metadata output */
	static FString GetViewportScreenshot(const FString& ArgumentsJson, FString& OutMetadataJson);

	/** Get scene summary */
	static FString GetSceneSummary(int32 PageSize = 100);

	// ==================== ATOMIC EDITOR TOOLS ====================

	/** Get detailed actor info by label or name */
	static FString ExecuteGetActor(const FString& ArgumentsJson);

	/** Set actor transform (location, rotation, scale) */
	static FString ExecuteSetActorTransform(const FString& ArgumentsJson);

	/** Select actors by labels */
	static FString ExecuteSelectActors(const FString& ArgumentsJson);

	/** Duplicate an actor with optional count and offset */
	static FString ExecuteDuplicateActor(const FString& ArgumentsJson);

	/** Snap an actor to the ground below it */
	static FString ExecuteSnapActorToGround(const FString& ArgumentsJson);

	/** Set rotation on multiple actors at once */
	static FString ExecuteSetActorsRotation(const FString& ArgumentsJson);

	// ==================== HELPERS ====================

	/** Focus viewport on the last created asset/actor */
	static void FocusViewportOnCreatedAsset(const FString& ResultJson);

	/**
	 * Find actor by Id (internal name) or Label (display name).
	 * Id takes priority - it's the stable unique identifier (Actor->GetName()).
	 * Label is the user-friendly display name shown in Outliner (Actor->GetActorLabel()).
	 */
	static AActor* FindActorByIdOrLabel(const FString& Id, const FString& Label);

	/** Legacy wrapper - Find actor by label or name (backwards compatibility) */
	static AActor* FindActorByLabelOrName(const FString& Label, const FString& Name);
};
