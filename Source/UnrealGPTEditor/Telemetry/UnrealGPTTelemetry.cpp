#include "UnrealGPTTelemetry.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"

FString UnrealGPTTelemetry::GetConversationLogPath(const FString& SessionId)
{
	const FString Filename = FString::Printf(TEXT("UnrealGPT_Conversation_%s.jsonl"), *SessionId);
	return FPaths::ProjectSavedDir() / TEXT("Logs") / Filename;
}

void UnrealGPTTelemetry::LogApiConversation(const FString& SessionId, const FString& Direction, const FString& JsonBody, int32 ResponseCode)
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	const FString ConversationLogPath = GetConversationLogPath(SessionId);

	TSharedPtr<FJsonObject> LogEntry = MakeShareable(new FJsonObject);
	LogEntry->SetStringField(TEXT("timestamp"), FDateTime::Now().ToIso8601());
	LogEntry->SetStringField(TEXT("direction"), Direction);

	if (ResponseCode > 0)
	{
		LogEntry->SetNumberField(TEXT("status_code"), ResponseCode);
	}

	TSharedPtr<FJsonObject> BodyJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
	if (FJsonSerializer::Deserialize(Reader, BodyJson) && BodyJson.IsValid())
	{
		LogEntry->SetObjectField(TEXT("body"), BodyJson);
	}
	else
	{
		LogEntry->SetStringField(TEXT("body_raw"), JsonBody.Left(50000));
	}

	FString LogLine;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&LogLine);
	FJsonSerializer::Serialize(LogEntry.ToSharedRef(), Writer);
	LogLine += TEXT("\n");

	FFileHelper::SaveStringToFile(
		LogLine,
		*ConversationLogPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		EFileWrite::FILEWRITE_Append
	);

	UE_LOG(LogTemp, Verbose, TEXT("UnrealGPT: Logged %s to conversation history"), *Direction);
}

void UnrealGPTTelemetry::LogRequestBodySummary(const FString& RequestBody, int32 MaxLogLength)
{
	if (MaxLogLength <= 0)
	{
		return;
	}

	if (RequestBody.Len() <= MaxLogLength)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body: %s"), *RequestBody);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body (truncated): %s..."), *RequestBody.Left(MaxLogLength));
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Request body length: %d characters"), RequestBody.Len());
	}
}
