#include "UnrealGPTRequestValidator.h"

FString UnrealGPTRequestValidator::SanitizeUserMessage(const FString& UserMessage)
{
	const int32 MaxMessageSize = 100000; // 100KB limit for user message text
	const int32 SizeLogThreshold = 10000;

	if (UserMessage.IsEmpty())
	{
		return UserMessage;
	}

	const int32 MessageSize = UserMessage.Len();
	FString ProcessedMessage = UserMessage;

	if (MessageSize > MaxMessageSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: User message is very large (%d chars). Truncating to %d chars to prevent API issues."),
			MessageSize, MaxMessageSize);
		ProcessedMessage = UserMessage.Left(MaxMessageSize) + TEXT("\n\n[Message truncated due to size limits]");
	}

	if (MessageSize > SizeLogThreshold)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: User message size: %d characters"), MessageSize);
	}

	return ProcessedMessage;
}

void UnrealGPTRequestValidator::LogImagePayloadSize(const TArray<FString>& ImageBase64)
{
	const int32 MaxImageDataSize = 2000000; // 2MB limit for total image data

	if (ImageBase64.Num() == 0)
	{
		return;
	}

	int32 TotalImageSize = 0;
	for (const FString& ImageData : ImageBase64)
	{
		TotalImageSize += ImageData.Len();
	}

	if (TotalImageSize > MaxImageDataSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Total image data is very large (%d bytes). This may cause API timeouts. Consider using smaller images."), TotalImageSize);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Including %d image(s) with total size: %d bytes"), ImageBase64.Num(), TotalImageSize);
	}
}
