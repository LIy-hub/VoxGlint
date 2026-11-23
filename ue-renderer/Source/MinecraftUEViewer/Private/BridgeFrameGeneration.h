#pragma once

#include "CoreMinimal.h"

namespace BridgeFrameGeneration
{
    struct FStatus
    {
        int32 Backend = 0;
        int32 SupportedModes = 0;
        int32 Multiplier = 0;
        bool bRestartRequired = false;

        bool operator==(const FStatus&) const = default;
    };

    void ConfigureStartup(int32 Backend);
    void PrepareGameViewport();
    FStatus Update(int32 RequestedBackend, int32 RequestedMultiplier);
    FString TraceStatus();
    bool WritesHudCoverage();
    const TCHAR* Name(int32 Backend);
}
