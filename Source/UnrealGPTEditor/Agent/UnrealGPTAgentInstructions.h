#pragma once

#include "CoreMinimal.h"

/**
 * Provides the system instructions for the UnrealGPT agent.
 * Extracted from UnrealGPTAgentClient.cpp to reduce file complexity.
 */
namespace UnrealGPTAgentInstructions
{
	/**
	 * Returns the complete agent instructions string.
	 * @param EngineVersion - The current Unreal Engine version string (e.g., "5.7")
	 * @return The full instructions string to be sent to the API
	 */
	FString GetInstructions(const FString& EngineVersion);
}
