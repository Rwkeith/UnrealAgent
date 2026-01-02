#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Represents a single parameter for a tool schema.
 */
struct FToolParameter
{
	FString Name;
	FString Type;         // "string", "integer", "boolean", "number", "object", "array"
	FString Description;
	bool bRequired = false;

	// For array types, the item type
	FString ArrayItemType;

	// Optional default value (as string for display)
	FString DefaultValue;

	FToolParameter() = default;

	FToolParameter(const FString& InName, const FString& InType, const FString& InDesc, bool bInRequired = false)
		: Name(InName), Type(InType), Description(InDesc), bRequired(bInRequired)
	{}

	static FToolParameter String(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		return FToolParameter(Name, TEXT("string"), Desc, bRequired);
	}

	static FToolParameter Integer(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		return FToolParameter(Name, TEXT("integer"), Desc, bRequired);
	}

	static FToolParameter Boolean(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		return FToolParameter(Name, TEXT("boolean"), Desc, bRequired);
	}

	static FToolParameter Number(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		return FToolParameter(Name, TEXT("number"), Desc, bRequired);
	}

	static FToolParameter Object(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		return FToolParameter(Name, TEXT("object"), Desc, bRequired);
	}

	static FToolParameter StringArray(const FString& Name, const FString& Desc, bool bRequired = false)
	{
		FToolParameter Param(Name, TEXT("array"), Desc, bRequired);
		Param.ArrayItemType = TEXT("string");
		return Param;
	}
};

/**
 * Represents a complete tool schema definition.
 */
struct FToolSchema
{
	FString Name;
	FString Description;
	TArray<FToolParameter> Parameters;

	FToolSchema() = default;

	FToolSchema(const FString& InName, const FString& InDesc)
		: Name(InName), Description(InDesc)
	{}

	FToolSchema& AddParam(const FToolParameter& Param)
	{
		Parameters.Add(Param);
		return *this;
	}
};

/**
 * Utility namespace for building tool schemas and converting them to JSON.
 */
namespace UnrealGPTToolSchemas
{
	/**
	 * Get all standard tool schemas.
	 *
	 * @param bEnablePython - Whether python_execute should be included
	 * @param bEnableViewport - Whether viewport_screenshot should be included
	 * @param bEnableReplicate - Whether replicate_generate should be included
	 * @return Array of tool schemas
	 */
	TArray<FToolSchema> GetStandardToolSchemas(bool bEnablePython, bool bEnableViewport, bool bEnableReplicate);

	/**
	 * Build a JSON parameters object from a tool schema.
	 */
	TSharedPtr<FJsonObject> BuildParametersJson(const FToolSchema& Schema);

	/**
	 * Build a complete tool JSON object from a schema.
	 *
	 * @param Schema - The tool schema
	 * @param bUseResponsesApi - Whether to use Responses API format (vs Chat Completions)
	 */
	TSharedPtr<FJsonObject> BuildToolJson(const FToolSchema& Schema, bool bUseResponsesApi);
}
