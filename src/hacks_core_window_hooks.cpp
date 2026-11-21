#include "pch.h"
#include "hacks_core.h"
#include "hacks_vars.h"
#include "win32_utils.h"

namespace
{
FORCEINLINE void UninstallWindowHook(HHOOK& hook)
{
    if (hook != nullptr)
    {
        UnhookWindowsHookEx(hook);
        hook = nullptr;
    }
}

FORCEINLINE INT HitTestToWMSZ(INT hittest)
{
    switch (hittest)
    {
    case HTLEFT:
        return WMSZ_LEFT;
    case HTTOP:
        return WMSZ_TOP;
    case HTRIGHT:
        return WMSZ_RIGHT;
    case HTBOTTOM:
        return WMSZ_BOTTOM;
    case HTTOPLEFT:
        return WMSZ_TOPLEFT;
    case HTTOPRIGHT:
        return WMSZ_TOPRIGHT;
    case HTBOTTOMLEFT:
        return WMSZ_BOTTOMLEFT;
    case HTBOTTOMRIGHT:
        return WMSZ_BOTTOMRIGHT;
    default:
        break;
    }
    return 0;
}

} // namespace

bool OpenHacksCore::InstallWindowHooks()
{
    if (InstallWindowHooksInternal())
        return true;

    mInstallHooksWin32Error = GetLastError();
    mInitErrors |= HooksInstallError;
    UninstallWindowHooks();
    return false;
}

bool OpenHacksCore::InstallWindowHooksInternal()
{
    HINSTANCE hmod = core_api::get_my_instance();
    const DWORD threadId = GetCurrentThreadId();

    mCallWndHook = SetWindowsHookExW(WH_CALLWNDPROC, StaticOpenHacksCallWndProc, hmod, threadId);
    if (mCallWndHook == nullptr)
        return false;

    mGetMsgHook = SetWindowsHookExW(WH_GETMESSAGE, StaticOpenHacksGetMessageProc, hmod, threadId);
    if (mGetMsgHook == nullptr)
        return false;

    return true;
}

void OpenHacksCore::UninstallWindowHooks()
{
    UninstallWindowHook(mGetMsgHook);
    UninstallWindowHook(mCallWndHook);
}

LRESULT OpenHacksCore::OpenHacksCallWndProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= HC_ACTION)
    {
        const auto pcwps = reinterpret_cast<PCWPSTRUCT>(lp);
        switch (pcwps->message)
        { // Fix window white background flickering problem
        case WM_NCCREATE:
        {
            if (mMainWindow == nullptr)
            {
                wchar_t className[MAX_PATH] = {};
                GetClassNameW(pcwps->hwnd, className, ARRAYSIZE(className));
                if (className == kDUIMainWindowClassName)
                {
                    SetClassLongPtr(pcwps->hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(NULL_BRUSH));
                }
            }
            break;
        }

        case WM_CREATE:
        {
            if (mMainWindow == nullptr)
            {
                wchar_t className[MAX_PATH] = {};
                GetClassNameW(pcwps->hwnd, className, ARRAYSIZE(className));
                if (className == kDUIMainWindowClassName)
                {
                    mMainWindow = pcwps->hwnd;
                    
                    LONG exStyle = GetWindowLong(pcwps->hwnd, GWL_EXSTYLE);
                    SetWindowLong(pcwps->hwnd, GWL_EXSTYLE, exStyle | WS_EX_COMPOSITED | WS_EX_LAYERED);
                    SetLayeredWindowAttributes(pcwps->hwnd, 0, 0, LWA_ALPHA);
                    mUsedCompositedStyle = true;
                    
                    mMainWindowOriginProc = (WNDPROC)SetWindowLongPtr(pcwps->hwnd, GWLP_WNDPROC, (LONG_PTR)StaticOpenHacksMainWindowProc);
                    OpenHacksVars::DPI = Utility::GetDPI(mMainMenuWindow);
                }
            }

            break;
        } // end

        default:
            break;
        }
    }

    return CallNextHookEx(mCallWndHook, code, wp, lp);
}

LRESULT OpenHacksCore::OpenHacksGetMessageProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= HC_ACTION && (UINT)wp == PM_REMOVE)
    {
        auto msg = (LPMSG)(lp);
        if (IsMainOrChildWindow(msg->hwnd))
        {
            switch (msg->message)
            {
            case WM_MOUSEMOVE:
                OnHookMouseMove(msg);
                break;

            case WM_LBUTTONDOWN:
                OnHookLButtonDown(msg);
                break;

            case WM_LBUTTONDBLCLK:
                OnHookLButtonDblClk(msg);
                break;

            case WM_LBUTTONUP:
                mCaptionArmed = false;
                if (mCaptionDragActive)
                    EndCaptionDrag();
                break;

            default:
                break;
            }
        }
    }

    return CallNextHookEx(mGetMsgHook, code, wp, lp);
}

void OpenHacksCore::OnHookMouseMove(LPMSG msg)
{
    if (OpenHacksVars::MainWindowFrameStyle != WindowFrameStyleNoBorder)
        return;

    GUITHREADINFO threadInfo = {};
    threadInfo.cbSize = sizeof(threadInfo);
    if (!GetGUIThreadInfo(GetCurrentThreadId(), &threadInfo))
        return;

    // MOD patch: caption drag loop - keep the window following the cursor and
    // remember whether the release happens at the top edge of the monitor.
    if (mCaptionDragActive)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000))
        {
            EndCaptionDrag();
        }
        else
        {
            POINT cursor = {};
            GetCursorPos(&cursor);
            int32_t left = mDragAnchorRect.left + (cursor.x - mDragAnchorCursor.x);
            int32_t top = mDragAnchorRect.top + (cursor.y - mDragAnchorCursor.y);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi))
            {
                const int32_t width = mDragAnchorRect.right - mDragAnchorRect.left;
                const int32_t minLeft = mi.rcWork.left - width + 120;
                const int32_t maxLeft = mi.rcWork.right - 120;
                if (left < minLeft) left = minLeft;
                if (left > maxLeft) left = maxLeft;
                if (top < mi.rcWork.top) top = mi.rcWork.top;

                constexpr int32_t kTopEdgeZone = 8;
                constexpr int32_t kMinDragUp = 8;
                const int32_t draggedUp = mCaptionDragStartY - cursor.y;
                mDragToTopPending = cursor.y <= mi.rcWork.top + kTopEdgeZone
                    && draggedUp >= kMinDragUp;
            }

            SetWindowPos(mMainWindow, nullptr, left, top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        msg->message = WM_NULL;
        return;
    }

    // MOD patch: the pseudo-caption lives inside a child panel window, so the mouse
    // capture belongs to that child. The armed caption drag has to be handled
    // before the capture filter, otherwise the drag never starts.
    if (mCaptionArmed)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000))
        {
            mCaptionArmed = false;
        }
        else
        {
            POINT cursor = {};
            GetCursorPos(&cursor);
            const int32_t dx = cursor.x - mCaptionPressPt.x;
            const int32_t dy = cursor.y - mCaptionPressPt.y;
            if (dx > 4 || dx < -4 || dy > 4 || dy < -4)
            {
                mCaptionArmed = false;
                StartCaptionMove(cursor);
            }
        }

        return;
    }

    if (threadInfo.hwndCapture != nullptr && threadInfo.hwndCapture != mMainWindow
        && !IsChild(mMainWindow, threadInfo.hwndCapture))
        return;

    if (threadInfo.flags & GUI_INMOVESIZE)
    {
        // MOD patch: while dragging the caption, remember whether the cursor
        // reached the top edge of the monitor work area.
        if (mCaptionDragActive)
        {
            POINT cursor = {};
            GetCursorPos(&cursor);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi))
            {
                constexpr int32_t kTopEdgeZone = 8;
                constexpr int32_t kMinDragUp = 8;
                const int32_t workTop = mi.rcWork.top;
                const int32_t draggedUp = mCaptionDragStartY - cursor.y;
                mDragToTopPending = cursor.y <= workTop + kTopEdgeZone && draggedUp >= kMinDragUp;
            }
        }

        return;
    }

    if (threadInfo.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return;

    const DWORD messagePos = GetMessagePos();
    const POINT pt = {GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos)};
    const Rect rectForNonSizeing = GetRectForNonSizing();
    if (rectForNonSizeing.IsPointIn(pt))
    {
        if (mRequireRevertCursor)
        {
            mRequireRevertCursor = false;
            SendMessage(mMainWindow, WM_SETCURSOR, (WPARAM)mMainWindow, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }

        return;
    }

    const int32_t hittest = (int32_t)SendMessage(mMainWindow, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
    if (hittest != HTCLIENT)
    {
        mRequireRevertCursor = true;
        SendMessage(mMainWindow, WM_SETCURSOR, (WPARAM)mMainWindow, MAKELPARAM(hittest, WM_MOUSEMOVE));
        msg->message = WM_NULL;
    }
}

void OpenHacksCore::StartCaptionMove(POINT pt)
{
    // MOD patch: dragging the caption of a maximized / fullscreen window brings it
    // back to a normal window, keeping the grabbed spot of the caption under the
    // cursor. A normal window is never repositioned here: a plain click must not
    // move the window at all.
    const auto& pseudoCaption = OpenHacksVars::PseudoCaptionSettings.get_value();
    const Rect rectPseudoCaption = pseudoCaption.ToRect(mMainWindow);
    const int32_t grabOffsetX = pt.x - rectPseudoCaption.left;
    const int32_t grabOffsetY = pt.y - rectPseudoCaption.top;

    const bool wasFullscreen = Utility::IsFullscreen(mMainWindow) != 0;
    const bool wasMaximized = Utility::IsMaximized(mMainWindow) != 0;
    const bool transitioning = wasFullscreen || wasMaximized || mSavedWindowState.has_value();

    if (transitioning)
    {
        // MOD patch: OHM "maximizes" by simply resizing a borderless window, so
        // GetWindowPlacement reports the maximized-like rect as the normal one.
        // The real pre-restore geometry lives in the state saved by Maximize(),
        // so it has to be captured before Restore() clears that state.
        WINDOWPLACEMENT savedNormal = {};
        savedNormal.length = sizeof(savedNormal);
        bool haveSavedNormal = false;
        if (mSavedWindowState.has_value() && mSavedWindowState->wp.length != 0)
        {
            savedNormal = mSavedWindowState->wp;
            haveSavedNormal = true;
        }

        WINDOWPLACEMENT normalPlacement = {};
        normalPlacement.length = sizeof(normalPlacement);
        GetWindowPlacement(mMainWindow, &normalPlacement);

        const WINDOWPLACEMENT& sizeSource = haveSavedNormal ? savedNormal : normalPlacement;
        int32_t width = sizeSource.rcNormalPosition.right - sizeSource.rcNormalPosition.left;
        int32_t height = sizeSource.rcNormalPosition.bottom - sizeSource.rcNormalPosition.top;

        MONITORINFO areaInfo = { sizeof(areaInfo) };
        GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &areaInfo);
        const int32_t workWidth = areaInfo.rcWork.right - areaInfo.rcWork.left;
        const int32_t workHeight = areaInfo.rcWork.bottom - areaInfo.rcWork.top;

        // MOD patch: a borderless window that only looked maximized stores the
        // maximized rect as its normal rect, so a "restore" would change nothing.
        // Ignore such a size and fall back to a remembered or derived one.
        const bool sizeLooksNormal = width > 0 && height > 0
            && (width < workWidth - 40 || height < workHeight - 40);
        if (!sizeLooksNormal)
        {
            if (mHasLastNormalRect)
            {
                width = mLastNormalRect.right - mLastNormalRect.left;
                height = mLastNormalRect.bottom - mLastNormalRect.top;
            }
            else
            {
                width = workWidth * 62 / 100;
                height = workHeight * 70 / 100;
            }
        }

        if (wasFullscreen)
            ExitFullscreen();

        if (Utility::IsMaximized(mMainWindow) || mSavedWindowState.has_value())
            Restore();

        if (Utility::IsMaximized(mMainWindow))
        {
            normalPlacement.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(mMainWindow, &normalPlacement);
            SendMessage(mMainWindow, WM_SIZE, 0, 0);
            mSavedWindowState.reset();
            OpenHacksVars::SavedWindowState.get_value() = WindowStateData();
        }

        if (width <= 0 || height <= 0)
        {
            RECT wr = {};
            GetWindowRect(mMainWindow, &wr);
            width = wr.right - wr.left;
            height = wr.bottom - wr.top;
        }

        int32_t left = pt.x - grabOffsetX;
        int32_t top = pt.y - grabOffsetY;

        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const int32_t minLeft = mi.rcWork.left - width + 120;
            const int32_t maxLeft = mi.rcWork.right - 120;
            if (left < minLeft) left = minLeft;
            if (left > maxLeft) left = maxLeft;
            if (top < mi.rcWork.top) top = mi.rcWork.top;
        }

        SetWindowPos(mMainWindow, nullptr, left, top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!transitioning)
    {
        RECT normalRect = {};
        if (GetWindowRect(mMainWindow, &normalRect)
            && normalRect.right - normalRect.left > 0)
        {
            mLastNormalRect = normalRect;
            mHasLastNormalRect = true;
        }
    }

    // MOD patch: the native SC_MOVE loop does not keep following the cursor once
    // the window has just been restored from a maximized state, so the drag is
    // driven by our own loop in OnHookMouseMove instead.
    GetWindowRect(mMainWindow, &mDragAnchorRect);
    mDragAnchorCursor = pt;
    mCaptionDragStartY = pt.y;
    mDragToTopPending = false;
    mCaptionDragActive = true;
}

void OpenHacksCore::EndCaptionDrag()
{
    const bool dragToTop = mDragToTopPending;
    mCaptionDragActive = false;
    mDragToTopPending = false;

    // MOD patch: released at the top edge of the screen -> maximize
    if (dragToTop && !Utility::IsMaximized(mMainWindow))
        Maximize();
}

void OpenHacksCore::OnHookLButtonDown(LPMSG msg)
{
    if (OpenHacksVars::MainWindowFrameStyle == WindowFrameStyleDefault)
        return;

    GUITHREADINFO threadInfo = {};
    threadInfo.cbSize = sizeof(threadInfo);
    if (GetGUIThreadInfo(GetCurrentThreadId(), &threadInfo))
    {
        if (threadInfo.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
            return;

        const DWORD messagePos = GetMessagePos();
        const POINT pt = {GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos)};

        // Check if click is in pseudo-caption area
        const auto& pseudoCaption = OpenHacksVars::PseudoCaptionSettings.get_value();
        Rect rectPseudoCaption = pseudoCaption.ToRect(mMainWindow);
        if (rectPseudoCaption.IsPointIn(pt))
        {
            // MOD patch: when maximized, do not restore on a plain click - arm the
            // drag instead and let OnHookMouseMove start it once the mouse moves.
            if (Utility::IsMaximized(mMainWindow) || mSavedWindowState.has_value())
            {
                mCaptionArmed = true;
                mCaptionPressPt = pt;
                return;
            }

            StartCaptionMove(pt);
            msg->message = WM_NULL;
            return;
        }

        if (OpenHacksVars::MainWindowFrameStyle == WindowFrameStyleNoBorder)
        {
            // simulate resizing
            const Rect rectForNonSizeing = GetRectForNonSizing();
            if (!rectForNonSizeing.IsPointIn(pt))
            {
                bool isInMoveSize = (threadInfo.flags & GUI_INMOVESIZE) != 0;
                
                if (isInMoveSize) return;
                
                const int32_t hittest = (int32_t)SendMessage(mMainWindow, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
                if (hittest != HTCLIENT)
                {
                    SendMessage(mMainWindow, WM_SETCURSOR, (WPARAM)mMainWindow, MAKELPARAM(hittest, WM_MOUSEMOVE));
                    SendMessage(mMainWindow, WM_SYSCOMMAND, SC_SIZE | HitTestToWMSZ(hittest), MAKELPARAM(pt.x, pt.y));
                    msg->message = WM_NULL;
                    return;
                }
            }
        }
    }
}

void OpenHacksCore::OnHookLButtonDblClk(LPMSG msg)
{
    if (OpenHacksVars::MainWindowFrameStyle == WindowFrameStyleDefault)
        return;

    GUITHREADINFO threadInfo = {};
    threadInfo.cbSize = sizeof(threadInfo);
    if (GetGUIThreadInfo(GetCurrentThreadId(), &threadInfo))
    {
        if (threadInfo.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
            return;

        const DWORD messagePos = GetMessagePos();
        const POINT pt = {GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos)};

        // Check if double-click is in pseudo-caption area
        const auto& pseudoCaption = OpenHacksVars::PseudoCaptionSettings.get_value();
        Rect rectPseudoCaption = pseudoCaption.ToRect(mMainWindow);
        if (rectPseudoCaption.IsPointIn(pt))
        {
            // If maximized or has saved state (custom maximize), restore it
            if (Utility::IsMaximized(mMainWindow) || mSavedWindowState.has_value())
            {
                Restore();
                msg->message = WM_NULL;
                return;
            }
            
            // If fullscreen, exit fullscreen
            if (Utility::IsFullscreen(mMainWindow))
            {
                ExitFullscreen();
                msg->message = WM_NULL;
                return;
            }
            
            // Normal case: maximize the window
            Maximize();
            msg->message = WM_NULL;
            return;
        }
    }
}
