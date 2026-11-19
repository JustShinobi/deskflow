/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsHook.h"
#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "deskflow/DisplayException.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

namespace {

struct HookEdgeConfig
{
  uint32_t monitorSides = 0;
  std::vector<deskflow::MonitorEdge> edges;
  std::vector<deskflow::MonitorRect> monitors;
};

constexpr int32_t kLocalEdgeWatchMargin = 32;

HookEdgeConfig s_edgeConfigs[2];
std::atomic<uint32_t> s_activeEdgeConfig{0};
std::atomic<bool> s_wasNearEdge{true};
std::atomic<int32_t> s_lastLocalX{0};
std::atomic<int32_t> s_lastLocalY{0};
std::atomic<bool> s_hasLastLocal{false};

std::atomic<int32_t> s_anchorX{0};
std::atomic<int32_t> s_anchorY{0};
std::atomic<int32_t> s_maxRelayDeltaX{640};
std::atomic<int32_t> s_maxRelayDeltaY{480};
std::atomic<int32_t> s_relayedDx{0};
std::atomic<int32_t> s_relayedDy{0};
std::atomic<bool> s_relayMovePosted{false};
std::atomic<bool> s_needsRecenter{false};

bool rectContains(const deskflow::MonitorRect &r, int32_t x, int32_t y)
{
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

int32_t edgeDepth(Direction side, const deskflow::MonitorRect &r, int32_t x, int32_t y)
{
  switch (side) {
  case Direction::Left:
    return x - r.x;
  case Direction::Right:
    return (r.x + r.width - 1) - x;
  case Direction::Top:
    return y - r.y;
  case Direction::Bottom:
    return (r.y + r.height - 1) - y;
  default:
    return INT32_MAX;
  }
}

} // namespace

static const char *g_name = "dfwhook";

static DWORD g_processID = 0;
static DWORD g_threadID = 0;
static HHOOK g_getMessage = nullptr;
static HHOOK g_keyboardLL = nullptr;
static HHOOK g_mouseLL = nullptr;
static bool g_screenSaver = false;
static std::atomic<EHookMode> g_mode{kHOOK_DISABLE};
static std::atomic<uint32_t> g_zoneSides{0};
static std::atomic<int32_t> g_zoneSize{0};
static std::atomic<int32_t> g_xComputer{0};
static std::atomic<int32_t> g_yComputer{0};
static std::atomic<int32_t> g_wComputer{0};
static std::atomic<int32_t> g_hComputer{0};
static WPARAM g_deadVirtKey = 0;
static WPARAM g_deadRelease = 0;
static LPARAM g_deadLParam = 0;
static BYTE g_deadKeyState[MSWindowsHook::kVirtualKeyCount] = {0};
static BYTE g_keyState[MSWindowsHook::kVirtualKeyCount] = {0};
static WCHAR g_lastChars[MSWindowsHook::kVirtualKeyCount] = {0};
static BYTE g_lastNoAltGr[MSWindowsHook::kVirtualKeyCount] = {0};
static bool g_keyStateValid = false;
static std::mutex g_keyStateMutex;
static DWORD g_hookThread = 0;
static bool g_fakeServerInput = false;
static BOOL g_isPrimary = TRUE;

MSWindowsHook::~MSWindowsHook()
{
  cleanup();

  if (g_processID == GetCurrentProcessId()) {
    uninstall();
    uninstallScreenSaver();
    g_processID = 0;
  }
}

void MSWindowsHook::loadLibrary()
{
  if (g_processID == 0) {
    g_processID = GetCurrentProcessId();
  }

  // initialize library
  if (init(GetCurrentThreadId()) == 0) {
    LOG_ERR("failed to init %s.dll, another program may be using it", g_name);
    LOG_INFO("restarting your computer may solve this error");
    throw DisplayOpenFailureException();
  }
}

int MSWindowsHook::init(DWORD threadID)
{
  // try to open process that last called init() to see if it's
  // still running or if it died without cleaning up.
  if (g_processID != 0 && g_processID != GetCurrentProcessId()) {
    HANDLE process = OpenProcess(STANDARD_RIGHTS_REQUIRED, FALSE, g_processID);
    if (process != nullptr) {
      // old process (probably) still exists so refuse to
      // reinitialize this DLL (and thus steal it from the
      // old process).
      int result = CloseHandle(process);
      if (result == false) {
        return 0;
      }
    }

    // clean up after old process.  the system should've already
    // removed the hooks so we just need to reset our state.
    g_processID = GetCurrentProcessId();
    g_threadID = 0;
    g_getMessage = nullptr;
    g_keyboardLL = nullptr;
    g_mouseLL = nullptr;
    g_screenSaver = false;
  }

  // save thread id.  we'll post messages to this thread's
  // message queue.
  g_threadID = threadID;

  // set defaults
  g_mode = kHOOK_DISABLE;
  g_zoneSides = 0;
  g_zoneSize = 0;
  g_xComputer = 0;
  g_yComputer = 0;
  g_wComputer = 0;
  g_hComputer = 0;

  return 1;
}

int MSWindowsHook::cleanup()
{
  if (g_processID == GetCurrentProcessId()) {
    g_threadID = 0;
  }

  return 1;
}

void MSWindowsHook::setSides(uint32_t sides)
{
  g_zoneSides.store(sides, std::memory_order_release);
  s_wasNearEdge.store(true, std::memory_order_release);
}

uint32_t MSWindowsHook::getSides()
{
  return g_zoneSides.load(std::memory_order_acquire);
}

void MSWindowsHook::setZone(int32_t x, int32_t y, int32_t w, int32_t h, int32_t jumpZoneSize)
{
  g_zoneSize.store(jumpZoneSize, std::memory_order_relaxed);
  g_xComputer.store(x, std::memory_order_relaxed);
  g_yComputer.store(y, std::memory_order_relaxed);
  g_wComputer.store(w, std::memory_order_relaxed);
  g_hComputer.store(h, std::memory_order_release);
  s_wasNearEdge.store(true, std::memory_order_release);
}

void MSWindowsHook::setMode(EHookMode mode)
{
  if (mode == g_mode.load(std::memory_order_acquire)) {
    return;
  }
  if (mode == kHOOK_RELAY_EVENTS) {
    s_relayedDx.store(0, std::memory_order_relaxed);
    s_relayedDy.store(0, std::memory_order_relaxed);
    s_relayMovePosted.store(false, std::memory_order_relaxed);
    s_needsRecenter.store(false, std::memory_order_relaxed);
  } else if (mode == kHOOK_WATCH_JUMP_ZONE) {
    POINT pt{};
    if (GetCursorPos(&pt)) {
      s_lastLocalX.store(pt.x, std::memory_order_relaxed);
      s_lastLocalY.store(pt.y, std::memory_order_relaxed);
      s_hasLastLocal.store(true, std::memory_order_relaxed);
    }
    s_wasNearEdge.store(true, std::memory_order_relaxed);
  }
  g_mode.store(mode, std::memory_order_release);
}

void MSWindowsHook::setMonitorEdges(uint32_t sides, const std::vector<deskflow::MonitorEdge> &edges)
{
  const uint32_t nextIdx = s_activeEdgeConfig.load(std::memory_order_relaxed) ^ 1u;
  HookEdgeConfig &cfg = s_edgeConfigs[nextIdx];
  cfg.monitorSides = sides;
  cfg.edges = edges;
  cfg.monitors = edges.empty() ? std::vector<deskflow::MonitorRect>{} : edges.front().monitors();
  s_activeEdgeConfig.store(nextIdx, std::memory_order_release);
  s_wasNearEdge.store(true, std::memory_order_release);
}

void MSWindowsHook::setRelayAnchor(int32_t centerX, int32_t centerY, int32_t maxDeltaX, int32_t maxDeltaY)
{
  s_anchorX.store(centerX, std::memory_order_relaxed);
  s_anchorY.store(centerY, std::memory_order_relaxed);
  s_maxRelayDeltaX.store(std::max<int32_t>(maxDeltaX, 64), std::memory_order_relaxed);
  s_maxRelayDeltaY.store(std::max<int32_t>(maxDeltaY, 64), std::memory_order_release);
  s_relayedDx.store(0, std::memory_order_relaxed);
  s_relayedDy.store(0, std::memory_order_relaxed);
  s_needsRecenter.store(false, std::memory_order_release);
}

bool MSWindowsHook::isLowLevel()
{
  return g_mouseLL != nullptr;
}

bool MSWindowsHook::consumeRelayedMotion(int32_t &dx, int32_t &dy, bool &needsRecenter)
{
  s_relayMovePosted.store(false, std::memory_order_release);
  dx = s_relayedDx.exchange(0, std::memory_order_acq_rel);
  dy = s_relayedDy.exchange(0, std::memory_order_acq_rel);
  needsRecenter = s_needsRecenter.exchange(false, std::memory_order_acq_rel);
  return dx != 0 || dy != 0 || needsRecenter;
}

bool MSWindowsHook::getPhysicalKeyState(BYTE keys[MSWindowsHook::kVirtualKeyCount])
{
  std::lock_guard<std::mutex> lock(g_keyStateMutex);
  if (g_keyStateValid) {
    std::memcpy(keys, g_keyState, sizeof(g_keyState));
  }
  return g_keyStateValid;
}

static void keyboardGetState(BYTE keys[MSWindowsHook::kVirtualKeyCount], DWORD vkCode, bool kf_up)
{
  // we have to use GetAsyncKeyState() rather than GetKeyState() because
  // we don't pass through most keys so the event synchronous state
  // doesn't get updated.  we do that because certain modifier keys have
  // side effects, like alt and the windows key.
  if (vkCode < 0 || vkCode >= MSWindowsHook::kVirtualKeyCount) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_keyStateMutex);

  // Keep track of key state on our own in case GetAsyncKeyState() fails
  g_keyState[vkCode] = kf_up ? 0 : 0x80;
  g_keyState[VK_SHIFT] = g_keyState[VK_LSHIFT] | g_keyState[VK_RSHIFT];

  SHORT key;
  // Test whether GetAsyncKeyState() is being honest with us
  key = GetAsyncKeyState(vkCode);

  if (key < 0) {
    // The only time we know for sure that GetAsyncKeyState() is working
    // is when it tells us that the current key is down.
    // In this case, update g_keyState to reflect what GetAsyncKeyState()
    // is telling us, just in case we have gotten out of sync

    for (int i = 0; i < MSWindowsHook::kVirtualKeyCount; ++i) {
      key = GetAsyncKeyState(i);
      g_keyState[i] = (BYTE)((key < 0) ? 0x80u : 0);
    }
  }

  // copy g_keyState to keys
  for (int i = 0; i < MSWindowsHook::kVirtualKeyCount; ++i) {
    keys[i] = g_keyState[i];
  }

  key = GetKeyState(VK_CAPITAL);
  keys[VK_CAPITAL] = (BYTE)(((key < 0) ? 0x80 : 0) | (key & 1));
  g_keyStateValid = true;
}

static WPARAM makeKeyMsg(UINT virtKey, WCHAR wc, bool noAltGr)
{
  return MAKEWPARAM((WORD)wc, MAKEWORD(virtKey & 0xff, noAltGr ? 1 : 0));
}

static void setDeadKey(WCHAR wc[], int size, UINT flags)
{
  if (g_deadVirtKey != 0) {
    auto virtualKey = static_cast<UINT>(g_deadVirtKey);
    auto scanCode = static_cast<UINT>((g_deadLParam & 0x10ff0000u) >> 16);
    if (ToUnicode(virtualKey, scanCode, g_deadKeyState, wc, size, flags) >= 2) {
      // If ToUnicode returned >=2, it means that we accidentally removed
      // a double dead key instead of restoring it. Thus, we call
      // ToUnicode again with the same parameters to restore the
      // internal dead key state.
      ToUnicode(virtualKey, scanCode, g_deadKeyState, wc, size, flags);

      // We need to keep track of this because g_deadVirtKey will be
      // cleared later on; this would cause the dead key release to
      // incorrectly restore the dead key state.
      g_deadRelease = g_deadVirtKey;
    }
  }
}

static WPARAM mapKeyDown(
    WPARAM wParam, LPARAM lParam, BYTE (&keys)[MSWindowsHook::kVirtualKeyCount], UINT control, UINT menu, UINT flags
)
{
  DWORD vkCode = static_cast<DWORD>(wParam);

  // map the key event to a character.  we have to put the dead
  // key back first and this has the side effect of removing it.
  WCHAR wc[] = {0, 0};
  setDeadKey(wc, 2, flags);

  UINT scanCode = ((lParam & 0x10ff0000u) >> 16);
  int n = ToUnicode((UINT)wParam, scanCode, keys, wc, 2, flags);

  // if mapping failed and ctrl and alt are pressed then try again
  // with both not pressed.  this handles the case where ctrl and
  // alt are being used as individual modifiers rather than AltGr.
  // we note that's the case in the message sent back to deskflow
  // because there's no simple way to deduce it after the fact.
  // we have to put the dead key back first, if there was one.
  bool noAltGr = false;
  if (n == 0 && (control & 0x80) != 0 && (menu & 0x80) != 0) {
    noAltGr = true;
    if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
      PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, wParam | 0x05000000, lParam);
    }
    setDeadKey(wc, 2, flags);

    BYTE keys2[MSWindowsHook::kVirtualKeyCount];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      keys2[i] = keys[i];
    }
    keys2[VK_LCONTROL] = 0;
    keys2[VK_RCONTROL] = 0;
    keys2[VK_CONTROL] = 0;
    keys2[VK_LMENU] = 0;
    keys2[VK_RMENU] = 0;
    keys2[VK_MENU] = 0;
    n = ToUnicode((UINT)wParam, scanCode, keys2, wc, 2, flags);
  }

  if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
    PostThreadMessage(
        g_threadID, DESKFLOW_MSG_DEBUG, (wc[0] & 0xffff) | ((wParam & 0xff) << 16) | ((n & 0xf) << 24) | 0x60000000,
        lParam
    );
  }
  WPARAM charAndVirtKey = 0;
  bool clearDeadKey = false;
  switch (n) {
  default:
    // key is a dead key
    g_deadVirtKey = wParam;
    g_deadLParam = lParam;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      g_deadKeyState[i] = keys[i];
    }
    break;

  case 0:
    // key doesn't map to a character.  this can happen if
    // non-character keys are pressed after a dead key.
    charAndVirtKey = makeKeyMsg((UINT)wParam, 0, noAltGr);
    if (vkCode < MSWindowsHook::kVirtualKeyCount) {
      g_lastChars[vkCode] = 0;
      g_lastNoAltGr[vkCode] = 0;
    }
    break;

  case 1:
    // key maps to a character composed with dead key
    charAndVirtKey = makeKeyMsg((UINT)wParam, wc[0], noAltGr);
    clearDeadKey = true;

    // the dead key is cleared below, so its release would otherwise go through as a normal key-up
    if (g_deadVirtKey != 0) {
      g_deadRelease = g_deadVirtKey;
    }
    if (vkCode < MSWindowsHook::kVirtualKeyCount) {
      g_lastChars[vkCode] = wc[0];
      g_lastNoAltGr[vkCode] = noAltGr ? 1 : 0;
    }
    break;

  case 2: {
    // previous dead key not composed.  send a fake key press
    // and release for the dead key to our window.
    WPARAM deadCharAndVirtKey = makeKeyMsg((UINT)g_deadVirtKey, wc[0], noAltGr);
    PostThreadMessage(g_threadID, DESKFLOW_MSG_KEY, deadCharAndVirtKey, g_deadLParam & 0x7fffffffu);
    PostThreadMessage(g_threadID, DESKFLOW_MSG_KEY, deadCharAndVirtKey, g_deadLParam | 0x80000000u);

    // use uncomposed character
    charAndVirtKey = makeKeyMsg((UINT)wParam, wc[1], noAltGr);
    clearDeadKey = true;
    if (vkCode < MSWindowsHook::kVirtualKeyCount) {
      g_lastChars[vkCode] = wc[1];
      g_lastNoAltGr[vkCode] = noAltGr ? 1 : 0;
    }
    break;
  }
  }

  // clear out old dead key state
  if (clearDeadKey) {
    g_deadVirtKey = 0;
    g_deadLParam = 0;
  }

  return charAndVirtKey;
}

static bool keyboardHookHandler(WPARAM wParam, LPARAM lParam)
{
  DWORD vkCode = static_cast<DWORD>(wParam);
  bool kf_up = (lParam & (KF_UP << 16)) != 0;

  // check for special events indicating if we should start or stop
  // passing events through and not report them to the server.  this
  // is used to allow the server to synthesize events locally but
  // not pick them up as user events.
  if (wParam == DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY && ((lParam >> 16) & 0xffu) == DESKFLOW_HOOK_FAKE_INPUT_SCANCODE) {
    // update flag
    g_fakeServerInput = ((lParam & 0x80000000u) == 0);
    if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
      PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, 0xff000000u | wParam, lParam);
    }

    // discard event
    return true;
  }

  // if we're expecting fake input then just pass the event through
  // and do not forward to the server
  if (g_fakeServerInput) {
    if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
      PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, 0xfe000000u | wParam, lParam);
    }
    return false;
  }

  // VK_RSHIFT may be sent with an extended scan code but right shift
  // is not an extended key so we reset that bit.
  if (wParam == VK_RSHIFT) {
    lParam &= ~0x01000000u;
  }

  // tell server about event
  if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
    PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, wParam, lParam);
  }

  // we need the keyboard state for ToAscii()
  BYTE keys[MSWindowsHook::kVirtualKeyCount];
  keyboardGetState(keys, vkCode, kf_up);

  // Update the physical state before ignoring a dead key release so it cannot remain down in the resync snapshot.
  if ((g_deadVirtKey == wParam || g_deadRelease == wParam) && (lParam & 0x80000000u) != 0) {
    g_deadRelease = 0;
    if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
      PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, wParam | 0x04000000, lParam);
    }
    return false;
  }

  // ToAscii() maps ctrl+letter to the corresponding control code
  // and ctrl+backspace to delete.  we don't want those translations
  // so clear the control modifier state.  however, if we want to
  // simulate AltGr (which is ctrl+alt) then we must not clear it.
  UINT control = keys[VK_CONTROL] | keys[VK_LCONTROL] | keys[VK_RCONTROL];
  UINT menu = keys[VK_MENU] | keys[VK_LMENU] | keys[VK_RMENU];
  if ((control & 0x80) == 0 || (menu & 0x80) == 0) {
    keys[VK_LCONTROL] = 0;
    keys[VK_RCONTROL] = 0;
    keys[VK_CONTROL] = 0;
  } else {
    keys[VK_LCONTROL] = 0x80;
    keys[VK_RCONTROL] = 0x80;
    keys[VK_CONTROL] = 0x80;
    keys[VK_LMENU] = 0x80;
    keys[VK_RMENU] = 0x80;
    keys[VK_MENU] = 0x80;
  }

  // ToAscii() needs to know if a menu is active for some reason.
  // we don't know and there doesn't appear to be any way to find
  // out.  so we'll just assume a menu is active if the menu key
  // is down.
  // FIXME -- figure out some way to check if a menu is active
  UINT flags = 0;
  if ((menu & 0x80) != 0)
    flags |= 1;

  // if we're on the server computer then just pass numpad keys with alt
  // key down as-is.  we won't pick up the resulting character but the
  // local app will.  if on a client computer then grab keys as usual;
  // if the client is a windows system it'll synthesize the expected
  // character.  if not then it'll probably just do nothing.
  if (g_mode != kHOOK_RELAY_EVENTS) {
    // we don't use virtual keys because we don't know what the
    // state of the numlock key is.  we'll hard code the scan codes
    // instead.  hopefully this works across all keyboards.
    UINT sc = (lParam & 0x01ff0000u) >> 16;
    if (menu && (sc >= 0x47u && sc <= 0x52u && sc != 0x4au && sc != 0x4eu)) {
      return false;
    }

    // We're on the server's own screen: we're only watching for the
    // cursor to reach a jump zone, we are NOT relaying keystrokes to a
    // client, so we have no need for the composed character here.
    PostThreadMessage(g_threadID, DESKFLOW_MSG_KEY, makeKeyMsg((UINT)wParam, 0, false), lParam);
    return false;
  }

  // calling ToUnicode on key-up flushes a pending dead key as a spacing character
  WPARAM charAndVirtKey = 0;
  if (kf_up) {
    WCHAR prev = (vkCode < MSWindowsHook::kVirtualKeyCount) ? g_lastChars[vkCode] : 0;
    bool noAltGr = (vkCode < MSWindowsHook::kVirtualKeyCount) && (g_lastNoAltGr[vkCode] != 0);
    charAndVirtKey = makeKeyMsg((UINT)wParam, prev, noAltGr);
  } else {
    charAndVirtKey = mapKeyDown(wParam, lParam, keys, control, menu, flags);
  }

  // forward message to our window.  do this whether or not we're
  // forwarding events to clients because this'll keep our thread's
  // key state table up to date.  that's important for querying
  // the scroll lock toggle state.
  // XXX -- with hot keys for actions we may only need to do this when
  // forwarding.
  if (charAndVirtKey != 0) {
    if (CLOG->getFilter() >= LogLevel::Level::Verbose) {
      PostThreadMessage(g_threadID, DESKFLOW_MSG_DEBUG, charAndVirtKey | 0x07000000, lParam);
    }
    PostThreadMessage(g_threadID, DESKFLOW_MSG_KEY, charAndVirtKey, lParam);
  }

  if (g_mode == kHOOK_RELAY_EVENTS) {
    // let certain keys pass through
    switch (wParam) {
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
      // pass event on.  we want to let these through to
      // the window proc because otherwise the keyboard
      // lights may not stay synchronized.
      break;

    case VK_HANGUL:
      // pass these modifiers if using a low level hook, discard
      // them if not.
      if (g_hookThread == 0) {
        return true;
      }
      break;

    default:
      // discard
      return true;
    }
  }

  return false;
}

#if !NO_GRAB_KEYBOARD
static LRESULT CALLBACK keyboardLLHook(int code, WPARAM wParam, LPARAM lParam)
{
  if (code >= 0) {
    // decode the message
    KBDLLHOOKSTRUCT *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);

    bool const injected = info->flags & LLKHF_INJECTED;
    if (!g_isPrimary && injected) {
      return CallNextHookEx(g_keyboardLL, code, wParam, lParam);
    }

    WPARAM wParam = info->vkCode;
    LPARAM lParam = 1;                // repeat code
    lParam |= (info->scanCode << 16); // scan code
    if (info->flags & LLKHF_EXTENDED) {
      lParam |= (1lu << 24); // extended key
    }
    if (info->flags & LLKHF_ALTDOWN) {
      lParam |= (1lu << 29); // context code
    }
    if (info->flags & LLKHF_UP) {
      lParam |= (1lu << 31); // transition
    }
    // FIXME -- bit 30 should be set if key was already down but
    // we don't know that info.  as a result we'll never generate
    // key repeat events.

    // handle the message
    if (keyboardHookHandler(wParam, lParam)) {
      return 1;
    }
  }

  return CallNextHookEx(g_keyboardLL, code, wParam, lParam);
}
#endif

//
// low-level mouse hook -- this allows us to capture and handle mouse
// events very early.  the earlier the better.
//

static bool mouseHookHandler(WPARAM wParam, int32_t x, int32_t y, int32_t data)
{
  switch (wParam) {
  case WM_LBUTTONDOWN:
  case WM_MBUTTONDOWN:
  case WM_RBUTTONDOWN:
  case WM_XBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
  case WM_MBUTTONDBLCLK:
  case WM_RBUTTONDBLCLK:
  case WM_XBUTTONDBLCLK:
  case WM_LBUTTONUP:
  case WM_MBUTTONUP:
  case WM_RBUTTONUP:
  case WM_XBUTTONUP:
  case WM_NCLBUTTONDOWN:
  case WM_NCMBUTTONDOWN:
  case WM_NCRBUTTONDOWN:
  case WM_NCXBUTTONDOWN:
  case WM_NCLBUTTONDBLCLK:
  case WM_NCMBUTTONDBLCLK:
  case WM_NCRBUTTONDBLCLK:
  case WM_NCXBUTTONDBLCLK:
  case WM_NCLBUTTONUP:
  case WM_NCMBUTTONUP:
  case WM_NCRBUTTONUP:
  case WM_NCXBUTTONUP:
    // always relay the event.  eat it if relaying.
    PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_BUTTON, wParam, data);
    return (g_mode == kHOOK_RELAY_EVENTS);

  case WM_MOUSEWHEEL:
    if (g_mode == kHOOK_RELAY_EVENTS) {
      // relay event
      PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_WHEEL, data, 0);
    }
    return (g_mode == kHOOK_RELAY_EVENTS);

  case WM_MOUSEHWHEEL:
    if (g_mode == kHOOK_RELAY_EVENTS) {
      // relay event
      PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_WHEEL, 0, data);
    }
    return (g_mode == kHOOK_RELAY_EVENTS);

  case WM_NCMOUSEMOVE:
  case WM_MOUSEMOVE: {
    const EHookMode mode = g_mode.load(std::memory_order_acquire);
    if (mode == kHOOK_RELAY_EVENTS) {
      if (g_mouseLL != nullptr) {
        // Low-level hook eats the move before Windows shifts the OS cursor, so
        // (x, y) is always relative to the stationary anchor (m_xCenter, m_yCenter).
        // Accumulate deltas lock-free and post at most one notification per drain.
        const int32_t dx = x - s_anchorX.load(std::memory_order_relaxed);
        const int32_t dy = y - s_anchorY.load(std::memory_order_relaxed);
        if (dx == 0 && dy == 0) {
          return true;
        }
        if (std::abs(dx) > s_maxRelayDeltaX.load(std::memory_order_relaxed) ||
            std::abs(dy) > s_maxRelayDeltaY.load(std::memory_order_relaxed)) {
          s_needsRecenter.store(true, std::memory_order_release);
        } else {
          s_relayedDx.fetch_add(dx, std::memory_order_relaxed);
          s_relayedDy.fetch_add(dy, std::memory_order_relaxed);
        }
        if (!s_relayMovePosted.exchange(true, std::memory_order_acq_rel)) {
          PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_MOVE, 0, 0);
        }
        return true;
      }
      // Legacy non-LL hook fallback
      PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_MOVE, x, y);
      return true;
    } else if (mode == kHOOK_WATCH_JUMP_ZONE) {
      const int32_t rawX = x;
      const int32_t rawY = y;
      const int32_t xComp = g_xComputer.load(std::memory_order_relaxed);
      const int32_t yComp = g_yComputer.load(std::memory_order_relaxed);
      const int32_t wComp = g_wComputer.load(std::memory_order_relaxed);
      const int32_t hComp = g_hComputer.load(std::memory_order_relaxed);
      const int32_t zoneSize = g_zoneSize.load(std::memory_order_relaxed);
      const uint32_t zoneSides = g_zoneSides.load(std::memory_order_relaxed);

      bool bogus = false;
      if (x < xComp) {
        x = xComp;
        bogus = true;
      } else if (x >= xComp + wComp) {
        x = xComp + wComp - 1;
        bogus = true;
      }
      if (y < yComp) {
        y = yComp;
        bogus = true;
      } else if (y >= yComp + hComp) {
        y = yComp + hComp - 1;
        bogus = true;
      }

      bool inside = false;
      bool nearEdge = false;
      const HookEdgeConfig &cfg = s_edgeConfigs[s_activeEdgeConfig.load(std::memory_order_acquire)];
      const bool havePrev = s_hasLastLocal.load(std::memory_order_relaxed);
      const int32_t prevX = havePrev ? s_lastLocalX.load(std::memory_order_relaxed) : rawX;
      const int32_t prevY = havePrev ? s_lastLocalY.load(std::memory_order_relaxed) : rawY;

      for (const auto &edge : cfg.edges) {
        if ((zoneSides & (1u << static_cast<int>(edge.side()))) == 0) {
          continue;
        }
        const bool onEdgeMonitor = edge.contains(rawX, rawY) || edge.contains(prevX, prevY);
        if (!onEdgeMonitor) {
          continue;
        }
        if (edgeDepth(edge.side(), edge.monitor(), rawX, rawY) < zoneSize + kLocalEdgeWatchMargin) {
          nearEdge = true;
        }
        int32_t projectedX = x;
        int32_t projectedY = y;
        if (!inside && edge.project(projectedX, projectedY, zoneSize)) {
          inside = true;
          nearEdge = true;
          break;
        }
      }

      if (cfg.monitors.empty()) {
        s_lastLocalX.store(x, std::memory_order_relaxed);
        s_lastLocalY.store(y, std::memory_order_relaxed);
        s_hasLastLocal.store(true, std::memory_order_relaxed);
      } else {
        for (const auto &mon : cfg.monitors) {
          if (rectContains(mon, rawX, rawY)) {
            s_lastLocalX.store(rawX, std::memory_order_relaxed);
            s_lastLocalY.store(rawY, std::memory_order_relaxed);
            s_hasLastLocal.store(true, std::memory_order_relaxed);
            break;
          }
        }
      }

      const uint32_t desktopSides = zoneSides & ~cfg.monitorSides;
      using enum DirectionMask;
      const int32_t watchMargin = zoneSize + kLocalEdgeWatchMargin;
      if ((desktopSides & static_cast<uint32_t>(LeftMask)) != 0) {
        if (x < xComp + watchMargin) {
          nearEdge = true;
        }
        if (!inside && x < xComp + zoneSize) {
          inside = true;
        }
      }
      if ((desktopSides & static_cast<uint32_t>(RightMask)) != 0) {
        if (x >= xComp + wComp - watchMargin) {
          nearEdge = true;
        }
        if (!inside && x >= xComp + wComp - zoneSize) {
          inside = true;
        }
      }
      if ((desktopSides & static_cast<uint32_t>(TopMask)) != 0) {
        if (y < yComp + watchMargin) {
          nearEdge = true;
        }
        if (!inside && y < yComp + zoneSize) {
          inside = true;
        }
      }
      if ((desktopSides & static_cast<uint32_t>(BottomMask)) != 0) {
        if (y >= yComp + hComp - watchMargin) {
          nearEdge = true;
        }
        if (!inside && y >= yComp + hComp - zoneSize) {
          inside = true;
        }
      }

      if (inside || nearEdge) {
        s_wasNearEdge.store(true, std::memory_order_relaxed);
        PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_MOVE, x, y);
      } else if (s_wasNearEdge.exchange(false, std::memory_order_relaxed)) {
        // Post one exit sample so Server::noSwitch() cancels switchDelay / arms double-tap.
        PostThreadMessage(g_threadID, DESKFLOW_MSG_MOUSE_MOVE, x, y);
      }

      return inside && !bogus;
    }
    break;
  }

    // pass the event
    return false;
  }

  static LRESULT CALLBACK mouseLLHook(int code, WPARAM wParam, LPARAM lParam)
  {
    if (code >= 0) {
      // decode the message
      MSLLHOOKSTRUCT *info = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);

      bool const injected = info->flags & LLMHF_INJECTED;
      if (!g_isPrimary && injected) {
        return CallNextHookEx(g_mouseLL, code, wParam, lParam);
      }

      int32_t x = static_cast<int32_t>(info->pt.x);
      int32_t y = static_cast<int32_t>(info->pt.y);
      int32_t w = static_cast<int16_t>(HIWORD(info->mouseData));

      // handle the message
      if (mouseHookHandler(wParam, x, y, w)) {
        return 1;
      }
    }

    return CallNextHookEx(g_mouseLL, code, wParam, lParam);
  }

  EHookResult MSWindowsHook::install()
  {
    assert(g_getMessage == nullptr || g_screenSaver);

    // must be initialized
    if (g_threadID == 0) {
      return kHOOK_FAILED;
    }

    // discard old dead keys
    g_deadVirtKey = 0;
    g_deadLParam = 0;
    g_deadRelease = 0;
    std::memset(g_lastChars, 0, sizeof(g_lastChars));
    std::memset(g_lastNoAltGr, 0, sizeof(g_lastNoAltGr));

    // reset fake input flag
    g_fakeServerInput = false;

    // install low-level hooks.  we require that they both get installed.
    g_mouseLL = SetWindowsHookEx(WH_MOUSE_LL, &mouseLLHook, nullptr, 0);
#if !NO_GRAB_KEYBOARD
    g_keyboardLL = SetWindowsHookEx(WH_KEYBOARD_LL, &keyboardLLHook, nullptr, 0);
    if (g_mouseLL == nullptr || g_keyboardLL == nullptr) {
      if (g_keyboardLL != nullptr) {
        UnhookWindowsHookEx(g_keyboardLL);
        g_keyboardLL = nullptr;
      }
      if (g_mouseLL != nullptr) {
        UnhookWindowsHookEx(g_mouseLL);
        g_mouseLL = nullptr;
      }
    }
#endif
    // clang-format off
  // check that we got all the hooks we wanted
  if (
      (g_mouseLL == nullptr) ||
#if !NO_GRAB_KEYBOARD
      (g_keyboardLL == nullptr)
#endif
  ) {
    uninstall();
    return kHOOK_FAILED;
  }
    // clang-format on
    if (g_keyboardLL != nullptr || g_mouseLL != nullptr) {
      g_hookThread = GetCurrentThreadId();
      return kHOOK_OKAY_LL;
    }

    return kHOOK_OKAY;
  }

  int MSWindowsHook::uninstall()
  {
    // discard old dead keys
    g_deadVirtKey = 0;
    g_deadLParam = 0;
    g_deadRelease = 0;
    std::memset(g_lastChars, 0, sizeof(g_lastChars));
    std::memset(g_lastNoAltGr, 0, sizeof(g_lastNoAltGr));

    // uninstall hooks
    if (g_keyboardLL != nullptr) {
      UnhookWindowsHookEx(g_keyboardLL);
      g_keyboardLL = nullptr;
    }
    if (g_mouseLL != nullptr) {
      UnhookWindowsHookEx(g_mouseLL);
      g_mouseLL = nullptr;
    }
    if (g_getMessage != nullptr && !g_screenSaver) {
      UnhookWindowsHookEx(g_getMessage);
      g_getMessage = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_keyStateMutex);
    std::memset(g_keyState, 0, sizeof(g_keyState));
    g_keyStateValid = false;

    return 1;
  }

  static LRESULT CALLBACK getMessageHook(int code, WPARAM wParam, LPARAM lParam)
  {
    if (code >= 0) {
      if (g_screenSaver) {
        MSG *msg = reinterpret_cast<MSG *>(lParam);
        if (msg->message == WM_SYSCOMMAND && msg->wParam == SC_SCREENSAVE) {
          // broadcast screen saver started message
          PostThreadMessage(g_threadID, DESKFLOW_MSG_SCREEN_SAVER, TRUE, 0);
        }
      }
    }

    return CallNextHookEx(g_getMessage, code, wParam, lParam);
  }

  int MSWindowsHook::installScreenSaver()
  {
    // must be initialized
    if (g_threadID == 0) {
      return 0;
    }

    // generate screen saver messages
    g_screenSaver = true;

    // install hook unless it's already installed
    if (g_getMessage == nullptr) {
      g_getMessage = SetWindowsHookEx(WH_GETMESSAGE, &getMessageHook, nullptr, 0);
    }

    return (g_getMessage != nullptr) ? 1 : 0;
  }

  int MSWindowsHook::uninstallScreenSaver()
  {
    // uninstall hook unless the mouse wheel hook is installed
    if (g_getMessage != nullptr) {
      UnhookWindowsHookEx(g_getMessage);
      g_getMessage = nullptr;
    }

    // screen saver hook is no longer installed
    g_screenSaver = false;

    return 1;
  }
