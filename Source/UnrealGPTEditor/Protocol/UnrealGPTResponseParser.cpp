#include "UnrealGPTResponseParser.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealGPTToolCallTypes.h"

void UnrealGPTResponseParser::ExtractFromResponseOutput(const TArray<TSharedPtr<FJsonValue>>& OutputArray, FResponseParseResult& OutResult)
{
	for (int32 i = 0; i < OutputArray.Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& OutputValue = OutputArray[i];
		TSharedPtr<FJsonObject> OutputObj = OutputValue.IsValid() ? OutputValue->AsObject() : nullptr;
		if (!OutputObj.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: Output item %d is not a valid object"), i);
			continue;
		}

		FString OutputType;
		OutputObj->TryGetStringField(TEXT("type"), OutputType);
		UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Output item %d type: %s"), i, *OutputType);

		if (OutputType == TEXT("function_call"))
		{
			FToolCallInfo Info;

			if (!OutputObj->TryGetStringField(TEXT("call_id"), Info.Id))
			{
				if (!OutputObj->TryGetStringField(TEXT("id"), Info.Id))
				{
					OutputObj->TryGetStringField(TEXT("function_call_id"), Info.Id);
				}
			}

			const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
			if (OutputObj->TryGetObjectField(TEXT("function"), FunctionObjPtr) && FunctionObjPtr && (*FunctionObjPtr).IsValid())
			{
				(*FunctionObjPtr)->TryGetStringField(TEXT("name"), Info.Name);
				(*FunctionObjPtr)->TryGetStringField(TEXT("arguments"), Info.Arguments);
			}
			else
			{
				OutputObj->TryGetStringField(TEXT("name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("function_name"), Info.Name);
				OutputObj->TryGetStringField(TEXT("arguments"), Info.Arguments);
				OutputObj->TryGetStringField(TEXT("function_arguments"), Info.Arguments);
			}

			if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
			{
				OutResult.ToolCalls.Add(MoveTemp(Info));
				UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Found function call - id: %s, name: %s"), *OutResult.ToolCalls.Last().Id, *OutResult.ToolCalls.Last().Name);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UnrealGPT: function_call output missing required fields - id: '%s', name: '%s'"), *Info.Id, *Info.Name);
			}
		}
		else if (OutputType == TEXT("file_search_call") || OutputType == TEXT("web_search_call"))
		{
			const bool bIsFileSearch = (OutputType == TEXT("file_search_call"));
			const FString ToolName = bIsFileSearch ? TEXT("file_search") : TEXT("web_search");

			FServerSideToolCall ServerSideCall;
			ServerSideCall.ToolName = ToolName;

			if (!OutputObj->TryGetStringField(TEXT("call_id"), ServerSideCall.CallId))
			{
				OutputObj->TryGetStringField(TEXT("id"), ServerSideCall.CallId);
			}

			TSharedPtr<FJsonObject> ArgsJson = MakeShareable(new FJsonObject);

			for (const auto& Pair : OutputObj->Values)
			{
				if (Pair.Key != TEXT("results") && Pair.Key != TEXT("type") && Pair.Key != TEXT("id"))
				{
					ArgsJson->SetField(Pair.Key, Pair.Value);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			if (OutputObj->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray)
			{
				ServerSideCall.ResultCount = ResultsArray->Num();
				if (ServerSideCall.ResultCount > 0)
				{
					ServerSideCall.ResultSummary = FString::Printf(TEXT("Found %d result(s):\n\n"), ServerSideCall.ResultCount);

					const int32 MaxToShow = FMath::Min(3, ServerSideCall.ResultCount);
					for (int32 ResultIdx = 0; ResultIdx < MaxToShow; ++ResultIdx)
					{
						const TSharedPtr<FJsonObject> ResultObj = (*ResultsArray)[ResultIdx]->AsObject();
						if (!ResultObj.IsValid())
						{
							continue;
						}

						FString FileName;
						ResultObj->TryGetStringField(TEXT("filename"), FileName);
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("name"), FileName);
						}
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("file_id"), FileName);
						}
						if (FileName.IsEmpty())
						{
							ResultObj->TryGetStringField(TEXT("id"), FileName);
						}

						FString Snippet;
						const TArray<TSharedPtr<FJsonValue>>* TextArray = nullptr;
						if (ResultObj->TryGetArrayField(TEXT("text"), TextArray) && TextArray && TextArray->Num() > 0)
						{
							for (const auto& TextVal : *TextArray)
							{
								const TSharedPtr<FJsonObject> TextObj = TextVal->AsObject();
								if (TextObj.IsValid())
								{
									FString TextContent;
									if (TextObj->TryGetStringField(TEXT("text"), TextContent) && !TextContent.IsEmpty())
									{
										Snippet = TextContent.Left(150);
										if (TextContent.Len() > 150)
										{
											Snippet += TEXT("...");
										}
										break;
									}
								}
							}
						}

						if (Snippet.IsEmpty())
						{
							const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
							if (ResultObj->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray && ContentArray->Num() > 0)
							{
								for (const auto& ContentVal : *ContentArray)
								{
									if (!ContentVal.IsValid())
									{
										continue;
									}

									FString ContentText;
									if (ContentVal->Type == EJson::String && ContentVal->TryGetString(ContentText) && !ContentText.IsEmpty())
									{
										Snippet = ContentText;
									}
									else
									{
										const TSharedPtr<FJsonObject> ContentObj = ContentVal->AsObject();
										if (ContentObj.IsValid())
										{
											if (ContentObj->TryGetStringField(TEXT("text"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
											else if (ContentObj->TryGetStringField(TEXT("content"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
											else if (ContentObj->TryGetStringField(TEXT("value"), ContentText) && !ContentText.IsEmpty())
											{
												Snippet = ContentText;
											}
										}
									}

									if (!Snippet.IsEmpty())
									{
										break;
									}
								}
							}

							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("text"), Snippet);
							}
							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("content"), Snippet);
							}
							if (Snippet.IsEmpty())
							{
								ResultObj->TryGetStringField(TEXT("snippet"), Snippet);
							}
							if (!Snippet.IsEmpty() && Snippet.Len() > 150)
							{
								Snippet = Snippet.Left(150) + TEXT("...");
							}
						}

						double ScoreValue = 0.0;
						const bool bHasScore = ResultObj->TryGetNumberField(TEXT("score"), ScoreValue);
						const FString ScoreSuffix = bHasScore
							? FString::Printf(TEXT(" (score %.3f)"), ScoreValue)
							: TEXT("");

						ServerSideCall.ResultSummary += FString::Printf(TEXT("%d. %s%s\n"), ResultIdx + 1,
							FileName.IsEmpty() ? TEXT("(unnamed)") : *FileName,
							*ScoreSuffix);
						if (!Snippet.IsEmpty())
						{
							ServerSideCall.ResultSummary += FString::Printf(TEXT("   %s\n"), *Snippet);
						}
						ServerSideCall.ResultSummary += TEXT("\n");
					}

					if (ServerSideCall.ResultCount > MaxToShow)
					{
						ServerSideCall.ResultSummary += FString::Printf(TEXT("... and %d more result(s)"), ServerSideCall.ResultCount - MaxToShow);
					}
				}
				else
				{
					ServerSideCall.ResultSummary = TEXT("No results found.");
				}
			}

			OutputObj->TryGetStringField(TEXT("status"), ServerSideCall.Status);

			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ServerSideCall.ArgsJson);
			FJsonSerializer::Serialize(ArgsJson.ToSharedRef(), Writer);

			UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Server-side %s - status: %s, results: %d"), *ToolName, *ServerSideCall.Status, ServerSideCall.ResultCount);

			OutResult.ServerSideToolCalls.Add(MoveTemp(ServerSideCall));
		}
		else if (OutputType == TEXT("message"))
		{
			const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
			if (!OutputObj->TryGetArrayField(TEXT("content"), ContentArray) || !ContentArray)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
			{
				TSharedPtr<FJsonObject> ContentObj = ContentValue.IsValid() ? ContentValue->AsObject() : nullptr;
				if (!ContentObj.IsValid())
				{
					continue;
				}

				FString ContentType;
				ContentObj->TryGetStringField(TEXT("type"), ContentType);

				if (ContentType == TEXT("output_text") || ContentType == TEXT("text"))
				{
					FString TextChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), TextChunk))
					{
						OutResult.AccumulatedText += TextChunk;
					}
				}
				else if (ContentType == TEXT("reasoning") || ContentType == TEXT("thought"))
				{
					FString ReasoningChunk;
					if (ContentObj->TryGetStringField(TEXT("text"), ReasoningChunk))
					{
						OutResult.ReasoningChunks.Add(ReasoningChunk);
					}
				}
				else if (ContentType == TEXT("tool_call"))
				{
					const TSharedPtr<FJsonObject>* ToolCallObjPtr = nullptr;
					if (ContentObj->TryGetObjectField(TEXT("tool_call"), ToolCallObjPtr) && ToolCallObjPtr && (*ToolCallObjPtr).IsValid())
					{
						FToolCallInfo Info;
						(*ToolCallObjPtr)->TryGetStringField(TEXT("id"), Info.Id);

						const TSharedPtr<FJsonObject>* FuncObjPtr = nullptr;
						if ((*ToolCallObjPtr)->TryGetObjectField(TEXT("function"), FuncObjPtr) && FuncObjPtr && (*FuncObjPtr).IsValid())
						{
							(*FuncObjPtr)->TryGetStringField(TEXT("name"), Info.Name);
							(*FuncObjPtr)->TryGetStringField(TEXT("arguments"), Info.Arguments);
						}

						if (!Info.Id.IsEmpty() && !Info.Name.IsEmpty())
						{
							OutResult.ToolCalls.Add(MoveTemp(Info));
						}
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT: Extracted %d tool calls, %d chars of text"), OutResult.ToolCalls.Num(), OutResult.AccumulatedText.Len());
}
