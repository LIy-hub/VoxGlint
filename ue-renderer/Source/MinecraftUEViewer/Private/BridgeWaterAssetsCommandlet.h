#pragma once

#include "Commandlets/Commandlet.h"
#include "BridgeWaterAssetsCommandlet.generated.h"

UCLASS()
class UBridgeWaterAssetsCommandlet final : public UCommandlet
{
    GENERATED_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
