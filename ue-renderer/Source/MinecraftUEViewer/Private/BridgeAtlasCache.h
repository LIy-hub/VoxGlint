#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"

struct FBridgeAtlasFile
{
    int32 Width = 0;
    int32 Height = 0;
    int32 Mips = 0;
    TArray<uint8> Pixels;
};
