using UnrealBuildTool;

public class VeilNetRuntime : ModuleRules
{
	public VeilNetRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"NetCore",
			"EasyHWIDRuntime",
			"HTTP",
			"Json"
		});
	}
}
