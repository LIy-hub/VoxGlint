#pragma once

#include "CoreMinimal.h"

namespace BridgeUpscaling
{
    enum class EMethod : uint8 { TSR, DLSS, FSR, XeSS, Pending };

    struct FSettings
    {
        EMethod Requested = EMethod::TSR;
        EMethod Active = EMethod::TSR;
        int32 Quality = 0;
        float RenderPercentage = 0.0f;
    };

    FSettings Resolve(int32 Requested, int32 Quality);
    void Apply(const FSettings& Settings);
    EMethod ObservedMethod();
    const TCHAR* Name(EMethod Method);
}
