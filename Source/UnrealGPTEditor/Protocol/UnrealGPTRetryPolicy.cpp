#include "UnrealGPTRetryPolicy.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

float UnrealGPTRetryPolicy::ParseRetryDelaySeconds(const FString& ErrorBody)
{
	float RetryDelaySeconds = 1.0f;

	TSharedPtr<FJsonObject> ErrorRoot;
	TSharedRef<TJsonReader<>> ErrorReader = TJsonReaderFactory<>::Create(ErrorBody);
	if (FJsonSerializer::Deserialize(ErrorReader, ErrorRoot) && ErrorRoot.IsValid())
	{
		const TSharedPtr<FJsonObject>* ErrorObjPtr = nullptr;
		if (ErrorRoot->TryGetObjectField(TEXT("error"), ErrorObjPtr) && ErrorObjPtr && (*ErrorObjPtr).IsValid())
		{
			FString Message;
			(*ErrorObjPtr)->TryGetStringField(TEXT("message"), Message);

			int32 MsIndex = Message.Find(TEXT("in "));
			if (MsIndex != INDEX_NONE)
			{
				FString DelayPart = Message.Mid(MsIndex + 3);
				if (DelayPart.Contains(TEXT("ms")))
				{
					int32 DelayMs = FCString::Atoi(*DelayPart);
					if (DelayMs > 0)
					{
						RetryDelaySeconds = FMath::Max(0.5f, DelayMs / 1000.0f + 0.1f);
					}
				}
				else if (DelayPart.Contains(TEXT("s")))
				{
					float DelayS = FCString::Atof(*DelayPart);
					if (DelayS > 0)
					{
						RetryDelaySeconds = DelayS + 0.1f;
					}
				}
			}
		}
	}

	return FMath::Clamp(RetryDelaySeconds, 0.5f, 30.0f);
}
