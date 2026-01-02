// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTSessionTypes.h"
#include "UnrealGPTSessionManager.generated.h"

// Note: Using simple delegates instead of dynamic multicast because FSessionInfo is not a USTRUCT
DECLARE_MULTICAST_DELEGATE(FOnSessionListChangedNative);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSessionLoaded, const FString&, SessionId, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSessionSaved, const FString&, SessionId, bool, bSuccess);

/**
 * Manages persistence and loading of conversation sessions.
 * Sessions are stored per-project at {ProjectDir}/Saved/UnrealGPT/Sessions/{SessionId}/session.json
 */
UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTSessionManager : public UObject
{
	GENERATED_BODY()

public:
	UUnrealGPTSessionManager();

	/** Initialize the session manager */
	void Initialize();

	// ==================== SESSION DIRECTORY MANAGEMENT ====================

	/** Get the sessions directory path for the current project */
	static FString GetSessionsDirectory();

	/** Get path for a specific session's directory */
	static FString GetSessionDirectory(const FString& SessionId);

	/** Get path for a specific session file */
	static FString GetSessionFilePath(const FString& SessionId);

	// ==================== SESSION LISTING ====================

	/** Get list of all available sessions (metadata only, sorted by last modified) */
	TArray<FSessionInfo> GetSessionList() const;

	/** Refresh the cached session list from disk */
	void RefreshSessionList();

	/** Check if a session exists */
	bool SessionExists(const FString& SessionId) const;

	// ==================== SESSION PERSISTENCE ====================

	/** Save session data to disk */
	bool SaveSession(const FSessionData& SessionData);

	/** Load a complete session from disk */
	bool LoadSession(const FString& SessionId, FSessionData& OutSessionData);

	/** Delete a session from disk */
	bool DeleteSession(const FString& SessionId);

	// ==================== AUTO-SAVE SUPPORT ====================

	/** Begin tracking a session for auto-save */
	void BeginAutoSave(const FString& SessionId);

	/** End auto-save tracking */
	void EndAutoSave();

	/** Get the currently tracked session ID */
	FString GetCurrentSessionId() const { return CurrentSessionId; }

	/** Check if auto-save is active */
	bool IsAutoSaveActive() const { return bAutoSaveActive; }

	/** Get the current session data being tracked */
	FSessionData& GetCurrentSessionData() { return CurrentSessionData; }
	const FSessionData& GetCurrentSessionData() const { return CurrentSessionData; }

	/** Set the current session data (used when loading) */
	void SetCurrentSessionData(const FSessionData& SessionData);

	/** Append message to current session */
	void AppendMessage(const FPersistedMessage& Message);

	/** Append tool call to current session */
	void AppendToolCall(const FPersistedToolCall& ToolCall);

	/** Update the session title (from first user message) */
	void UpdateSessionTitle(const FString& FirstUserMessage);

	/** Trigger a save of the current session */
	bool SaveCurrentSession();

	// ==================== DELEGATES ====================

	/** Native delegate for session list changes (not Blueprint-accessible since FSessionInfo is not a USTRUCT) */
	FOnSessionListChangedNative OnSessionListChanged;

	UPROPERTY(BlueprintAssignable)
	FOnSessionLoaded OnSessionLoaded;

	UPROPERTY(BlueprintAssignable)
	FOnSessionSaved OnSessionSaved;

private:
	/** Parse session info from a JSON file without loading full content */
	static bool ParseSessionInfo(const FString& FilePath, FSessionInfo& OutInfo);

	/** Generate a title from the first user message */
	static FString GenerateTitle(const FString& FirstUserMessage);

	/** Ensure the sessions directory exists */
	static bool EnsureSessionsDirectoryExists();

	/** Ensure a specific session's directory exists */
	static bool EnsureSessionDirectoryExists(const FString& SessionId);

	/** Maximum file size to load (100MB) */
	static constexpr int64 MaxSessionFileSizeBytes = 100 * 1024 * 1024;

	/** Maximum number of sessions to keep in list */
	static constexpr int32 MaxSessionsInList = 50;

	/** Cached list of sessions */
	TArray<FSessionInfo> CachedSessionList;

	/** Currently active session being auto-saved */
	FString CurrentSessionId;

	/** Accumulated data for current session */
	FSessionData CurrentSessionData;

	/** Whether auto-save is active */
	bool bAutoSaveActive;

	/** Whether title has been set for current session */
	bool bTitleSet;
};
