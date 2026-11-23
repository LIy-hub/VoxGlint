#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING
class UMinecraftWeatherSubsystem;
class ABridgeViewer;
void PollBridgeWeatherProbe(const UMinecraftWeatherSubsystem& Weather, const ABridgeViewer& Viewer);
#endif
