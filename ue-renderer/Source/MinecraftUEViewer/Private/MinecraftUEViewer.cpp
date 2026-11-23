#include "Modules/ModuleManager.h"
#include "BridgeFrameGeneration.h"
#include "BridgeQuality.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

class FMinecraftUEViewerModule final : public FDefaultGameModuleImpl
{
    void StartupModule() override
    {
#if WITH_EDITOR
        AddShaderSourceDirectoryMapping(TEXT("/MinecraftUEViewer"), FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));
#endif

#if PLATFORM_WINDOWS
        BridgeFrameGeneration::ConfigureStartup(BridgeQuality::StartupFrameGenerationBackend());
        const FString ParentPid = FPlatformMisc::GetEnvironmentVariable(TEXT("MCUE_PARENT_PID"));
        if (ParentPid.IsEmpty())
        {
            return;
        }
        uint32 Pid = 0;
        if (LexTryParseString(Pid, *ParentPid) && Pid != 0)
        {
            Parent = ::OpenProcess(SYNCHRONIZE, false, Pid);
        }
        if (!Parent || !::RegisterWaitForSingleObject(&ParentWatch, Parent, OnParentExited, nullptr, INFINITE, WT_EXECUTEONLYONCE))
        {
            FPlatformMisc::RequestExit(false, TEXT("MinecraftUEViewer.ParentWatchFailed"));
        }
#endif
    }

    void ShutdownModule() override
    {
#if PLATFORM_WINDOWS
        if (ParentWatch)
        {
            ::UnregisterWaitEx(ParentWatch, INVALID_HANDLE_VALUE);
        }
        if (Parent)
        {
            ::CloseHandle(Parent);
        }
#endif
    }

#if PLATFORM_WINDOWS
    static VOID CALLBACK OnParentExited(PVOID, BOOLEAN)
    {
        // The parent may exit while the game thread is blocked in the graphics driver.
        ::TerminateProcess(::GetCurrentProcess(), 0);
    }

    HANDLE Parent = nullptr;
    HANDLE ParentWatch = nullptr;
#endif
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMinecraftUEViewerModule, MinecraftUEViewer, "MinecraftUEViewer");
