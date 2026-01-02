#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Engine/World.h"
#include "UnrealGPTSceneContext.generated.h"

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTSceneContext : public UObject
{
	GENERATED_BODY()

public:
	/** Capture a screenshot of the active viewport and return as base64 PNG */
	static FString CaptureViewportScreenshot();

	/** Capture a screenshot resized to fit within max dimensions, returned as base64 JPEG for smaller size */
	static FString CaptureViewportScreenshotResized(int32 MaxWidth = 1024, int32 MaxHeight = 768);

	/**
	 * Enhanced viewport screenshot with metadata.
	 * @param OutMetadataJson - JSON string with camera transform, FOV, resolution, selected actors
	 * @param FocusActorLabel - Optional: if specified, focus viewport on this actor before capture
	 * @return Base64-encoded image data
	 */
	static FString CaptureViewportScreenshotWithMetadata(FString& OutMetadataJson, const FString& FocusActorLabel = TEXT(""));

	/** Get a JSON summary of the current scene */
	static FString GetSceneSummary(int32 PageSize = 100, int32 PageIndex = 0);

	/** Get a compact JSON summary of the current scene (no component details, just actor names/classes/locations) */
	static FString GetCompactSceneSummary(int32 MaxActors = 50);

	/** Generic scene query: filters actors/components based on simple criteria.
	 *  ArgumentsJson is a JSON object with optional fields like:
	 *    - class_contains: substring to match in Actor.Class
	 *    - label_contains: substring to match in Actor label
	 *    - name_contains: substring to match in Actor name
	 *    - component_class_contains: substring to match in component class names
	 *    - max_results: maximum number of results to return (default 20)
	 */
	static FString QueryScene(const FString& ArgumentsJson);

	/** Get summary of selected actors only */
	static FString GetSelectedActorsSummary();

private:
	/** Capture viewport using Slate rendering */
	static bool CaptureViewportToImage(TArray<uint8>& OutImageData, int32& OutWidth, int32& OutHeight);

	/** Serialize actor to JSON */
	static TSharedPtr<FJsonObject> SerializeActor(AActor* Actor);

	/** Serialize component to JSON */
	static TSharedPtr<FJsonObject> SerializeComponent(UActorComponent* Component);
};

