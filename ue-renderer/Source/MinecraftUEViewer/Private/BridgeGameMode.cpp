#include "BridgeGameMode.h"

#include "BridgeViewer.h"
#include "BridgeViewportClient.h"
#include "Engine/World.h"

void ABridgeHud::DrawHUD()
{
    Super::DrawHUD();
    if (UBridgeViewportClient* Viewport = Cast<UBridgeViewportClient>(GetWorld()->GetGameViewport()))
    {
        Viewport->DrawMinecraftUi(Canvas);
    }
}

ABridgeGameMode::ABridgeGameMode()
{
    DefaultPawnClass = nullptr;
    SpectatorClass = nullptr;
    HUDClass = ABridgeHud::StaticClass();
}

void ABridgeGameMode::StartPlay()
{
    Super::StartPlay();
    GetWorld()->SpawnActor<ABridgeViewer>();
}
