// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TestingPlace : ModuleRules
{
	public TestingPlace(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"VeilNetRuntime",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "ReplicationGraph"});

		PublicIncludePaths.AddRange(new string[] {
			"TestingPlace",
			"TestingPlace/Variant_Horror",
			"TestingPlace/Variant_Horror/UI",
			"TestingPlace/Variant_Shooter",
			"TestingPlace/Variant_Shooter/AI",
			"TestingPlace/Variant_Shooter/UI",
			"TestingPlace/Variant_Shooter/Weapons"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
