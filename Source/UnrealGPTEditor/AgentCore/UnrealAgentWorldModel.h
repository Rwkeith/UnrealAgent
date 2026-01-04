// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentWorldModel.generated.h"

/**
 * Cached state of an actor in the world model.
 *
 * This is the agent's understanding of an actor - it may be stale
 * or based on assumptions rather than verified data.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FActorState
{
	GENERATED_BODY()

	// ==================== IDENTITY ====================

	/** Unique actor ID (stable across queries) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ActorId;

	/** Actor label (display name) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Label;

	/** Actor class name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ClassName;

	/** Full actor path */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ActorPath;

	// ==================== TRANSFORM ====================

	/** World location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Location = FVector::ZeroVector;

	/** World rotation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FRotator Rotation = FRotator::ZeroRotator;

	/** World scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Scale = FVector::OneVector;

	/** Bounding box */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FBox Bounds;

	// ==================== PROPERTIES ====================

	/** Static mesh path (if applicable) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString MeshPath;

	/** Actor tags */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Tags;

	/** Custom properties */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FString> Properties;

	/** Component names */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Components;

	// ==================== METADATA ====================

	/** How confident are we in this data */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EActorConfidence Confidence = EActorConfidence::Confirmed;

	/** When this was last verified via scene query */
	FDateTime LastVerified;

	/** When this was last modified by the agent */
	FDateTime LastModified;

	/** Which goal created/modified this actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString SourceGoalId;

	// ==================== METHODS ====================

	FActorState()
		: LastVerified(FDateTime::Now())
		, LastModified(FDateTime::Now())
	{
	}

	/** Check if data is stale (older than threshold) */
	bool IsStale(float MaxAgeSeconds = 60.0f) const
	{
		FTimespan Age = FDateTime::Now() - LastVerified;
		return Age.GetTotalSeconds() > MaxAgeSeconds;
	}

	/** Mark as recently verified */
	void MarkVerified()
	{
		LastVerified = FDateTime::Now();
		Confidence = EActorConfidence::Confirmed;
	}

	/** Mark as assumed (based on our action, not verified) */
	void MarkAssumed()
	{
		Confidence = EActorConfidence::Assumed;
	}

	/** Get full transform */
	FTransform GetTransform() const
	{
		return FTransform(Rotation.Quaternion(), Location, Scale);
	}

	/** Set from transform */
	void SetTransform(const FTransform& Transform)
	{
		Location = Transform.GetLocation();
		Rotation = Transform.GetRotation().Rotator();
		Scale = Transform.GetScale3D();
		LastModified = FDateTime::Now();
	}

	/** Check if actor has a specific tag */
	bool HasTag(const FString& Tag) const
	{
		return Tags.Contains(Tag);
	}

	/** Get a property with default value */
	FString GetProperty(const FString& Key, const FString& Default = TEXT("")) const
	{
		const FString* Value = Properties.Find(Key);
		return Value ? *Value : Default;
	}
};

/**
 * Query parameters for finding actors in the world model.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FActorQuery
{
	GENERATED_BODY()

	/** Filter by class name (supports wildcards) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ClassFilter;

	/** Filter by label (supports contains/wildcards) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString LabelFilter;

	/** Filter by tag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString TagFilter;

	/** Filter by bounding box */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FBox BoundsFilter;

	/** Include stale data */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIncludeStale = false;

	/** Maximum results */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 MaxResults = 100;

	FActorQuery()
	{
		BoundsFilter.Init();
	}

	/** Create a class filter query */
	static FActorQuery ByClass(const FString& ClassName, int32 Max = 100)
	{
		FActorQuery Query;
		Query.ClassFilter = ClassName;
		Query.MaxResults = Max;
		return Query;
	}

	/** Create a label filter query */
	static FActorQuery ByLabel(const FString& Label, int32 Max = 100)
	{
		FActorQuery Query;
		Query.LabelFilter = Label;
		Query.MaxResults = Max;
		return Query;
	}

	/** Create a tag filter query */
	static FActorQuery ByTag(const FString& Tag, int32 Max = 100)
	{
		FActorQuery Query;
		Query.TagFilter = Tag;
		Query.MaxResults = Max;
		return Query;
	}

	/** Create a bounds filter query */
	static FActorQuery InBounds(const FBox& Bounds, int32 Max = 100)
	{
		FActorQuery Query;
		Query.BoundsFilter = Bounds;
		Query.MaxResults = Max;
		return Query;
	}
};

/**
 * World constraint that limits agent actions.
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FWorldConstraint
{
	GENERATED_BODY()

	/** Constraint description */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/** Type of constraint */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ConstraintType;  // "NoModify", "NoBounds", "NoDelete"

	/** Target (actor ID, tag, or class) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Target;

	/** Is this constraint active */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bActive = true;
};

/**
 * The agent's model of the world state.
 *
 * Key difference from LLM-controlled:
 * - Agent queries this FIRST before scene_query (cached data)
 * - Agent decides WHEN to refresh (not every time)
 * - Modifications are tracked for undo/debugging
 * - Constraints can prevent modifications
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FAgentWorldModel
{
	GENERATED_BODY()

	// ==================== ACTOR STATE ====================

	/** Known actors by ID */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FActorState> KnownActors;

	/** When was the last full scene refresh */
	FDateTime LastFullRefresh;

	// ==================== MODIFICATIONS ====================

	/** Modifications made this session */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FWorldModification> Modifications;

	// ==================== CONSTRAINTS ====================

	/** Active constraints */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FWorldConstraint> Constraints;

	// ==================== ASSET KNOWLEDGE ====================

	/** Known available assets (mesh paths, materials, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FString> AvailableAssets;  // Path -> Type

	// ==================== METHODS ====================

	FAgentWorldModel()
		: LastFullRefresh(FDateTime::MinValue())
	{
	}

	/** Check if we need a full refresh */
	bool NeedsRefresh(float MaxAgeSeconds = 300.0f) const
	{
		if (LastFullRefresh == FDateTime::MinValue())
		{
			return true;
		}
		FTimespan Age = FDateTime::Now() - LastFullRefresh;
		return Age.GetTotalSeconds() > MaxAgeSeconds;
	}

	/** Mark as refreshed */
	void MarkRefreshed()
	{
		LastFullRefresh = FDateTime::Now();
	}

	/** Clear all cached data */
	void Clear()
	{
		KnownActors.Empty();
		Modifications.Empty();
		LastFullRefresh = FDateTime::MinValue();
	}

	// ==================== ACTOR OPERATIONS ====================

	/** Add or update an actor */
	void UpsertActor(const FActorState& Actor)
	{
		KnownActors.Add(Actor.ActorId, Actor);
	}

	/** Find an actor by ID */
	FActorState* FindActor(const FString& ActorId)
	{
		return KnownActors.Find(ActorId);
	}

	/** Find an actor by ID (const) */
	const FActorState* FindActor(const FString& ActorId) const
	{
		return KnownActors.Find(ActorId);
	}

	/** Remove an actor */
	bool RemoveActor(const FString& ActorId)
	{
		return KnownActors.Remove(ActorId) > 0;
	}

	/** Query actors matching criteria */
	TArray<FActorState> QueryActors(const FActorQuery& Query) const;

	/** Count actors matching criteria */
	int32 CountActors(const FActorQuery& Query) const;

	/** Check if an area is clear (no actors in bounds) */
	bool IsAreaClear(const FBox& Bounds) const;

	/** Get actors in area */
	TArray<FActorState> GetActorsInArea(const FBox& Bounds) const;

	// ==================== MODIFICATION TRACKING ====================

	/** Track a modification */
	void TrackModification(const FString& Type, const FString& ActorId,
		const FString& GoalId, const FString& StepId);

	/** Get modifications for a goal */
	TArray<FWorldModification> GetModificationsForGoal(const FString& GoalId) const;

	/** Get recent modifications */
	TArray<FWorldModification> GetRecentModifications(int32 Count = 10) const;

	// ==================== CONSTRAINTS ====================

	/** Add a constraint */
	void AddConstraint(const FString& Description, const FString& Type, const FString& Target);

	/** Check if an actor can be modified */
	bool CanModifyActor(const FString& ActorId) const;

	/** Check if an area can be modified */
	bool CanModifyArea(const FBox& Bounds) const;

	/** Get active constraints that block an action */
	TArray<FWorldConstraint> GetBlockingConstraints(const FString& ActorId) const;

	// ==================== STATISTICS ====================

	/** Get summary statistics */
	FString GetStatsSummary() const;

	/** Get count of known actors */
	int32 GetActorCount() const { return KnownActors.Num(); }

	/** Get count of confirmed (non-stale) actors */
	int32 GetConfirmedActorCount() const;

	/** Get count of modifications */
	int32 GetModificationCount() const { return Modifications.Num(); }
};

/**
 * Manages the world model with scene synchronization.
 */
class UNREALGPTEDITOR_API FAgentWorldModelManager
{
public:
	FAgentWorldModelManager() = default;

	/** Get the world model */
	FAgentWorldModel& GetWorldModel() { return WorldModel; }
	const FAgentWorldModel& GetWorldModel() const { return WorldModel; }

	// ==================== SYNCHRONIZATION ====================

	/** Refresh the entire scene from Unreal */
	void RefreshFullScene();

	/** Refresh a specific area */
	void RefreshArea(const FBox& Bounds);

	/** Refresh a specific actor */
	void RefreshActor(const FString& ActorId);

	/** Update world model from scene_query results */
	void UpdateFromSceneQueryResult(const FString& ResultJson);

	/** Update world model from get_actor result */
	void UpdateFromGetActorResult(const FString& ResultJson);

	// ==================== TOOL RESULT PROCESSING ====================

	/** Process a tool result and update world model accordingly */
	void ProcessToolResult(const FString& ToolName, const TMap<FString, FString>& Args,
		const FToolResult& Result, const FString& GoalId, const FString& StepId);

	// ==================== PREDICTIONS ====================

	/**
	 * Predict the result of a tool call (for planning).
	 * Returns expected world state changes without executing.
	 */
	TArray<FWorldModification> PredictToolResult(const FString& ToolName,
		const TMap<FString, FString>& Args) const;

private:
	FAgentWorldModel WorldModel;

	/** Parse actor state from JSON */
	FActorState ParseActorFromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
