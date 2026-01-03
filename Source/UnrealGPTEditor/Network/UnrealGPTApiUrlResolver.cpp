#include "UnrealGPTApiUrlResolver.h"
#include "UnrealGPTSettings.h"

FString UnrealGPTApiUrlResolver::GetEffectiveApiUrl()
{
	// Always get a fresh reference to settings to avoid accessing invalid cached pointers
	// Settings can become invalid if the object is garbage collected
	UUnrealGPTSettings* SafeSettings = GetMutableDefault<UUnrealGPTSettings>();
	if (!SafeSettings || !IsValid(SafeSettings))
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealGPT: Settings is null or invalid and could not be retrieved"));
		return TEXT("https://api.openai.com/v1/responses"); // Default fallback
	}
	
	// Build effective URL based on BaseUrlOverride and ApiEndpoint
	FString BaseUrl = SafeSettings->BaseUrlOverride;
	FString ApiEndpoint = SafeSettings->ApiEndpoint;

	// If no override is set, use ApiEndpoint as-is (caller should provide full URL)
	if (BaseUrl.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (no override): %s"), *ApiEndpoint);
		return ApiEndpoint;
	}

	// Normalize base URL (remove trailing slash)
	if (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.RemoveAt(BaseUrl.Len() - 1);
	}

	// If ApiEndpoint is empty, just use the base URL
	if (ApiEndpoint.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override only): %s"), *BaseUrl);
		return BaseUrl;
	}

	// If ApiEndpoint is a full URL, extract its path portion and append to BaseUrl
	int32 ProtocolIndex = ApiEndpoint.Find(TEXT("://"));
	if (ProtocolIndex != INDEX_NONE)
	{
		int32 PathStartIndex = ApiEndpoint.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ProtocolIndex + 3);
		if (PathStartIndex != INDEX_NONE && PathStartIndex < ApiEndpoint.Len())
		{
			FString Path = ApiEndpoint.Mid(PathStartIndex);
			if (!Path.StartsWith(TEXT("/")))
			{
				Path = TEXT("/") + Path;
			}

			const FString EffectiveUrl = BaseUrl + Path;
			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + parsed path): %s"), *EffectiveUrl);
			return EffectiveUrl;
		}
	}

	// Otherwise treat ApiEndpoint as a path relative to BaseUrl
	FString Path = ApiEndpoint;
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/") + Path;
	}

	const FString EffectiveUrl = BaseUrl + Path;
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Effective API URL (override + relative path): %s"), *EffectiveUrl);
	return EffectiveUrl;
}
