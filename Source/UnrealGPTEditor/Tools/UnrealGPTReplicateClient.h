#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTReplicateClient.generated.h"

class UUnrealGPTSettings;

/**
 * Static utility class for interacting with the Replicate API.
 * Handles image, audio, video, and 3D model generation.
 *
 * Extracted from UnrealGPTAgentClient to reduce complexity.
 */
UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTReplicateClient : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Generate content using the Replicate API.
	 *
	 * @param ArgumentsJson - JSON string with generation parameters:
	 *   - prompt (required): The generation prompt
	 *   - output_kind: "image", "video", "audio", "3d" (default: "image")
	 *   - output_subkind: For audio - "sfx", "music", "speech"
	 *   - version: Specific Replicate model version (optional, uses defaults from settings)
	 *
	 * @return JSON string with status, message, and details including downloaded file paths
	 */
	static FString Generate(const FString& ArgumentsJson);

private:
	/**
	 * Perform a blocking HTTP request.
	 *
	 * @param Url - The URL to request
	 * @param Verb - HTTP verb (GET, POST)
	 * @param Body - Request body for POST requests
	 * @param AuthToken - Bearer token for authorization
	 * @param OutResponse - Output response body
	 * @param TimeoutSeconds - Request timeout
	 * @return true if request succeeded (2xx status)
	 */
	static bool PerformHttpRequest(const FString& Url, const FString& Verb, const FString& Body,
		const FString& AuthToken, FString& OutResponse, int32 TimeoutSeconds);

	/**
	 * Download a file from a URL to the staging folder.
	 *
	 * @param Uri - The URL to download from
	 * @param OutputKind - Type of output (image, audio, video, 3d) for folder selection
	 * @param AuthToken - Bearer token for authorization
	 * @return Local file path if successful, empty string otherwise
	 */
	static FString DownloadFile(const FString& Uri, const FString& OutputKind, const FString& AuthToken);

	/**
	 * Get the appropriate staging folder for a given output kind.
	 *
	 * @param OutputKind - Type of output (image, audio, video, 3d)
	 * @return Absolute path to the staging folder
	 */
	static FString GetStagingFolder(const FString& OutputKind);

	/**
	 * Recursively collect HTTP/HTTPS URLs from a JSON value.
	 * Used to extract output URIs from Replicate API responses.
	 *
	 * @param JsonValue - The JSON value to search
	 * @param OutUris - Output array of found URIs
	 */
	static void CollectUrisFromJsonValue(const TSharedPtr<FJsonValue>& JsonValue, TArray<FString>& OutUris);
};
