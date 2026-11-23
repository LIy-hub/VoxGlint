using UnrealBuildTool;

public class MinecraftUEViewerTarget : TargetRules
{
    public MinecraftUEViewerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("MinecraftUEViewer");
    }
}
