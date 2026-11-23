using UnrealBuildTool;

public class MinecraftUEViewerEditorTarget : TargetRules
{
    public MinecraftUEViewerEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("MinecraftUEViewer");
    }
}
