// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentWorldModel.h"
#include "UnrealGPTSceneContext.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

// ==================== FAgentWorldModel ====================

TArray<FActorState> FAgentWorldModel::QueryActors(const FActorQuery& Query) const
{
	TArray<FActorState> Results;

	for (const auto& Pair : KnownActors)
	{
		const FActorState& Actor = Pair.Value;

		// Skip stale actors unless requested
		if (!Query.bIncludeStale && Actor.Confidence == EActorConfidence::Stale)
		{
			continue;
		}

		// Class filter
		if (!Query.ClassFilter.IsEmpty())
		{
			if (Query.ClassFilter.Contains(TEXT("*")))
			{
				// Wildcard match
				FString Pattern = Query.ClassFilter.Replace(TEXT("*"), TEXT(""));
				if (!Actor.ClassName.Contains(Pattern))
				{
					continue;
				}
			}
			else if (Actor.ClassName != Query.ClassFilter)
			{
				continue;
			}
		}

		// Label filter
		if (!Query.LabelFilter.IsEmpty())
		{
			if (Query.LabelFilter.Contains(TEXT("*")))
			{
				FString Pattern = Query.LabelFilter.Replace(TEXT("*"), TEXT(""));
				if (!Actor.Label.Contains(Pattern))
				{
					continue;
				}
			}
			else if (!Actor.Label.Contains(Query.LabelFilter))
			{
				continue;
			}
		}

		// Tag filter
		if (!Query.TagFilter.IsEmpty())
		{
			if (!Actor.HasTag(Query.TagFilter))
			{
				continue;
			}
		}

		// Bounds filter
		if (Query.BoundsFilter.IsValid)
		{
			if (!Query.BoundsFilter.Intersect(Actor.Bounds))
			{
				continue;
			}
		}

		Results.Add(Actor);

		// Max results
		if (Results.Num() >= Query.MaxResults)
		{
			break;
		}
	}

	return Results;
}

int32 FAgentWorldModel::CountActors(const FActorQuery& Query) const
{
	// For counting, we don't limit by MaxResults
	FActorQuery CountQuery = Query;
	CountQuery.MaxResults = INT32_MAX;
	return QueryActors(CountQuery).Num();
}

bool FAgentWorldModel::IsAreaClear(const FBox& Bounds) const
{
	for (const auto& Pair : KnownActors)
	{
		const FActorState& Actor = Pair.Value;
		if (Actor.Confidence != EActorConfidence::Stale && Bounds.Intersect(Actor.Bounds))
		{
			return false;
		}
	}
	return true;
}

TArray<FActorState> FAgentWorldModel::GetActorsInArea(const FBox& Bounds) const
{
	return QueryActors(FActorQuery::InBounds(Bounds));
}

void FAgentWorldModel::TrackModification(const FString& Type, const FString& ActorId,
	const FString& GoalId, const FString& StepId)
{
	FWorldModification Mod;
	Mod.ModificationType = Type;
	Mod.ActorId = ActorId;
	Mod.SourceGoalId = GoalId;
	Mod.SourceStepId = StepId;
	Mod.Timestamp = FDateTime::Now();
	Modifications.Add(Mod);
}

TArray<FWorldModification> FAgentWorldModel::GetModificationsForGoal(const FString& GoalId) const
{
	TArray<FWorldModification> Result;
	for (const FWorldModification& Mod : Modifications)
	{
		if (Mod.SourceGoalId == GoalId)
		{
			Result.Add(Mod);
		}
	}
	return Result;
}

TArray<FWorldModification> FAgentWorldModel::GetRecentModifications(int32 Count) const
{
	TArray<FWorldModification> Result;
	int32 Start = FMath::Max(0, Modifications.Num() - Count);
	for (int32 i = Start; i < Modifications.Num(); i++)
	{
		Result.Add(Modifications[i]);
	}
	return Result;
}

void FAgentWorldModel::AddConstraint(const FString& Description, const FString& Type, const FString& Target)
{
	FWorldConstraint Constraint;
	Constraint.Description = Description;
	Constraint.ConstraintType = Type;
	Constraint.Target = Target;
	Constraint.bActive = true;
	Constraints.Add(Constraint);
}

bool FAgentWorldModel::CanModifyActor(const FString& ActorId) const
{
	const FActorState* Actor = FindActor(ActorId);
	if (!Actor)
	{
		return true;  // Unknown actor, assume can modify
	}

	for (const FWorldConstraint& Constraint : Constraints)
	{
		if (!Constraint.bActive)
		{
			continue;
		}

		if (Constraint.ConstraintType == TEXT("NoModify"))
		{
			// Check if target matches actor ID, label, class, or tag
			if (Constraint.Target == ActorId ||
				Constraint.Target == Actor->Label ||
				Constraint.Target == Actor->ClassName ||
				Actor->HasTag(Constraint.Target))
			{
				return false;
			}
		}
	}

	return true;
}

bool FAgentWorldModel::CanModifyArea(const FBox& Bounds) const
{
	for (const FWorldConstraint& Constraint : Constraints)
	{
		if (!Constraint.bActive)
		{
			continue;
		}

		if (Constraint.ConstraintType == TEXT("NoBounds"))
		{
			// Parse bounds from target (format: "MinX,MinY,MinZ,MaxX,MaxY,MaxZ")
			TArray<FString> Parts;
			Constraint.Target.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() == 6)
			{
				FBox ConstraintBounds(
					FVector(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2])),
					FVector(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]), FCString::Atof(*Parts[5]))
				);
				if (Bounds.Intersect(ConstraintBounds))
				{
					return false;
				}
			}
		}
	}

	return true;
}

TArray<FWorldConstraint> FAgentWorldModel::GetBlockingConstraints(const FString& ActorId) const
{
	TArray<FWorldConstraint> Result;

	const FActorState* Actor = FindActor(ActorId);
	if (!Actor)
	{
		return Result;
	}

	for (const FWorldConstraint& Constraint : Constraints)
	{
		if (!Constraint.bActive)
		{
			continue;
		}

		if (Constraint.Target == ActorId ||
			Constraint.Target == Actor->Label ||
			Constraint.Target == Actor->ClassName ||
			Actor->HasTag(Constraint.Target))
		{
			Result.Add(Constraint);
		}
	}

	return Result;
}

FString FAgentWorldModel::GetStatsSummary() const
{
	FString Summary;
	Summary += FString::Printf(TEXT("World Model Statistics:\n"));
	Summary += FString::Printf(TEXT("  Total Actors: %d\n"), KnownActors.Num());
	Summary += FString::Printf(TEXT("  Confirmed: %d\n"), GetConfirmedActorCount());
	Summary += FString::Printf(TEXT("  Modifications: %d\n"), Modifications.Num());
	Summary += FString::Printf(TEXT("  Constraints: %d\n"), Constraints.Num());

	if (LastFullRefresh != FDateTime::MinValue())
	{
		FTimespan Age = FDateTime::Now() - LastFullRefresh;
		Summary += FString::Printf(TEXT("  Last Refresh: %.0f seconds ago\n"), Age.GetTotalSeconds());
	}
	else
	{
		Summary += TEXT("  Last Refresh: Never\n");
	}

	return Summary;
}

int32 FAgentWorldModel::GetConfirmedActorCount() const
{
	int32 Count = 0;
	for (const auto& Pair : KnownActors)
	{
		if (Pair.Value.Confidence == EActorConfidence::Confirmed)
		{
			Count++;
		}
	}
	return Count;
}

// ==================== FAgentWorldModelManager ====================

void FAgentWorldModelManager::RefreshFullScene()
{
	// Call real scene_query with max results to get scene snapshot
	TSharedRef<FJsonObject> ArgsObject = MakeShared<FJsonObject>();
	ArgsObject->SetNumberField(TEXT("max_results"), 200);

	FString ArgsJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
	FJsonSerializer::Serialize(ArgsObject, Writer);

	FString ResultJson = UUnrealGPTSceneContext::QueryScene(ArgsJson);

	// Parse result - scene_query returns an array directly
	if (!ResultJson.IsEmpty() && ResultJson.StartsWith(TEXT("[")))
	{
		TSharedPtr<FJsonValue> JsonValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);

		if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorsArray;
			if (JsonValue->TryGetArray(ActorsArray))
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArray)
				{
					const TSharedPtr<FJsonObject>* ActorObject;
					if (ActorValue->TryGetObject(ActorObject))
					{
						FActorState Actor = ParseActorFromJson(*ActorObject);
						Actor.MarkVerified();
						WorldModel.UpsertActor(Actor);
					}
				}
			}
		}
	}

	WorldModel.MarkRefreshed();
}

void FAgentWorldModelManager::RefreshArea(const FBox& Bounds)
{
	// Mark actors in area as stale first
	for (auto& Pair : WorldModel.KnownActors)
	{
		if (Bounds.Intersect(Pair.Value.Bounds))
		{
			Pair.Value.Confidence = EActorConfidence::Stale;
		}
	}

	// Query scene with bounds filter (scene_query doesn't support bounds directly,
	// so we query all and filter client-side)
	RefreshFullScene();
}

void FAgentWorldModelManager::RefreshActor(const FString& ActorId)
{
	// Mark as stale first
	FActorState* Actor = WorldModel.FindActor(ActorId);
	if (Actor)
	{
		Actor->Confidence = EActorConfidence::Stale;
	}

	// Query by label to refresh this specific actor
	TSharedRef<FJsonObject> ArgsObject = MakeShared<FJsonObject>();
	ArgsObject->SetStringField(TEXT("label_contains"), ActorId);
	ArgsObject->SetNumberField(TEXT("max_results"), 10);

	FString ArgsJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
	FJsonSerializer::Serialize(ArgsObject, Writer);

	FString ResultJson = UUnrealGPTSceneContext::QueryScene(ArgsJson);

	if (!ResultJson.IsEmpty() && ResultJson.StartsWith(TEXT("[")))
	{
		TSharedPtr<FJsonValue> JsonValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);

		if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorsArray;
			if (JsonValue->TryGetArray(ActorsArray))
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArray)
				{
					const TSharedPtr<FJsonObject>* ActorObject;
					if (ActorValue->TryGetObject(ActorObject))
					{
						FActorState ParsedActor = ParseActorFromJson(*ActorObject);
						ParsedActor.MarkVerified();
						WorldModel.UpsertActor(ParsedActor);
					}
				}
			}
		}
	}
}

void FAgentWorldModelManager::UpdateFromSceneQueryResult(const FString& ResultJson)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorsArray;
	if (!JsonObject->TryGetArrayField(TEXT("actors"), ActorsArray))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArray)
	{
		const TSharedPtr<FJsonObject>* ActorObject;
		if (ActorValue->TryGetObject(ActorObject))
		{
			FActorState Actor = ParseActorFromJson(*ActorObject);
			Actor.MarkVerified();
			WorldModel.UpsertActor(Actor);
		}
	}

	WorldModel.MarkRefreshed();
}

void FAgentWorldModelManager::UpdateFromGetActorResult(const FString& ResultJson)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return;
	}

	FActorState Actor = ParseActorFromJson(JsonObject);
	if (!Actor.ActorId.IsEmpty())
	{
		Actor.MarkVerified();
		WorldModel.UpsertActor(Actor);
	}
}

void FAgentWorldModelManager::ProcessToolResult(const FString& ToolName,
	const TMap<FString, FString>& Args, const FToolResult& Result,
	const FString& GoalId, const FString& StepId)
{
	if (!Result.bSuccess)
	{
		return;
	}

	if (ToolName == TEXT("scene_query"))
	{
		UpdateFromSceneQueryResult(Result.RawOutput);
	}
	else if (ToolName == TEXT("get_actor"))
	{
		UpdateFromGetActorResult(Result.RawOutput);
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		const FString* ActorId = Args.Find(TEXT("actor_id"));
		if (ActorId)
		{
			FActorState* Actor = WorldModel.FindActor(*ActorId);
			if (Actor)
			{
				// Update transform based on args
				const FString* LocationStr = Args.Find(TEXT("location"));
				const FString* RotationStr = Args.Find(TEXT("rotation"));
				const FString* ScaleStr = Args.Find(TEXT("scale"));

				if (LocationStr)
				{
					// Parse "X,Y,Z" format
					TArray<FString> Parts;
					LocationStr->ParseIntoArray(Parts, TEXT(","));
					if (Parts.Num() == 3)
					{
						Actor->Location = FVector(
							FCString::Atof(*Parts[0]),
							FCString::Atof(*Parts[1]),
							FCString::Atof(*Parts[2]));
					}
				}

				Actor->MarkAssumed();  // Not verified yet
				WorldModel.TrackModification(TEXT("Modified"), *ActorId, GoalId, StepId);
			}
		}
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		// New actor was created
		for (const FString& NewActorId : Result.AffectedActorIds)
		{
			WorldModel.TrackModification(TEXT("Created"), NewActorId, GoalId, StepId);
		}
	}
	else if (ToolName == TEXT("python_execute"))
	{
		// Python could have done anything - mark relevant actors as stale
		// In a more sophisticated version, we could parse the Python code
		// to understand what it modified
		for (const FString& ActorId : Result.AffectedActorIds)
		{
			FActorState* Actor = WorldModel.FindActor(ActorId);
			if (Actor)
			{
				Actor->Confidence = EActorConfidence::Stale;
			}
			WorldModel.TrackModification(TEXT("Modified"), ActorId, GoalId, StepId);
		}
	}
}

TArray<FWorldModification> FAgentWorldModelManager::PredictToolResult(
	const FString& ToolName, const TMap<FString, FString>& Args) const
{
	TArray<FWorldModification> Predictions;

	if (ToolName == TEXT("set_actor_transform"))
	{
		const FString* ActorId = Args.Find(TEXT("actor_id"));
		if (ActorId)
		{
			FWorldModification Mod;
			Mod.ModificationType = TEXT("Modified");
			Mod.ActorId = *ActorId;
			Predictions.Add(Mod);
		}
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		const FString* ActorId = Args.Find(TEXT("actor_id"));
		if (ActorId)
		{
			FWorldModification Mod;
			Mod.ModificationType = TEXT("Created");
			Mod.ActorId = TEXT("NewActor_<generated>");
			Predictions.Add(Mod);
		}
	}
	else if (ToolName == TEXT("python_execute"))
	{
		// Hard to predict without parsing Python
		// Could analyze for patterns like spawn_actor, etc.
		FWorldModification Mod;
		Mod.ModificationType = TEXT("Unknown");
		Mod.ActorId = TEXT("Unknown");
		Predictions.Add(Mod);
	}

	return Predictions;
}

FActorState FAgentWorldModelManager::ParseActorFromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FActorState Actor;

	if (!JsonObject.IsValid())
	{
		return Actor;
	}

	Actor.ActorId = JsonObject->GetStringField(TEXT("id"));
	Actor.Label = JsonObject->GetStringField(TEXT("label"));
	Actor.ClassName = JsonObject->GetStringField(TEXT("class"));
	Actor.ActorPath = JsonObject->GetStringField(TEXT("path"));
	Actor.MeshPath = JsonObject->GetStringField(TEXT("mesh"));

	// Parse location
	const TSharedPtr<FJsonObject>* LocationObj;
	if (JsonObject->TryGetObjectField(TEXT("location"), LocationObj))
	{
		Actor.Location = FVector(
			(*LocationObj)->GetNumberField(TEXT("x")),
			(*LocationObj)->GetNumberField(TEXT("y")),
			(*LocationObj)->GetNumberField(TEXT("z"))
		);
	}

	// Parse rotation
	const TSharedPtr<FJsonObject>* RotationObj;
	if (JsonObject->TryGetObjectField(TEXT("rotation"), RotationObj))
	{
		Actor.Rotation = FRotator(
			(*RotationObj)->GetNumberField(TEXT("pitch")),
			(*RotationObj)->GetNumberField(TEXT("yaw")),
			(*RotationObj)->GetNumberField(TEXT("roll"))
		);
	}

	// Parse scale
	const TSharedPtr<FJsonObject>* ScaleObj;
	if (JsonObject->TryGetObjectField(TEXT("scale"), ScaleObj))
	{
		Actor.Scale = FVector(
			(*ScaleObj)->GetNumberField(TEXT("x")),
			(*ScaleObj)->GetNumberField(TEXT("y")),
			(*ScaleObj)->GetNumberField(TEXT("z"))
		);
	}

	// Parse bounds
	const TSharedPtr<FJsonObject>* BoundsObj;
	if (JsonObject->TryGetObjectField(TEXT("bounds"), BoundsObj))
	{
		const TSharedPtr<FJsonObject>* MinObj;
		const TSharedPtr<FJsonObject>* MaxObj;
		if ((*BoundsObj)->TryGetObjectField(TEXT("min"), MinObj) &&
			(*BoundsObj)->TryGetObjectField(TEXT("max"), MaxObj))
		{
			Actor.Bounds = FBox(
				FVector((*MinObj)->GetNumberField(TEXT("x")),
						(*MinObj)->GetNumberField(TEXT("y")),
						(*MinObj)->GetNumberField(TEXT("z"))),
				FVector((*MaxObj)->GetNumberField(TEXT("x")),
						(*MaxObj)->GetNumberField(TEXT("y")),
						(*MaxObj)->GetNumberField(TEXT("z")))
			);
		}
	}

	// Parse tags
	const TArray<TSharedPtr<FJsonValue>>* TagsArray;
	if (JsonObject->TryGetArrayField(TEXT("tags"), TagsArray))
	{
		for (const TSharedPtr<FJsonValue>& TagValue : *TagsArray)
		{
			Actor.Tags.Add(TagValue->AsString());
		}
	}

	// Parse components
	const TArray<TSharedPtr<FJsonValue>>* ComponentsArray;
	if (JsonObject->TryGetArrayField(TEXT("components"), ComponentsArray))
	{
		for (const TSharedPtr<FJsonValue>& CompValue : *ComponentsArray)
		{
			Actor.Components.Add(CompValue->AsString());
		}
	}

	return Actor;
}
