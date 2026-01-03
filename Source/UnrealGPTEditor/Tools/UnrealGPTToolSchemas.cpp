#include "UnrealGPTToolSchemas.h"
#include "Serialization/JsonSerializer.h"

TArray<FToolSchema> UnrealGPTToolSchemas::GetStandardToolSchemas(bool bEnablePython, bool bEnableViewport, bool bEnableReplicate)
{
	TArray<FToolSchema> Schemas;

	// python_execute
	if (bEnablePython)
	{
		FToolSchema PythonSchema(TEXT("python_execute"),
			TEXT("Execute Python code in Unreal Engine editor. Use this to manipulate actors, spawn objects, modify properties, automate Content Browser and asset/Blueprint operations, and perform other editor tasks not possible with other tools. ")
			TEXT("Code runs in the editor Python environment with access to the 'unreal' module and editor subsystems. ")
			TEXT("All executions are wrapped in an Editor transaction for Undo support.\n\n")
			TEXT("Returns a standard JSON envelope:\n")
			TEXT("{\n")
			TEXT("  \"status\": \"ok\" | \"error\",\n")
			TEXT("  \"message\": \"human-readable summary\",\n")
			TEXT("  \"details\": { ... custom data ... },\n")
			TEXT("  \"logs\": { \"stdout\": \"...\", \"warnings\": [...] },\n")
			TEXT("  \"transaction\": { \"used\": true, \"id\": 123 },\n")
			TEXT("  \"error\": { \"type\": \"ExceptionType\", \"traceback\": \"...\" }  // only on error\n")
			TEXT("}\n\n")
			TEXT("Your code can modify the 'result' dict to set status, message, and details. ")
			TEXT("If you populate 'result[\"details\"][\"actor_label\"]' or 'result[\"details\"][\"actor_name\"]', the viewport will auto-focus on it."));
		PythonSchema.AddParam(FToolParameter::String(TEXT("code"), TEXT("Python code to execute"), true));
		Schemas.Add(PythonSchema);
	}

	// viewport_screenshot
	if (bEnableViewport)
	{
		FToolSchema ScreenshotSchema(TEXT("viewport_screenshot"),
			TEXT("Capture a screenshot of the active viewport.\n\n")
			TEXT("The image is sent as multimodal input - you will SEE the viewport and must ANALYZE it. ")
			TEXT("Describe what you observe: object placement, lighting, colors, scale, and whether the scene matches the user's request.\n\n")
			TEXT("Optional: Pass focus_actor with an actor label to auto-frame that actor before capture.\n\n")
			TEXT("Returns JSON metadata alongside the image:\n")
			TEXT("{\n")
			TEXT("  \"camera\": {\"location\": {...}, \"rotation\": {...}, \"fov\": 90},\n")
			TEXT("  \"resolution\": {\"width\": 1024, \"height\": 768},\n")
			TEXT("  \"selected_actors\": [\"ActorLabel1\", ...],\n")
			TEXT("  \"focused_actor\": \"ActorLabel\"  // if focus_actor was used\n")
			TEXT("}"));
		ScreenshotSchema.AddParam(FToolParameter::String(TEXT("focus_actor"),
			TEXT("Optional actor label to focus/frame before capturing. The viewport camera will move to show this actor.")));
		Schemas.Add(ScreenshotSchema);
	}

	// scene_query - always enabled
	{
		FToolSchema SceneQuerySchema(TEXT("scene_query"),
			TEXT("Search the current level for actors matching simple filters.\n\n")
			TEXT("Returns a summary object with pagination info and an actors array:\n")
			TEXT("{\n")
			TEXT("  \"summary\": { \"total_matched\": 47, \"returned\": 20, \"has_more\": true, \"offset\": 0, \"top_classes\": [...] },\n")
			TEXT("  \"actors\": [ { \"id\": \"StaticMeshActor_42\", \"label\": \"Bridge\", \"class\": \"...\" }, ... ]\n")
			TEXT("}\n\n")
			TEXT("Each actor returns:\n")
			TEXT("- id: stable unique identifier (use this for subsequent tool calls)\n")
			TEXT("- label: user-friendly display name (may have duplicates)\n")
			TEXT("- class: actor class name\n\n")
			TEXT("Use the 'fields' parameter to request additional data (comma-separated):\n")
			TEXT("- location: actor world position {x,y,z}\n")
			TEXT("- rotation: actor rotation {pitch,yaw,roll}\n")
			TEXT("- scale: actor scale {x,y,z}\n")
			TEXT("- bounds: bounding box {origin, extent}\n")
			TEXT("- components: root_component info and static_mesh_path\n")
			TEXT("- tags: actor tags array\n")
			TEXT("- folder: folder path in outliner\n")
			TEXT("- parent: parent actor if attached\n\n")
			TEXT("Example: fields=\"location,rotation\" or fields=\"location,bounds,tags\"\n\n")
			TEXT("IMPORTANT: Use the returned 'id' field for subsequent operations to avoid label collisions."));
		SceneQuerySchema.AddParam(FToolParameter::String(TEXT("class_contains"),
			TEXT("Optional substring to match in actor class names, e.g., 'DirectionalLight', 'StaticMeshActor'.")));
		SceneQuerySchema.AddParam(FToolParameter::String(TEXT("label_contains"),
			TEXT("Optional substring to match in actor labels as shown in the Outliner.")));
		SceneQuerySchema.AddParam(FToolParameter::String(TEXT("name_contains"),
			TEXT("Optional substring to match in actor object names.")));
		SceneQuerySchema.AddParam(FToolParameter::String(TEXT("component_class_contains"),
			TEXT("Optional substring to match in component class names, e.g., 'DirectionalLightComponent'.")));
		SceneQuerySchema.AddParam(FToolParameter::Integer(TEXT("max_results"),
			TEXT("Maximum number of matching actors to return (default 20).")));
		SceneQuerySchema.AddParam(FToolParameter::Integer(TEXT("offset"),
			TEXT("Number of matching actors to skip for pagination (default 0).")));
		SceneQuerySchema.AddParam(FToolParameter::String(TEXT("fields"),
			TEXT("Comma-separated list of additional fields to include: location,rotation,scale,bounds,components,tags,folder,parent. Default: none (minimal output).")));
		Schemas.Add(SceneQuerySchema);
	}

	// reflection_query - always enabled
	{
		FToolSchema ReflectionSchema(TEXT("reflection_query"),
			TEXT("Inspect an Unreal UClass via the reflection system at runtime. ")
			TEXT("Given a class_name (C++ or Blueprint), this returns a JSON schema describing its reflected properties and functions, ")
			TEXT("including names, C++ types, and high-signal flags that matter for Python/Blueprint access."));
		ReflectionSchema.AddParam(FToolParameter::String(TEXT("class_name"),
			TEXT("Name or path of the UClass to inspect. You can pass a short name like 'StaticMeshActor' or a fully qualified path like '/Script/Engine.StaticMeshActor'."), true));
		Schemas.Add(ReflectionSchema);
	}

	// replicate_generate
	if (bEnableReplicate)
	{
		FToolSchema ReplicateSchema(TEXT("replicate_generate"),
			TEXT("Generate content using Replicate (images, video, audio, or 3D files) via the Replicate HTTP API. ")
			TEXT("Returns JSON with 'status', 'message', and 'details.files' containing local file paths for any downloaded outputs. ")
			TEXT("After calling this, use python_execute (for example with the 'unrealgpt_mcp_import' helpers) to import the files as Unreal assets, ")
			TEXT("then verify placement with scene_query and/or viewport_screenshot."));
		ReplicateSchema.AddParam(FToolParameter::String(TEXT("prompt"),
			TEXT("Text prompt describing what to generate (image, video, audio, or 3D asset). For example: ")
			TEXT("'seamless square floral rock wall texture' or 'short ambient forest soundscape'."), true));
		ReplicateSchema.AddParam(FToolParameter::String(TEXT("version"),
			TEXT("Optional Replicate model identifier. You can pass either a full version id or an 'owner/model' slug for official models (for example 'black-forest-labs/flux-dev').")));
		ReplicateSchema.AddParam(FToolParameter::String(TEXT("output_kind"),
			TEXT("Optional output kind hint: 'image', 'video', 'audio', or '3d'. Used to pick a staging folder, downstream Unreal import helper, ")
			TEXT("and default Replicate model from plugin settings when no explicit model version is provided.")));
		ReplicateSchema.AddParam(FToolParameter::String(TEXT("output_subkind"),
			TEXT("Optional sub-kind for audio or other outputs, e.g. 'sfx', 'music', or 'speech'. ")
			TEXT("This is used to choose between SFX, music, and speech Replicate models configured in settings when 'version' is omitted.")));
		Schemas.Add(ReplicateSchema);
	}

	// ==================== ATOMIC EDITOR TOOLS ====================

	// get_actor
	{
		FToolSchema GetActorSchema(TEXT("get_actor"),
			TEXT("Get detailed information about a specific actor.\n\n")
			TEXT("Accepts 'id' (stable internal name, preferred) or 'label' (display name).\n")
			TEXT("Use 'id' when you need guaranteed unique matching (e.g., from scene_query results).\n\n")
			TEXT("Returns JSON with transform, bounds, class, components, and more:\n")
			TEXT("{\n")
			TEXT("  \"status\": \"ok\",\n")
			TEXT("  \"actor\": {\n")
			TEXT("    \"id\": \"StaticMeshActor_42\",  // stable unique identifier\n")
			TEXT("    \"label\": \"Bridge_Deck\",      // user-friendly display name\n")
			TEXT("    \"class\": \"...\",\n")
			TEXT("    \"location\": {...}, \"rotation\": {...}, \"scale\": {...},\n")
			TEXT("    \"bounds\": {\"origin\": {...}, \"extent\": {...}},\n")
			TEXT("    \"mobility\": \"Static|Stationary|Movable\",\n")
			TEXT("    \"static_mesh_path\": \"...\",\n")
			TEXT("    \"tags\": [...], \"folder_path\": \"...\"\n")
			TEXT("  }\n")
			TEXT("}"));
		GetActorSchema.AddParam(FToolParameter::String(TEXT("id"),
			TEXT("Actor id (internal name from scene_query). Preferred - guaranteed unique.")));
		GetActorSchema.AddParam(FToolParameter::String(TEXT("label"),
			TEXT("Actor label (as shown in Outliner). Fallback if id not provided.")));
		Schemas.Add(GetActorSchema);
	}

	// set_actor_transform
	{
		FToolSchema SetTransformSchema(TEXT("set_actor_transform"),
			TEXT("Set an actor's transform (location, rotation, scale). Only provided fields are changed.\n\n")
			TEXT("Accepts 'id' (stable, preferred) or 'label' (display name).\n")
			TEXT("Returns the new transform with actor id/label. Wrapped in an Editor transaction for Undo."));
		SetTransformSchema.AddParam(FToolParameter::String(TEXT("id"),
			TEXT("Actor id (internal name). Preferred - guaranteed unique.")));
		SetTransformSchema.AddParam(FToolParameter::String(TEXT("label"),
			TEXT("Actor label (Outliner name). Fallback if id not provided.")));
		SetTransformSchema.AddParam(FToolParameter::Object(TEXT("location"),
			TEXT("New location {x, y, z}. Omit to keep current.")));
		SetTransformSchema.AddParam(FToolParameter::Object(TEXT("rotation"),
			TEXT("New rotation {pitch, yaw, roll} in degrees. Omit to keep current.")));
		SetTransformSchema.AddParam(FToolParameter::Object(TEXT("scale"),
			TEXT("New scale {x, y, z}. Omit to keep current.")));
		SetTransformSchema.AddParam(FToolParameter::Boolean(TEXT("sweep"),
			TEXT("If true, sweep to new location (checks for collisions). Default false.")));
		Schemas.Add(SetTransformSchema);
	}

	// select_actors
	{
		FToolSchema SelectSchema(TEXT("select_actors"),
			TEXT("Select one or more actors.\n\n")
			TEXT("Accepts 'ids' array (preferred) or 'labels' array (backwards compatible).\n")
			TEXT("Returns selected actors with both id and label for each."));
		SelectSchema.AddParam(FToolParameter::StringArray(TEXT("ids"),
			TEXT("Array of actor ids (internal names). Preferred - guaranteed unique.")));
		SelectSchema.AddParam(FToolParameter::StringArray(TEXT("labels"),
			TEXT("Array of actor labels. Fallback if ids not provided.")));
		SelectSchema.AddParam(FToolParameter::Boolean(TEXT("add_to_selection"),
			TEXT("If true, add to current selection. If false (default), replace selection.")));
		Schemas.Add(SelectSchema);
	}

	// duplicate_actor
	{
		FToolSchema DuplicateSchema(TEXT("duplicate_actor"),
			TEXT("Duplicate an actor one or more times with optional offset between copies.\n\n")
			TEXT("Accepts 'id' (preferred) or 'label' to identify the source actor.\n")
			TEXT("Returns created actors with both id and label. Wrapped in an Editor transaction for Undo."));
		DuplicateSchema.AddParam(FToolParameter::String(TEXT("id"),
			TEXT("Actor id (internal name) to duplicate. Preferred - guaranteed unique.")));
		DuplicateSchema.AddParam(FToolParameter::String(TEXT("label"),
			TEXT("Actor label to duplicate. Fallback if id not provided.")));
		DuplicateSchema.AddParam(FToolParameter::Integer(TEXT("count"),
			TEXT("Number of copies to create. Default 1.")));
		DuplicateSchema.AddParam(FToolParameter::Object(TEXT("offset"),
			TEXT("Offset {x, y, z} between each copy. Default {0, 0, 0}.")));
		Schemas.Add(DuplicateSchema);
	}

	// snap_actor_to_ground
	{
		FToolSchema SnapSchema(TEXT("snap_actor_to_ground"),
			TEXT("Snap an actor to the ground/surface below it using a line trace.\n\n")
			TEXT("Accepts 'id' (preferred) or 'label' to identify the actor.\n")
			TEXT("Returns the new location with actor id/label. Wrapped in an Editor transaction for Undo."));
		SnapSchema.AddParam(FToolParameter::String(TEXT("id"),
			TEXT("Actor id (internal name). Preferred - guaranteed unique.")));
		SnapSchema.AddParam(FToolParameter::String(TEXT("label"),
			TEXT("Actor label. Fallback if id not provided.")));
		SnapSchema.AddParam(FToolParameter::Boolean(TEXT("align_to_normal"),
			TEXT("If true, align actor rotation to surface normal. Default false.")));
		SnapSchema.AddParam(FToolParameter::Number(TEXT("offset"),
			TEXT("Vertical offset from ground after snapping. Default 0.")));
		Schemas.Add(SnapSchema);
	}

	// set_actors_rotation
	{
		FToolSchema BatchRotationSchema(TEXT("set_actors_rotation"),
			TEXT("Set rotation on multiple actors at once. Efficient for batch rotation updates.\n\n")
			TEXT("Accepts 'ids' array (preferred) or 'labels' array (backwards compatible).\n")
			TEXT("Returns modified actors with both id and label. Wrapped in an Editor transaction for Undo."));
		BatchRotationSchema.AddParam(FToolParameter::StringArray(TEXT("ids"),
			TEXT("Array of actor ids (internal names). Preferred - guaranteed unique.")));
		BatchRotationSchema.AddParam(FToolParameter::StringArray(TEXT("labels"),
			TEXT("Array of actor labels. Fallback if ids not provided.")));
		BatchRotationSchema.AddParam(FToolParameter::Object(TEXT("rotation"),
			TEXT("New rotation {pitch, yaw, roll} in degrees to apply to all actors."), true));
		BatchRotationSchema.AddParam(FToolParameter::Boolean(TEXT("relative"),
			TEXT("If true, add rotation to current rotation (relative). If false (default), set absolute rotation.")));
		Schemas.Add(BatchRotationSchema);
	}

	return Schemas;
}

TSharedPtr<FJsonObject> UnrealGPTToolSchemas::BuildParametersJson(const FToolSchema& Schema)
{
	TSharedPtr<FJsonObject> ParamsObj = MakeShareable(new FJsonObject);
	ParamsObj->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> PropertiesObj = MakeShareable(new FJsonObject);
	TArray<TSharedPtr<FJsonValue>> RequiredArray;

	for (const FToolParameter& Param : Schema.Parameters)
	{
		TSharedPtr<FJsonObject> PropObj = MakeShareable(new FJsonObject);
		PropObj->SetStringField(TEXT("type"), Param.Type);
		PropObj->SetStringField(TEXT("description"), Param.Description);

		// Handle array types
		if (Param.Type == TEXT("array") && !Param.ArrayItemType.IsEmpty())
		{
			TSharedPtr<FJsonObject> ItemsObj = MakeShareable(new FJsonObject);
			ItemsObj->SetStringField(TEXT("type"), Param.ArrayItemType);
			PropObj->SetObjectField(TEXT("items"), ItemsObj);
		}

		// Handle default values
		if (!Param.DefaultValue.IsEmpty())
		{
			if (Param.Type == TEXT("integer") || Param.Type == TEXT("number"))
			{
				PropObj->SetNumberField(TEXT("default"), FCString::Atof(*Param.DefaultValue));
			}
			else if (Param.Type == TEXT("boolean"))
			{
				PropObj->SetBoolField(TEXT("default"), Param.DefaultValue.ToBool());
			}
			else
			{
				PropObj->SetStringField(TEXT("default"), Param.DefaultValue);
			}
		}

		PropertiesObj->SetObjectField(Param.Name, PropObj);

		if (Param.bRequired)
		{
			RequiredArray.Add(MakeShareable(new FJsonValueString(Param.Name)));
		}
	}

	ParamsObj->SetObjectField(TEXT("properties"), PropertiesObj);

	if (RequiredArray.Num() > 0)
	{
		ParamsObj->SetArrayField(TEXT("required"), RequiredArray);
	}

	return ParamsObj;
}

TSharedPtr<FJsonObject> UnrealGPTToolSchemas::BuildToolJson(const FToolSchema& Schema)
{
	TSharedPtr<FJsonObject> Tool = MakeShareable(new FJsonObject);
	Tool->SetStringField(TEXT("type"), TEXT("function"));

	TSharedPtr<FJsonObject> ParamsJson = BuildParametersJson(Schema);

	Tool->SetStringField(TEXT("name"), Schema.Name);
	Tool->SetStringField(TEXT("description"), Schema.Description);
	Tool->SetObjectField(TEXT("parameters"), ParamsJson);

	return Tool;
}
