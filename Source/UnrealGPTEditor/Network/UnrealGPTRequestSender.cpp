#include "UnrealGPTRequestSender.h"
#include "UnrealGPTAgentClient.h"
#include "UnrealGPTApiUrlResolver.h"
#include "UnrealGPTHttpClient.h"
#include "UnrealGPTSettings.h"

void UnrealGPTRequestSender::SendRequest(UUnrealGPTAgentClient* Client, const FString& RequestBody)
{
	if (!Client)
	{
		return;
	}

	Client->LastRequestBody = RequestBody;

	const UUnrealGPTSettings* Settings = Client->Settings ? Client->Settings : GetMutableDefault<UUnrealGPTSettings>();
	const FString ApiKey = Settings ? Settings->ApiKey : FString();

	Client->CurrentRequest = UnrealGPTHttpClient::BuildJsonPost(
		UnrealGPTHttpClient::CreateRequest(),
		UnrealGPTApiUrlResolver::GetEffectiveApiUrl(),
		ApiKey,
		RequestBody,
		Client);

	Client->bRequestInProgress = true;
	Client->RequestStartTime = FPlatformTime::Seconds();

	const double TimeoutSeconds = Settings ? Settings->ExecutionTimeoutSeconds : 0.0;
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Starting HTTP request (timeout: %.1f seconds)"), TimeoutSeconds);
	Client->CurrentRequest->ProcessRequest();
}
