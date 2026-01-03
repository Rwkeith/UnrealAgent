#include "UnrealGPTSceneContext.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Math/IntRect.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RenderCommandFence.h"
#include "LevelEditorViewport.h"
#include "SLevelViewport.h"

FString UUnrealGPTSceneContext::CaptureViewportScreenshot()
{
	TArray<uint8> ImageData;
	int32 Width = 0;
	int32 Height = 0;

	if (!CaptureViewportToImage(ImageData, Width, Height))
	{
		return TEXT("");
	}

	// Encode to base64
	FString Base64String = FBase64::Encode(ImageData);

	// Log the size for debugging
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Screenshot captured: %dx%d, base64 size: %d bytes"), Width, Height, Base64String.Len());

	return Base64String;
}

FString UUnrealGPTSceneContext::CaptureViewportScreenshotResized(int32 MaxWidth, int32 MaxHeight)
{
	if (!GEditor)
	{
		return TEXT("");
	}

	FViewport* ViewportWidget = GEditor->GetActiveViewport();
	if (!ViewportWidget)
	{
		return TEXT("");
	}

	int32 OrigWidth = ViewportWidget->GetSizeXY().X;
	int32 OrigHeight = ViewportWidget->GetSizeXY().Y;

	if (OrigWidth <= 0 || OrigHeight <= 0)
	{
		return TEXT("");
	}

	// Calculate scale factor to fit within max dimensions while maintaining aspect ratio
	float ScaleX = (float)MaxWidth / (float)OrigWidth;
	float ScaleY = (float)MaxHeight / (float)OrigHeight;
	float Scale = FMath::Min(ScaleX, ScaleY);

	// Only downscale, never upscale
	if (Scale >= 1.0f)
	{
		// No resize needed, use regular capture
		return CaptureViewportScreenshot();
	}

	int32 NewWidth = FMath::Max(1, FMath::RoundToInt(OrigWidth * Scale));
	int32 NewHeight = FMath::Max(1, FMath::RoundToInt(OrigHeight * Scale));

	// Capture full resolution first
	TArray<uint8> FullImageData;
	int32 CapturedWidth = 0;
	int32 CapturedHeight = 0;

	if (!CaptureViewportToImage(FullImageData, CapturedWidth, CapturedHeight))
	{
		return TEXT("");
	}

	// Decode the PNG to resize it
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> SourceWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!SourceWrapper.IsValid() || !SourceWrapper->SetCompressed(FullImageData.GetData(), FullImageData.Num()))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to decode captured image for resizing"));
		// Fall back to full resolution
		FString Base64String = FBase64::Encode(FullImageData);
		return Base64String;
	}

	TArray<uint8> RawData;
	if (!SourceWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to get raw image data for resizing"));
		FString Base64String = FBase64::Encode(FullImageData);
		return Base64String;
	}

	// Simple bilinear downscale
	TArray<FColor> SourceBitmap;
	SourceBitmap.SetNumUninitialized(CapturedWidth * CapturedHeight);
	FMemory::Memcpy(SourceBitmap.GetData(), RawData.GetData(), RawData.Num());

	TArray<FColor> ResizedBitmap;
	ResizedBitmap.SetNumUninitialized(NewWidth * NewHeight);

	for (int32 y = 0; y < NewHeight; ++y)
	{
		for (int32 x = 0; x < NewWidth; ++x)
		{
			// Map to source coordinates
			float SrcX = (float)x / Scale;
			float SrcY = (float)y / Scale;

			int32 SrcXi = FMath::Clamp(FMath::FloorToInt(SrcX), 0, CapturedWidth - 1);
			int32 SrcYi = FMath::Clamp(FMath::FloorToInt(SrcY), 0, CapturedHeight - 1);

			// Simple nearest neighbor for speed
			int32 SrcIndex = SrcYi * CapturedWidth + SrcXi;
			int32 DstIndex = y * NewWidth + x;
			ResizedBitmap[DstIndex] = SourceBitmap[SrcIndex];
		}
	}

	// Compress to JPEG for smaller size (PNG is too large for API)
	TSharedPtr<IImageWrapper> JpegWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!JpegWrapper.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to create JPEG wrapper"));
		FString Base64String = FBase64::Encode(FullImageData);
		return Base64String;
	}

	if (!JpegWrapper->SetRaw(ResizedBitmap.GetData(), ResizedBitmap.Num() * sizeof(FColor), NewWidth, NewHeight, ERGBFormat::BGRA, 8))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to set resized image data"));
		FString Base64String = FBase64::Encode(FullImageData);
		return Base64String;
	}

	// Get compressed JPEG with quality 85 (good balance of size vs quality)
	// Note: GetCompressed returns TArray64<uint8> in UE5, need to copy to regular TArray
	TArray64<uint8> CompressedData64 = JpegWrapper->GetCompressed(85);
	if (CompressedData64.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: JPEG compression failed"));
		FString Base64String = FBase64::Encode(FullImageData);
		return Base64String;
	}

	// Copy to regular TArray for FBase64::Encode compatibility
	TArray<uint8> CompressedData;
	CompressedData.SetNumUninitialized(CompressedData64.Num());
	FMemory::Memcpy(CompressedData.GetData(), CompressedData64.GetData(), CompressedData64.Num());

	FString Base64String = FBase64::Encode(CompressedData);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Resized screenshot: %dx%d -> %dx%d, base64 size: %d bytes (was %d)"),
		CapturedWidth, CapturedHeight, NewWidth, NewHeight, Base64String.Len(), FBase64::Encode(FullImageData).Len());

	return Base64String;
}

FString UUnrealGPTSceneContext::CaptureViewportScreenshotWithMetadata(FString& OutMetadataJson, const FString& FocusActorLabel)
{
	if (!GEditor)
	{
		OutMetadataJson = TEXT("{\"error\": \"Editor not available\"}");
		return TEXT("");
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutMetadataJson = TEXT("{\"error\": \"No world available\"}");
		return TEXT("");
	}

	// If focus_actor is specified, find and focus on that actor before capture
	FString FocusedActorLabel;
	if (!FocusActorLabel.IsEmpty())
	{
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor && !Actor->IsPendingKillPending() && Actor->GetActorLabel() == FocusActorLabel)
			{
				// Select and focus on this actor
				GEditor->SelectNone(false, true);
				GEditor->SelectActor(Actor, true, true);
				GEditor->MoveViewportCamerasToActor(*Actor, false);
				FocusedActorLabel = FocusActorLabel;

				// Small delay to allow viewport to update (flush commands)
				FRenderCommandFence Fence;
				Fence.BeginFence();
				Fence.Wait();

				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Focused viewport on actor: %s"), *FocusActorLabel);
				break;
			}
		}

		if (FocusedActorLabel.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Could not find actor with label: %s"), *FocusActorLabel);
		}
	}

	// Build metadata JSON
	TSharedPtr<FJsonObject> MetadataObj = MakeShareable(new FJsonObject);

	// Get viewport client for camera info
	FViewport* Viewport = GEditor->GetActiveViewport();
	if (Viewport)
	{
		// Resolution
		TSharedPtr<FJsonObject> ResolutionObj = MakeShareable(new FJsonObject);
		ResolutionObj->SetNumberField(TEXT("width"), Viewport->GetSizeXY().X);
		ResolutionObj->SetNumberField(TEXT("height"), Viewport->GetSizeXY().Y);
		MetadataObj->SetObjectField(TEXT("resolution"), ResolutionObj);

		// Try to get camera info from the level editor viewport
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<SLevelViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();

		if (ActiveLevelViewport.IsValid())
		{
			FLevelEditorViewportClient& ViewportClient = ActiveLevelViewport->GetLevelViewportClient();

			// Camera transform
			TSharedPtr<FJsonObject> CameraObj = MakeShareable(new FJsonObject);

			FVector CamLocation = ViewportClient.GetViewLocation();
			TSharedPtr<FJsonObject> LocationObj = MakeShareable(new FJsonObject);
			LocationObj->SetNumberField(TEXT("x"), CamLocation.X);
			LocationObj->SetNumberField(TEXT("y"), CamLocation.Y);
			LocationObj->SetNumberField(TEXT("z"), CamLocation.Z);
			CameraObj->SetObjectField(TEXT("location"), LocationObj);

			FRotator CamRotation = ViewportClient.GetViewRotation();
			TSharedPtr<FJsonObject> RotationObj = MakeShareable(new FJsonObject);
			RotationObj->SetNumberField(TEXT("pitch"), CamRotation.Pitch);
			RotationObj->SetNumberField(TEXT("yaw"), CamRotation.Yaw);
			RotationObj->SetNumberField(TEXT("roll"), CamRotation.Roll);
			CameraObj->SetObjectField(TEXT("rotation"), RotationObj);

			// FOV
			CameraObj->SetNumberField(TEXT("fov"), ViewportClient.ViewFOV);

			MetadataObj->SetObjectField(TEXT("camera"), CameraObj);
		}
	}

	// Get selected actors
	TArray<TSharedPtr<FJsonValue>> SelectedActorsArray;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (SelectedActors)
	{
		for (FSelectionIterator It(*SelectedActors); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				SelectedActorsArray.Add(MakeShareable(new FJsonValueString(Actor->GetActorLabel())));
			}
		}
	}
	MetadataObj->SetArrayField(TEXT("selected_actors"), SelectedActorsArray);

	// Add focused actor if we focused on one
	if (!FocusedActorLabel.IsEmpty())
	{
		MetadataObj->SetStringField(TEXT("focused_actor"), FocusedActorLabel);
	}

	// Serialize metadata to JSON string
	FString MetadataString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MetadataString);
	FJsonSerializer::Serialize(MetadataObj.ToSharedRef(), Writer);
	OutMetadataJson = MetadataString;

	// Capture the screenshot (reuse existing resized capture)
	FString ImageBase64 = CaptureViewportScreenshotResized(1024, 768);

	return ImageBase64;
}

bool UUnrealGPTSceneContext::CaptureViewportToImage(TArray<uint8>& OutImageData, int32& OutWidth, int32& OutHeight)
{
	if (!GEditor)
	{
		return false;
	}

	FViewport* ViewportWidget = GEditor->GetActiveViewport();
	if (!ViewportWidget)
	{
		return false;
	}

	OutWidth = ViewportWidget->GetSizeXY().X;
	OutHeight = ViewportWidget->GetSizeXY().Y;

	if (OutWidth <= 0 || OutHeight <= 0)
	{
		return false;
	}

	// Check if we're on the game thread (required for ReadPixels)
	if (!IsInGameThread())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: CaptureViewportToImage must be called from game thread"));
		return false;
	}

	// Flush all rendering commands to ensure the viewport is in a stable state
	// This helps prevent accessing render resources that are being destroyed
	FRenderCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();

	// Re-check viewport size after flushing to ensure it's still valid
	// Re-acquire the viewport pointer in case it changed during flush
	FViewport* CurrentViewport = GEditor->GetActiveViewport();
	if (!CurrentViewport || CurrentViewport != ViewportWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Viewport changed or became invalid after flush"));
		return false;
	}

	// Re-check size to ensure viewport is still valid
	FIntPoint ViewportSize = CurrentViewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Viewport has invalid size after flush: %dx%d"), ViewportSize.X, ViewportSize.Y);
		return false;
	}

	// Update dimensions if they changed
	if (ViewportSize.X != OutWidth || ViewportSize.Y != OutHeight)
	{
		OutWidth = ViewportSize.X;
		OutHeight = ViewportSize.Y;
	}

	// Use the current viewport for ReadPixels
	ViewportWidget = CurrentViewport;

	// Use a safer approach: read pixels with proper error handling
	TArray<FColor> Bitmap;
	FIntRect Rect(0, 0, OutWidth, OutHeight);
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);
	
	// Attempt to read pixels - this can fail if render resources are invalid
	// We'll check the result carefully
	bool bReadSuccess = ViewportWidget->ReadPixels(Bitmap, ReadFlags, Rect);

	if (!bReadSuccess)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: ReadPixels returned false - viewport may be invalid"));
		return false;
	}

	// Validate the bitmap data
	if (Bitmap.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: ReadPixels returned empty bitmap"));
		return false;
	}

	const int32 ExpectedPixelCount = OutWidth * OutHeight;
	if (Bitmap.Num() != ExpectedPixelCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Invalid bitmap size: %d (expected %d)"), Bitmap.Num(), ExpectedPixelCount);
		return false;
	}

	// Convert to PNG
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!ImageWrapper.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to create image wrapper"));
		return false;
	}

	// Set raw image data - make sure we're using the correct format
	const int32 ImageDataSize = Bitmap.Num() * sizeof(FColor);
	if (!ImageWrapper->SetRaw(Bitmap.GetData(), ImageDataSize, OutWidth, OutHeight, ERGBFormat::BGRA, 8))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to set raw image data"));
		return false;
	}

	OutImageData = ImageWrapper->GetCompressed();
	if (OutImageData.Num() <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Image compression produced empty result"));
		return false;
	}

	return true;
}

FString UUnrealGPTSceneContext::GetSceneSummary(int32 PageSize, int32 PageIndex)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("{}");
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	int32 ActorCount = 0;
	int32 StartIndex = PageIndex * PageSize;
	int32 EndIndex = StartIndex + PageSize;

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (ActorCount >= StartIndex && ActorCount < EndIndex)
		{
			TSharedPtr<FJsonObject> ActorJson = SerializeActor(Actor);
			if (ActorJson.IsValid())
			{
				ActorsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			}
		}

		ActorCount++;
	}

	TSharedPtr<FJsonObject> SummaryJson = MakeShareable(new FJsonObject);
	SummaryJson->SetNumberField(TEXT("total_actors"), ActorCount);
	SummaryJson->SetNumberField(TEXT("page_size"), PageSize);
	SummaryJson->SetNumberField(TEXT("page_index"), PageIndex);
	SummaryJson->SetNumberField(TEXT("actors_on_page"), ActorsArray.Num());
	SummaryJson->SetArrayField(TEXT("actors"), ActorsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SummaryJson.ToSharedRef(), Writer);

	return OutputString;
}

FString UUnrealGPTSceneContext::GetCompactSceneSummary(int32 MaxActors)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("{}");
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	int32 ActorCount = 0;
	int32 IncludedCount = 0;

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		ActorCount++;

		// Only include up to MaxActors in detail
		if (IncludedCount < MaxActors)
		{
			// Compact serialization: just name, label, class, and location (no components)
			TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
			ActorJson->SetStringField(TEXT("name"), Actor->GetName());
			ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
			ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

			// Location only (no rotation/scale to save tokens)
			const FVector Location = Actor->GetActorLocation();
			TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
			LocationJson->SetNumberField(TEXT("x"), FMath::RoundToInt(Location.X));
			LocationJson->SetNumberField(TEXT("y"), FMath::RoundToInt(Location.Y));
			LocationJson->SetNumberField(TEXT("z"), FMath::RoundToInt(Location.Z));
			ActorJson->SetObjectField(TEXT("location"), LocationJson);

			ActorsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			IncludedCount++;
		}
	}

	TSharedPtr<FJsonObject> SummaryJson = MakeShareable(new FJsonObject);
	SummaryJson->SetNumberField(TEXT("total_actors"), ActorCount);
	SummaryJson->SetNumberField(TEXT("included_actors"), IncludedCount);
	SummaryJson->SetArrayField(TEXT("actors"), ActorsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SummaryJson.ToSharedRef(), Writer);

	return OutputString;
}

FString UUnrealGPTSceneContext::QueryScene(const FString& ArgumentsJson)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> SummaryObj = MakeShareable(new FJsonObject);
		SummaryObj->SetNumberField(TEXT("total_matched"), 0);
		SummaryObj->SetNumberField(TEXT("returned"), 0);
		SummaryObj->SetBoolField(TEXT("has_more"), false);
		ErrorResult->SetObjectField(TEXT("summary"), SummaryObj);
		ErrorResult->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ErrorResult.ToSharedRef(), Writer);
		return OutputString;
	}

	// Parse arguments JSON
	TSharedPtr<FJsonObject> ArgsObj;
	if (!ArgumentsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		FJsonSerializer::Deserialize(Reader, ArgsObj);
	}

	auto GetStringArg = [&ArgsObj](const FString& Key) -> FString
	{
		FString Value;
		if (ArgsObj.IsValid())
		{
			ArgsObj->TryGetStringField(Key, Value);
		}
		return Value;
	};

	auto GetIntArg = [&ArgsObj](const FString& Key, int32 DefaultValue) -> int32
	{
		int32 Value = DefaultValue;
		if (ArgsObj.IsValid())
		{
			ArgsObj->TryGetNumberField(Key, Value);
		}
		return Value;
	};

	// Filter parameters
	const FString ClassContains = GetStringArg(TEXT("class_contains"));
	const FString LabelContains = GetStringArg(TEXT("label_contains"));
	const FString NameContains = GetStringArg(TEXT("name_contains"));
	const FString ComponentClassContains = GetStringArg(TEXT("component_class_contains"));
	const int32 MaxResults = FMath::Max(1, GetIntArg(TEXT("max_results"), 20));
	const int32 Offset = FMath::Max(0, GetIntArg(TEXT("offset"), 0));

	// Parse 'fields' parameter - comma-separated list of field names
	const FString FieldsStr = GetStringArg(TEXT("fields"));
	TSet<FString> RequestedFields;
	if (!FieldsStr.IsEmpty())
	{
		TArray<FString> FieldParts;
		FieldsStr.ParseIntoArray(FieldParts, TEXT(","), true);
		for (FString& Part : FieldParts)
		{
			Part.TrimStartAndEndInline();
			Part.ToLowerInline();
			RequestedFields.Add(Part);
		}
	}

	// Check which fields are requested
	const bool bIncludeLocation = RequestedFields.Contains(TEXT("location"));
	const bool bIncludeRotation = RequestedFields.Contains(TEXT("rotation"));
	const bool bIncludeScale = RequestedFields.Contains(TEXT("scale"));
	const bool bIncludeBounds = RequestedFields.Contains(TEXT("bounds"));
	const bool bIncludeComponents = RequestedFields.Contains(TEXT("components"));
	const bool bIncludeTags = RequestedFields.Contains(TEXT("tags"));
	const bool bIncludeFolder = RequestedFields.Contains(TEXT("folder"));
	const bool bIncludeParent = RequestedFields.Contains(TEXT("parent"));

	// First pass: collect all matching actors and count classes
	TArray<AActor*> MatchingActors;
	TMap<FString, int32> ClassCounts;

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		const FString ClassName = Actor->GetClass()->GetName();
		const FString Label = Actor->GetActorLabel();
		const FString Name = Actor->GetName();

		// Apply simple substring filters (case-insensitive)
		auto MatchesFilter = [](const FString& Source, const FString& Substr) -> bool
		{
			return Substr.IsEmpty() || Source.Contains(Substr, ESearchCase::IgnoreCase);
		};

		if (!MatchesFilter(ClassName, ClassContains))
		{
			continue;
		}
		if (!MatchesFilter(Label, LabelContains))
		{
			continue;
		}
		if (!MatchesFilter(Name, NameContains))
		{
			continue;
		}

		// Optional component class filter
		if (!ComponentClassContains.IsEmpty())
		{
			bool bHasMatchingComponent = false;
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (Component && Component->GetClass()->GetName().Contains(ComponentClassContains, ESearchCase::IgnoreCase))
				{
					bHasMatchingComponent = true;
					break;
				}
			}

			if (!bHasMatchingComponent)
			{
				continue;
			}
		}

		// Actor matches all filters
		MatchingActors.Add(Actor);

		// Count class occurrences for summary
		int32& Count = ClassCounts.FindOrAdd(ClassName);
		Count++;
	}

	const int32 TotalMatched = MatchingActors.Num();

	// Second pass: build results for the requested page
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	const int32 StartIndex = FMath::Min(Offset, TotalMatched);
	const int32 EndIndex = FMath::Min(Offset + MaxResults, TotalMatched);

	for (int32 i = StartIndex; i < EndIndex; ++i)
	{
		AActor* Actor = MatchingActors[i];
		const FString ClassName = Actor->GetClass()->GetName();
		const FString Label = Actor->GetActorLabel();
		const FString Id = Actor->GetName(); // Internal name is the stable unique identifier

		// Build JSON object for this actor - minimal by default
		TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
		ActorJson->SetStringField(TEXT("id"), Id);       // Stable unique identifier
		ActorJson->SetStringField(TEXT("label"), Label); // User-friendly display name
		ActorJson->SetStringField(TEXT("class"), ClassName);

		// Optional: location
		if (bIncludeLocation)
		{
			const FVector Location = Actor->GetActorLocation();
			TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
			LocationJson->SetNumberField(TEXT("x"), Location.X);
			LocationJson->SetNumberField(TEXT("y"), Location.Y);
			LocationJson->SetNumberField(TEXT("z"), Location.Z);
			ActorJson->SetObjectField(TEXT("location"), LocationJson);
		}

		// Optional: rotation
		if (bIncludeRotation)
		{
			const FRotator Rotation = Actor->GetActorRotation();
			TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
			RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
			ActorJson->SetObjectField(TEXT("rotation"), RotationJson);
		}

		// Optional: scale
		if (bIncludeScale)
		{
			const FVector Scale = Actor->GetActorScale3D();
			TSharedPtr<FJsonObject> ScaleJson = MakeShareable(new FJsonObject);
			ScaleJson->SetNumberField(TEXT("x"), Scale.X);
			ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
			ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
			ActorJson->SetObjectField(TEXT("scale"), ScaleJson);
		}

		// Optional: bounding box
		if (bIncludeBounds)
		{
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);

			TSharedPtr<FJsonObject> BoundsJson = MakeShareable(new FJsonObject);
			TSharedPtr<FJsonObject> OriginJson = MakeShareable(new FJsonObject);
			OriginJson->SetNumberField(TEXT("x"), Origin.X);
			OriginJson->SetNumberField(TEXT("y"), Origin.Y);
			OriginJson->SetNumberField(TEXT("z"), Origin.Z);
			BoundsJson->SetObjectField(TEXT("origin"), OriginJson);

			TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject);
			ExtentJson->SetNumberField(TEXT("x"), Extent.X);
			ExtentJson->SetNumberField(TEXT("y"), Extent.Y);
			ExtentJson->SetNumberField(TEXT("z"), Extent.Z);
			BoundsJson->SetObjectField(TEXT("extent"), ExtentJson);

			ActorJson->SetObjectField(TEXT("bounds"), BoundsJson);
		}

		// Optional: root component info and static mesh path
		if (bIncludeComponents)
		{
			USceneComponent* RootComp = Actor->GetRootComponent();
			if (RootComp)
			{
				TSharedPtr<FJsonObject> RootCompJson = MakeShareable(new FJsonObject);
				RootCompJson->SetStringField(TEXT("class"), RootComp->GetClass()->GetName());

				// Mobility
				FString MobilityStr;
				switch (RootComp->Mobility)
				{
					case EComponentMobility::Static: MobilityStr = TEXT("Static"); break;
					case EComponentMobility::Stationary: MobilityStr = TEXT("Stationary"); break;
					case EComponentMobility::Movable: MobilityStr = TEXT("Movable"); break;
					default: MobilityStr = TEXT("Unknown"); break;
				}
				RootCompJson->SetStringField(TEXT("mobility"), MobilityStr);

				ActorJson->SetObjectField(TEXT("root_component"), RootCompJson);
			}

			// Look for StaticMeshComponent and get mesh asset path
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component))
				{
					if (UStaticMesh* Mesh = SMC->GetStaticMesh())
					{
						ActorJson->SetStringField(TEXT("static_mesh_path"), Mesh->GetPathName());
						break; // Just get the first one
					}
				}
			}
		}

		// Optional: tags
		if (bIncludeTags)
		{
			TArray<TSharedPtr<FJsonValue>> TagsArray;
			for (const FName& Tag : Actor->Tags)
			{
				TagsArray.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
			}
			ActorJson->SetArrayField(TEXT("tags"), TagsArray);
		}

		// Optional: folder path
		if (bIncludeFolder)
		{
			ActorJson->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		}

		// Optional: parent attachment
		if (bIncludeParent)
		{
			if (AActor* ParentActor = Actor->GetAttachParentActor())
			{
				ActorJson->SetStringField(TEXT("parent_actor"), ParentActor->GetActorLabel());
			}
		}

		ResultsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
	}

	// Build top_classes array (sorted by count, top 5)
	ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
	TArray<TSharedPtr<FJsonValue>> TopClassesArray;
	int32 ClassIndex = 0;
	for (const auto& Pair : ClassCounts)
	{
		if (ClassIndex >= 5) break;
		TopClassesArray.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("%s (%d)"), *Pair.Key, Pair.Value))));
		ClassIndex++;
	}

	// Build summary object
	TSharedPtr<FJsonObject> SummaryObj = MakeShareable(new FJsonObject);
	SummaryObj->SetNumberField(TEXT("total_matched"), TotalMatched);
	SummaryObj->SetNumberField(TEXT("returned"), ResultsArray.Num());
	SummaryObj->SetNumberField(TEXT("offset"), Offset);
	SummaryObj->SetBoolField(TEXT("has_more"), EndIndex < TotalMatched);
	SummaryObj->SetArrayField(TEXT("top_classes"), TopClassesArray);

	// Build final result object
	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
	ResultObj->SetObjectField(TEXT("summary"), SummaryObj);
	ResultObj->SetArrayField(TEXT("actors"), ResultsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return OutputString;
}

FString UUnrealGPTSceneContext::GetSelectedActorsSummary()
{
	TArray<AActor*> SelectedActors;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(SelectedActors);

	TArray<TSharedPtr<FJsonValue>> ActorsArray;

	for (AActor* Actor : SelectedActors)
	{
		if (Actor && !Actor->IsPendingKillPending())
		{
			TSharedPtr<FJsonObject> ActorJson = SerializeActor(Actor);
			if (ActorJson.IsValid())
			{
				ActorsArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
			}
		}
	}

	TSharedPtr<FJsonObject> SummaryJson = MakeShareable(new FJsonObject);
	SummaryJson->SetNumberField(TEXT("selected_count"), SelectedActors.Num());
	SummaryJson->SetArrayField(TEXT("actors"), ActorsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SummaryJson.ToSharedRef(), Writer);

	return OutputString;
}

TSharedPtr<FJsonObject> UUnrealGPTSceneContext::SerializeActor(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
	
	ActorJson->SetStringField(TEXT("name"), Actor->GetName());
	ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
	ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	// Transform
	FTransform Transform = Actor->GetActorTransform();
	TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject);
	LocationJson->SetNumberField(TEXT("x"), Transform.GetLocation().X);
	LocationJson->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
	LocationJson->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
	ActorJson->SetObjectField(TEXT("location"), LocationJson);

	TSharedPtr<FJsonObject> RotationJson = MakeShareable(new FJsonObject);
	FRotator Rotation = Transform.GetRotation().Rotator();
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	ActorJson->SetObjectField(TEXT("rotation"), RotationJson);

	TSharedPtr<FJsonObject> ScaleJson = MakeShareable(new FJsonObject);
	FVector Scale = Transform.GetScale3D();
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	ActorJson->SetObjectField(TEXT("scale"), ScaleJson);

	// Components
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	for (UActorComponent* Component : Components)
	{
		if (Component)
		{
			TSharedPtr<FJsonObject> ComponentJson = SerializeComponent(Component);
			if (ComponentJson.IsValid())
			{
				ComponentsArray.Add(MakeShareable(new FJsonValueObject(ComponentJson)));
			}
		}
	}
	ActorJson->SetArrayField(TEXT("components"), ComponentsArray);

	return ActorJson;
}

TSharedPtr<FJsonObject> UUnrealGPTSceneContext::SerializeComponent(UActorComponent* Component)
{
	if (!Component)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> ComponentJson = MakeShareable(new FJsonObject);
	ComponentJson->SetStringField(TEXT("name"), Component->GetName());
	ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
	ComponentJson->SetBoolField(TEXT("is_active"), Component->IsActive());

	return ComponentJson;
}

