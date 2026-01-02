// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Represents a single message entry in a persisted session.
 * Matches FAgentMessage but includes images inline for persistence.
 */
struct FPersistedMessage
{
	FString Role;                    // "user", "assistant", "system", "tool"
	FString Content;
	TArray<FString> ImageBase64;     // Inline base64 images for this message
	TArray<FString> ToolCallIds;     // For assistant messages with tool_calls
	FString ToolCallId;              // For tool messages
	FString ToolCallsJson;           // Tool calls array as JSON string
	FDateTime Timestamp;             // When message was created

	FPersistedMessage()
		: Timestamp(FDateTime::Now())
	{
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

		JsonObject->SetStringField(TEXT("role"), Role);
		JsonObject->SetStringField(TEXT("content"), Content);
		JsonObject->SetStringField(TEXT("tool_call_id"), ToolCallId);
		JsonObject->SetStringField(TEXT("tool_calls_json"), ToolCallsJson);
		JsonObject->SetStringField(TEXT("timestamp"), Timestamp.ToIso8601());

		// Images array
		TArray<TSharedPtr<FJsonValue>> ImagesArray;
		for (const FString& Image : ImageBase64)
		{
			ImagesArray.Add(MakeShareable(new FJsonValueString(Image)));
		}
		JsonObject->SetArrayField(TEXT("images_base64"), ImagesArray);

		// Tool call IDs array
		TArray<TSharedPtr<FJsonValue>> ToolCallIdsArray;
		for (const FString& Id : ToolCallIds)
		{
			ToolCallIdsArray.Add(MakeShareable(new FJsonValueString(Id)));
		}
		JsonObject->SetArrayField(TEXT("tool_call_ids"), ToolCallIdsArray);

		return JsonObject;
	}

	static FPersistedMessage FromJson(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FPersistedMessage Message;

		if (!JsonObject.IsValid())
		{
			return Message;
		}

		Message.Role = JsonObject->GetStringField(TEXT("role"));
		Message.Content = JsonObject->GetStringField(TEXT("content"));
		Message.ToolCallId = JsonObject->GetStringField(TEXT("tool_call_id"));
		Message.ToolCallsJson = JsonObject->GetStringField(TEXT("tool_calls_json"));

		// Parse timestamp
		FString TimestampStr;
		if (JsonObject->TryGetStringField(TEXT("timestamp"), TimestampStr))
		{
			FDateTime::ParseIso8601(*TimestampStr, Message.Timestamp);
		}

		// Parse images array
		const TArray<TSharedPtr<FJsonValue>>* ImagesArray;
		if (JsonObject->TryGetArrayField(TEXT("images_base64"), ImagesArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ImagesArray)
			{
				FString ImageData = Value->AsString();
				if (!ImageData.IsEmpty())
				{
					Message.ImageBase64.Add(ImageData);
				}
			}
		}

		// Parse tool call IDs array
		const TArray<TSharedPtr<FJsonValue>>* ToolCallIdsArray;
		if (JsonObject->TryGetArrayField(TEXT("tool_call_ids"), ToolCallIdsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ToolCallIdsArray)
			{
				Message.ToolCallIds.Add(Value->AsString());
			}
		}

		return Message;
	}
};

/**
 * Represents a tool call display entry for UI reconstruction.
 */
struct FPersistedToolCall
{
	FString ToolName;
	FString Arguments;
	FString Result;
	FDateTime Timestamp;

	FPersistedToolCall()
		: Timestamp(FDateTime::Now())
	{
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

		JsonObject->SetStringField(TEXT("tool_name"), ToolName);
		JsonObject->SetStringField(TEXT("arguments"), Arguments);
		JsonObject->SetStringField(TEXT("result"), Result);
		JsonObject->SetStringField(TEXT("timestamp"), Timestamp.ToIso8601());

		return JsonObject;
	}

	static FPersistedToolCall FromJson(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FPersistedToolCall ToolCall;

		if (!JsonObject.IsValid())
		{
			return ToolCall;
		}

		ToolCall.ToolName = JsonObject->GetStringField(TEXT("tool_name"));
		ToolCall.Arguments = JsonObject->GetStringField(TEXT("arguments"));
		ToolCall.Result = JsonObject->GetStringField(TEXT("result"));

		FString TimestampStr;
		if (JsonObject->TryGetStringField(TEXT("timestamp"), TimestampStr))
		{
			FDateTime::ParseIso8601(*TimestampStr, ToolCall.Timestamp);
		}

		return ToolCall;
	}
};

/**
 * Complete session metadata and content.
 */
struct FSessionData
{
	static constexpr int32 CurrentSchemaVersion = 1;

	int32 SchemaVersion;
	FString SessionId;              // YYYYMMDD_HHMMSS format
	FString Title;                  // Auto-generated from first user message
	FDateTime CreatedAt;
	FDateTime LastModifiedAt;
	FString PreviousResponseId;     // For Responses API state
	TArray<FPersistedMessage> Messages;
	TArray<FPersistedToolCall> ToolCalls;

	FSessionData()
		: SchemaVersion(CurrentSchemaVersion)
		, CreatedAt(FDateTime::Now())
		, LastModifiedAt(FDateTime::Now())
	{
	}

	int32 GetMessageCount() const
	{
		return Messages.Num();
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

		JsonObject->SetNumberField(TEXT("schema_version"), SchemaVersion);
		JsonObject->SetStringField(TEXT("session_id"), SessionId);
		JsonObject->SetStringField(TEXT("title"), Title);
		JsonObject->SetStringField(TEXT("created_at"), CreatedAt.ToIso8601());
		JsonObject->SetStringField(TEXT("last_modified_at"), LastModifiedAt.ToIso8601());
		JsonObject->SetStringField(TEXT("previous_response_id"), PreviousResponseId);
		JsonObject->SetNumberField(TEXT("message_count"), Messages.Num());

		// Messages array
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		for (const FPersistedMessage& Msg : Messages)
		{
			MessagesArray.Add(MakeShareable(new FJsonValueObject(Msg.ToJson())));
		}
		JsonObject->SetArrayField(TEXT("messages"), MessagesArray);

		// Tool calls array
		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		for (const FPersistedToolCall& TC : ToolCalls)
		{
			ToolCallsArray.Add(MakeShareable(new FJsonValueObject(TC.ToJson())));
		}
		JsonObject->SetArrayField(TEXT("tool_calls"), ToolCallsArray);

		return JsonObject;
	}

	static FSessionData FromJson(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FSessionData Session;

		if (!JsonObject.IsValid())
		{
			return Session;
		}

		Session.SchemaVersion = JsonObject->GetIntegerField(TEXT("schema_version"));
		Session.SessionId = JsonObject->GetStringField(TEXT("session_id"));
		Session.Title = JsonObject->GetStringField(TEXT("title"));
		Session.PreviousResponseId = JsonObject->GetStringField(TEXT("previous_response_id"));

		// Parse timestamps
		FString CreatedAtStr, LastModifiedAtStr;
		if (JsonObject->TryGetStringField(TEXT("created_at"), CreatedAtStr))
		{
			FDateTime::ParseIso8601(*CreatedAtStr, Session.CreatedAt);
		}
		if (JsonObject->TryGetStringField(TEXT("last_modified_at"), LastModifiedAtStr))
		{
			FDateTime::ParseIso8601(*LastModifiedAtStr, Session.LastModifiedAt);
		}

		// Parse messages array
		const TArray<TSharedPtr<FJsonValue>>* MessagesArray;
		if (JsonObject->TryGetArrayField(TEXT("messages"), MessagesArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *MessagesArray)
			{
				const TSharedPtr<FJsonObject>* MsgObject;
				if (Value->TryGetObject(MsgObject))
				{
					Session.Messages.Add(FPersistedMessage::FromJson(*MsgObject));
				}
			}
		}

		// Parse tool calls array
		const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray;
		if (JsonObject->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ToolCallsArray)
			{
				const TSharedPtr<FJsonObject>* TCObject;
				if (Value->TryGetObject(TCObject))
				{
					Session.ToolCalls.Add(FPersistedToolCall::FromJson(*TCObject));
				}
			}
		}

		return Session;
	}
};

/**
 * Lightweight session info for listing (without loading full content).
 */
struct FSessionInfo
{
	FString SessionId;
	FString Title;
	FDateTime CreatedAt;
	FDateTime LastModifiedAt;
	int32 MessageCount;
	FString FilePath;

	FSessionInfo()
		: MessageCount(0)
	{
	}

	// For sorting by LastModifiedAt (newest first)
	bool operator<(const FSessionInfo& Other) const
	{
		return LastModifiedAt > Other.LastModifiedAt;
	}
};
