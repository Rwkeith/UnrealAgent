#include "UnrealGPTToolExecutor.h"
#include "UnrealGPTSceneContext.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "IPythonScriptPlugin.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

namespace
{
	// ==================== JSON HELPER FUNCTIONS ====================

	/** Build a JSON object from an FVector */
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	/** Build a JSON object from an FRotator */
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	/** Build a standard tool result JSON string with status, message, and optional details */
	FString MakeToolResult(const FString& Status, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
	{
		TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
		ResultObj->SetStringField(TEXT("status"), Status);
		ResultObj->SetStringField(TEXT("message"), Message);
		if (Details.IsValid())
		{
			ResultObj->SetObjectField(TEXT("details"), Details);
		}

		FString ResultString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
		FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
		return ResultString;
	}

	/** Shorthand for error results */
	FString MakeErrorResult(const FString& Message)
	{
		return MakeToolResult(TEXT("error"), Message);
	}

	/** Shorthand for success results */
	FString MakeSuccessResult(const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
	{
		return MakeToolResult(TEXT("ok"), Message, Details);
	}

	/** Helper to indent arbitrary Python source one level (4 spaces), preserving empty lines. */
	FString IndentPythonCode(const FString& Code)
	{
		FString Indented;
		TArray<FString> Lines;
		Code.ParseIntoArrayLines(Lines);

		for (const FString& Line : Lines)
		{
			if (Line.TrimStartAndEnd().IsEmpty())
			{
				Indented += TEXT("    \n");
			}
			else
			{
				Indented += TEXT("    ") + Line + TEXT("\n");
			}
		}

		return Indented;
	}
}

// ==================== PYTHON EXECUTION ====================

FString UUnrealGPTToolExecutor::ExecutePythonCode(const FString& Code)
{
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: ExecutePythonCode ENTRY - Code length: %d chars"), Code.Len());

	if (!IPythonScriptPlugin::Get()->IsPythonAvailable())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Python is not available"));
		return TEXT("Error: Python is not available in this Unreal Engine installation");
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Python is available, preparing execution..."));

	// Use a deterministic result file the Python wrapper can write to.
	const FString ResultFilePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("UnrealGPT_PythonResult.json"));

	// Clear any previous result.
	IFileManager::Get().Delete(*ResultFilePath, /*RequireExists=*/false, /*EvenIfReadOnly=*/true);

	const FString IndentedUserCode = IndentPythonCode(Code);

	// Build a wrapper that:
	// - defines a standard JSON envelope with status, message, details, logs, error
	// - wraps execution in an Editor transaction for Undo support
	// - captures stdout/stderr
	// - runs user code inside try/except/finally (where the code can modify `result`)
	// - writes the final `result` to ResultFilePath as JSON
	FString WrappedCode;
	WrappedCode += TEXT("import json, traceback, sys, io\n");
	WrappedCode += TEXT("import unreal\n\n");

	// Standard envelope with all fields
	WrappedCode += TEXT("# Standard result envelope - user code can update these fields\n");
	WrappedCode += TEXT("result = {\n");
	WrappedCode += TEXT("    \"status\": \"ok\",\n");
	WrappedCode += TEXT("    \"message\": \"Python code executed. No custom result message was set.\",\n");
	WrappedCode += TEXT("    \"details\": {},\n");
	WrappedCode += TEXT("}\n\n");

	// Internal state tracking (prefixed with underscore to avoid conflicts)
	WrappedCode += TEXT("# Internal state - do not modify\n");
	WrappedCode += TEXT("_stdout_capture = io.StringIO()\n");
	WrappedCode += TEXT("_old_stdout = sys.stdout\n");
	WrappedCode += TEXT("_transaction_id = -1\n");
	WrappedCode += TEXT("_execution_error = None\n\n");

	// Capture stdout
	WrappedCode += TEXT("sys.stdout = _stdout_capture\n\n");

	// Begin Editor transaction for Undo support
	WrappedCode += TEXT("# Begin Editor transaction for Undo support\n");
	WrappedCode += TEXT("try:\n");
	WrappedCode += TEXT("    _transaction_id = unreal.SystemLibrary.begin_transaction(\"UnrealGPT\", \"UnrealGPT Python Execution\", None)\n");
	WrappedCode += TEXT("except Exception:\n");
	WrappedCode += TEXT("    pass  # Transaction API may not be available in all contexts\n\n");

	// Main try/except/finally block
	WrappedCode += TEXT("try:\n");
	WrappedCode += IndentedUserCode;
	WrappedCode += TEXT("\n"); // Ensure newline after user code
	WrappedCode += TEXT("except Exception as e:\n");
	WrappedCode += TEXT("    _execution_error = e\n");
	WrappedCode += TEXT("    result[\"status\"] = \"error\"\n");
	WrappedCode += TEXT("    result[\"message\"] = str(e)\n");
	WrappedCode += TEXT("    result[\"error\"] = {\n");
	WrappedCode += TEXT("        \"type\": type(e).__name__,\n");
	WrappedCode += TEXT("        \"traceback\": traceback.format_exc()\n");
	WrappedCode += TEXT("    }\n");
	WrappedCode += TEXT("    # Cancel transaction on error\n");
	WrappedCode += TEXT("    if _transaction_id >= 0:\n");
	WrappedCode += TEXT("        try:\n");
	WrappedCode += TEXT("            unreal.SystemLibrary.cancel_transaction(_transaction_id)\n");
	WrappedCode += TEXT("        except Exception:\n");
	WrappedCode += TEXT("            pass\n");
	WrappedCode += TEXT("finally:\n");
	WrappedCode += TEXT("    # Always restore stdout\n");
	WrappedCode += TEXT("    sys.stdout = _old_stdout\n");
	WrappedCode += TEXT("    _captured_stdout = _stdout_capture.getvalue()\n\n");

	// End transaction on success (outside try/finally to avoid issues)
	WrappedCode += TEXT("# End transaction if started successfully and no error\n");
	WrappedCode += TEXT("if _execution_error is None and _transaction_id >= 0:\n");
	WrappedCode += TEXT("    try:\n");
	WrappedCode += TEXT("        unreal.SystemLibrary.end_transaction()\n");
	WrappedCode += TEXT("    except Exception:\n");
	WrappedCode += TEXT("        pass\n\n");

	// Ensure result is a dict and add metadata
	WrappedCode += TEXT("# Ensure result is a valid dict and add logs/transaction metadata\n");
	WrappedCode += TEXT("if not isinstance(result, dict):\n");
	WrappedCode += TEXT("    result = {\"status\": \"ok\", \"message\": \"Code executed.\", \"details\": {\"raw_result\": str(result)}}\n");
	WrappedCode += TEXT("result.setdefault(\"status\", \"ok\")\n");
	WrappedCode += TEXT("result.setdefault(\"message\", \"\")\n");
	WrappedCode += TEXT("result.setdefault(\"details\", {})\n");
	WrappedCode += TEXT("result[\"logs\"] = {\"stdout\": _captured_stdout}\n");
	WrappedCode += TEXT("result[\"transaction\"] = {\"used\": _transaction_id >= 0, \"id\": _transaction_id}\n\n");

	// Write result to file
	FString PythonPath = ResultFilePath.Replace(TEXT("\\"), TEXT("/"));
	WrappedCode += TEXT("# Write result to file\n");
	WrappedCode += TEXT("_result_path = r\"") + PythonPath + TEXT("\"\n");
	WrappedCode += TEXT("try:\n");
	WrappedCode += TEXT("    with open(_result_path, \"w\", encoding=\"utf-8\") as f:\n");
	WrappedCode += TEXT("        f.write(json.dumps(result, default=str))\n");
	WrappedCode += TEXT("except Exception as write_err:\n");
	WrappedCode += TEXT("    unreal.log_error(f\"UnrealGPT: Failed to write result file: {write_err}\")\n");

	// Debug: Write the wrapped code to a file for inspection
	const FString DebugCodePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("UnrealGPT_DebugWrappedCode.py"));
	FFileHelper::SaveStringToFile(WrappedCode, *DebugCodePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Debug wrapped code saved to: %s"), *DebugCodePath);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Starting Python execution via IPythonScriptPlugin..."));
	const double StartTime = FPlatformTime::Seconds();

	IPythonScriptPlugin::Get()->ExecPythonCommand(*WrappedCode);

	const double EndTime = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Python execution completed in %.3f seconds"), EndTime - StartTime);

	// Try to read the JSON result back from disk so the agent can reason about it.
	FString ResultJson;
	if (FPaths::FileExists(ResultFilePath))
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Result file exists, reading..."));
		if (FFileHelper::LoadFileToString(ResultJson, *ResultFilePath))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Result JSON loaded (%d chars): %s"),
				ResultJson.Len(),
				*ResultJson.Left(500)); // Log first 500 chars
			// Try to focus viewport on the last created asset
			FocusViewportOnCreatedAsset(ResultJson);
			return ResultJson;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to load result file to string"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Result file does not exist at: %s"), *ResultFilePath);
	}

	// Fallback if no structured result was produced.
	UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: No structured result produced, returning fallback message"));
	return TEXT(
		"Python code was sent to the Unreal Editor for execution, but no structured result JSON "
		"was produced. The script may have succeeded or failed; check the Unreal Python log for "
		"details, and consider writing to the shared `result` dict for future runs.");
}

// ==================== VIEWPORT / SCENE ====================

FString UUnrealGPTToolExecutor::GetViewportScreenshot(const FString& ArgumentsJson, FString& OutMetadataJson)
{
	// Parse optional focus_actor argument
	FString FocusActorLabel;
	if (!ArgumentsJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			ArgsObj->TryGetStringField(TEXT("focus_actor"), FocusActorLabel);
		}
	}

	// Use enhanced version that returns metadata alongside the image
	return UUnrealGPTSceneContext::CaptureViewportScreenshotWithMetadata(OutMetadataJson, FocusActorLabel);
}

FString UUnrealGPTToolExecutor::GetSceneSummary(int32 PageSize)
{
	return UUnrealGPTSceneContext::GetSceneSummary(PageSize);
}

// ==================== HELPERS ====================

AActor* UUnrealGPTToolExecutor::FindActorByIdOrLabel(const FString& Id, const FString& Label)
{
	if (!GEditor)
	{
		return nullptr;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return nullptr;
	}

	// Priority: Id (internal name) first, then Label
	// Id is the stable internal name (Actor->GetName()), guaranteed unique within a level
	// Label is the user-friendly display name (Actor->GetActorLabel()), may have duplicates

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		// Check Id first (exact match on internal name) - this is the stable identifier
		if (!Id.IsEmpty() && Actor->GetName() == Id)
		{
			return Actor;
		}
	}

	// If no Id match, fall back to Label search
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		if (!Label.IsEmpty() && Actor->GetActorLabel() == Label)
		{
			return Actor;
		}
	}

	return nullptr;
}

// Legacy wrapper for backwards compatibility
AActor* UUnrealGPTToolExecutor::FindActorByLabelOrName(const FString& Label, const FString& Name)
{
	// Name parameter was the internal name, which is now called Id
	return FindActorByIdOrLabel(Name, Label);
}

void UUnrealGPTToolExecutor::FocusViewportOnCreatedAsset(const FString& ResultJson)
{
	if (!GEditor || ResultJson.IsEmpty())
	{
		return;
	}

	// Parse the result JSON to check for created asset information
	TSharedPtr<FJsonObject> ResultObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, ResultObj) || !ResultObj.IsValid())
	{
		return;
	}

	// Check if the operation was successful
	FString Status;
	if (!ResultObj->TryGetStringField(TEXT("status"), Status) || Status != TEXT("ok"))
	{
		return; // Don't focus on failed operations
	}

	// Check for actor information in details
	const TSharedPtr<FJsonObject>* DetailsObj = nullptr;
	if (!ResultObj->TryGetObjectField(TEXT("details"), DetailsObj) || !DetailsObj || !DetailsObj->IsValid())
	{
		return;
	}

	FString ActorName;
	FString ActorLabel;
	FString AssetPath;

	(*DetailsObj)->TryGetStringField(TEXT("actor_name"), ActorName);
	(*DetailsObj)->TryGetStringField(TEXT("actor_label"), ActorLabel);
	(*DetailsObj)->TryGetStringField(TEXT("asset_path"), AssetPath);

	// If we have actor information, try to find and focus on the actor
	if (!ActorName.IsEmpty() || !ActorLabel.IsEmpty())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return;
		}

		AActor* FoundActor = nullptr;

		// Search for actor by name or label
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (!Actor || Actor->IsPendingKillPending())
			{
				continue;
			}

			// Check by name or label
			if ((!ActorName.IsEmpty() && Actor->GetName() == ActorName) ||
				(!ActorLabel.IsEmpty() && Actor->GetActorLabel() == ActorLabel))
			{
				FoundActor = Actor;
				break;
			}
		}

		if (FoundActor)
		{
			// Select the actor
			GEditor->SelectActor(FoundActor, true, true);

			// Focus viewport cameras on the actor (pass by reference)
			GEditor->MoveViewportCamerasToActor(*FoundActor, false);

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Focused viewport on created actor: %s"), *FoundActor->GetActorLabel());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Could not find created actor (name: %s, label: %s)"), *ActorName, *ActorLabel);
		}
	}
}

// ==================== ATOMIC EDITOR TOOLS ====================

FString UUnrealGPTToolExecutor::ExecuteGetActor(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse get_actor arguments\"}");
	}

	FString Id, Label;
	ArgsObj->TryGetStringField(TEXT("id"), Id);
	ArgsObj->TryGetStringField(TEXT("label"), Label);

	if (Id.IsEmpty() && Label.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Must provide 'id' or 'label'\"}");
	}

	AActor* Actor = FindActorByIdOrLabel(Id, Label);
	if (!Actor)
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Actor not found: %s\"}"),
			!Id.IsEmpty() ? *Id : *Label);
	}

	// Build detailed actor info
	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
	ResultObj->SetStringField(TEXT("status"), TEXT("ok"));

	TSharedPtr<FJsonObject> ActorObj = MakeShareable(new FJsonObject);
	ActorObj->SetStringField(TEXT("id"), Actor->GetName());       // Stable unique identifier
	ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel()); // User-friendly display name
	ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	// Location
	FVector Location = Actor->GetActorLocation();
	ActorObj->SetObjectField(TEXT("location"), MakeVectorJson(Location));

	// Rotation
	FRotator Rotation = Actor->GetActorRotation();
	ActorObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Rotation));

	// Scale
	FVector Scale = Actor->GetActorScale3D();
	ActorObj->SetObjectField(TEXT("scale"), MakeVectorJson(Scale));

	// Bounds
	FVector Origin, Extent;
	Actor->GetActorBounds(false, Origin, Extent);
	TSharedPtr<FJsonObject> BoundsObj = MakeShareable(new FJsonObject);
	BoundsObj->SetObjectField(TEXT("origin"), MakeVectorJson(Origin));
	BoundsObj->SetObjectField(TEXT("extent"), MakeVectorJson(Extent));
	ActorObj->SetObjectField(TEXT("bounds"), BoundsObj);

	// Mobility
	USceneComponent* RootComp = Actor->GetRootComponent();
	if (RootComp)
	{
		FString MobilityStr;
		switch (RootComp->Mobility)
		{
			case EComponentMobility::Static: MobilityStr = TEXT("Static"); break;
			case EComponentMobility::Stationary: MobilityStr = TEXT("Stationary"); break;
			case EComponentMobility::Movable: MobilityStr = TEXT("Movable"); break;
			default: MobilityStr = TEXT("Unknown"); break;
		}
		ActorObj->SetStringField(TEXT("mobility"), MobilityStr);
	}

	// Static mesh path (if applicable)
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Component : Components)
	{
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component))
		{
			if (UStaticMesh* Mesh = SMC->GetStaticMesh())
			{
				ActorObj->SetStringField(TEXT("static_mesh_path"), Mesh->GetPathName());
				break;
			}
		}
	}

	// Tags
	TArray<TSharedPtr<FJsonValue>> TagsArray;
	for (const FName& Tag : Actor->Tags)
	{
		TagsArray.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
	}
	ActorObj->SetArrayField(TEXT("tags"), TagsArray);

	// Folder path
	ActorObj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());

	ResultObj->SetObjectField(TEXT("actor"), ActorObj);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return ResultString;
}

FString UUnrealGPTToolExecutor::ExecuteSetActorTransform(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return MakeErrorResult(TEXT("Failed to parse set_actor_transform arguments"));
	}

	FString Id, Label;
	ArgsObj->TryGetStringField(TEXT("id"), Id);
	ArgsObj->TryGetStringField(TEXT("label"), Label);

	if (Id.IsEmpty() && Label.IsEmpty())
	{
		return MakeErrorResult(TEXT("Must provide 'id' or 'label'"));
	}

	AActor* Actor = FindActorByIdOrLabel(Id, Label);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found: %s"), !Id.IsEmpty() ? *Id : *Label));
	}

	// Begin transaction for undo support
	GEditor->BeginTransaction(NSLOCTEXT("UnrealGPT", "SetActorTransform", "Set Actor Transform"));
	Actor->Modify();

	// Get current transform
	FVector Location = Actor->GetActorLocation();
	FRotator Rotation = Actor->GetActorRotation();
	FVector Scale = Actor->GetActorScale3D();

	bool bSweep = false;
	ArgsObj->TryGetBoolField(TEXT("sweep"), bSweep);

	// Parse and apply location
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
	{
		double X = Location.X, Y = Location.Y, Z = Location.Z;
		(*LocObj)->TryGetNumberField(TEXT("x"), X);
		(*LocObj)->TryGetNumberField(TEXT("y"), Y);
		(*LocObj)->TryGetNumberField(TEXT("z"), Z);
		Location = FVector(X, Y, Z);
	}

	// Parse and apply rotation
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
	{
		double Pitch = Rotation.Pitch, Yaw = Rotation.Yaw, Roll = Rotation.Roll;
		(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
		Rotation = FRotator(Pitch, Yaw, Roll);
	}

	// Parse and apply scale
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj)
	{
		double X = Scale.X, Y = Scale.Y, Z = Scale.Z;
		(*ScaleObj)->TryGetNumberField(TEXT("x"), X);
		(*ScaleObj)->TryGetNumberField(TEXT("y"), Y);
		(*ScaleObj)->TryGetNumberField(TEXT("z"), Z);
		Scale = FVector(X, Y, Z);
	}

	// Apply transforms
	Actor->SetActorLocation(Location, bSweep);
	Actor->SetActorRotation(Rotation);
	Actor->SetActorScale3D(Scale);

	GEditor->EndTransaction();

	// Build result using helpers
	TSharedPtr<FJsonObject> NewTransform = MakeShareable(new FJsonObject);
	NewTransform->SetObjectField(TEXT("location"), MakeVectorJson(Location));
	NewTransform->SetObjectField(TEXT("rotation"), MakeRotatorJson(Rotation));
	NewTransform->SetObjectField(TEXT("scale"), MakeVectorJson(Scale));

	TSharedPtr<FJsonObject> Details = MakeShareable(new FJsonObject);
	Details->SetStringField(TEXT("id"), Actor->GetName());
	Details->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Details->SetObjectField(TEXT("transform"), NewTransform);

	return MakeSuccessResult(FString::Printf(TEXT("Transform updated for %s"), *Actor->GetActorLabel()), Details);
}

FString UUnrealGPTToolExecutor::ExecuteSelectActors(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse select_actors arguments\"}");
	}

	// Support both 'ids' (preferred) and 'labels' (backwards compatible)
	const TArray<TSharedPtr<FJsonValue>>* IdsArray = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* LabelsArray = nullptr;
	ArgsObj->TryGetArrayField(TEXT("ids"), IdsArray);
	ArgsObj->TryGetArrayField(TEXT("labels"), LabelsArray);

	if ((!IdsArray || IdsArray->Num() == 0) && (!LabelsArray || LabelsArray->Num() == 0))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Must provide 'ids' or 'labels' array\"}");
	}

	bool bAddToSelection = false;
	ArgsObj->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);

	if (!bAddToSelection)
	{
		GEditor->SelectNone(false, true);
	}

	// Track selected actors with both id and label
	TArray<TPair<FString, FString>> SelectedActors; // id, label pairs
	TArray<FString> NotFoundItems;

	// Process ids first (preferred)
	if (IdsArray && IdsArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& IdVal : *IdsArray)
		{
			FString Id = IdVal->AsString();
			if (Id.IsEmpty())
			{
				continue;
			}

			AActor* Actor = FindActorByIdOrLabel(Id, TEXT(""));
			if (Actor)
			{
				GEditor->SelectActor(Actor, true, true);
				SelectedActors.Add(TPair<FString, FString>(Actor->GetName(), Actor->GetActorLabel()));
			}
			else
			{
				NotFoundItems.Add(Id);
			}
		}
	}

	// Then process labels (backwards compatible)
	if (LabelsArray && LabelsArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& LabelVal : *LabelsArray)
		{
			FString Label = LabelVal->AsString();
			if (Label.IsEmpty())
			{
				continue;
			}

			AActor* Actor = FindActorByIdOrLabel(TEXT(""), Label);
			if (Actor)
			{
				GEditor->SelectActor(Actor, true, true);
				SelectedActors.Add(TPair<FString, FString>(Actor->GetName(), Actor->GetActorLabel()));
			}
			else
			{
				NotFoundItems.Add(Label);
			}
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
	ResultObj->SetStringField(TEXT("status"), TEXT("ok"));
	ResultObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Selected %d actors"), SelectedActors.Num()));

	// Return selected actors with both id and label
	TArray<TSharedPtr<FJsonValue>> SelectedArray;
	for (const auto& Pair : SelectedActors)
	{
		TSharedPtr<FJsonObject> ActorRef = MakeShareable(new FJsonObject);
		ActorRef->SetStringField(TEXT("id"), Pair.Key);
		ActorRef->SetStringField(TEXT("label"), Pair.Value);
		SelectedArray.Add(MakeShareable(new FJsonValueObject(ActorRef)));
	}
	ResultObj->SetArrayField(TEXT("selected"), SelectedArray);

	if (NotFoundItems.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArray;
		for (const FString& Item : NotFoundItems)
		{
			NotFoundArray.Add(MakeShareable(new FJsonValueString(Item)));
		}
		ResultObj->SetArrayField(TEXT("not_found"), NotFoundArray);
	}

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return ResultString;
}

FString UUnrealGPTToolExecutor::ExecuteDuplicateActor(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse duplicate_actor arguments\"}");
	}

	FString Id, Label;
	ArgsObj->TryGetStringField(TEXT("id"), Id);
	ArgsObj->TryGetStringField(TEXT("label"), Label);

	if (Id.IsEmpty() && Label.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Must provide 'id' or 'label'\"}");
	}

	AActor* SourceActor = FindActorByIdOrLabel(Id, Label);
	if (!SourceActor)
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Actor not found: %s\"}"), !Id.IsEmpty() ? *Id : *Label);
	}

	int32 Count = 1;
	ArgsObj->TryGetNumberField(TEXT("count"), Count);
	Count = FMath::Max(1, FMath::Min(Count, 100)); // Cap at 100 to prevent abuse

	FVector Offset = FVector::ZeroVector;
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (ArgsObj->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj)
	{
		double X = 0, Y = 0, Z = 0;
		(*OffsetObj)->TryGetNumberField(TEXT("x"), X);
		(*OffsetObj)->TryGetNumberField(TEXT("y"), Y);
		(*OffsetObj)->TryGetNumberField(TEXT("z"), Z);
		Offset = FVector(X, Y, Z);
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("{\"status\":\"error\",\"message\":\"No world available\"}");
	}

	GEditor->BeginTransaction(NSLOCTEXT("UnrealGPT", "DuplicateActor", "Duplicate Actor"));

	// Track created actors with both id and label
	TArray<TPair<FString, FString>> CreatedActors; // id, label pairs
	FVector CurrentOffset = Offset;

	for (int32 i = 0; i < Count; ++i)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Template = SourceActor;

		AActor* NewActor = World->SpawnActor<AActor>(SourceActor->GetClass(), SourceActor->GetActorLocation() + CurrentOffset, SourceActor->GetActorRotation(), SpawnParams);
		if (NewActor)
		{
			NewActor->SetActorScale3D(SourceActor->GetActorScale3D());
			CreatedActors.Add(TPair<FString, FString>(NewActor->GetName(), NewActor->GetActorLabel()));
		}

		CurrentOffset += Offset;
	}

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
	ResultObj->SetStringField(TEXT("status"), TEXT("ok"));
	ResultObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Created %d copies of %s"), CreatedActors.Num(), *SourceActor->GetActorLabel()));

	// Include source actor info
	TSharedPtr<FJsonObject> SourceObj = MakeShareable(new FJsonObject);
	SourceObj->SetStringField(TEXT("id"), SourceActor->GetName());
	SourceObj->SetStringField(TEXT("label"), SourceActor->GetActorLabel());
	ResultObj->SetObjectField(TEXT("source"), SourceObj);

	// Return created actors with both id and label
	TArray<TSharedPtr<FJsonValue>> CreatedArray;
	for (const auto& Pair : CreatedActors)
	{
		TSharedPtr<FJsonObject> ActorRef = MakeShareable(new FJsonObject);
		ActorRef->SetStringField(TEXT("id"), Pair.Key);
		ActorRef->SetStringField(TEXT("label"), Pair.Value);
		CreatedArray.Add(MakeShareable(new FJsonValueObject(ActorRef)));
	}
	ResultObj->SetArrayField(TEXT("created_actors"), CreatedArray);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return ResultString;
}

FString UUnrealGPTToolExecutor::ExecuteSnapActorToGround(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return MakeErrorResult(TEXT("Failed to parse snap_actor_to_ground arguments"));
	}

	FString Id, Label;
	ArgsObj->TryGetStringField(TEXT("id"), Id);
	ArgsObj->TryGetStringField(TEXT("label"), Label);

	if (Id.IsEmpty() && Label.IsEmpty())
	{
		return MakeErrorResult(TEXT("Must provide 'id' or 'label'"));
	}

	AActor* Actor = FindActorByIdOrLabel(Id, Label);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found: %s"), !Id.IsEmpty() ? *Id : *Label));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No world available"));
	}

	bool bAlignToNormal = false;
	ArgsObj->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

	double VerticalOffset = 0.0;
	ArgsObj->TryGetNumberField(TEXT("offset"), VerticalOffset);

	// Get actor bounds to find bottom
	FVector Origin, Extent;
	Actor->GetActorBounds(false, Origin, Extent);
	FVector ActorLocation = Actor->GetActorLocation();

	// Line trace from actor center downward
	FVector TraceStart = ActorLocation;
	FVector TraceEnd = ActorLocation - FVector(0, 0, 100000.0f); // 1km down

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Actor);

	bool bHit = World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, QueryParams);

	if (!bHit)
	{
		return MakeErrorResult(TEXT("No ground found below actor"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("UnrealGPT", "SnapActorToGround", "Snap Actor To Ground"));
	Actor->Modify();

	// Calculate new Z position: ground hit point + half actor extent + offset
	float NewZ = HitResult.ImpactPoint.Z + Extent.Z + VerticalOffset;
	FVector NewLocation = FVector(ActorLocation.X, ActorLocation.Y, NewZ);
	Actor->SetActorLocation(NewLocation);

	// Optionally align to surface normal
	if (bAlignToNormal)
	{
		FRotator NewRotation = HitResult.ImpactNormal.Rotation();
		// Adjust so "up" aligns with normal (rotate -90 pitch)
		NewRotation.Pitch -= 90.0f;
		Actor->SetActorRotation(NewRotation);
	}

	GEditor->EndTransaction();

	// Build result using helpers
	TSharedPtr<FJsonObject> Details = MakeShareable(new FJsonObject);
	Details->SetStringField(TEXT("id"), Actor->GetName());
	Details->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Details->SetObjectField(TEXT("new_location"), MakeVectorJson(NewLocation));
	Details->SetObjectField(TEXT("ground_hit"), MakeVectorJson(HitResult.ImpactPoint));

	return MakeSuccessResult(FString::Printf(TEXT("Snapped %s to ground"), *Actor->GetActorLabel()), Details);
}

FString UUnrealGPTToolExecutor::ExecuteSetActorsRotation(const FString& ArgumentsJson)
{
	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return MakeErrorResult(TEXT("Failed to parse set_actors_rotation arguments"));
	}

	// Support both 'ids' (preferred) and 'labels' (backwards compatible)
	const TArray<TSharedPtr<FJsonValue>>* IdsArray = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* LabelsArray = nullptr;
	ArgsObj->TryGetArrayField(TEXT("ids"), IdsArray);
	ArgsObj->TryGetArrayField(TEXT("labels"), LabelsArray);

	if ((!IdsArray || IdsArray->Num() == 0) && (!LabelsArray || LabelsArray->Num() == 0))
	{
		return MakeErrorResult(TEXT("Must provide 'ids' or 'labels' array"));
	}

	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (!ArgsObj->TryGetObjectField(TEXT("rotation"), RotObj) || !RotObj)
	{
		return MakeErrorResult(TEXT("Missing required field: rotation"));
	}

	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
	(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
	(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
	FRotator NewRotation(Pitch, Yaw, Roll);

	bool bRelative = false;
	ArgsObj->TryGetBoolField(TEXT("relative"), bRelative);

	// Begin transaction for undo support
	GEditor->BeginTransaction(NSLOCTEXT("UnrealGPT", "SetActorsRotation", "Set Actors Rotation"));

	// Track modified actors with both id and label
	TArray<TPair<FString, FString>> ModifiedActors; // id, label pairs
	TArray<FString> NotFoundItems;

	// Process ids first (preferred)
	if (IdsArray && IdsArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& IdVal : *IdsArray)
		{
			FString Id = IdVal->AsString();
			if (Id.IsEmpty())
			{
				continue;
			}

			AActor* Actor = FindActorByIdOrLabel(Id, TEXT(""));
			if (Actor)
			{
				Actor->Modify();

				FRotator FinalRotation = NewRotation;
				if (bRelative)
				{
					FinalRotation = Actor->GetActorRotation() + NewRotation;
				}

				Actor->SetActorRotation(FinalRotation);
				ModifiedActors.Add(TPair<FString, FString>(Actor->GetName(), Actor->GetActorLabel()));
			}
			else
			{
				NotFoundItems.Add(Id);
			}
		}
	}

	// Then process labels (backwards compatible)
	if (LabelsArray && LabelsArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& LabelVal : *LabelsArray)
		{
			FString Label = LabelVal->AsString();
			if (Label.IsEmpty())
			{
				continue;
			}

			AActor* Actor = FindActorByIdOrLabel(TEXT(""), Label);
			if (Actor)
			{
				Actor->Modify();

				FRotator FinalRotation = NewRotation;
				if (bRelative)
				{
					FinalRotation = Actor->GetActorRotation() + NewRotation;
				}

				Actor->SetActorRotation(FinalRotation);
				ModifiedActors.Add(TPair<FString, FString>(Actor->GetName(), Actor->GetActorLabel()));
			}
			else
			{
				NotFoundItems.Add(Label);
			}
		}
	}

	GEditor->EndTransaction();

	// Build result
	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);

	if (ModifiedActors.Num() > 0)
	{
		ResultObj->SetStringField(TEXT("status"), TEXT("ok"));
		ResultObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Rotation updated on %d actor(s)"), ModifiedActors.Num()));
	}
	else
	{
		ResultObj->SetStringField(TEXT("status"), TEXT("error"));
		ResultObj->SetStringField(TEXT("message"), TEXT("No actors were found/modified"));
	}

	ResultObj->SetNumberField(TEXT("modified_count"), ModifiedActors.Num());

	// Include sample of modified actors with both id and label (limit to first 10)
	TArray<TSharedPtr<FJsonValue>> ModifiedSample;
	int32 SampleLimit = FMath::Min(ModifiedActors.Num(), 10);
	for (int32 i = 0; i < SampleLimit; ++i)
	{
		TSharedPtr<FJsonObject> ActorRef = MakeShareable(new FJsonObject);
		ActorRef->SetStringField(TEXT("id"), ModifiedActors[i].Key);
		ActorRef->SetStringField(TEXT("label"), ModifiedActors[i].Value);
		ModifiedSample.Add(MakeShareable(new FJsonValueObject(ActorRef)));
	}
	ResultObj->SetArrayField(TEXT("modified_sample"), ModifiedSample);

	if (NotFoundItems.Num() > 0)
	{
		ResultObj->SetNumberField(TEXT("not_found_count"), NotFoundItems.Num());
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		int32 NotFoundLimit = FMath::Min(NotFoundItems.Num(), 5);
		for (int32 i = 0; i < NotFoundLimit; ++i)
		{
			NotFoundArr.Add(MakeShareable(new FJsonValueString(NotFoundItems[i])));
		}
		ResultObj->SetArrayField(TEXT("not_found_sample"), NotFoundArr);
	}

	ResultObj->SetObjectField(TEXT("applied_rotation"), MakeRotatorJson(NewRotation));
	ResultObj->SetBoolField(TEXT("relative"), bRelative);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	return ResultString;
}
