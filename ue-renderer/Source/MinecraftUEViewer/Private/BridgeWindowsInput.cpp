#include "BridgeWindowsInput.h"

#if PLATFORM_WINDOWS

#include "BridgeViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Widgets/SWindow.h"
#include "Windows/WindowsHWrapper.h"
#include <dwmapi.h>

namespace
{
    bool KeyIsDown(int32 Key)
    {
        return ::GetKeyState(Key) < 0;
    }

    bool PhysicalKeyIsDown(int32 Key)
    {
        return (::GetAsyncKeyState(Key) & 0x8000) != 0;
    }

    bool PhotoModifiersAreDown()
    {
        return PhysicalKeyIsDown(VK_CONTROL) && !PhysicalKeyIsDown(VK_SHIFT) &&
            !PhysicalKeyIsDown(VK_MENU) && !PhysicalKeyIsDown(VK_LWIN) && !PhysicalKeyIsDown(VK_RWIN);
    }

    uint32 ScanCode(WPARAM VirtualKey, LPARAM State)
    {
        uint32 Scan = (uint32(State) >> 16) & 0x1ff;
        if (Scan == 0)
        {
            const uint32 Mapped = ::MapVirtualKeyW(uint32(VirtualKey), MAPVK_VK_TO_VSC_EX);
            Scan = (Mapped & 0xff) | ((Mapped >> 8) == 0xe0 ? 0x100 : 0);
        }
        // Normalize alternate encodings to the scan codes exposed by GLFW on Windows.
        if (Scan == 0x54) return 0x137;  // Alt+Print Screen
        if (Scan == 0x146) return 0x45;  // Ctrl+Pause
        if (Scan == 0x136) return 0x36;  // Right Shift reported by some IMEs
        return Scan;
    }

    bool IsAltGrPrefix(WPARAM VirtualKey, LPARAM State)
    {
        if (VirtualKey != VK_CONTROL || (uint32(State) & (1u << 24)) != 0)
        {
            return false;
        }
        MSG Next{};
        return ::PeekMessageW(&Next, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) &&
            Next.time == uint32(::GetMessageTime()) && Next.wParam == VK_MENU &&
            (uint32(Next.lParam) & (1u << 24)) != 0 &&
            (uint32(Next.lParam ^ State) & (1u << 31)) == 0;
    }
}

class FBridgeTextInputContext final : public ITextInputMethodContext
{
public:
    explicit FBridgeTextInputContext(FBridgeWindowsInput& InOwner) : Owner(InOwner) {}

    bool IsComposing() override { return bComposing; }
    bool IsReadOnly() override { return !bEnabled; }
    uint32 GetTextLength() override { return Buffer.Len(); }
    TSharedPtr<FGenericWindow> GetWindow() override { return Owner.NativeWindow.Pin(); }

    void GetSelectionRange(uint32& Begin, uint32& Length, ECaretPosition& Caret) override
    {
        Begin = SelectionBegin;
        Length = SelectionLength;
        Caret = CaretPosition;
    }

    void SetSelectionRange(uint32 Begin, uint32 Length, ECaretPosition Caret) override
    {
        SelectionBegin = FMath::Min(Begin, GetTextLength());
        SelectionLength = FMath::Min(Length, GetTextLength() - SelectionBegin);
        CaretPosition = Caret;
    }

    void GetTextInRange(uint32 Begin, uint32 Length, FString& Text) override
    {
        Text = Buffer.Mid(Begin, Length);
    }

    void SetTextInRange(uint32 Begin, uint32 Length, const FString& Text) override
    {
        Begin = FMath::Min(Begin, GetTextLength());
        Length = FMath::Min(Length, GetTextLength() - Begin);
        Buffer = Buffer.Left(Begin) + Text + Buffer.Mid(Begin + Length);
        CommittedLength = FMath::Min(CommittedLength, int32(Begin));
        if (!bComposing)
        {
            Commit();
        }
    }

    int32 GetCharacterIndexFromPoint(const FVector2D&) override { return SelectionBegin; }

    bool GetTextBounds(uint32, uint32, FVector2D& Position, FVector2D& Size) override
    {
        FVector2D Origin, WindowSize;
        GetScreenBounds(Origin, WindowSize);
        // The editable control lives in Minecraft. Anchor the candidate UI to the last click,
        // or to the lower left of the viewport when a text screen was opened from the keyboard.
        Position = Anchor.Get(Origin + FVector2D(12, FMath::Max(0.0, WindowSize.Y - 48)));
        Size = FVector2D(1, 18);
        return false;
    }

    void GetScreenBounds(FVector2D& Position, FVector2D& Size) override
    {
        RECT Client{};
        POINT Origin{};
        ::GetClientRect(Owner.Window, &Client);
        ::ClientToScreen(Owner.Window, &Origin);
        Position = FVector2D(Origin.x, Origin.y);
        Size = FVector2D(Client.right - Client.left, Client.bottom - Client.top);
    }

    void BeginComposition() override { bComposing = true; }
    void UpdateCompositionRange(int32, uint32) override {}

    void EndComposition() override
    {
        bComposing = false;
        CompositionEndedAt = uint32(::GetMessageTime());
        Commit();
    }

    bool OwnsEditingKey(WPARAM VirtualKey) const
    {
        if (!bEnabled || (!bComposing && CompositionEndedAt != uint32(::GetMessageTime())))
        {
            return false;
        }
        // Raw key messages may still reach a viewport while the IME selects or cancels a candidate.
        return VirtualKey == VK_RETURN || VirtualKey == VK_ESCAPE || VirtualKey == VK_SPACE || VirtualKey == VK_TAB ||
               VirtualKey == VK_BACK || VirtualKey == VK_DELETE || (VirtualKey >= VK_PRIOR && VirtualKey <= VK_DOWN);
    }

    void TrimCommitted(ITextInputMethodChangeNotifier& Notifier)
    {
        if (bComposing || Buffer.IsEmpty() || CommittedLength != Buffer.Len())
        {
            return;
        }
        const int32 OldLength = Buffer.Len();
        Clear();
        Notifier.NotifyTextChanged(0, OldLength, 0);
        Notifier.NotifySelectionChanged();
    }

    void Clear()
    {
        Buffer.Reset();
        CommittedLength = SelectionBegin = SelectionLength = 0;
        bComposing = false;
    }

    bool bEnabled = false;
    TOptional<FVector2D> Anchor;

private:
    void Commit()
    {
        if (bEnabled && CommittedLength < Buffer.Len())
        {
            Owner.SubmitText(Buffer.Mid(CommittedLength));
        }
        CommittedLength = Buffer.Len();
    }

    FBridgeWindowsInput& Owner;
    FString Buffer;
    uint32 SelectionBegin = 0;
    uint32 SelectionLength = 0;
    ECaretPosition CaretPosition = ECaretPosition::Ending;
    int32 CommittedLength = 0;
    uint32 CompositionEndedAt = MAX_uint32;
    bool bComposing = false;
};

uint8 FBridgeWindowsInput::Modifiers()
{
    return uint8((KeyIsDown(VK_SHIFT) ? 1 : 0)
        | (KeyIsDown(VK_CONTROL) ? 2 : 0)
        | (KeyIsDown(VK_MENU) ? 4 : 0)
        | (KeyIsDown(VK_LWIN) || KeyIsDown(VK_RWIN) ? 8 : 0)
        | ((::GetKeyState(VK_CAPITAL) & 1) ? 16 : 0)
        | ((::GetKeyState(VK_NUMLOCK) & 1) ? 32 : 0));
}

FBridgeWindowsInput::FBridgeWindowsInput(const TSharedRef<FWindowsApplication>& InApplication,
    UBridgeViewportClient* InOwner)
    : Application(InApplication), Owner(InOwner),
      bWaitForMinecraft(FParse::Param(FCommandLine::Get(), TEXT("BridgeWaitForMinecraft"))),
      bReleaseShortcut(FParse::Param(FCommandLine::Get(), TEXT("BridgeReleaseShortcut")))
{
    InApplication->AddMessageHandler(*this);
}

FBridgeWindowsInput::~FBridgeWindowsInput()
{
    if (const TSharedPtr<FWindowsApplication> App = Application.Pin())
    {
        Reset();
        if (TextInput && TextNotifier)
        {
            if (ITextInputMethodSystem* System = App->GetTextInputMethodSystem())
            {
                System->UnregisterContext(TextInput.ToSharedRef());
            }
        }
        App->RemoveMessageHandler(*this);
    }
}

void FBridgeWindowsInput::SetWindow(const TSharedPtr<FGenericWindow>& InWindow)
{
    const HWND NewWindow = InWindow ? static_cast<HWND>(InWindow->GetOSWindowHandle()) : nullptr;
    if (Window != NewWindow)
    {
        bPhotoShortcutHeld = false;
        bPhotoShortcutArmed = false;
    }
    NativeWindow = InWindow;
    Window = NewWindow;
}

bool FBridgeWindowsInput::HasForeground() const
{
    return Window && ::GetForegroundWindow() == Window;
}

void FBridgeWindowsInput::PollPhotoShortcut()
{
    if (!HasForeground())
    {
        bPhotoShortcutHeld = false;
        bPhotoShortcutArmed = false;
        return;
    }
    if (!PhysicalKeyIsDown(VK_HOME))
    {
        bPhotoShortcutHeld = false;
        bPhotoShortcutArmed = true;
        return;
    }
    // A held chord can be present in Windows even when the viewport never receives its key message.
    if (UBridgeViewportClient* Viewport = Owner.Get(); Viewport && bPhotoShortcutArmed &&
        !bPhotoShortcutHeld && PhotoModifiersAreDown())
    {
        TriggerPhotoShortcut(*Viewport);
    }
}

bool FBridgeWindowsInput::TriggerPhotoShortcut(UBridgeViewportClient& Viewport)
{
    if (Viewport.IsConsoleActive())
    {
        return false;
    }
    bPhotoShortcutHeld = true;
    Viewport.RequestPhoto();
    return true;
}

void FBridgeWindowsInput::SetTextInputEnabled(bool bEnabled)
{
    if (!bEnabled && !TextInput)
    {
        return;
    }
    const TSharedPtr<FWindowsApplication> App = Application.Pin();
    ITextInputMethodSystem* System = App ? App->GetTextInputMethodSystem() : nullptr;
    if (!System)
    {
        return;
    }
    bEnabled &= HasForeground();
    if (bEnabled && !TextInput)
    {
        TextInput = MakeShared<FBridgeTextInputContext>(*this);
        TextNotifier = System->RegisterContext(TextInput.ToSharedRef());
    }
    if (!TextNotifier)
    {
        return;
    }
    const TSharedRef<FBridgeTextInputContext> Context = TextInput.ToSharedRef();
    if (Context->bEnabled != bEnabled)
    {
        Context->bEnabled = bEnabled;
        if (bEnabled)
        {
            TextNotifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Created);
            System->ActivateContext(Context);
        }
        else
        {
            if (Context->IsComposing())
            {
                TextNotifier->CancelComposition();
            }
            if (System->IsActiveContext(Context))
            {
                System->DeactivateContext(Context);
            }
            Context->Clear();
            Context->Anchor.Reset();
        }
    }
    if (bEnabled)
    {
        Context->TrimCommitted(*TextNotifier);
    }
}

void FBridgeWindowsInput::Reset()
{
    HighSurrogate = 0;
    SetTextInputEnabled(false);
}

bool FBridgeWindowsInput::RevealWindow()
{
    if (!Window || !::IsWindowVisible(Window))
    {
        return false;
    }
    if (CloakedWindow == Window)
    {
        const BOOL Cloak = 0;
        if (FAILED(::DwmSetWindowAttribute(Window, DWMWA_CLOAK, &Cloak, sizeof(Cloak))))
        {
            return false;
        }
        ::EnableWindow(Window, 1);
        CloakedWindow = nullptr;
    }
    bWaitForMinecraft = false;
    return true;
}

bool FBridgeWindowsInput::ProcessMessage(HWND Hwnd, uint32 Message, WPARAM WParam, LPARAM LParam,
    int32& OutResult)
{
    if (bWaitForMinecraft && !CloakedWindow &&
        ((Message == WM_SHOWWINDOW && WParam != 0) ||
         (Message == WM_WINDOWPOSCHANGING && (reinterpret_cast<const WINDOWPOS*>(LParam)->flags & SWP_SHOWWINDOW))))
    {
        for (const TSharedRef<SWindow>& Candidate : FSlateApplication::Get().GetTopLevelWindows())
        {
            const TSharedPtr<FGenericWindow> Native = Candidate->GetNativeWindow();
            if (Native && Native->GetOSWindowHandle() == Hwnd && !Candidate->GetParentWindow())
            {
                // Keep the swap chain live while DWM withholds the startup window from the desktop.
                CloakedWindow = Hwnd;
                const BOOL Cloak = 1;
                const HRESULT Result = ::DwmSetWindowAttribute(Hwnd, DWMWA_CLOAK, &Cloak, sizeof(Cloak));
                if (FAILED(Result))
                {
                    CloakedWindow = nullptr;
                    UE_LOG(LogTemp, Warning, TEXT("Could not cloak the UE startup window: 0x%08x."), uint32(Result));
                }
                else
                {
                    ::EnableWindow(Hwnd, 0);
                }
                break;
            }
        }
    }
    UBridgeViewportClient* Viewport = Owner.Get();
    if (!Viewport || Window != Hwnd)
    {
        return false;
    }
    if (Message == WM_KILLFOCUS || (Message == WM_ACTIVATE && LOWORD(WParam) == WA_INACTIVE))
    {
        bConsoleShortcutHeld = false;
        bPhotoShortcutHeld = false;
        bPhotoShortcutArmed = false;
        Viewport->RequestControlRelease(TEXT("native window lost focus"));
        return false;
    }
    if (!HasForeground())
    {
        return false;
    }
    if (WParam == VK_HOME && (Message == WM_KEYDOWN || Message == WM_KEYUP ||
        Message == WM_SYSKEYDOWN || Message == WM_SYSKEYUP))
    {
        if (bPhotoShortcutHeld)
        {
            bPhotoShortcutHeld = Message == WM_KEYDOWN || Message == WM_SYSKEYDOWN;
            OutResult = 0;
            return true;
        }
        if (Message == WM_KEYDOWN && (uint32(LParam) & (1u << 30)) == 0 &&
            ((Modifiers() & 0x0f) == 2 || PhotoModifiersAreDown()) && TriggerPhotoShortcut(*Viewport))
        {
            OutResult = 0;
            return true;
        }
    }
    // Intercept Ctrl+grave before automatic capture can send it to Minecraft.
    if (WParam == VK_OEM_3 && Message == WM_KEYDOWN && (uint32(LParam) & (1u << 30)) == 0)
    {
        const uint8 Mods = Modifiers();
        bConsoleShortcutHeld = (Mods & 0x0e) == 2 && Viewport->ToggleBridgeConsole();
    }
    if (bConsoleShortcutHeld)
    {
        if (WParam == VK_OEM_3 && (Message == WM_KEYDOWN || Message == WM_KEYUP ||
            Message == WM_SYSKEYDOWN || Message == WM_SYSKEYUP))
        {
            bConsoleShortcutHeld = Message == WM_KEYDOWN || Message == WM_SYSKEYDOWN;
            OutResult = 0;
            return true;
        }
        if (Message == WM_CHAR && (WParam == '`' || WParam == '~'))
        {
            OutResult = 0;
            return true;
        }
    }
    if (Viewport->IsConsoleActive())
    {
        return false; // Let UE handle text entry, command history and Escape.
    }
    if (!Viewport->IsControlCaptured() && Message != WM_MOUSEHWHEEL)
    {
        const bool bNewKey = (Message == WM_KEYDOWN || Message == WM_SYSKEYDOWN) && (uint32(LParam) & (1u << 30)) == 0;
        const bool bSystemKey = WParam == VK_MENU || WParam == VK_LWIN || WParam == VK_RWIN || WParam == VK_PROCESSKEY ||
            (KeyIsDown(VK_MENU) && (WParam == VK_TAB || WParam == VK_F4 || WParam == VK_SPACE));
        if (!bNewKey || bSystemKey || !Viewport->CaptureFromKeyboard())
        {
            return false;
        }
    }
    OutResult = 0;
    switch (Message)
    {
        case WM_LBUTTONDOWN:
            if (TextInput)
            {
                if (TextInput->IsComposing())
                {
                    SetTextInputEnabled(false);
                }
                POINT Cursor{};
                if (::GetCursorPos(&Cursor))
                {
                    TextInput->Anchor = FVector2D(Cursor.x, Cursor.y);
                }
            }
            return false;
        case WM_MOUSEHWHEEL:
        {
            const FIntPoint Position(int16(LOWORD(LParam)), int16(HIWORD(LParam)));
            // GLFW reverses the Windows horizontal axis before passing it to Minecraft.
            return Viewport->ForwardHorizontalWheel(-float(GET_WHEEL_DELTA_WPARAM(WParam)) / WHEEL_DELTA, Position);
        }
        case WM_ENTERSIZEMOVE:
            Viewport->RequestControlRelease(TEXT("window move or resize"));
            return false;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return HandleKey(Message, WParam, LParam);
        case WM_CHAR:
            HandleCharacter(uint32(WParam), true);
            return true;
        case WM_SYSCHAR:
            return true;
        case WM_UNICHAR:
            if (WParam == UNICODE_NOCHAR)
            {
                OutResult = 1;
            }
            else
            {
                HandleCharacter(uint32(WParam), false);
            }
            return true;
        default:
            return false;
    }
}

bool FBridgeWindowsInput::HandleKey(uint32 Message, WPARAM VirtualKey, LPARAM State)
{
    using BridgeWire::EInputAction;
    UBridgeViewportClient* Viewport = Owner.Get();
    const bool bReleased = Message == WM_KEYUP || Message == WM_SYSKEYUP;
    const uint8 Mods = Modifiers();
    if (bReleaseShortcut && VirtualKey == VK_F12 && !bReleased && (Mods & 0x0f) == 3)
    {
        Viewport->RequestControlRelease(TEXT("Ctrl+Shift+F12"));
        return false; // Let the viewport release its Slate capture during the key event.
    }
    if ((Mods & 4) != 0 && VirtualKey == VK_SPACE)
    {
        Viewport->RequestControlRelease(TEXT("system window menu"));
        return false;
    }
    if ((Mods & 4) != 0 && (VirtualKey == VK_TAB || VirtualKey == VK_F4))
    {
        return false;
    }
    if (VirtualKey == VK_PROCESSKEY)
    {
        return false;
    }
    if (!bReleased && TextInput && TextInput->OwnsEditingKey(VirtualKey))
    {
        return true;
    }
    if (IsAltGrPrefix(VirtualKey, State))
    {
        return true;
    }
    const uint32 Scan = ScanCode(VirtualKey, State);
    if (Scan == 0 || Scan >= 512)
    {
        return true;
    }
    if (VirtualKey == VK_SNAPSHOT)
    {
        if (bReleased)
        {
            Viewport->ForwardKeyboard({EInputAction::Press, Scan, Mods});
            Viewport->ForwardKeyboard({EInputAction::Release, Scan, Mods});
        }
        return true;
    }
    const EInputAction Action = bReleased ? EInputAction::Release :
        ((uint32(State) & (1u << 30)) != 0 ? EInputAction::Repeat : EInputAction::Press);
    Viewport->ForwardKeyboard({Action, Scan, Mods});
    if (bReleased && VirtualKey == VK_SHIFT && (Mods & 1) == 0)
    {
        // Windows can omit the first release when both Shift keys were held.
        Viewport->ForwardKeyboard({EInputAction::Release, Scan == 0x2a ? 0x36u : 0x2au, Mods});
    }
    return true;
}

void FBridgeWindowsInput::HandleCharacter(uint32 Codepoint, bool bUtf16)
{
    if (bUtf16 && Codepoint >= 0xd800 && Codepoint <= 0xdbff)
    {
        HighSurrogate = Codepoint;
        return;
    }
    if (bUtf16 && Codepoint >= 0xdc00 && Codepoint <= 0xdfff && HighSurrogate != 0)
    {
        Codepoint = 0x10000 + ((HighSurrogate - 0xd800) << 10) + (Codepoint - 0xdc00);
    }
    HighSurrogate = 0;
    if (Codepoint < 0x20 || (Codepoint >= 0x7f && Codepoint <= 0x9f) ||
        (Codepoint >= 0xd800 && Codepoint <= 0xdfff) || Codepoint > 0x10ffff)
    {
        return;
    }
    if (UBridgeViewportClient* Viewport = Owner.Get())
    {
        Viewport->ForwardKeyboard({BridgeWire::EInputAction::Character, Codepoint, 0});
    }
}

void FBridgeWindowsInput::SubmitText(const FString& Text)
{
    HighSurrogate = 0;
    for (TCHAR Unit : Text)
    {
        HandleCharacter(uint32(Unit), true);
    }
    HighSurrogate = 0;
}

#endif
