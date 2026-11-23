#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "Windows/WindowsApplication.h"

class UBridgeViewportClient;
class FBridgeTextInputContext;
class ITextInputMethodChangeNotifier;

class FBridgeWindowsInput final : public IWindowsMessageHandler
{
public:
    FBridgeWindowsInput(const TSharedRef<FWindowsApplication>& InApplication, UBridgeViewportClient* InOwner);
    ~FBridgeWindowsInput();

    bool ProcessMessage(HWND Hwnd, uint32 Msg, WPARAM WParam, LPARAM LParam, int32& OutResult) override;
    void SetWindow(const TSharedPtr<FGenericWindow>& InWindow);
    void SetTextInputEnabled(bool bEnabled);
    bool HasForeground() const;
    void PollPhotoShortcut();
    bool RevealWindow();
    static uint8 Modifiers();
    void Reset();

private:
    friend class FBridgeTextInputContext;

    bool HandleKey(uint32 Message, WPARAM VirtualKey, LPARAM State);
    bool TriggerPhotoShortcut(UBridgeViewportClient& Viewport);
    void HandleCharacter(uint32 CodeUnit, bool bUtf16);
    void SubmitText(const FString& Text);

    TWeakPtr<FWindowsApplication> Application;
    TWeakPtr<FGenericWindow> NativeWindow;
    TWeakObjectPtr<UBridgeViewportClient> Owner;
    TSharedPtr<FBridgeTextInputContext> TextInput;
    TSharedPtr<ITextInputMethodChangeNotifier> TextNotifier;
    HWND Window = nullptr;
    HWND CloakedWindow = nullptr;
    bool bWaitForMinecraft = false;
    bool bReleaseShortcut = false;
    bool bConsoleShortcutHeld = false;
    bool bPhotoShortcutHeld = false;
    bool bPhotoShortcutArmed = false;
    uint32 HighSurrogate = 0;
};

#endif
