#include "UnrealGPTToolDefinitionBuilder.h"
#include "UnrealGPTSettings.h"
#include "UnrealGPTToolSchemas.h"

TArray<TSharedPtr<FJsonObject>> UnrealGPTToolDefinitionBuilder::BuildToolDefinitions(const UUnrealGPTSettings* Settings)
{
	TArray<TSharedPtr<FJsonObject>> Tools;

	// Determine which tools to enable based on settings
	const bool bEnablePython = Settings && Settings->bEnablePythonExecution;
	const bool bEnableViewport = Settings && Settings->bEnableViewportScreenshot;
	const bool bEnableReplicate = Settings && Settings->bEnableReplicateTool && !Settings->ReplicateApiToken.IsEmpty();

	// Get all standard tool schemas and convert to JSON
	TArray<FToolSchema> Schemas = UnrealGPTToolSchemas::GetStandardToolSchemas(bEnablePython, bEnableViewport, bEnableReplicate);
	for (const FToolSchema& Schema : Schemas)
	{
		Tools.Add(UnrealGPTToolSchemas::BuildToolJson(Schema));
	}

	// OpenAI-hosted web_search and file_search tools.
	// These use a different JSON format and aren't function tools.
	TSharedPtr<FJsonObject> WebSearchTool = MakeShareable(new FJsonObject);
	WebSearchTool->SetStringField(TEXT("type"), TEXT("web_search"));
	Tools.Add(WebSearchTool);

	// file_search over UE Python API docs (only if VectorStoreId is configured)
	if (Settings && !Settings->VectorStoreId.IsEmpty())
	{
		TSharedPtr<FJsonObject> FileSearchTool = MakeShareable(new FJsonObject);
		FileSearchTool->SetStringField(TEXT("type"), TEXT("file_search"));

		TArray<TSharedPtr<FJsonValue>> VectorStores;
		VectorStores.Add(MakeShareable(new FJsonValueString(Settings->VectorStoreId)));
		FileSearchTool->SetArrayField(TEXT("vector_store_ids"), VectorStores);
		FileSearchTool->SetNumberField(TEXT("max_num_results"), 20);

		Tools.Add(FileSearchTool);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: file_search enabled with vector store: %s"), *Settings->VectorStoreId);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: file_search disabled (no Vector Store ID configured)"));
	}

	return Tools;
}
