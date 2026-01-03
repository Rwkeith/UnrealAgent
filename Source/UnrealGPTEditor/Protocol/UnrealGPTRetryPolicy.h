#pragma once

#include "CoreMinimal.h"

class UnrealGPTRetryPolicy
{
public:
	static float ParseRetryDelaySeconds(const FString& ErrorBody);
};
