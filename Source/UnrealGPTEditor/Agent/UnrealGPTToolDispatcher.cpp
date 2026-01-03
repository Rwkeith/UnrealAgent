#include "UnrealGPTToolDispatcher.h"
#include "UnrealGPTJsonHelpers.h"
#include "UnrealGPTReplicateClient.h"
#include "UnrealGPTSceneContext.h"
#include "UnrealGPTToolExecutor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

FString UnrealGPTToolDispatcher::ExecuteToolCall(
	const FString& ToolName,
	const FString& ArgumentsJson,
	bool& bLastToolWasPythonExecute,
	bool& bLastSceneQueryFoundResults,
	TFunction<void(const FString&, const FString&)> BroadcastToolCall)
{
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: ExecuteToolCall ENTRY - Tool: %s, Args length: %d"), *ToolName, ArgumentsJson.Len());

	FString Result;

	const bool bIsPythonExecute = (ToolName == TEXT("python_execute"));
	const bool bIsSceneQuery = (ToolName == TEXT("scene_query"));

	if (bIsPythonExecute)
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			FString Code;
			if (ArgsObj->TryGetStringField(TEXT("code"), Code))
			{
				Result = UUnrealGPTToolExecutor::ExecutePythonCode(Code);
			}
		}
	}
	else if (ToolName == TEXT("viewport_screenshot"))
	{
		FString MetadataJson;
		FString ImageBase64 = UUnrealGPTToolExecutor::GetViewportScreenshot(ArgumentsJson, MetadataJson);

		// The image will be sent as multimodal input separately.
		// Return the metadata JSON as the tool result so the model has context.
		// The image base64 is stored separately for multimodal handling.
		if (!ImageBase64.IsEmpty())
		{
			Result = MetadataJson;
			Result += TEXT("\n__IMAGE_BASE64__\n") + ImageBase64;
		}
		else
		{
			Result = MetadataJson; // Will contain error info
		}
	}
	else if (bIsSceneQuery)
	{
		Result = UUnrealGPTSceneContext::QueryScene(ArgumentsJson);

		bLastSceneQueryFoundResults = !Result.IsEmpty() && Result != TEXT("[]") && Result.StartsWith(TEXT("["));
		if (bLastSceneQueryFoundResults)
		{
			TSharedPtr<FJsonValue> JsonValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
			if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
				if (JsonValue->Type == EJson::Array && JsonValue->TryGetArray(JsonArray))
				{
					bLastSceneQueryFoundResults = (JsonArray->Num() > 0);
					if (bLastSceneQueryFoundResults)
					{
						UE_LOG(LogTemp, Log, TEXT("UnrealGPT: scene_query found %d results - will block subsequent python_execute"), JsonArray->Num());
					}
				}
				else
				{
					bLastSceneQueryFoundResults = false;
				}
			}
			else
			{
				bLastSceneQueryFoundResults = false;
			}
		}
	}
	else if (ToolName == TEXT("reflection_query"))
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse reflection_query arguments\"}");
		}

		FString ClassName;
		if (!ArgsObj->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: class_name\"}");
		}

		UClass* TargetClass = FindObject<UClass>(nullptr, *ClassName);
		if (!TargetClass)
		{
			TargetClass = LoadObject<UClass>(nullptr, *ClassName);
		}

		Result = UnrealGPTJsonHelpers::BuildReflectionSchemaJson(TargetClass);
	}
	else if (ToolName == TEXT("replicate_generate"))
	{
		Result = UUnrealGPTReplicateClient::Generate(ArgumentsJson);
	}
	else if (ToolName == TEXT("file_search") || ToolName == TEXT("web_search"))
	{
		Result = FString::Printf(TEXT("Tool '%s' executed successfully by server."), *ToolName);
	}
	// ==================== ATOMIC EDITOR TOOLS (delegated to ToolExecutor) ====================
	else if (ToolName == TEXT("get_actor"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteGetActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSetActorTransform(ArgumentsJson);
	}
	else if (ToolName == TEXT("select_actors"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSelectActors(ArgumentsJson);
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteDuplicateActor(ArgumentsJson);
	}
	else if (ToolName == TEXT("snap_actor_to_ground"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSnapActorToGround(ArgumentsJson);
	}
	else if (ToolName == TEXT("set_actors_rotation"))
	{
		Result = UUnrealGPTToolExecutor::ExecuteSetActorsRotation(ArgumentsJson);
	}
	else
	{
		Result = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
	}

	bLastToolWasPythonExecute = bIsPythonExecute;

	if (!bIsSceneQuery)
	{
		bLastSceneQueryFoundResults = false;
	}

	if (BroadcastToolCall)
	{
		BroadcastToolCall(ToolName, ArgumentsJson);
	}

	return Result;
}
