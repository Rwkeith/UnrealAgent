#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UUnrealGPTSettings;

namespace UnrealGPTToolDefinitionBuilder
{
	TArray<TSharedPtr<FJsonObject>> BuildToolDefinitions(const UUnrealGPTSettings* Settings);
}
