#pragma once

#include "CoreMinimal.h"

/**
 * Information about a tool call extracted from API response.
 */
struct FToolCallInfo
{
	FString Id;
	FString Name;
	FString Arguments;
};
