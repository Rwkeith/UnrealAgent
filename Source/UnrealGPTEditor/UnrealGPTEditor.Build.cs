using UnrealBuildTool;

public class UnrealGPTEditor : ModuleRules
{
	public UnrealGPTEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
				ModuleDirectory,
				System.IO.Path.Combine(ModuleDirectory, "Agent"),
				System.IO.Path.Combine(ModuleDirectory, "Protocol"),
				System.IO.Path.Combine(ModuleDirectory, "Network"),
				System.IO.Path.Combine(ModuleDirectory, "Conversation"),
				System.IO.Path.Combine(ModuleDirectory, "Telemetry"),
				System.IO.Path.Combine(ModuleDirectory, "Types"),
				System.IO.Path.Combine(ModuleDirectory, "Tools"),
				System.IO.Path.Combine(ModuleDirectory, "UI"),
				System.IO.Path.Combine(ModuleDirectory, "Session")
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealGPT",
				"DeveloperSettings",
				"Projects" // For IPluginManager
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"EditorStyle",
				"EditorWidgets",
				"ToolMenus",
				"UnrealEd",
				"LevelEditor",
				"WorkspaceMenuStructure",
				"PropertyEditor",
				"Settings",
				"HTTP",
				"Json",
				"JsonUtilities",
				"PythonScriptPlugin",
				"EditorSubsystem",
				"ToolWidgets",
				"ApplicationCore",
				"InputCore",
				"AudioCapture",
				"AudioCaptureCore",
				"AudioMixer",
				"RHI",
				"RenderCore"
			}
		);
	}
}

