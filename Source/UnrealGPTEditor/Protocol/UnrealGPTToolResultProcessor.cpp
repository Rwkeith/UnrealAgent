#include "UnrealGPTToolResultProcessor.h"

FProcessedToolResult UnrealGPTToolResultProcessor::ProcessResult(
	const FString& ToolName,
	const FString& ToolResult,
	int32 MaxToolResultSize)
{
	FProcessedToolResult Output;
	Output.ResultForHistory = ToolResult;

	const bool bIsScreenshot = (ToolName == TEXT("viewport_screenshot"));
	const FString ImageSeparator = TEXT("\n__IMAGE_BASE64__\n");

	if (bIsScreenshot && !ToolResult.IsEmpty())
	{
		const int32 SeparatorIndex = ToolResult.Find(ImageSeparator);
		if (SeparatorIndex != INDEX_NONE)
		{
			const FString MetadataJson = ToolResult.Left(SeparatorIndex);
			const FString ImageBase64 = ToolResult.Mid(SeparatorIndex + ImageSeparator.Len());
			if (!ImageBase64.IsEmpty())
			{
				Output.Images.Add(ImageBase64);
			}
			Output.ResultForHistory = MetadataJson;
		}
		else
		{
			Output.Images.Add(ToolResult);
		}
	}

	if (Output.ResultForHistory.Len() > MaxToolResultSize)
	{
		const bool bIsBase64Image = Output.ResultForHistory.StartsWith(TEXT("iVBORw0KGgo")) || Output.ResultForHistory.StartsWith(TEXT("/9j/"));
		if (bIsScreenshot && bIsBase64Image)
		{
			Output.ResultForHistory = TEXT("Screenshot captured successfully. [Base64 image data omitted from history to prevent context overflow - ")
				TEXT("the image was captured and can be viewed in the UI. Length: ") + FString::FromInt(ToolResult.Len()) + TEXT(" characters]");
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large screenshot result (%d chars) to prevent context overflow"), ToolResult.Len());
		}
		else
		{
			Output.ResultForHistory = Output.ResultForHistory.Left(MaxToolResultSize) +
				TEXT("\n\n[Result truncated - original length: ") + FString::FromInt(ToolResult.Len()) +
				TEXT(" characters.]");
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Truncated large tool result (%d chars) to prevent context overflow"), ToolResult.Len());
		}
	}

	return Output;
}
