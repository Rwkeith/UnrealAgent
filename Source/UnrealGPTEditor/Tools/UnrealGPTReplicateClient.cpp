#include "UnrealGPTReplicateClient.h"
#include "UnrealGPTSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Http.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"

FString UUnrealGPTReplicateClient::Generate(const FString& ArgumentsJson)
{
	UUnrealGPTSettings* Settings = GetMutableDefault<UUnrealGPTSettings>();

	if (!Settings || !Settings->bEnableReplicateTool || Settings->ReplicateApiToken.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Replicate tool is not enabled or API token is missing in settings\"}");
	}

	TSharedPtr<FJsonObject> ArgsObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse replicate_generate arguments\"}");
	}

	FString Prompt;
	if (!(ArgsObj->TryGetStringField(TEXT("prompt"), Prompt) && !Prompt.IsEmpty()))
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Missing required field: prompt\"}");
	}

	FString OutputKind;
	ArgsObj->TryGetStringField(TEXT("output_kind"), OutputKind);
	OutputKind = OutputKind.ToLower();
	if (OutputKind.IsEmpty())
	{
		OutputKind = TEXT("image");
	}

	// Resolve the effective Replicate model version to use:
	// 1) If the tool call explicitly provided a 'version', respect that.
	// 2) Otherwise, pick a default per output kind from settings where possible.
	FString Version;
	if (!ArgsObj->TryGetStringField(TEXT("version"), Version) || Version.IsEmpty())
	{
		if (OutputKind == TEXT("image"))
		{
			Version = Settings->ReplicateImageModel;
		}
		else if (OutputKind == TEXT("video"))
		{
			Version = Settings->ReplicateVideoModel;
		}
		else if (OutputKind == TEXT("audio"))
		{
			// For audio we distinguish SFX vs music via conventions in the prompt;
			// by default prefer the SFX model, and the model itself can be a music model if desired.
			Version = Settings->ReplicateSFXModel;
		}
		else if (OutputKind == TEXT("3d") || OutputKind == TEXT("3d_model") || OutputKind == TEXT("model") || OutputKind == TEXT("mesh"))
		{
			Version = Settings->Replicate3DModel;
		}

		// If still empty, try additional hints from optional 'output_subkind',
		// e.g. 'sfx', 'music', or 'speech' for audio cases.
		if (Version.IsEmpty())
		{
			FString OutputSubkind;
			if (ArgsObj->TryGetStringField(TEXT("output_subkind"), OutputSubkind))
			{
				OutputSubkind = OutputSubkind.ToLower();

				if (OutputSubkind == TEXT("sfx"))
				{
					Version = Settings->ReplicateSFXModel;
				}
				else if (OutputSubkind == TEXT("music"))
				{
					Version = Settings->ReplicateMusicModel;
				}
				else if (OutputSubkind == TEXT("speech") || OutputSubkind == TEXT("voice"))
				{
					Version = Settings->ReplicateSpeechModel;
				}
			}
		}
	}

	// Detect when the configured identifier looks like an owner/name model slug
	// instead of a raw version id. For official models, Replicate supports
	// POST /v1/models/{owner}/{name}/predictions without a version field.
	const bool bLooksLikeModelSlug = Version.Contains(TEXT("/"));

	// If we still don't have any identifier at this point, fail fast with a clear error
	// instead of sending an invalid request to Replicate.
	if (Version.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction requires a model identifier. Configure a default model (owner/name slug or version id) in UnrealGPT settings or pass 'version' explicitly in replicate_generate arguments.\"}");
	}

	// Build Replicate prediction request body.
	TSharedPtr<FJsonObject> RequestObj = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> InputObj = MakeShareable(new FJsonObject);
	InputObj->SetStringField(TEXT("prompt"), Prompt);

	// For image generation, request PNG output directly from the model where supported.
	// Many Replicate image models accept an 'output_format' parameter; models that do not
	// simply ignore unknown fields, so this is safe as a default.
	if (OutputKind == TEXT("image"))
	{
		InputObj->SetStringField(TEXT("output_format"), TEXT("png"));
	}

	RequestObj->SetObjectField(TEXT("input"), InputObj);

	FString RequestBody;
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
		FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);
	}

	FString ApiUrl = Settings->ReplicateApiUrl.IsEmpty()
		? TEXT("https://api.replicate.com/v1/predictions")
		: Settings->ReplicateApiUrl;

	// If the identifier looks like an owner/name slug and we are using the default
	// predictions endpoint, route this call through the official models endpoint
	// so the user does not need to look up a separate version id.
	const bool bIsDefaultPredictionsEndpoint =
		(ApiUrl == TEXT("https://api.replicate.com/v1/predictions") || ApiUrl.EndsWith(TEXT("/v1/predictions")));

	const bool bUseOfficialModelsEndpoint = bLooksLikeModelSlug && bIsDefaultPredictionsEndpoint;

	if (bUseOfficialModelsEndpoint)
	{
		ApiUrl = FString::Printf(TEXT("https://api.replicate.com/v1/models/%s/predictions"), *Version);
	}
	else
	{
		// For the unified predictions endpoint, send the identifier as the 'version' field.
		RequestObj->SetStringField(TEXT("version"), Version);
		// Re-serialize with version field
		RequestBody.Empty();
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
		FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);
	}

	// 1) Create prediction
	FString CreateResponse;
	if (!PerformHttpRequest(ApiUrl, TEXT("POST"), RequestBody, Settings->ReplicateApiToken, CreateResponse, 60))
	{
		return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Failed to create Replicate prediction: %s\"}"), *CreateResponse);
	}

	TSharedPtr<FJsonObject> CreateObj;
	{
		TSharedRef<TJsonReader<>> CreateReader = TJsonReaderFactory<>::Create(CreateResponse);
		if (!(FJsonSerializer::Deserialize(CreateReader, CreateObj) && CreateObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse Replicate create prediction response\"}");
		}
	}

	FString PollUrl;
	{
		const TSharedPtr<FJsonObject>* UrlsObj = nullptr;
		if (CreateObj->TryGetObjectField(TEXT("urls"), UrlsObj) && UrlsObj && UrlsObj->IsValid())
		{
			(*UrlsObj)->TryGetStringField(TEXT("get"), PollUrl);
		}
	}

	if (PollUrl.IsEmpty())
	{
		return TEXT("{\"status\":\"error\",\"message\":\"Replicate response did not include a poll URL\"}");
	}

	// 2) Poll prediction until it completes
	FString FinalResponse;
	TSharedPtr<FJsonObject> FinalObj;
	const int32 MaxPollSeconds = 300;
	const double PollStart = FPlatformTime::Seconds();
	while (true)
	{
		if (!PerformHttpRequest(PollUrl, TEXT("GET"), FString(), Settings->ReplicateApiToken, FinalResponse, 60))
		{
			return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Failed while polling Replicate prediction: %s\"}"), *FinalResponse);
		}

		TSharedRef<TJsonReader<>> FinalReader = TJsonReaderFactory<>::Create(FinalResponse);
		if (!(FJsonSerializer::Deserialize(FinalReader, FinalObj) && FinalObj.IsValid()))
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Failed to parse Replicate poll response\"}");
		}

		FString PredStatus;
		FinalObj->TryGetStringField(TEXT("status"), PredStatus);

		if (PredStatus == TEXT("succeeded"))
		{
			break;
		}

		if (PredStatus == TEXT("failed") || PredStatus == TEXT("canceled"))
		{
			FString ErrorMsg;
			FinalObj->TryGetStringField(TEXT("error"), ErrorMsg);
			return FString::Printf(TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction %s: %s\"}"), *PredStatus, *ErrorMsg);
		}

		if (FPlatformTime::Seconds() - PollStart > MaxPollSeconds)
		{
			return TEXT("{\"status\":\"error\",\"message\":\"Replicate prediction polling timed out\"}");
		}

		FPlatformProcess::Sleep(0.5f);
	}

	// 3) Extract output URIs
	TArray<FString> OutputUris;

	// Prefer the 'output' field if present.
	{
		const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
		if (FinalObj->TryGetArrayField(TEXT("output"), OutputArray) && OutputArray)
		{
			for (const TSharedPtr<FJsonValue>& Val : *OutputArray)
			{
				CollectUrisFromJsonValue(Val, OutputUris);
			}
		}
		else
		{
			const TSharedPtr<FJsonObject>* OutputObj = nullptr;
			if (FinalObj->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj && OutputObj->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*OutputObj)->Values)
				{
					CollectUrisFromJsonValue(Pair.Value, OutputUris);
				}
			}
			else
			{
				FString OutputStr;
				if (FinalObj->TryGetStringField(TEXT("output"), OutputStr) &&
					(OutputStr.StartsWith(TEXT("http://")) || OutputStr.StartsWith(TEXT("https://"))))
				{
					OutputUris.AddUnique(OutputStr);
				}
			}
		}
	}

	// As a fallback, scan the entire response object for HTTPS URLs if we didn't find any in 'output'.
	if (OutputUris.Num() == 0)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FinalObj->Values)
		{
			CollectUrisFromJsonValue(Pair.Value, OutputUris);
		}
	}

	// 4) Download any output files and build result JSON.
	TArray<TSharedPtr<FJsonValue>> FilesArray;
	for (const FString& Uri : OutputUris)
	{
		const FString LocalPath = DownloadFile(Uri, OutputKind, Settings->ReplicateApiToken);
		if (!LocalPath.IsEmpty())
		{
			TSharedPtr<FJsonObject> FileObj = MakeShareable(new FJsonObject);
			FileObj->SetStringField(TEXT("local_path"), LocalPath);
			FileObj->SetStringField(TEXT("mime_type"), FPaths::GetExtension(LocalPath));
			FileObj->SetStringField(TEXT("description"), TEXT("Downloaded output from Replicate prediction"));
			FilesArray.Add(MakeShareable(new FJsonValueObject(FileObj)));
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
	ResultObj->SetStringField(TEXT("status"), TEXT("success"));

	const int32 NumFiles = FilesArray.Num();
	ResultObj->SetStringField(
		TEXT("message"),
		FString::Printf(TEXT("Replicate prediction succeeded with %d downloaded file(s)."), NumFiles));

	TSharedPtr<FJsonObject> DetailsObj = MakeShareable(new FJsonObject);
	DetailsObj->SetStringField(TEXT("provider"), TEXT("replicate"));
	DetailsObj->SetStringField(TEXT("output_kind"), OutputKind);
	DetailsObj->SetArrayField(TEXT("files"), FilesArray);

	ResultObj->SetObjectField(TEXT("details"), DetailsObj);

	FString ResultJsonString;
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultJsonString);
		FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
	}

	return ResultJsonString;
}

bool UUnrealGPTReplicateClient::PerformHttpRequest(const FString& Url, const FString& Verb, const FString& Body,
	const FString& AuthToken, FString& OutResponse, int32 TimeoutSeconds)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	// Replicate HTTP API expects Bearer tokens: Authorization: Bearer <token>
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));

	if (!Body.IsEmpty())
	{
		Request->SetContentAsString(Body);
	}

	bool bRequestComplete = false;
	bool bSuccess = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr Req, FHttpResponsePtr Res, bool bConnected)
		{
			if (bConnected && Res.IsValid() && Res->GetResponseCode() >= 200 && Res->GetResponseCode() < 300)
			{
				OutResponse = Res->GetContentAsString();
				bSuccess = true;
			}
			else if (Res.IsValid())
			{
				OutResponse = Res->GetContentAsString();
			}
			bRequestComplete = true;
		});

	Request->ProcessRequest();

	const double StartTime = FPlatformTime::Seconds();
	while (!bRequestComplete)
	{
		FPlatformProcess::Sleep(0.01f);
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			Request->CancelRequest();
			OutResponse = TEXT("{\"error\":\"Request timed out\"}");
			return false;
		}
	}

	return bSuccess;
}

FString UUnrealGPTReplicateClient::DownloadFile(const FString& Uri, const FString& OutputKind, const FString& AuthToken)
{
	TArray<uint8> Content;

	// Try HTTP download.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Uri);
	Request->SetVerb(TEXT("GET"));
	// Replicate file URLs may require the same Bearer token.
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));

	bool bComplete = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr Req, FHttpResponsePtr Res, bool bSuccess)
		{
			if (bSuccess && Res.IsValid() && Res->GetResponseCode() == 200)
			{
				Content = Res->GetContent();
			}
			bComplete = true;
		});

	Request->ProcessRequest();

	const double StartTime = FPlatformTime::Seconds();
	while (!bComplete)
	{
		FPlatformProcess::Sleep(0.01f);
		if (FPlatformTime::Seconds() - StartTime > 120.0)
		{
			Request->CancelRequest();
			break;
		}
	}

	if (Content.Num() == 0)
	{
		return FString();
	}

	FString Ext = FPaths::GetExtension(Uri);
	if (Ext.IsEmpty())
	{
		Ext = TEXT("dat");
	}

	const FString Filename = FGuid::NewGuid().ToString() + TEXT(".") + Ext;
	const FString SavePath = GetStagingFolder(OutputKind) / Filename;

	// Ensure directory exists
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SavePath), true);

	if (FFileHelper::SaveArrayToFile(Content, *SavePath))
	{
		return SavePath;
	}

	return FString();
}

FString UUnrealGPTReplicateClient::GetStagingFolder(const FString& OutputKind)
{
	const FString BasePath = FPaths::ProjectContentDir() / TEXT("UnrealGPT/Generated");
	const FString K = OutputKind.ToLower();

	if (K == TEXT("image"))
	{
		return BasePath / TEXT("Images");
	}
	if (K == TEXT("audio"))
	{
		return BasePath / TEXT("Audio");
	}
	if (K == TEXT("video"))
	{
		return BasePath / TEXT("Video");
	}
	if (K == TEXT("3d") || K == TEXT("3d_model") || K == TEXT("model") || K == TEXT("mesh"))
	{
		return BasePath / TEXT("Models");
	}

	return BasePath / TEXT("Misc");
}

void UUnrealGPTReplicateClient::CollectUrisFromJsonValue(const TSharedPtr<FJsonValue>& Val, TArray<FString>& OutUris)
{
	if (!Val.IsValid())
	{
		return;
	}

	switch (Val->Type)
	{
	case EJson::String:
		{
			const FString Str = Val->AsString();
			if (Str.StartsWith(TEXT("http://")) || Str.StartsWith(TEXT("https://")))
			{
				OutUris.AddUnique(Str);
			}
			break;
		}
	case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
			for (const TSharedPtr<FJsonValue>& Elem : Arr)
			{
				CollectUrisFromJsonValue(Elem, OutUris);
			}
			break;
		}
	case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (Obj.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
				{
					CollectUrisFromJsonValue(Pair.Value, OutUris);
				}
			}
			break;
		}
	default:
		break;
	}
}
