#pragma once

#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "BridgeGameMode.generated.h"

UCLASS()
class ABridgeHud : public AHUD
{
    GENERATED_BODY()

    virtual void DrawHUD() override;
};

UCLASS()
class ABridgeGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ABridgeGameMode();
    virtual void StartPlay() override;
};
