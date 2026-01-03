#include "UnrealGPTJsonHelpers.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UnrealGPTJsonHelpers
{
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	FString MakeToolResult(const FString& Status, const FString& Message, TSharedPtr<FJsonObject> Details)
	{
		TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
		ResultObj->SetStringField(TEXT("status"), Status);
		ResultObj->SetStringField(TEXT("message"), Message);
		if (Details.IsValid())
		{
			ResultObj->SetObjectField(TEXT("details"), Details);
		}

		FString ResultString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
		FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
		return ResultString;
	}

	FString MakeErrorResult(const FString& Message)
	{
		return MakeToolResult(TEXT("error"), Message);
	}

	FString MakeSuccessResult(const FString& Message, TSharedPtr<FJsonObject> Details)
	{
		return MakeToolResult(TEXT("ok"), Message, Details);
	}

	TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property)
	{
		if (!Property)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> PropJson = MakeShareable(new FJsonObject);
		PropJson->SetStringField(TEXT("name"), Property->GetName());
		PropJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType(nullptr, 0));
		PropJson->SetStringField(TEXT("ue_type"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT("Unknown"));

		// Basic, high-signal property flags that are relevant for Python/Blueprint use.
		TArray<FString> Flags;
		if (Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Flags.Add(TEXT("Edit"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Flags.Add(TEXT("BlueprintVisible"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient))
		{
			Flags.Add(TEXT("Transient"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Config))
		{
			Flags.Add(TEXT("Config"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			PropJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		return PropJson;
	}

	TSharedPtr<FJsonObject> BuildFunctionJson(UFunction* Function)
	{
		if (!Function)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> FuncJson = MakeShareable(new FJsonObject);
		FuncJson->SetStringField(TEXT("name"), Function->GetName());

		// Function flags: only expose the ones that matter for scripting.
		TArray<FString> Flags;
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			Flags.Add(TEXT("BlueprintCallable"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			Flags.Add(TEXT("BlueprintPure"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			Flags.Add(TEXT("BlueprintEvent"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Net))
		{
			Flags.Add(TEXT("Net"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Flags.Add(TEXT("Static"));
		}

		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			FuncJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		// Parameters and return type.
		TArray<TSharedPtr<FJsonValue>> ParamsJson;
		TSharedPtr<FJsonObject> ReturnJson;

		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			FProperty* ParamProp = *ParamIt;
			if (!ParamProp)
			{
				continue;
			}

			const bool bIsReturn = ParamProp->HasAnyPropertyFlags(CPF_ReturnParm);
			if (bIsReturn)
			{
				ReturnJson = MakeShareable(new FJsonObject);
				ReturnJson->SetStringField(TEXT("name"), ParamProp->GetName());
				ReturnJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
				ReturnJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
				continue;
			}

			if (!ParamProp->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("name"), ParamProp->GetName());
			ParamJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
			ParamJson->SetStringField(TEXT("ue_type"), ParamProp->GetClass() ? ParamProp->GetClass()->GetName() : TEXT("Unknown"));
			ParamJson->SetBoolField(TEXT("is_out"), ParamProp->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm));
			ParamsJson.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}

		if (ParamsJson.Num() > 0)
		{
			FuncJson->SetArrayField(TEXT("parameters"), ParamsJson);
		}

		if (ReturnJson.IsValid())
		{
			FuncJson->SetObjectField(TEXT("return"), ReturnJson);
		}

		return FuncJson;
	}

	FString BuildReflectionSchemaJson(UClass* Class)
	{
		if (!Class)
		{
			TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
			ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
			ErrorObj->SetStringField(TEXT("message"), TEXT("Class not found"));

			FString ErrorJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ErrorJson);
			FJsonSerializer::Serialize(ErrorObj.ToSharedRef(), Writer);
			return ErrorJson;
		}

		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("class_name"), Class->GetName());
		Root->SetStringField(TEXT("path_name"), Class->GetPathName());
		Root->SetStringField(TEXT("cpp_type"), FString::Printf(TEXT("%s*"), *Class->GetName()));

		// Properties
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			TSharedPtr<FJsonObject> PropJson = BuildPropertyJson(Property);
			if (PropJson.IsValid())
			{
				PropertiesJson.Add(MakeShareable(new FJsonValueObject(PropJson)));
			}
		}
		Root->SetArrayField(TEXT("properties"), PropertiesJson);

		// Functions
		TArray<TSharedPtr<FJsonValue>> FunctionsJson;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			TSharedPtr<FJsonObject> FuncJson = BuildFunctionJson(Function);
			if (FuncJson.IsValid())
			{
				FunctionsJson.Add(MakeShareable(new FJsonValueObject(FuncJson)));
			}
		}
		Root->SetArrayField(TEXT("functions"), FunctionsJson);

		FString OutJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutJson;
	}
}
