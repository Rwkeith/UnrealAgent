// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealGPTSessionManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

UUnrealGPTSessionManager::UUnrealGPTSessionManager()
	: bAutoSaveActive(false)
	, bTitleSet(false)
{
}

void UUnrealGPTSessionManager::Initialize()
{
	EnsureSessionsDirectoryExists();
	RefreshSessionList();
}

// ==================== DIRECTORY MANAGEMENT ====================

FString UUnrealGPTSessionManager::GetSessionsDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealGPT"), TEXT("Sessions"));
}

FString UUnrealGPTSessionManager::GetSessionDirectory(const FString& SessionId)
{
	return FPaths::Combine(GetSessionsDirectory(), SessionId);
}

FString UUnrealGPTSessionManager::GetSessionFilePath(const FString& SessionId)
{
	return FPaths::Combine(GetSessionDirectory(SessionId), TEXT("session.json"));
}

bool UUnrealGPTSessionManager::EnsureSessionsDirectoryExists()
{
	FString SessionsDir = GetSessionsDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*SessionsDir))
	{
		if (!PlatformFile.CreateDirectoryTree(*SessionsDir))
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to create sessions directory: %s"), *SessionsDir);
			return false;
		}
	}
	return true;
}

bool UUnrealGPTSessionManager::EnsureSessionDirectoryExists(const FString& SessionId)
{
	FString SessionDir = GetSessionDirectory(SessionId);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*SessionDir))
	{
		if (!PlatformFile.CreateDirectoryTree(*SessionDir))
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to create session directory: %s"), *SessionDir);
			return false;
		}
	}
	return true;
}

// ==================== SESSION LISTING ====================

TArray<FSessionInfo> UUnrealGPTSessionManager::GetSessionList() const
{
	return CachedSessionList;
}

void UUnrealGPTSessionManager::RefreshSessionList()
{
	CachedSessionList.Empty();

	FString SessionsDir = GetSessionsDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*SessionsDir))
	{
		return;
	}

	// Find all session directories
	TArray<FString> SessionDirs;
	PlatformFile.IterateDirectory(*SessionsDir, [&SessionDirs](const TCHAR* FilenameOrDirectory, bool bIsDirectory) -> bool
	{
		if (bIsDirectory)
		{
			SessionDirs.Add(FilenameOrDirectory);
		}
		return true; // Continue iteration
	});

	// Parse session info from each directory
	for (const FString& SessionDir : SessionDirs)
	{
		FString SessionId = FPaths::GetCleanFilename(SessionDir);
		FString SessionFilePath = FPaths::Combine(SessionDir, TEXT("session.json"));

		FSessionInfo Info;
		if (ParseSessionInfo(SessionFilePath, Info))
		{
			Info.SessionId = SessionId;
			Info.FilePath = SessionFilePath;
			CachedSessionList.Add(Info);
		}
	}

	// Sort by last modified (newest first)
	CachedSessionList.Sort();

	// Limit to max sessions
	if (CachedSessionList.Num() > MaxSessionsInList)
	{
		CachedSessionList.SetNum(MaxSessionsInList);
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found %d sessions"), CachedSessionList.Num());

	// Broadcast change
	OnSessionListChanged.Broadcast();
}

bool UUnrealGPTSessionManager::SessionExists(const FString& SessionId) const
{
	FString SessionFilePath = GetSessionFilePath(SessionId);
	return FPaths::FileExists(SessionFilePath);
}

bool UUnrealGPTSessionManager::ParseSessionInfo(const FString& FilePath, FSessionInfo& OutInfo)
{
	if (!FPaths::FileExists(FilePath))
	{
		return false;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Could not read session file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Failed to parse session JSON: %s"), *FilePath);
		return false;
	}

	// Extract only the metadata we need for listing
	OutInfo.Title = JsonObject->GetStringField(TEXT("title"));
	OutInfo.MessageCount = JsonObject->GetIntegerField(TEXT("message_count"));

	FString CreatedAtStr, LastModifiedAtStr;
	if (JsonObject->TryGetStringField(TEXT("created_at"), CreatedAtStr))
	{
		FDateTime::ParseIso8601(*CreatedAtStr, OutInfo.CreatedAt);
	}
	if (JsonObject->TryGetStringField(TEXT("last_modified_at"), LastModifiedAtStr))
	{
		FDateTime::ParseIso8601(*LastModifiedAtStr, OutInfo.LastModifiedAt);
	}

	return true;
}

// ==================== SESSION PERSISTENCE ====================

bool UUnrealGPTSessionManager::SaveSession(const FSessionData& SessionData)
{
	if (SessionData.SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Cannot save session with empty ID"));
		return false;
	}

	// Ensure directory exists
	if (!EnsureSessionDirectoryExists(SessionData.SessionId))
	{
		return false;
	}

	FString FilePath = GetSessionFilePath(SessionData.SessionId);
	FString TempPath = FilePath + TEXT(".tmp");
	FString BackupPath = FilePath + TEXT(".backup");

	// Serialize to JSON
	TSharedPtr<FJsonObject> JsonObject = SessionData.ToJson();
	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);

	if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to serialize session JSON"));
		return false;
	}

	// Write to temp file first (atomic write pattern)
	if (!FFileHelper::SaveStringToFile(JsonString, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to write temp session file: %s"), *TempPath);
		return false;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Create backup of existing file
	if (FPaths::FileExists(FilePath))
	{
		PlatformFile.DeleteFile(*BackupPath);
		PlatformFile.MoveFile(*BackupPath, *FilePath);
	}

	// Rename temp to final
	if (!PlatformFile.MoveFile(*FilePath, *TempPath))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to rename temp to final session file"));
		// Try to restore backup
		if (FPaths::FileExists(BackupPath))
		{
			PlatformFile.MoveFile(*FilePath, *BackupPath);
		}
		return false;
	}

	// Clean up backup (optional - could keep for recovery)
	PlatformFile.DeleteFile(*BackupPath);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Saved session %s with %d messages"), *SessionData.SessionId, SessionData.Messages.Num());

	OnSessionSaved.Broadcast(SessionData.SessionId, true);
	return true;
}

bool UUnrealGPTSessionManager::LoadSession(const FString& SessionId, FSessionData& OutSessionData)
{
	FString FilePath = GetSessionFilePath(SessionId);

	// Check file exists
	if (!FPaths::FileExists(FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Session file not found: %s"), *FilePath);
		OnSessionLoaded.Broadcast(SessionId, false);
		return false;
	}

	// Check file size
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	int64 FileSize = PlatformFile.FileSize(*FilePath);
	if (FileSize > MaxSessionFileSizeBytes)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Session file too large (%lld MB), refusing to load: %s"),
			FileSize / (1024 * 1024), *FilePath);
		OnSessionLoaded.Broadcast(SessionId, false);
		return false;
	}

	// Read file
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Could not read session file: %s"), *FilePath);
		OnSessionLoaded.Broadcast(SessionId, false);
		return false;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to parse session JSON: %s"), *FilePath);

		// Attempt backup recovery
		FString BackupPath = FilePath + TEXT(".backup");
		if (FPaths::FileExists(BackupPath))
		{
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Attempting recovery from backup..."));
			if (FFileHelper::LoadFileToString(JsonString, *BackupPath))
			{
				Reader = TJsonReaderFactory<>::Create(JsonString);
				if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
				{
					UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Successfully recovered from backup"));
				}
				else
				{
					OnSessionLoaded.Broadcast(SessionId, false);
					return false;
				}
			}
			else
			{
				OnSessionLoaded.Broadcast(SessionId, false);
				return false;
			}
		}
		else
		{
			OnSessionLoaded.Broadcast(SessionId, false);
			return false;
		}
	}

	// Validate schema version
	int32 SchemaVersion = JsonObject->GetIntegerField(TEXT("schema_version"));
	if (SchemaVersion > FSessionData::CurrentSchemaVersion)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Session uses newer schema (v%d), some features may not load correctly"), SchemaVersion);
	}

	OutSessionData = FSessionData::FromJson(JsonObject);

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Loaded session %s with %d messages"), *SessionId, OutSessionData.Messages.Num());

	OnSessionLoaded.Broadcast(SessionId, true);
	return true;
}

bool UUnrealGPTSessionManager::DeleteSession(const FString& SessionId)
{
	FString SessionDir = GetSessionDirectory(SessionId);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*SessionDir))
	{
		return true; // Already doesn't exist
	}

	// Delete all files in directory
	bool bSuccess = PlatformFile.DeleteDirectoryRecursively(*SessionDir);

	if (bSuccess)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Deleted session: %s"), *SessionId);
		RefreshSessionList();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Failed to delete session: %s"), *SessionId);
	}

	return bSuccess;
}

// ==================== AUTO-SAVE SUPPORT ====================

void UUnrealGPTSessionManager::BeginAutoSave(const FString& SessionId)
{
	CurrentSessionId = SessionId;
	CurrentSessionData = FSessionData();
	CurrentSessionData.SessionId = SessionId;
	CurrentSessionData.CreatedAt = FDateTime::Now();
	CurrentSessionData.LastModifiedAt = FDateTime::Now();
	bAutoSaveActive = true;
	bTitleSet = false;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Started auto-save for session: %s"), *SessionId);
}

void UUnrealGPTSessionManager::EndAutoSave()
{
	if (bAutoSaveActive && !CurrentSessionId.IsEmpty())
	{
		// Final save before ending
		SaveCurrentSession();
	}

	CurrentSessionId.Empty();
	CurrentSessionData = FSessionData();
	bAutoSaveActive = false;
	bTitleSet = false;

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Ended auto-save"));
}

void UUnrealGPTSessionManager::SetCurrentSessionData(const FSessionData& SessionData)
{
	CurrentSessionData = SessionData;
	CurrentSessionId = SessionData.SessionId;
	bTitleSet = !SessionData.Title.IsEmpty();

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: SetCurrentSessionData - %d messages, %d tool calls"),
		CurrentSessionData.Messages.Num(), CurrentSessionData.ToolCalls.Num());
}

void UUnrealGPTSessionManager::AppendMessage(const FPersistedMessage& Message)
{
	if (!bAutoSaveActive)
	{
		return;
	}

	CurrentSessionData.Messages.Add(Message);
	CurrentSessionData.LastModifiedAt = FDateTime::Now();

	// Auto-set title from first user message
	if (!bTitleSet && Message.Role == TEXT("user") && !Message.Content.IsEmpty())
	{
		UpdateSessionTitle(Message.Content);
	}
}

void UUnrealGPTSessionManager::AppendToolCall(const FPersistedToolCall& ToolCall)
{
	if (!bAutoSaveActive)
	{
		return;
	}

	CurrentSessionData.ToolCalls.Add(ToolCall);
	CurrentSessionData.LastModifiedAt = FDateTime::Now();
}

void UUnrealGPTSessionManager::UpdateSessionTitle(const FString& FirstUserMessage)
{
	CurrentSessionData.Title = GenerateTitle(FirstUserMessage);
	bTitleSet = true;
}

FString UUnrealGPTSessionManager::GenerateTitle(const FString& FirstUserMessage)
{
	// Take first 50 characters of the message as title
	FString Title = FirstUserMessage.Left(50);

	// Remove newlines and excessive whitespace
	Title.ReplaceInline(TEXT("\r"), TEXT(" "));
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));

	// Collapse multiple spaces
	while (Title.Contains(TEXT("  ")))
	{
		Title.ReplaceInline(TEXT("  "), TEXT(" "));
	}

	Title.TrimStartAndEndInline();

	// Add ellipsis if truncated
	if (FirstUserMessage.Len() > 50)
	{
		Title += TEXT("...");
	}

	// Fallback if empty
	if (Title.IsEmpty())
	{
		Title = TEXT("New Conversation");
	}

	return Title;
}

bool UUnrealGPTSessionManager::SaveCurrentSession()
{
	if (!bAutoSaveActive || CurrentSessionId.IsEmpty())
	{
		return false;
	}

	// Don't save empty sessions
	if (CurrentSessionData.Messages.Num() == 0)
	{
		return true; // Not an error, just nothing to save
	}

	bool bSuccess = SaveSession(CurrentSessionData);

	if (bSuccess)
	{
		// Refresh list to include this session
		RefreshSessionList();
	}

	return bSuccess;
}
