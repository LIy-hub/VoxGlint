using System.IO;
using UnrealBuildTool;

public class MinecraftUEViewer : ModuleRules
{
    public MinecraftUEViewer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Bridge implementation files have translation-unit-local helpers and namespace imports.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "InputCore", "Sockets", "ProceduralMeshComponent",
            "ApplicationCore", "Slate", "SlateCore", "RenderCore", "Renderer", "RHI", "ImageCore", "Niagara"
        });
        PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "AssetRegistry", "UnrealEd", "NiagaraShader" });
            PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Plugins/FX/Niagara/Source/Niagara/Internal"));
            PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Plugins/FX/Niagara/Source/NiagaraShader/Internal"));
        }
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.Add("D3D12RHI");
            PublicSystemLibraries.Add("dwmapi.lib");
        }
        AddOptionalGraphicsPlugin(Target, "DLSS", "MCUE_WITH_DLSS", "DLSSBlueprint");
        AddOptionalGraphicsPlugin(Target, "XeSS", "MCUE_WITH_XESS", "XeSSBlueprint", "XeFGBlueprint", "XeFGRHI");
        AddOptionalGraphicsPlugin(Target, "FSR", "MCUE_WITH_FSR", "FFXFrameInterpolation", "FFXShared");
        AddOptionalGraphicsPlugin(Target, "StreamlineDLSSG", "MCUE_WITH_DLSSG",
            "StreamlineDLSSGBlueprint", "StreamlineReflexBlueprint");
    }

    private void AddOptionalGraphicsPlugin(ReadOnlyTargetRules Target, string Plugin, string Define, params string[] Modules)
    {
        bool Enabled = Target.Platform == UnrealTargetPlatform.Win64 && Target.ProjectFile != null &&
            File.Exists(Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins", "Upscalers", Plugin, Plugin + ".uplugin"));
        PrivateDefinitions.Add(Define + (Enabled ? "=1" : "=0"));
        if (Enabled)
        {
            PrivateDependencyModuleNames.AddRange(Modules);
        }
    }
}
