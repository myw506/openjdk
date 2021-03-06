/*
 * Copyright (c) 1996, 2007, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * This file has been modified by Azul Systems, Inc. in 2014. These
 * modifications are Copyright (c) 2014 Azul Systems, Inc., and are made
 * available on the same license terms set forth above. 
 */

#include "awt_Toolkit.h"
#include "awt_TextArea.h"
#include "awt_TextComponent.h"
#include "awt_dlls.h"
#include "awt_KeyboardFocusManager.h"
#include "awt_Canvas.h"
#include "awt_Unicode.h"
#include "awt_Window.h"

/* IMPORTANT! Read the README.JNI file for notes on JNI converted AWT code.
 */

/***********************************************************************/
// Struct for _ReplaceText() method
struct ReplaceTextStruct {
  jobject textComponent;
  jstring text;
  int start, end;
};

/************************************************************************
 * AwtTextArea fields
 */

jfieldID AwtTextArea::scrollbarVisibilityID;

WNDPROC AwtTextArea::sm_pDefWindowProc = NULL;
BOOL AwtTextArea::sm_RichEdit20 = (IS_WIN98 || IS_NT);

/************************************************************************
 * AwtTextArea methods
 */

AwtTextArea::AwtTextArea() {
    m_bIgnoreEnChange = FALSE;
    m_bCanUndo        = FALSE;
    m_hEditCtrl       = NULL;
    m_lHDeltaAccum    = 0;
    m_lVDeltaAccum    = 0;
}

AwtTextArea::~AwtTextArea()
{
}

void AwtTextArea::Dispose()
{
    if (m_hEditCtrl != NULL) {
        VERIFY(::DestroyWindow(m_hEditCtrl));
        m_hEditCtrl = NULL;
    }
    AwtTextComponent::Dispose();
}

LPCTSTR AwtTextArea::GetClassName() {
    load_rich_edit_library();
    return sm_RichEdit20 ? RICHEDIT_CLASS : TEXT("RICHEDIT");
}

/* Create a new AwtTextArea object and window.   */
AwtTextArea* AwtTextArea::Create(jobject peer, jobject parent)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    jobject target = NULL;
    AwtTextArea* c = NULL;

    try {
        if (env->EnsureLocalCapacity(1) < 0) {
            return NULL;
        }

        PDATA pData;
        AwtCanvas* awtParent;
        JNI_CHECK_PEER_GOTO(parent, done);

        awtParent = (AwtCanvas*)pData;
        JNI_CHECK_NULL_GOTO(awtParent, "null awtParent", done);

        target = env->GetObjectField(peer, AwtObject::targetID);
        JNI_CHECK_NULL_GOTO(target, "null target", done);

        c = new AwtTextArea();

        {
            /* Adjust style for scrollbar visibility and word wrap  */
          DWORD scroll_style;
          jint scrollbarVisibility =
              env->GetIntField(target, AwtTextArea::scrollbarVisibilityID);

          switch (scrollbarVisibility) {
            case java_awt_TextArea_SCROLLBARS_NONE:
              scroll_style = ES_AUTOVSCROLL;
              break;
            case java_awt_TextArea_SCROLLBARS_VERTICAL_ONLY:
              scroll_style = WS_VSCROLL | ES_AUTOVSCROLL;
              break;
            case java_awt_TextArea_SCROLLBARS_HORIZONTAL_ONLY:
              scroll_style = WS_HSCROLL | ES_AUTOHSCROLL | ES_AUTOVSCROLL;
              break;
            case java_awt_TextArea_SCROLLBARS_BOTH:
              scroll_style = WS_VSCROLL | WS_HSCROLL |
                  ES_AUTOVSCROLL | ES_AUTOHSCROLL;
              break;
          }

          /*
           * Specify ES_DISABLENOSCROLL - RichEdit control style to disable
           * scrollbars instead of hiding them when not needed.
           */
          DWORD style = WS_CHILD | WS_CLIPSIBLINGS | ES_LEFT | ES_MULTILINE |
              ES_WANTRETURN | scroll_style |
              (IS_WIN4X ? 0 : WS_BORDER) | ES_DISABLENOSCROLL;
          DWORD exStyle = IS_WIN4X ? WS_EX_CLIENTEDGE : 0;
          if (GetRTL()) {
              exStyle |= WS_EX_RIGHT | WS_EX_LEFTSCROLLBAR;
              if (GetRTLReadingOrder())
                  exStyle |= WS_EX_RTLREADING;
          }

          jint x = env->GetIntField(target, AwtComponent::xID);
          jint y = env->GetIntField(target, AwtComponent::yID);
          jint width = env->GetIntField(target, AwtComponent::widthID);
          jint height = env->GetIntField(target, AwtComponent::heightID);

          c->CreateHWnd(env, L"", style, exStyle,
                        x, y, width, height,
                        awtParent->GetHWnd(),
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                awtParent->CreateControlID())),
                        ::GetSysColor(COLOR_WINDOWTEXT),
                        ::GetSysColor(COLOR_WINDOW),
                        peer);

          // Fix for 4753116.
          // If it is not win95 (we are using Richedit 2.0)
          // we set plain text mode, in which the control is
          // similar to a standard edit control:
          //  - The text in a plain text control can have only
          //    one format.
          //  - The user cannot paste rich text formats, such as RTF
          //    or embedded objects into a plain text control.
          //  - Rich text mode controls always have a default
          //    end-of-document marker or carriage return,
          //    to format paragraphs.
          // kdm@sparc.spb.su
          if (sm_RichEdit20) {
              c->SendMessage(EM_SETTEXTMODE, TM_PLAINTEXT, 0);
          }

          c->m_backgroundColorSet = TRUE;
          /* suppress inheriting parent's color. */
          c->UpdateBackground(env, target);
          c->SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(1, 1));
          /*
           * Fix for BugTraq Id 4260109.
           * Set the text limit to the maximum.
           * Use EM_EXLIMITTEXT for RichEdit controls.
           * For some reason RichEdit 1.0 becomes read-only if the
           * specified limit is greater than 0x7FFFFFFD.
           */
          c->SendMessage(EM_EXLIMITTEXT, 0, 0x7FFFFFFD);

          /* Unregister RichEdit built-in drop target. */
          VERIFY(::RevokeDragDrop(c->GetHWnd()) != DRAGDROP_E_INVALIDHWND);

          /* To enforce CF_TEXT format for paste operations. */
          VERIFY(c->SendMessage(EM_SETOLECALLBACK, 0,
                                (LPARAM)&GetOleCallback()));

          c->SendMessage(EM_SETEVENTMASK, 0, ENM_CHANGE);
        }
    } catch (...) {
        env->DeleteLocalRef(target);
        throw;
    }

done:
    env->DeleteLocalRef(target);

    return c;
}

void AwtTextArea::EditSetSel(CHARRANGE &cr) {
    // Fix for 5003402: added restoring/hiding selection to enable automatic scrolling
    LockWindowUpdate(GetHWnd());
    SendMessage(EM_HIDESELECTION, FALSE, TRUE);
    SendMessage(EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    SendMessage(EM_HIDESELECTION, TRUE, TRUE);
    // 6417581: LockWindowUpdate doesn't force expected drawing
    if (IS_WINVISTA && cr.cpMin == cr.cpMax) {
        ::InvalidateRect(GetHWnd(), NULL, TRUE);
    }
    LockWindowUpdate(NULL);
}

void AwtTextArea::EditGetSel(CHARRANGE &cr) {
    SendMessage(EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
}

LONG AwtTextArea::EditGetCharFromPos(POINT& pt) {
    return static_cast<LONG>(SendMessage(EM_CHARFROMPOS, 0,
            reinterpret_cast<LPARAM>(&pt)));
}

/* Count how many '\n's are there in jStr */
size_t AwtTextArea::CountNewLines(JNIEnv *env, jstring jStr, size_t maxlen)
{
    size_t nNewlines = 0;

    if (jStr == NULL) {
        return nNewlines;
    }
    /*
     * Fix for BugTraq Id 4260109.
     * Don't use TO_WSTRING since it allocates memory on the stack
     * causing stack overflow when the text is very long.
     */
    size_t length = env->GetStringLength(jStr) + 1;
    WCHAR *string = new WCHAR[length];
    env->GetStringRegion(jStr, 0, static_cast<jsize>(length - 1), string);
    string[length-1] = '\0';
    for (size_t i = 0; i < maxlen && i < length - 1; i++) {
        if (string[i] == L'\n') {
            nNewlines++;
        }
    }
    delete[] string;
    return nNewlines;
}

BOOL AwtTextArea::InheritsNativeMouseWheelBehavior() {return true;}

MsgRouting
AwtTextArea::PreProcessMsg(MSG& msg)
{
    MsgRouting mr = mrPassAlong;
    static BOOL bPassAlongWmLButtonUp = TRUE;

    if (msg.message == WM_LBUTTONDBLCLK) {
        bPassAlongWmLButtonUp = FALSE;
    }

    /*
     * For some reason RichEdit 1.0 filters out WM_LBUTTONUP after
     * WM_LBUTTONDBLCLK. To work around this "feature" we send WM_LBUTTONUP
     * directly to the window procedure and consume instead of passing it
     * to the next hook.
     */
    if (msg.message == WM_LBUTTONUP && bPassAlongWmLButtonUp == FALSE) {
        SendMessage(WM_LBUTTONUP, msg.wParam, msg.lParam);
        bPassAlongWmLButtonUp = TRUE;
        mr = mrConsume;
    }

    if (mr == mrPassAlong) {
        mr = AwtComponent::PreProcessMsg(msg);
    }

    return mr;
}

LRESULT
AwtTextArea::WindowProc(UINT message, WPARAM wParam, LPARAM lParam) {

    LRESULT retValue = 0;
    MsgRouting mr = mrDoDefault;

    switch (message) {
        case WM_PRINTCLIENT:
          {
            FORMATRANGE fr;
            HDC hPrinterDC = (HDC)wParam;
            int nHorizRes = ::GetDeviceCaps(hPrinterDC, HORZRES);
            int nVertRes = ::GetDeviceCaps(hPrinterDC, VERTRES);
            int nLogPixelsX = ::GetDeviceCaps(hPrinterDC, LOGPIXELSX);
            int nLogPixelsY = ::GetDeviceCaps(hPrinterDC, LOGPIXELSY);

            // Ensure the printer DC is in MM_TEXT mode.
            ::SetMapMode ( hPrinterDC, MM_TEXT );

            // Rendering to the same DC we are measuring.
            ::ZeroMemory(&fr, sizeof(fr));
            fr.hdc = fr.hdcTarget = hPrinterDC;
            // Set up the page.
            fr.rcPage.left     = fr.rcPage.top = 0;
            fr.rcPage.right    = (nHorizRes/nLogPixelsX) * 1440; // in twips
            fr.rcPage.bottom   = (nVertRes/nLogPixelsY) * 1440;
            fr.rc.left   = fr.rcPage.left;
            fr.rc.top    = fr.rcPage.top;
            fr.rc.right  = fr.rcPage.right;
            fr.rc.bottom = fr.rcPage.bottom;

            // start printing from the first visible line
            LRESULT nLine = SendMessage(EM_GETFIRSTVISIBLELINE, 0, 0);
            LONG startCh = static_cast<LONG>(SendMessage(EM_LINEINDEX,
                                                         (WPARAM)nLine, 0));
            fr.chrg.cpMin = startCh;
            fr.chrg.cpMax = -1;

            SendMessage(EM_FORMATRANGE, TRUE, (LPARAM)&fr);
          }

        break;
    case EM_SETCHARFORMAT:
    case WM_SETFONT:
        SetIgnoreEnChange(TRUE);
        break;
    }

    retValue = AwtComponent::WindowProc(message, wParam, lParam);

    switch (message) {
    case EM_SETCHARFORMAT:
    case WM_SETFONT:
        SetIgnoreEnChange(FALSE);
        break;
    }

    return retValue;
}

/*
 * This routine is a window procedure for the subclass of the standard edit control
 * used to generate context menu. RichEdit controls don't have built-in context menu.
 * To implement this functionality we have to create an invisible edit control and
 * forward WM_CONTEXTMENU messages from a RichEdit control to this helper edit control.
 * While the edit control context menu is active we intercept the message generated in
 * response to particular item selection and forward it back to the RichEdit control.
 * (See AwtTextArea::WmContextMenu for more details).
 */
LRESULT
AwtTextArea::EditProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

    static BOOL bContextMenuActive = FALSE;

    LRESULT retValue = 0;
    MsgRouting mr = mrDoDefault;

    DASSERT(::IsWindow(::GetParent(hWnd)));

    switch (message) {
    case WM_SETFOCUS:
        ::SendMessage(::GetParent(hWnd), EM_HIDESELECTION, FALSE, 0);
        break;
    case WM_KILLFOCUS:
        ::SendMessage(::GetParent(hWnd), EM_HIDESELECTION, TRUE, 0);
        break;

    case WM_UNDO:
    case WM_CUT:
    case WM_COPY:
    case WM_PASTE:
    case WM_CLEAR:
    case EM_SETSEL:
        if (bContextMenuActive) {
            ::SendMessage(::GetParent(hWnd), message, wParam, lParam);
            mr = mrConsume;
        }
        break;
    case WM_CONTEXTMENU:
        bContextMenuActive = TRUE;
        break;
    }

    if (mr == mrDoDefault) {
        DASSERT(sm_pDefWindowProc != NULL);
        retValue = ::CallWindowProc(sm_pDefWindowProc,
                                    hWnd, message, wParam, lParam);
    }

    if (message == WM_CONTEXTMENU) {
        bContextMenuActive = FALSE;
    }

    return retValue;
}

MsgRouting
AwtTextArea::WmContextMenu(HWND hCtrl, UINT xPos, UINT yPos) {

    /* Use the system provided edit control class to generate context menu. */
    if (m_hEditCtrl == NULL) {
        DWORD dwStyle = WS_CHILD;
        DWORD dwExStyle = 0;
        m_hEditCtrl = ::CreateWindowEx(dwExStyle,
                                        L"EDIT",
                                        L"TEXT",
                                        dwStyle,
                                        0, 0, 0, 0,
                                        GetHWnd(),
                                        reinterpret_cast<HMENU>(
                                         static_cast<INT_PTR>(
                                             CreateControlID())),
                                        AwtToolkit::GetInstance().GetModuleHandle(),
                                        NULL);
        DASSERT(m_hEditCtrl != NULL);
        if (sm_pDefWindowProc == NULL) {
            sm_pDefWindowProc = (WNDPROC)::GetWindowLongPtr(m_hEditCtrl,
                                                         GWLP_WNDPROC);
        }
        ::SetLastError(0);
        INT_PTR ret = ::SetWindowLongPtr(m_hEditCtrl, GWLP_WNDPROC,
                                   (INT_PTR)AwtTextArea::EditProc);
        DASSERT(ret != 0 || ::GetLastError() == 0);
    }

    /*
     * Tricks on the edit control to ensure that its context menu has
     * the correct set of enabled items according to the RichEdit state.
     */
    ::SetWindowText(m_hEditCtrl, TEXT("TEXT"));

    if (m_bCanUndo == TRUE && SendMessage(EM_CANUNDO)) {
        /* Enable 'Undo' item. */
        ::SendMessage(m_hEditCtrl, WM_CHAR, 'A', 0);
    }

    {
        /*
         * Initial selection for the edit control - (0,1).
         * This enables 'Cut', 'Copy' and 'Delete' and 'Select All'.
         */
        INT nStart = 0;
        INT nEnd = 1;
        if (SendMessage(EM_SELECTIONTYPE) == SEL_EMPTY) {
            /*
             * RichEdit selection is empty - clear selection of the edit control.
             * This disables 'Cut', 'Copy' and 'Delete'.
             */
            nStart = -1;
            nEnd = 0;
        } else {

            CHARRANGE cr;
            EditGetSel(cr);
            /* Check if all the text is selected. */
            if (cr.cpMin == 0) {

                int len = 0;
                if (m_isWin95) {
                    len = ::GetWindowTextLengthA(GetHWnd());
                } else {
                    len = ::GetWindowTextLengthW(GetHWnd());
                }
                if (cr.cpMin == 0 && cr.cpMax >= len) {
                    /*
                     * All the text is selected in RichEdit - select all the
                     * text in the edit control. This disables 'Select All'.
                     */
                    nStart = 0;
                    nEnd = -1;
                }
            }
        }
        ::SendMessage(m_hEditCtrl, EM_SETSEL, (WPARAM)nStart, (LPARAM)nEnd);
    }

    /* Disable 'Paste' item if the RichEdit control is read-only. */
    ::SendMessage(m_hEditCtrl, EM_SETREADONLY,
                  GetStyle() & ES_READONLY ? TRUE : FALSE, 0);

    POINT p;
    p.x = xPos;
    p.y = yPos;

    /*
     * If the context menu is requested with SHIFT+F10 or VK_APPS key,
     * we position its top left corner to the center of the RichEdit
     * client rect.
     */
    if (p.x == -1 && p.y == -1) {
        RECT r;
        VERIFY(::GetClientRect(GetHWnd(), &r));
        p.x = (r.left + r.right) / 2;
        p.y = (r.top + r.bottom) / 2;
        VERIFY(::ClientToScreen(GetHWnd(), &p));
    }

    ::SendMessage(m_hEditCtrl, WM_CONTEXTMENU, (WPARAM)m_hEditCtrl,
                  MAKELPARAM(p.x, p.y));
    /*
     * After the context menu is dismissed focus is owned by the edit contol.
     * Return focus to parent.
     */
    if (IsFocusable() && AwtComponent::sm_focusOwner != GetHWnd()) {
        JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);
        jobject target = GetTarget(env);
        env->CallStaticVoidMethod
            (AwtKeyboardFocusManager::keyboardFocusManagerCls,
             AwtKeyboardFocusManager::heavyweightButtonDownMID,
             target, TimeHelper::getMessageTimeUTC());
        env->DeleteLocalRef(target);
        AwtSetFocus();
    }

    return mrConsume;
}

MsgRouting
AwtTextArea::WmNcHitTest(UINT x, UINT y, LRESULT& retVal)
{
    if (::IsWindow(AwtWindow::GetModalBlocker(AwtComponent::GetTopLevelParentForWindow(GetHWnd())))) {
        retVal = HTCLIENT;
        return mrConsume;
    }
    return AwtTextComponent::WmNcHitTest(x, y, retVal);
}


MsgRouting
AwtTextArea::WmNotify(UINT notifyCode)
{
    if (notifyCode == EN_CHANGE) {
        /*
         * Ignore notifications if the text hasn't been changed.
         * EN_CHANGE sent on character formatting changes as well.
         */
        if (m_bIgnoreEnChange == FALSE) {
            m_bCanUndo = TRUE;
            DoCallback("valueChanged", "()V");
        } else {
            m_bCanUndo = FALSE;
        }
    }
    return mrDoDefault;
}

MsgRouting
AwtTextArea::HandleEvent(MSG *msg, BOOL synthetic)
{
    MsgRouting returnVal;
    /*
     * RichEdit 1.0 control starts internal message loop if the
     * left mouse button is pressed while the cursor is not over
     * the current selection or the current selection is empty.
     * Because of this we don't receive WM_MOUSEMOVE messages
     * while the left mouse button is pressed. To work around
     * this behavior we process the relevant mouse messages
     * by ourselves.
     * By consuming WM_MOUSEMOVE messages we also don't give
     * the RichEdit control a chance to recognize a drag gesture
     * and initiate its own drag-n-drop operation.
     */
    if (msg->message == WM_LBUTTONDOWN || msg->message == WM_LBUTTONDBLCLK) {

        if (IsFocusable() && AwtComponent::sm_focusOwner != GetHWnd()) {
            JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);
            jobject target = GetTarget(env);
            env->CallStaticVoidMethod
                (AwtKeyboardFocusManager::keyboardFocusManagerCls,
                 AwtKeyboardFocusManager::heavyweightButtonDownMID,
                 target, ((jlong)msg->time) & 0xFFFFFFFF);
            env->DeleteLocalRef(target);
            AwtSetFocus();
        }

        CHARRANGE cr;

        LONG lCurPos = EditGetCharFromPos(msg->pt);

        EditGetSel(cr);
        /*
         * NOTE: Plain EDIT control always clears selection on mouse
         * button press. We are clearing the current selection only if
         * the mouse pointer is not over the selected region.
         * In this case we sacrifice backward compatibility
         * to allow dnd of the current selection.
         */
        if (lCurPos < cr.cpMin || cr.cpMax <= lCurPos) {
            if (msg->message == WM_LBUTTONDBLCLK) {
                SetStartSelectionPos(static_cast<LONG>(SendMessage(
                    EM_FINDWORDBREAK, WB_MOVEWORDLEFT, lCurPos)));
                SetEndSelectionPos(static_cast<LONG>(SendMessage(
                    EM_FINDWORDBREAK, WB_MOVEWORDRIGHT, lCurPos)));
            } else {
                SetStartSelectionPos(lCurPos);
                SetEndSelectionPos(lCurPos);
            }
            cr.cpMin = GetStartSelectionPos();
            cr.cpMax = GetEndSelectionPos();
            EditSetSel(cr);
        }

        delete msg;
        return mrConsume;
    } else if (msg->message == WM_LBUTTONUP) {

        /*
         * If the left mouse button is pressed on the selected region
         * we don't clear the current selection. We clear it on button
         * release instead. This is to allow dnd of the current selection.
         */
        if (GetStartSelectionPos() == -1 && GetEndSelectionPos() == -1) {
            CHARRANGE cr;

            LONG lCurPos = EditGetCharFromPos(msg->pt);

            cr.cpMin = lCurPos;
            cr.cpMax = lCurPos;
            EditSetSel(cr);
        }

        /*
         * Cleanup the state variables when left mouse button is released.
         * These state variables are designed to reflect the selection state
         * while the left mouse button is pressed and be set to -1 otherwise.
         */
        SetStartSelectionPos(-1);
        SetEndSelectionPos(-1);
        SetLastSelectionPos(-1);

        delete msg;
        return mrConsume;
    } else if (msg->message == WM_MOUSEMOVE && (msg->wParam & MK_LBUTTON)) {

        /*
         * We consume WM_MOUSEMOVE while the left mouse button is pressed,
         * so we have to simulate autoscrolling when mouse is moved outside
         * of the client area.
         */
        POINT p;
        RECT r;
        BOOL bScrollLeft = FALSE;
        BOOL bScrollRight = FALSE;
        BOOL bScrollUp = FALSE;
        BOOL bScrollDown = FALSE;

        p.x = msg->pt.x;
        p.y = msg->pt.y;
        VERIFY(::GetClientRect(GetHWnd(), &r));

        if (p.x < 0) {
            bScrollLeft = TRUE;
            p.x = 0;
        } else if (p.x > r.right) {
            bScrollRight = TRUE;
            p.x = r.right - 1;
        }
        if (p.y < 0) {
            bScrollUp = TRUE;
            p.y = 0;
        } else if (p.y > r.bottom) {
            bScrollDown = TRUE;
            p.y = r.bottom - 1;
        }

        LONG lCurPos = EditGetCharFromPos(p);

        if (GetStartSelectionPos() != -1 &&
            GetEndSelectionPos() != -1 &&
            lCurPos != GetLastSelectionPos()) {

            CHARRANGE cr;

            SetLastSelectionPos(lCurPos);

            cr.cpMin = GetStartSelectionPos();
            cr.cpMax = GetLastSelectionPos();

            EditSetSel(cr);
        }

        if (bScrollLeft == TRUE || bScrollRight == TRUE) {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;

            VERIFY(::GetScrollInfo(GetHWnd(), SB_HORZ, &si));
            if (bScrollLeft == TRUE) {
                si.nPos = si.nPos - si.nPage / 2;
                si.nPos = max(si.nMin, si.nPos);
            } else if (bScrollRight == TRUE) {
                si.nPos = si.nPos + si.nPage / 2;
                si.nPos = min(si.nPos, si.nMax);
            }
            /*
             * Okay to use 16-bit position since RichEdit control adjusts
             * its scrollbars so that their range is always 16-bit.
             */
            DASSERT(abs(si.nPos) < 0x8000);
            SendMessage(WM_HSCROLL,
                        MAKEWPARAM(SB_THUMBPOSITION, LOWORD(si.nPos)));
        }
        if (bScrollUp == TRUE) {
            SendMessage(EM_LINESCROLL, 0, -1);
        } else if (bScrollDown == TRUE) {
            SendMessage(EM_LINESCROLL, 0, 1);
        }
        delete msg;
        return mrConsume;
    } else if (msg->message == WM_RBUTTONUP ||
               (msg->message == WM_SYSKEYDOWN && msg->wParam == VK_F10 &&
                HIBYTE(::GetKeyState(VK_SHIFT)))) {
        POINT p;
        if (msg->message == WM_RBUTTONUP) {
            VERIFY(::GetCursorPos(&p));
        } else {
            p.x = -1;
            p.y = -1;
        }
        if (!::PostMessage(GetHWnd(), WM_CONTEXTMENU, (WPARAM)GetHWnd(),
                           MAKELPARAM(p.x, p.y))) {
            JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);
            JNU_ThrowInternalError(env, "Message not posted, native event queue may be full.");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    } else if (msg->message == WM_MOUSEWHEEL) {
        // 4417236: If there is an old version of RichEd32.dll which
        // does not provide the mouse wheel scrolling we have to
        // interpret WM_MOUSEWHEEL as a sequence of scroll messages.
        // kdm@sparc.spb.su
        UINT platfScrollLines = 3;
        // Retrieve a number of scroll lines.
        ::SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0,
                               &platfScrollLines, 0);

        if (platfScrollLines > 0) {
            HWND hWnd = GetHWnd();
            LONG styles = ::GetWindowLong(hWnd, GWL_STYLE);

            RECT rect;
            // rect.left and rect.top are zero.
            // rect.right and rect.bottom contain the width and height
            VERIFY(::GetClientRect(hWnd, &rect));

            // calculate a number of visible lines
            TEXTMETRIC tm;
            HDC hDC = ::GetDC(hWnd);
            DASSERT(hDC != NULL);
            VERIFY(::GetTextMetrics(hDC, &tm));
            VERIFY(::ReleaseDC(hWnd, hDC));
            LONG visibleLines = rect.bottom / tm.tmHeight + 1;

            LONG lineCount = static_cast<LONG>(::SendMessage(hWnd,
                EM_GETLINECOUNT, 0, 0));
            BOOL sb_vert_disabled = (styles & WS_VSCROLL) == 0
              || (lineCount <= visibleLines);

            LONG *delta_accum = &m_lVDeltaAccum;
            UINT wm_msg = WM_VSCROLL;
            int sb_type = SB_VERT;

            if (sb_vert_disabled && (styles & WS_HSCROLL)) {
                delta_accum = &m_lHDeltaAccum;
                wm_msg = WM_HSCROLL;
                sb_type = SB_HORZ;
            }

            int delta = (short) HIWORD(msg->wParam);
            *delta_accum += delta;
            if (abs(*delta_accum) >= WHEEL_DELTA) {
                if (platfScrollLines == WHEEL_PAGESCROLL) {
                    // Synthesize a page down or a page up message.
                    ::SendMessage(hWnd, wm_msg,
                                  (delta > 0) ? SB_PAGEUP : SB_PAGEDOWN, 0);
                    *delta_accum = 0;
                } else {
                    // We provide a friendly behavior of text scrolling.
                    // During of scrolling the text can be out of the client
                    // area's boundary. Here we try to prevent this behavior.
                    SCROLLINFO si;
                    ::ZeroMemory(&si, sizeof(si));
                    si.cbSize = sizeof(SCROLLINFO);
                    si.fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
                    int actualScrollLines =
                        labs(platfScrollLines * (*delta_accum / WHEEL_DELTA));
                    for (int i = 0; i < actualScrollLines; i++) {
                        if (::GetScrollInfo(hWnd, sb_type, &si)) {
                            if ((wm_msg == WM_VSCROLL)
                                && ((*delta_accum < 0
                                     && si.nPos >= (si.nMax - (int) si.nPage))
                                    || (*delta_accum > 0
                                        && si.nPos <= si.nMin))) {
                                break;
                            }
                        }
                        // Here we don't send EM_LINESCROLL or EM_SCROLL
                        // messages to rich edit because it doesn't
                        // provide horizontal scrolling.
                        // So it's only possible to scroll by 1 line
                        // at a time to prevent scrolling when the
                        // scrollbar thumb reaches its boundary position.
                        ::SendMessage(hWnd, wm_msg,
                            (*delta_accum>0) ? SB_LINEUP : SB_LINEDOWN, 0);
                    }
                    *delta_accum %= WHEEL_DELTA;
                }
            } else {
                *delta_accum = 0;
            }
        }
        delete msg;
        return mrConsume;
        // 4417236: end of fix
    }

    /*
     * Store the 'synthetic' parameter so that the WM_PASTE security check
     * happens only for synthetic events.
     */
    m_synthetic = synthetic;
    returnVal = AwtComponent::HandleEvent(msg, synthetic);
    m_synthetic = FALSE;

    return returnVal;
}

int AwtTextArea::GetText(LPTSTR buffer, int size)
{
    // Due to a known limitation of the MSLU, GetWindowText cannot be
    // issued for the Unicode RichEdit control on Win9x. Use EM_GETTEXTEX instead.
    if (sm_RichEdit20 && !IS_NT) {
        GETTEXTEX gte;
        gte.cb            = size * sizeof(TCHAR);
        gte.flags         = GT_USECRLF;
        gte.codepage      = 1200; // implies Unicode
        gte.lpDefaultChar = NULL;
        gte.lpUsedDefChar = NULL;
        return (int)SendMessage(EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buffer);
    } else {
        return ::GetWindowText(GetHWnd(), buffer, size);
    }
}

/*
 * WM_CTLCOLOR is not sent by rich edit controls.
 * Use EM_SETCHARFORMAT and EM_SETBKGNDCOLOR to set
 * respectively foreground and background color.
 */
void AwtTextArea::SetColor(COLORREF c) {
    AwtComponent::SetColor(c);

    CHARFORMAT cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;

    cf.crTextColor = ::IsWindowEnabled(GetHWnd()) ? GetColor() : ::GetSysColor(COLOR_3DSHADOW);

    /*
     * The documentation for EM_GETCHARFORMAT is not exactly
     * correct. It appears that wParam has the same meaning
     * as for EM_SETCHARFORMAT. Our task is to secure that
     * all the characters in the control have the required
     * formatting. That's why we use SCF_ALL.
     */
    VERIFY(SendMessage(EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf));
    VERIFY(SendMessage(EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf));
}

/*
 * In responce to EM_SETBKGNDCOLOR rich edit changes
 * its bg color and repaints itself so we don't need
 * to force repaint.
 */
void AwtTextArea::SetBackgroundColor(COLORREF c) {
    AwtComponent::SetBackgroundColor(c);
    SendMessage(EM_SETBKGNDCOLOR, (WPARAM)FALSE, (LPARAM)GetBackgroundColor());
}

/*
 * Disabled edit control has grayed foreground.
 * Disabled RichEdit 1.0 control has original foreground.
 * Thus we have to set grayed foreground manually.
 */
void AwtTextArea::Enable(BOOL bEnable)
{
    AwtComponent::Enable(bEnable);

    SetColor(GetColor());
}


/* Fix for 4776535, 4648702
 * If width is 0 or 1 Windows hides the horizontal scroll bar even
 * if the WS_HSCROLL style is set. It is a bug in Windows.
 * As a workaround we should set an initial width to 2.
 * kdm@sparc.spb.su
 */
void AwtTextArea::Reshape(int x, int y, int w, int h)
{
    if (w < 2) {
        w = 2;
    }
    AwtTextComponent::Reshape(x, y, w, h);
}

LONG AwtTextArea::getJavaSelPos(LONG orgPos)
{
    long wlen;
    long pos = orgPos;
    long cur = 0;
    long retval = 0;
    LPTSTR wbuf;

    if ((wlen = GetTextLength()) == 0)
        return 0;
    wbuf = new TCHAR[wlen + 1];
    GetText(wbuf, wlen + 1);
    if (m_isLFonly == TRUE) {
        wlen = RemoveCR(wbuf);
    }

    while (cur < pos && cur < wlen) {
        if (wbuf[cur] == _T('\r') && wbuf[cur + 1] == _T('\n')) {
            pos++;
        }
        cur++;
        retval++;
    }
    delete[] wbuf;
    return retval;
}

LONG AwtTextArea::getWin32SelPos(LONG orgPos)
{
    if (GetTextLength() == 0)
       return 0;
    return orgPos;
}

void AwtTextArea::SetSelRange(LONG start, LONG end)
{
    CHARRANGE cr;
    cr.cpMin = getWin32SelPos(start);
    cr.cpMax = getWin32SelPos(end);
    EditSetSel(cr);
}


void AwtTextArea::_ReplaceText(void *param)
{
    JNIEnv *env = (JNIEnv *)JNU_GetEnv(jvm, JNI_VERSION_1_2);

    ReplaceTextStruct *rts = (ReplaceTextStruct *)param;

    jobject textComponent = rts->textComponent;
    jstring text = rts->text;
    jint start = rts->start;
    jint end = rts->end;

    AwtTextComponent *c = NULL;

    PDATA pData;
    JNI_CHECK_PEER_GOTO(textComponent, done);
    JNI_CHECK_NULL_GOTO(text, "null string", done);

    c = (AwtTextComponent *)pData;
    if (::IsWindow(c->GetHWnd()))
    {
      jsize length = env->GetStringLength(text) + 1;
      // Bugid 4141477 - Can't use TO_WSTRING here because it uses alloca
      // WCHAR* buffer = TO_WSTRING(text);
      WCHAR *buffer = new WCHAR[length];
      env->GetStringRegion(text, 0, length-1, buffer);
      buffer[length-1] = '\0';

      c->CheckLineSeparator(buffer);
      c->RemoveCR(buffer);
      // Fix for 5003402: added restoring/hiding selection to enable automatic scrolling
      LockWindowUpdate(c->GetHWnd());
      c->SendMessage(EM_HIDESELECTION, FALSE, TRUE);
      c->SendMessageW(EM_SETSEL, start, end);
      c->SendMessageW(EM_REPLACESEL, FALSE, (LPARAM)buffer);
      c->SendMessage(EM_HIDESELECTION, TRUE, TRUE);
      LockWindowUpdate(NULL);

      delete[] buffer;
    }

done:
    env->DeleteGlobalRef(textComponent);
    env->DeleteGlobalRef(text);

    delete rts;
}


/************************************************************************
 * TextArea native methods
 */

extern "C" {

/*
 * Class:     java_awt_TextArea
 * Method:    initIDs
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_java_awt_TextArea_initIDs(JNIEnv *env, jclass cls)
{
    TRY;

    AwtTextArea::scrollbarVisibilityID =
        env->GetFieldID(cls, "scrollbarVisibility", "I");

    DASSERT(AwtTextArea::scrollbarVisibilityID != NULL);

    CATCH_BAD_ALLOC;
}

} /* extern "C" */


/************************************************************************
 * WTextAreaPeer native methods
 */

extern "C" {

/*
 * Class:     sun_awt_windows_WTextAreaPeer
 * Method:    create
 * Signature: (Lsun/awt/windows/WComponentPeer;)V
 */
JNIEXPORT void JNICALL
Java_sun_awt_windows_WTextAreaPeer_create(JNIEnv *env, jobject self,
                                          jobject parent)
{
    TRY;

    PDATA pData;
    JNI_CHECK_PEER_RETURN(parent);
    AwtToolkit::CreateComponent(self, parent,
                                (AwtToolkit::ComponentFactory)
                                AwtTextArea::Create);
    JNI_CHECK_PEER_CREATION_RETURN(self);

    CATCH_BAD_ALLOC;
}

/*
 * Class:     sun_awt_windows_WTextAreaPeer
 * Method:    replaceText
 * Signature: (Ljava/lang/String;II)V
 */
JNIEXPORT void JNICALL
Java_sun_awt_windows_WTextAreaPeer_replaceText(JNIEnv *env, jobject self,
                                               jstring text,
                                               jint start, jint end)
{
    TRY;

    jobject selfGlobalRef = env->NewGlobalRef(self);
    jstring textGlobalRef = (jstring)env->NewGlobalRef(text);

    ReplaceTextStruct *rts = new ReplaceTextStruct;
    rts->textComponent = selfGlobalRef;
    rts->text = textGlobalRef;
    rts->start = start;
    rts->end = end;

    AwtToolkit::GetInstance().SyncCall(AwtTextArea::_ReplaceText, rts);
    // global refs and rts are deleted in _ReplaceText()

    CATCH_BAD_ALLOC;
}

/*
 * Class:     sun_awt_windows_WTextAreaPeer
 * Method:    insertText
 * Signature: (Ljava/lang/String;I)V
 */
JNIEXPORT void JNICALL
Java_sun_awt_windows_WTextAreaPeer_insertText(JNIEnv *env, jobject self,
                                              jstring text, jint pos)
{
    Java_sun_awt_windows_WTextAreaPeer_replaceText(env, self, text, pos, pos);
}

} /* extern "C" */


AwtTextArea::OleCallback AwtTextArea::sm_oleCallback;

/************************************************************************
 * Inner class OleCallback definition.
 */

AwtTextArea::OleCallback::OleCallback() {
    m_refs = 0;
    AddRef();
}

STDMETHODIMP
AwtTextArea::OleCallback::QueryInterface(REFIID riid, LPVOID * ppvObj) {

    TRY;

    if (::IsEqualIID(riid, IID_IUnknown)) {
        *ppvObj = (void __RPC_FAR *__RPC_FAR)(IUnknown*)this;
        AddRef();
        return S_OK;
    } else if (::IsEqualIID(riid, IID_IRichEditOleCallback)) {
        *ppvObj = (void __RPC_FAR *__RPC_FAR)(IRichEditOleCallback*)this;
        AddRef();
        return S_OK;
    } else {
        *ppvObj = NULL;
        return E_NOINTERFACE;
    }

    CATCH_BAD_ALLOC_RET(E_OUTOFMEMORY);
}

STDMETHODIMP_(ULONG)
AwtTextArea::OleCallback::AddRef() {
    return ++m_refs;
}

STDMETHODIMP_(ULONG)
AwtTextArea::OleCallback::Release() {
    int refs;

    if ((refs = --m_refs) == 0) delete this;

    return (ULONG)refs;
}

STDMETHODIMP
AwtTextArea::OleCallback::GetNewStorage(LPSTORAGE FAR * ppstg) {
    return E_NOTIMPL;
}

STDMETHODIMP
AwtTextArea::OleCallback::GetInPlaceContext(LPOLEINPLACEFRAME FAR * ppipframe,
                                                 LPOLEINPLACEUIWINDOW FAR* ppipuiDoc,
                                                 LPOLEINPLACEFRAMEINFO pipfinfo)
{
    return E_NOTIMPL;
}

STDMETHODIMP
AwtTextArea::OleCallback::ShowContainerUI(BOOL fShow) {
    return E_NOTIMPL;
}

STDMETHODIMP
AwtTextArea::OleCallback::QueryInsertObject(LPCLSID pclsid,
                                                 LPSTORAGE pstg,
                                                 LONG cp) {
    return NOERROR;
}

STDMETHODIMP
AwtTextArea::OleCallback::DeleteObject(LPOLEOBJECT poleobj) {
    return NOERROR;
}

STDMETHODIMP
AwtTextArea::OleCallback::QueryAcceptData(LPDATAOBJECT pdataobj,
                                               CLIPFORMAT *pcfFormat,
                                               DWORD reco,
                                               BOOL fReally,
                                               HGLOBAL hMetaPict) {
    if (reco == RECO_PASTE) {
        // If CF_TEXT format is available edit controls will select it,
        // otherwise if it is WinNT or Win2000 and CF_UNICODETEXT is
        // available it will be selected, otherwise if CF_OEMTEXT is
        // available it will be selected.
        if (::IsClipboardFormatAvailable(CF_TEXT)) {
            *pcfFormat = CF_TEXT;
        } else if (!m_isWin95 && ::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            *pcfFormat = CF_UNICODETEXT;
        } else if (::IsClipboardFormatAvailable(CF_OEMTEXT)) {
            *pcfFormat = CF_OEMTEXT;
        } else {
            // Don't allow rich edit to paste clipboard data
            // in other formats.
            *pcfFormat = CF_TEXT;
        }
    }

    return NOERROR;
}

STDMETHODIMP
AwtTextArea::OleCallback::ContextSensitiveHelp(BOOL fEnterMode) {
    return NOERROR;
}

STDMETHODIMP
AwtTextArea::OleCallback::GetClipboardData(CHARRANGE *pchrg,
                                                DWORD reco,
                                                LPDATAOBJECT *ppdataobj) {
    return E_NOTIMPL;
}

STDMETHODIMP
AwtTextArea::OleCallback::GetDragDropEffect(BOOL fDrag,
                                                 DWORD grfKeyState,
                                                 LPDWORD pdwEffect) {

    return E_NOTIMPL;
}


STDMETHODIMP
AwtTextArea::OleCallback::GetContextMenu(WORD seltype,
                                              LPOLEOBJECT lpoleobj,
                                              CHARRANGE FAR * lpchrg,
                                              HMENU FAR * lphmenu) {
    return E_NOTIMPL;
}
