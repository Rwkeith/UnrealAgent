#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FProperty;
class UClass;
class UFunction;

namespace UnrealGPTJsonHelpers
{
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& V);
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& R);
	FString MakeToolResult(const FString& Status, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);
	FString MakeErrorResult(const FString& Message);
	FString MakeSuccessResult(const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);
	TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property);
	TSharedPtr<FJsonObject> BuildFunctionJson(UFunction* Function);
	FString BuildReflectionSchemaJson(UClass* Class);
}
