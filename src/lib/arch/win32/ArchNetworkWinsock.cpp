/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/win32/ArchNetworkWinsock.h"
#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "arch/IArchMultithread.h"
#include "arch/win32/ArchMultithreadWindows.h"
#include "arch/win32/XArchWindows.h"
#include "base/Log.h"

#include <atomic>
#include <cmath>
#include <malloc.h>
#include <mutex>

static const int s_family[] = {
    PF_UNSPEC,
    PF_INET,
    PF_INET6,
};
static const int s_type[] = {SOCK_DGRAM, SOCK_STREAM};

static SOCKET(PASCAL FAR *accept_winsock)(SOCKET s, struct sockaddr FAR *addr, int FAR *addrlen);
static int(PASCAL FAR *bind_winsock)(SOCKET s, const struct sockaddr FAR *addr, int namelen);
static int(PASCAL FAR *close_winsock)(SOCKET s);
static int(PASCAL FAR *connect_winsock)(SOCKET s, const struct sockaddr FAR *name, int namelen);
static int(PASCAL FAR *gethostname_winsock)(char FAR *name, int namelen);
static int(PASCAL FAR *getsockerror_winsock)(void);
static int(PASCAL FAR *getsockopt_winsock)(SOCKET s, int level, int optname, void FAR *optval, int FAR *optlen);
static u_short(PASCAL FAR *htons_winsock)(u_short v);
static char FAR *(PASCAL FAR *inet_ntoa_winsock)(struct in_addr in);
static unsigned long(PASCAL FAR *inet_addr_winsock)(const char FAR *cp);
static int(PASCAL FAR *ioctl_winsock)(SOCKET s, int cmd, void FAR *data);
static int(PASCAL FAR *listen_winsock)(SOCKET s, int backlog);
static u_short(PASCAL FAR *ntohs_winsock)(u_short v);
static int(PASCAL FAR *recv_winsock)(SOCKET s, void FAR *buf, int len, int flags);
static int(PASCAL FAR *recvfrom_winsock)(
    SOCKET s, void FAR *buf, int len, int flags, struct sockaddr FAR *from, int FAR *fromlen
);
static int(PASCAL FAR *select_winsock)(
    int nfds, fd_set FAR *readfds, fd_set FAR *writefds, fd_set FAR *exceptfds, const struct timeval FAR *timeout
);
static int(PASCAL FAR *send_winsock)(SOCKET s, const void FAR *buf, int len, int flags);
static int(PASCAL FAR *sendto_winsock)(
    SOCKET s, const void FAR *buf, int len, int flags, const struct sockaddr FAR *to, int tolen
);
static int(PASCAL FAR *setsockopt_winsock)(SOCKET s, int level, int optname, const void FAR *optval, int optlen);
static int(PASCAL FAR *shutdown_winsock)(SOCKET s, int how);
static SOCKET(PASCAL FAR *socket_winsock)(int af, int type, int protocol);
static struct hostent FAR *(PASCAL FAR *gethostbyaddr_winsock)(const char FAR *addr, int len, int type);
static struct hostent FAR *(PASCAL FAR *gethostbyname_winsock)(const char FAR *name);
static int(PASCAL FAR *WSACleanup_winsock)(void);
static int(PASCAL FAR *WSAFDIsSet_winsock)(SOCKET, fd_set FAR *fdset);
static WSAEVENT(PASCAL FAR *WSACreateEvent_winsock)(void);
static BOOL(PASCAL FAR *WSACloseEvent_winsock)(WSAEVENT);
static BOOL(PASCAL FAR *WSASetEvent_winsock)(WSAEVENT);
static BOOL(PASCAL FAR *WSAResetEvent_winsock)(WSAEVENT);
static int(PASCAL FAR *WSAEventSelect_winsock)(SOCKET, WSAEVENT, long);
static DWORD(PASCAL FAR *WSAWaitForMultipleEvents_winsock)(DWORD, const WSAEVENT FAR *, BOOL, DWORD, BOOL);
static int(PASCAL FAR *WSAEnumNetworkEvents_winsock)(SOCKET, WSAEVENT, LPWSANETWORKEVENTS);

#undef FD_ISSET
#define FD_ISSET(fd, set) WSAFDIsSet_winsock((SOCKET)(fd), (fd_set FAR *)(set))

#define setfunc(var, name, type) var = (type)netGetProcAddress(module, #name)

static HMODULE s_networkModule = nullptr;

namespace {

struct QosVersionWin32
{
  USHORT majorVersion;
  USHORT minorVersion;
};

using QosCreateHandleFn = BOOL(WINAPI *)(QosVersionWin32 *version, PHANDLE qosHandle);
using QosAddSocketToFlowFn =
    BOOL(WINAPI *)(HANDLE qosHandle, SOCKET socket, PSOCKADDR destAddr, int trafficType, DWORD flags, DWORD *flowId);
using QosRemoveSocketFromFlowFn = BOOL(WINAPI *)(HANDLE qosHandle, SOCKET socket, DWORD flowId, DWORD flags);
using QosSetFlowFn = BOOL(WINAPI *)(
    HANDLE qosHandle, DWORD flowId, int operation, ULONG size, PVOID buffer, DWORD flags, LPOVERLAPPED overlapped
);

constexpr int kQosTrafficTypeAudioVideo = 3;
constexpr int kQosTrafficTypeVoice = 4;
constexpr DWORD kQosNonAdaptiveFlow = 0x00000002;
constexpr int kQosSetOutgoingDscpValue = 2;
constexpr DWORD kDscpExpeditedForwarding = 46;

std::once_flag s_qosInitFlag;
HMODULE s_qwaveModule = nullptr;
HANDLE s_qosHandle = nullptr;
QosAddSocketToFlowFn s_qosAddSocketToFlow = nullptr;
QosRemoveSocketFromFlowFn s_qosRemoveSocketFromFlow = nullptr;
QosSetFlowFn s_qosSetFlow = nullptr;

void initQwaveOnce()
{
  s_qwaveModule = ::LoadLibraryW(L"qwave.dll");
  if (s_qwaveModule == nullptr) {
    return;
  }
  auto createHandle = reinterpret_cast<QosCreateHandleFn>(::GetProcAddress(s_qwaveModule, "QOSCreateHandle"));
  s_qosAddSocketToFlow = reinterpret_cast<QosAddSocketToFlowFn>(::GetProcAddress(s_qwaveModule, "QOSAddSocketToFlow"));
  s_qosRemoveSocketFromFlow =
      reinterpret_cast<QosRemoveSocketFromFlowFn>(::GetProcAddress(s_qwaveModule, "QOSRemoveSocketFromFlow"));
  s_qosSetFlow = reinterpret_cast<QosSetFlowFn>(::GetProcAddress(s_qwaveModule, "QOSSetFlow"));

  if (createHandle != nullptr && s_qosAddSocketToFlow != nullptr && s_qosRemoveSocketFromFlow != nullptr) {
    QosVersionWin32 ver{1, 0};
    if (!createHandle(&ver, &s_qosHandle)) {
      s_qosHandle = nullptr;
    }
  }
}

std::atomic<uint64_t> s_nextInstanceId{1};
thread_local WSAEVENT *tl_unblockEvent = nullptr;
thread_local uint64_t tl_unblockInstanceId = 0;

} // namespace

static FARPROC netGetProcAddress(HMODULE module, LPCSTR name)
{
  FARPROC func = ::GetProcAddress(module, name);
  if (!func) {
    throw ArchNetworkSupportException("");
  }
  return func;
}

ArchNetAddressImpl *ArchNetAddressImpl::alloc(size_t size)
{
  size_t totalSize = size + ADDR_HDR_SIZE;
  ArchNetAddressImpl *addr = (ArchNetAddressImpl *)malloc(totalSize);
  addr->m_len = (int)size;
  return addr;
}

//
// ArchNetworkWinsock
//

ArchNetworkWinsock::ArchNetworkWinsock() : m_instanceId(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
{
}

ArchNetworkWinsock::~ArchNetworkWinsock()
{
  if (tl_unblockInstanceId == m_instanceId) {
    tl_unblockEvent = nullptr;
    tl_unblockInstanceId = 0;
  }
  if (s_networkModule != nullptr) {
    WSACleanup_winsock();
    ::FreeLibrary(s_networkModule);

    WSACleanup_winsock = nullptr;
    s_networkModule = nullptr;
  }

  EventList::iterator it;
  for (it = m_unblockEvents.begin(); it != m_unblockEvents.end(); it++) {
    delete *it;
  }
}

void ArchNetworkWinsock::init()
{
  static const wchar_t *s_library[] = {L"ws2_32.dll"};

  assert(WSACleanup_winsock == nullptr);
  assert(s_networkModule == nullptr);

  // try each winsock library
  for (size_t i = 0; i < sizeof(s_library) / sizeof(s_library[0]); ++i) {
    try {
      initModule((HMODULE)::LoadLibrary(s_library[i]));
      return;
    } catch (ArchNetworkException &) {
      // ignore
    }
  }

  // can't initialize any library
  throw ArchNetworkSupportException("Cannot load winsock library");
}

void ArchNetworkWinsock::initModule(HMODULE module)
{
  if (module == nullptr) {
    throw ArchNetworkSupportException("");
  }

  // get startup function address
  int(PASCAL FAR * startup)(WORD, LPWSADATA);
  setfunc(startup, WSAStartup, int(PASCAL FAR *)(WORD, LPWSADATA));

  // startup network library
  WORD version = MAKEWORD(2 /*major*/, 2 /*minor*/);
  WSADATA data;
  int err = startup(version, &data);
  if (data.wVersion != version) {
    throw ArchNetworkSupportException(windowsErrorToString(err));
  }
  if (err != 0) {
    // some other initialization error
    throwError(err);
  }

  // get function addresses
  setfunc(accept_winsock, accept, SOCKET(PASCAL FAR *)(SOCKET s, struct sockaddr FAR * addr, int FAR *addrlen));
  setfunc(bind_winsock, bind, int(PASCAL FAR *)(SOCKET s, const struct sockaddr FAR *addr, int namelen));
  setfunc(close_winsock, closesocket, int(PASCAL FAR *)(SOCKET s));
  setfunc(connect_winsock, connect, int(PASCAL FAR *)(SOCKET s, const struct sockaddr FAR *name, int namelen));
  setfunc(gethostname_winsock, gethostname, int(PASCAL FAR *)(char FAR *name, int namelen));
  setfunc(getsockerror_winsock, WSAGetLastError, int(PASCAL FAR *)(void));
  setfunc(
      getsockopt_winsock, getsockopt,
      int(PASCAL FAR *)(SOCKET s, int level, int optname, void FAR *optval, int FAR *optlen)
  );
  setfunc(htons_winsock, htons, u_short(PASCAL FAR *)(u_short v));
  setfunc(inet_ntoa_winsock, inet_ntoa, char FAR *(PASCAL FAR *)(struct in_addr in));
  setfunc(inet_addr_winsock, inet_addr, unsigned long(PASCAL FAR *)(const char FAR *cp));
  setfunc(ioctl_winsock, ioctlsocket, int(PASCAL FAR *)(SOCKET s, int cmd, void FAR *));
  setfunc(listen_winsock, listen, int(PASCAL FAR *)(SOCKET s, int backlog));
  setfunc(ntohs_winsock, ntohs, u_short(PASCAL FAR *)(u_short v));
  setfunc(recv_winsock, recv, int(PASCAL FAR *)(SOCKET s, void FAR *buf, int len, int flags));
  setfunc(
      recvfrom_winsock, recvfrom,
      int(PASCAL FAR *)(SOCKET s, void FAR *buf, int len, int flags, struct sockaddr FAR *from, int FAR *fromlen)
  );
  setfunc(
      select_winsock, select,
      int(PASCAL FAR *)(
          int nfds, fd_set FAR *readfds, fd_set FAR *writefds, fd_set FAR *exceptfds, const struct timeval FAR *timeout
      )
  );
  setfunc(send_winsock, send, int(PASCAL FAR *)(SOCKET s, const void FAR *buf, int len, int flags));
  setfunc(
      sendto_winsock, sendto,
      int(PASCAL FAR *)(SOCKET s, const void FAR *buf, int len, int flags, const struct sockaddr FAR *to, int tolen)
  );
  setfunc(
      setsockopt_winsock, setsockopt,
      int(PASCAL FAR *)(SOCKET s, int level, int optname, const void FAR *optval, int optlen)
  );
  setfunc(shutdown_winsock, shutdown, int(PASCAL FAR *)(SOCKET s, int how));
  setfunc(socket_winsock, socket, SOCKET(PASCAL FAR *)(int af, int type, int protocol));
  setfunc(
      gethostbyaddr_winsock, gethostbyaddr, struct hostent FAR * (PASCAL FAR *)(const char FAR *addr, int len, int type)
  );
  setfunc(gethostbyname_winsock, gethostbyname, struct hostent FAR * (PASCAL FAR *)(const char FAR *name));
  setfunc(WSACleanup_winsock, WSACleanup, int(PASCAL FAR *)(void));
  setfunc(WSAFDIsSet_winsock, __WSAFDIsSet, int(PASCAL FAR *)(SOCKET, fd_set FAR *));
  setfunc(WSACreateEvent_winsock, WSACreateEvent, WSAEVENT(PASCAL FAR *)(void));
  setfunc(WSACloseEvent_winsock, WSACloseEvent, BOOL(PASCAL FAR *)(WSAEVENT));
  setfunc(WSASetEvent_winsock, WSASetEvent, BOOL(PASCAL FAR *)(WSAEVENT));
  setfunc(WSAResetEvent_winsock, WSAResetEvent, BOOL(PASCAL FAR *)(WSAEVENT));
  setfunc(WSAEventSelect_winsock, WSAEventSelect, int(PASCAL FAR *)(SOCKET, WSAEVENT, long));
  setfunc(
      WSAWaitForMultipleEvents_winsock, WSAWaitForMultipleEvents,
      DWORD(PASCAL FAR *)(DWORD, const WSAEVENT FAR *, BOOL, DWORD, BOOL)
  );
  setfunc(WSAEnumNetworkEvents_winsock, WSAEnumNetworkEvents, int(PASCAL FAR *)(SOCKET, WSAEVENT, LPWSANETWORKEVENTS));

  s_networkModule = module;
}

ArchSocket ArchNetworkWinsock::newSocket(AddressFamily family, SocketType type)
{
  // create socket
  SOCKET fd = socket_winsock(s_family[static_cast<int>(family)], s_type[static_cast<int>(type)], 0);
  if (fd == INVALID_SOCKET) {
    throwError(getsockerror_winsock());
  }
  try {
    setBlockingOnSocket(fd, false);
    if (family == AddressFamily::INet6) {
      BOOL flag = 0;
      int size = sizeof(flag);
      if (setsockopt_winsock(fd, IPPROTO_IPV6, IPV6_V6ONLY, &flag, size) == SOCKET_ERROR) {
        throwError(getsockerror_winsock());
      }
    }
    if (type == SocketType::DataGram) {
      // Ignore ICMP Port Unreachable resets on UDP sockets (SIO_UDP_CONNRESET = _WSAIOW(IOC_VENDOR, 12) = 0x9800000C).
      BOOL newBehavior = FALSE;
      ioctl_winsock(fd, static_cast<int>(0x9800000Cu), &newBehavior);
    }
  } catch (...) {
    close_winsock(fd);
    throw;
  }

  // allocate socket object
  ArchSocketImpl *socket = new ArchSocketImpl;
  socket->m_socket = fd;
  socket->m_refCount = 1;
  socket->m_event = WSACreateEvent_winsock();
  socket->m_pollWrite = true;
  return socket;
}

ArchSocket ArchNetworkWinsock::copySocket(ArchSocket s)
{
  assert(s != nullptr);

  // ref the socket and return it
  std::scoped_lock lock{m_mutex};
  ++s->m_refCount;
  return s;
}

void ArchNetworkWinsock::closeSocket(ArchSocket s)
{
  assert(s != nullptr);

  // unref the socket and note if it should be released
  bool doClose = false;
  {
    std::scoped_lock lock{m_mutex};
    doClose = (--s->m_refCount == 0);
  }

  // close the socket if necessary
  if (doClose) {
    if (s_qosHandle != nullptr && s_qosRemoveSocketFromFlow != nullptr) {
      for (DWORD flowId : s->m_qosFlowIds) {
        if (flowId != 0) {
          s_qosRemoveSocketFromFlow(s_qosHandle, s->m_socket, flowId, 0);
        }
      }
      s->m_qosFlowIds.clear();
    }
    if (close_winsock(s->m_socket) == SOCKET_ERROR) {
      // close failed.  restore the last ref and throw.
      int err = getsockerror_winsock();
      {
        std::scoped_lock lock{m_mutex};
        ++s->m_refCount;
      }
      throwError(err);
    }
    WSACloseEvent_winsock(s->m_event);
    delete s;
  }
}

void ArchNetworkWinsock::closeSocketForRead(ArchSocket s)
{
  assert(s != nullptr);

  if (shutdown_winsock(s->m_socket, SD_RECEIVE) == SOCKET_ERROR) {
    if (getsockerror_winsock() != WSAENOTCONN) {
      throwError(getsockerror_winsock());
    }
  }
}

void ArchNetworkWinsock::closeSocketForWrite(ArchSocket s)
{
  assert(s != nullptr);

  if (shutdown_winsock(s->m_socket, SD_SEND) == SOCKET_ERROR) {
    if (getsockerror_winsock() != WSAENOTCONN) {
      throwError(getsockerror_winsock());
    }
  }
}

void ArchNetworkWinsock::bindSocket(ArchSocket s, ArchNetAddress addr)
{
  assert(s != nullptr);
  assert(addr != nullptr);

  if (bind_winsock(s->m_socket, TYPED_ADDR(struct sockaddr, addr), addr->m_len) != ERROR_SUCCESS) {
    throwError(getsockerror_winsock());
  }
}

void ArchNetworkWinsock::listenOnSocket(ArchSocket s)
{
  assert(s != nullptr);

  if (listen_winsock(s->m_socket, SOMAXCONN) != ERROR_SUCCESS) {
    throwError(getsockerror_winsock());
  }
}

ArchSocket ArchNetworkWinsock::acceptSocket(ArchSocket s, ArchNetAddress *const addr)
{
  assert(s != nullptr);

  // create new socket and temporary address
  ArchSocketImpl *socket = new ArchSocketImpl;
  ArchNetAddress tmp = ArchNetAddressImpl::alloc(sizeof(struct sockaddr_in6));

  // accept on socket
  SOCKET fd = accept_winsock(s->m_socket, TYPED_ADDR(struct sockaddr, tmp), &tmp->m_len);
  if (fd == INVALID_SOCKET) {
    int err = getsockerror_winsock();
    delete socket;
    free(tmp);
    if (addr) {
      *addr = nullptr;
    }
    if (err == WSAEWOULDBLOCK) {
      return nullptr;
    }
    throwError(err);
  }

  try {
    setBlockingOnSocket(fd, false);
  } catch (...) {
    close_winsock(fd);
    delete socket;
    free(tmp);
    if (addr) {
      *addr = nullptr;
    }
    throw;
  }

  // initialize socket
  socket->m_socket = fd;
  socket->m_refCount = 1;
  socket->m_event = WSACreateEvent_winsock();
  socket->m_pollWrite = true;

  // copy address if requested
  if (addr != nullptr) {
    *addr = ARCH->copyAddr(tmp);
  }

  free(tmp);
  return socket;
}

bool ArchNetworkWinsock::connectSocket(ArchSocket s, ArchNetAddress addr)
{
  assert(s != nullptr);
  assert(addr != nullptr);

  if (connect_winsock(s->m_socket, TYPED_ADDR(struct sockaddr, addr), addr->m_len) == SOCKET_ERROR) {
    if (getsockerror_winsock() == WSAEISCONN) {
      return true;
    }
    if (getsockerror_winsock() == WSAEWOULDBLOCK) {
      return false;
    }
    throwError(getsockerror_winsock());
  }
  return true;
}

int ArchNetworkWinsock::pollSocket(PollEntry pe[], int num, double timeout)
{
  int i;
  DWORD n;

  // prepare sockets and wait list
  bool canWrite = false;
  WSAEVENT *events = (WSAEVENT *)alloca((num + 1) * sizeof(WSAEVENT));
  for (i = 0, n = 0; i < num; ++i) {
    // reset return flags
    pe[i].m_revents = 0;

    // set invalid flag if socket is bogus then go to next socket
    if (pe[i].m_socket == nullptr) {
      pe[i].m_revents |= PollEventMask::Invalid;
      continue;
    }

    // select desired events
    long socketEvents = 0;
    if ((pe[i].m_events & PollEventMask::In) != 0) {
      socketEvents |= FD_READ | FD_ACCEPT | FD_CLOSE;
    }
    if ((pe[i].m_events & PollEventMask::Out) != 0) {
      socketEvents |= FD_WRITE | FD_CONNECT | FD_CLOSE;

      // if m_pollWrite is false then we assume the socket is
      // writable.  winsock doesn't signal writability except
      // when the state changes from unwritable.
      if (!pe[i].m_socket->m_pollWrite) {
        canWrite = true;
        pe[i].m_revents |= PollEventMask::Out;
      }
    }

    // if no events then ignore socket
    if (socketEvents == 0) {
      continue;
    }

    // select socket for desired events
    WSAEventSelect_winsock(pe[i].m_socket->m_socket, pe[i].m_socket->m_event, socketEvents);

    // add socket event to wait list
    events[n++] = pe[i].m_socket->m_event;
  }

  // if no sockets then return immediately
  if (n == 0) {
    return 0;
  }

  // add the unblock event (cached in TLS per ArchNetworkWinsock instance to avoid per-poll mutex/list scans)
  WSAEVENT *unblockEvent = (tl_unblockInstanceId == m_instanceId) ? tl_unblockEvent : nullptr;
  if (unblockEvent == nullptr) {
    ArchMultithreadWindows *mt = ArchMultithreadWindows::getInstance();
    ArchThread thread = mt->newCurrentThread();
    unblockEvent = static_cast<WSAEVENT *>(mt->getNetworkDataForThread(thread));
    ARCH->closeThread(thread);
    if (unblockEvent == nullptr) {
      unblockEvent = new WSAEVENT;
      {
        std::scoped_lock lock{m_mutex};
        m_unblockEvents.push_back(unblockEvent);
      }
      *unblockEvent = WSACreateEvent_winsock();
      mt->setNetworkDataForCurrentThread(unblockEvent);
    }
    tl_unblockEvent = unblockEvent;
    tl_unblockInstanceId = m_instanceId;
  }
  events[n++] = *unblockEvent;

  // prepare timeout
  DWORD t;
  if (canWrite) {
    // if we know we can write then don't block
    t = 0;
  } else if (timeout < 0.0) {
    t = INFINITE;
  } else if (timeout > 0.0) {
    t = static_cast<DWORD>(std::ceil(1000.0 * timeout));
  } else {
    t = 0;
  }

  // wait
  DWORD result = WSAWaitForMultipleEvents_winsock(n, events, FALSE, t, FALSE);

  // reset the unblock event only when it actually fired
  if (result == WSA_WAIT_EVENT_0 + n - 1) {
    WSAResetEvent_winsock(*unblockEvent);
    return 0;
  }

  // handle results
  if (result == WSA_WAIT_FAILED) {
    if (getsockerror_winsock() == WSAEINTR) {
      // interrupted system call
      ARCH->testCancelThread();
      return 0;
    }
    throwError(getsockerror_winsock());
  }
  if (result == WSA_WAIT_TIMEOUT && !canWrite) {
    return 0;
  }
  for (i = 0, n = 0; i < num; ++i) {
    // skip events we didn't check
    if (pe[i].m_socket == nullptr || (pe[i].m_events & (PollEventMask::In | PollEventMask::Out)) == 0) {
      continue;
    }

    // get events
    WSANETWORKEVENTS info;
    if (WSAEnumNetworkEvents_winsock(pe[i].m_socket->m_socket, pe[i].m_socket->m_event, &info) == SOCKET_ERROR) {
      continue;
    }
    if ((info.lNetworkEvents & FD_READ) != 0) {
      pe[i].m_revents |= PollEventMask::In;
    }
    if ((info.lNetworkEvents & FD_ACCEPT) != 0) {
      pe[i].m_revents |= PollEventMask::In;
    }
    if ((info.lNetworkEvents & FD_WRITE) != 0) {
      pe[i].m_revents |= PollEventMask::Out;

      // socket is now writable so don't bothing polling for
      // writable until it becomes unwritable.
      pe[i].m_socket->m_pollWrite = false;
    }
    if ((info.lNetworkEvents & FD_CONNECT) != 0) {
      if (info.iErrorCode[FD_CONNECT_BIT] != 0) {
        pe[i].m_revents |= PollEventMask::Error;
      } else {
        pe[i].m_revents |= PollEventMask::Out;
        pe[i].m_socket->m_pollWrite = false;
      }
    }
    if ((info.lNetworkEvents & FD_CLOSE) != 0) {
      if (info.iErrorCode[FD_CLOSE_BIT] != 0) {
        pe[i].m_revents |= PollEventMask::Error;
      } else {
        if ((pe[i].m_events & PollEventMask::In) != 0) {
          pe[i].m_revents |= PollEventMask::In;
        }
        if ((pe[i].m_events & PollEventMask::Out) != 0) {
          pe[i].m_revents |= PollEventMask::Out;
        }
      }
    }
    if (pe[i].m_revents != 0) {
      ++n;
    }
  }

  return (int)n;
}

void ArchNetworkWinsock::unblockPollSocket(ArchThread thread)
{
  // set the unblock event
  ArchMultithreadWindows *mt = ArchMultithreadWindows::getInstance();
  WSAEVENT *unblockEvent = (WSAEVENT *)mt->getNetworkDataForThread(thread);
  if (unblockEvent != nullptr) {
    WSASetEvent_winsock(*unblockEvent);
  }
}

size_t ArchNetworkWinsock::readSocket(ArchSocket s, void *buf, size_t len)
{
  assert(s != nullptr);

  int n = recv_winsock(s->m_socket, buf, (int)len, 0);
  if (n == SOCKET_ERROR) {
    int err = getsockerror_winsock();
    if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
      return 0;
    }
    throwError(err);
  }
  return static_cast<size_t>(n);
}

size_t ArchNetworkWinsock::writeSocket(ArchSocket s, const void *buf, size_t len)
{
  assert(s != nullptr);

  int n = send_winsock(s->m_socket, buf, (int)len, 0);
  if (n == SOCKET_ERROR) {
    int err = getsockerror_winsock();
    if (err == WSAEINTR) {
      return 0;
    }
    if (err == WSAEWOULDBLOCK) {
      s->m_pollWrite = true;
      return 0;
    }
    throwError(err);
  }
  return static_cast<size_t>(n);
}

size_t ArchNetworkWinsock::readDatagram(ArchSocket s, void *buf, size_t len, ArchNetAddress *fromAddr)
{
  assert(s != nullptr);
  if (fromAddr != nullptr) {
    *fromAddr = nullptr;
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    sockaddr_storage storage{};
    int fromLen = sizeof(storage);
    int n = recvfrom_winsock(
        s->m_socket, buf, static_cast<int>(len), 0, reinterpret_cast<struct sockaddr *>(&storage), &fromLen
    );
    if (n == SOCKET_ERROR) {
      const int err = getsockerror_winsock();
      if (err == WSAEMSGSIZE || err == WSAECONNRESET) {
        // Oversized datagram or ICMP reset already popped from Winsock queue; advance to next packet.
        continue;
      }
      if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
        return 0;
      }
      throwError(err);
    }

    if (fromAddr != nullptr && fromLen > 0) {
      ArchNetAddress addr = ArchNetAddressImpl::alloc(static_cast<size_t>(fromLen));
      memcpy(&addr->m_addr, &storage, static_cast<size_t>(fromLen));
      *fromAddr = addr;
    }
    return static_cast<size_t>(n);
  }
  return 0;
}

size_t ArchNetworkWinsock::writeDatagram(ArchSocket s, const void *buf, size_t len, ArchNetAddress toAddr)
{
  assert(s != nullptr);
  assert(toAddr != nullptr);

  int n = sendto_winsock(
      s->m_socket, buf, static_cast<int>(len), 0, TYPED_ADDR(const struct sockaddr, toAddr), toAddr->m_len
  );
  if (n == SOCKET_ERROR) {
    const int err = getsockerror_winsock();
    if (err == WSAEINTR || err == WSAEWOULDBLOCK || err == WSAENOBUFS) {
      return 0;
    }
    throwError(err);
  }
  return static_cast<size_t>(n);
}

void ArchNetworkWinsock::setLowLatencyOnSocket(ArchSocket s, ArchNetAddress peer)
{
  if (s == nullptr) {
    return;
  }

  // Best-effort DSCP EF (46 << 2 = 0xB8) for IPv4 and IPv6
  const int tos = 0xB8;
  setsockopt_winsock(s->m_socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#ifdef IPV6_TCLASS
  setsockopt_winsock(s->m_socket, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
#endif

  // Windows 10/11 NDIS + 802.11 WMM (AC_VO / AC_VI) hardware queue tagging via qWave
  std::call_once(s_qosInitFlag, initQwaveOnce);
  if (s_qosHandle != nullptr && s_qosAddSocketToFlow != nullptr) {
    PSOCKADDR dest = (peer != nullptr) ? TYPED_ADDR(struct sockaddr, peer) : nullptr;
    DWORD flowId = 0;
    if (!s_qosAddSocketToFlow(s_qosHandle, s->m_socket, dest, kQosTrafficTypeVoice, kQosNonAdaptiveFlow, &flowId)) {
      s_qosAddSocketToFlow(s_qosHandle, s->m_socket, dest, kQosTrafficTypeAudioVideo, kQosNonAdaptiveFlow, &flowId);
    }
    if (flowId != 0) {
      {
        std::scoped_lock lock{m_mutex};
        s->m_qosFlowIds.push_back(flowId);
      }
      if (s_qosSetFlow != nullptr) {
        DWORD dscp = kDscpExpeditedForwarding;
        s_qosSetFlow(s_qosHandle, flowId, kQosSetOutgoingDscpValue, sizeof(dscp), &dscp, 0, nullptr);
      }
    }
  }
}

void ArchNetworkWinsock::resetPollWriteOnSocket(ArchSocket s)
{
  assert(s != nullptr);
  s->m_pollWrite = true;
}

void ArchNetworkWinsock::throwErrorOnSocket(ArchSocket s)
{
  assert(s != nullptr);

  // get the error from the socket layer
  int err = 0;
  int size = sizeof(err);
  if (getsockopt_winsock(s->m_socket, SOL_SOCKET, SO_ERROR, &err, &size) == SOCKET_ERROR) {
    err = getsockerror_winsock();
  }

  // throw if there's an error
  if (err != 0) {
    throwError(err);
  }
}

void ArchNetworkWinsock::setBlockingOnSocket(SOCKET s, bool blocking)
{
  assert(s != 0);

  int flag = blocking ? 0 : 1;
  if (ioctl_winsock(s, FIONBIO, &flag) == SOCKET_ERROR) {
    throwError(getsockerror_winsock());
  }
}

bool ArchNetworkWinsock::setNoDelayOnSocket(ArchSocket s, bool noDelay)
{
  assert(s != nullptr);

  // get old state
  BOOL oflag;
  int size = sizeof(oflag);
  if (getsockopt_winsock(s->m_socket, IPPROTO_TCP, TCP_NODELAY, &oflag, &size) == SOCKET_ERROR) {
    throwError(getsockerror_winsock());
  }

  // set new state
  BOOL flag = noDelay ? 1 : 0;
  size = sizeof(flag);
  if (setsockopt_winsock(s->m_socket, IPPROTO_TCP, TCP_NODELAY, &flag, size) == SOCKET_ERROR) {
    throwError(getsockerror_winsock());
  }

  return (oflag != 0);
}

void ArchNetworkWinsock::setKeepAliveOnSocket(ArchSocket s, bool keepAlive)
{
  if (!s) {
    throwError(WSAEINVAL);
  }
  int opt = keepAlive;
  if (setsockopt_winsock(s->m_socket, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) == SOCKET_ERROR) {
    throwError(getsockerror_winsock());
  }
}

bool ArchNetworkWinsock::setReuseAddrOnSocket(ArchSocket s, bool reuse)
{
  LOG_ERR("socket re-use not supported on windows");
  return false;
}

ArchNetAddress ArchNetworkWinsock::newAnyAddr(AddressFamily family)
{
  ArchNetAddressImpl *addr = nullptr;
  switch (family) {
  case AddressFamily::INet: {
    addr = ArchNetAddressImpl::alloc(sizeof(struct sockaddr_in));
    struct sockaddr_in *ipAddr = TYPED_ADDR(struct sockaddr_in, addr);
    ipAddr->sin_family = AF_INET;
    ipAddr->sin_port = 0;
    ipAddr->sin_addr.s_addr = INADDR_ANY;
    break;
  }

  case AddressFamily::INet6: {
    addr = ArchNetAddressImpl::alloc(sizeof(struct sockaddr_in6));
    struct sockaddr_in6 *ipAddr = TYPED_ADDR(struct sockaddr_in6, addr);
    ipAddr->sin6_family = AF_INET6;
    ipAddr->sin6_port = 0;
    memcpy(&ipAddr->sin6_addr, &in6addr_any, sizeof(in6addr_any));
    break;
  }

  default:
    assert(0 && "invalid family");
  }
  return addr;
}

ArchNetAddress ArchNetworkWinsock::copyAddr(ArchNetAddress addr)
{
  assert(addr != nullptr);

  ArchNetAddressImpl *copy = ArchNetAddressImpl::alloc(addr->m_len);
  memcpy(TYPED_ADDR(void, copy), TYPED_ADDR(void, addr), addr->m_len);
  return copy;
}

std::vector<ArchNetAddress> ArchNetworkWinsock::nameToAddr(const std::string &name)
{
  // allocate address
  std::vector<ArchNetAddressImpl *> addresses;

  struct addrinfo hints;
  struct addrinfo *pResult = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  int ret = -1;

  if ((ret = getaddrinfo(name.c_str(), nullptr, &hints, &pResult)) != 0) {
    throwNameError(ret);
  }

  for (auto address = pResult; address != nullptr; address = address->ai_next) {
    const size_t addrLen = (address->ai_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    auto *allocated = ArchNetAddressImpl::alloc(addrLen);
    memcpy(&allocated->m_addr, address->ai_addr, addrLen);
    addresses.push_back(allocated);
  }

  freeaddrinfo(pResult);
  return addresses;
}

void ArchNetworkWinsock::closeAddr(ArchNetAddress addr)
{
  assert(addr != nullptr);

  free(addr);
}

std::string ArchNetworkWinsock::addrToName(ArchNetAddress addr)
{
  assert(addr != nullptr);

  char host[1024];
  char service[20];
  int ret =
      getnameinfo(TYPED_ADDR(struct sockaddr, addr), addr->m_len, host, sizeof(host), service, sizeof(service), 0);

  if (ret != 0) {
    throwNameError(ret);
  }

  // return (primary) name
  std::string name = host;
  return name;
}

std::string ArchNetworkWinsock::addrToString(ArchNetAddress addr)
{
  assert(addr != nullptr);

  switch (getAddrFamily(addr)) {
  case AddressFamily::INet: {
    struct sockaddr_in *ipAddr = TYPED_ADDR(struct sockaddr_in, addr);
    return inet_ntoa_winsock(ipAddr->sin_addr);
  }

  case AddressFamily::INet6: {
    char strAddr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 *ipAddr = TYPED_ADDR(struct sockaddr_in6, addr);
    inet_ntop(AF_INET6, &ipAddr->sin6_addr, strAddr, INET6_ADDRSTRLEN);
    return strAddr;
  }

  default:
    assert(0 && "unknown address family");
    return "";
  }
}

IArchNetwork::AddressFamily ArchNetworkWinsock::getAddrFamily(ArchNetAddress addr)
{
  assert(addr != nullptr);

  switch (addr->m_addr.ss_family) {
  case AF_INET:
    return AddressFamily::INet;

  case AF_INET6:
    return AddressFamily::INet6;

  default:
    return AddressFamily::Unknown;
  }
}

void ArchNetworkWinsock::setAddrPort(ArchNetAddress addr, int port)
{
  assert(addr != nullptr);

  switch (getAddrFamily(addr)) {
  case AddressFamily::INet: {
    struct sockaddr_in *ipAddr = TYPED_ADDR(struct sockaddr_in, addr);
    ipAddr->sin_port = htons_winsock(static_cast<u_short>(port));
    break;
  }

  case AddressFamily::INet6: {
    struct sockaddr_in6 *ipAddr = TYPED_ADDR(struct sockaddr_in6, addr);
    ipAddr->sin6_port = htons_winsock(static_cast<u_short>(port));
    break;
  }

  default:
    assert(0 && "unknown address family");
    break;
  }
}

int ArchNetworkWinsock::getAddrPort(ArchNetAddress addr)
{
  assert(addr != nullptr);

  switch (getAddrFamily(addr)) {
  case AddressFamily::INet: {
    struct sockaddr_in *ipAddr = TYPED_ADDR(struct sockaddr_in, addr);
    return ntohs_winsock(ipAddr->sin_port);
  }

  case AddressFamily::INet6: {
    struct sockaddr_in6 *ipAddr = TYPED_ADDR(struct sockaddr_in6, addr);
    return ntohs_winsock(ipAddr->sin6_port);
  }

  default:
    assert(0 && "unknown address family");
    return 0;
  }
}

bool ArchNetworkWinsock::isAnyAddr(ArchNetAddress addr)
{
  assert(addr != nullptr);

  switch (getAddrFamily(addr)) {
  case AddressFamily::INet: {
    struct sockaddr_in *ipAddr = TYPED_ADDR(struct sockaddr_in, addr);
    return (addr->m_len == sizeof(struct sockaddr_in) && ipAddr->sin_addr.s_addr == INADDR_ANY);
  }

  case AddressFamily::INet6: {
    struct sockaddr_in6 *ipAddr = TYPED_ADDR(struct sockaddr_in6, addr);
    return (
        addr->m_len == sizeof(struct sockaddr_in6) && memcmp(&ipAddr->sin6_addr, &in6addr_any, sizeof(in6addr_any)) == 0
    );
  }

  default:
    assert(0 && "unknown address family");
    return true;
  }
}

bool ArchNetworkWinsock::isEqualAddr(ArchNetAddress a, ArchNetAddress b)
{
  return (a == b || (a->m_len == b->m_len && memcmp(&a->m_addr, &b->m_addr, a->m_len) == 0));
}

[[noreturn]] void ArchNetworkWinsock::throwError(int err) const
{
  switch (err) {
  case WSAEACCES:
    throw ArchNetworkAccessException(windowsErrorToString(err));

  case WSAEMFILE:
  case WSAENOBUFS:
  case WSAENETDOWN:
    throw ArchNetworkResourceException(windowsErrorToString(err));

  case WSAEPROTOTYPE:
  case WSAEPROTONOSUPPORT:
  case WSAEAFNOSUPPORT:
  case WSAEPFNOSUPPORT:
  case WSAESOCKTNOSUPPORT:
  case WSAEINVAL:
  case WSAENOPROTOOPT:
  case WSAEOPNOTSUPP:
  case WSAESHUTDOWN:
  case WSANOTINITIALISED:
  case WSAVERNOTSUPPORTED:
  case WSASYSNOTREADY:
    throw ArchNetworkSupportException(windowsErrorToString(err));

  case WSAEADDRNOTAVAIL:
    throw ArchNetworkNoAddressException(windowsErrorToString(err));

  case WSAEADDRINUSE:
    throw ArchNetworkAddressInUseException(windowsErrorToString(err));

  case WSAEHOSTUNREACH:
  case WSAENETUNREACH:
    throw ArchNetworkNoRouteException(windowsErrorToString(err));

  case WSAENOTCONN:
    throw ArchNetworkNotConnectedException(windowsErrorToString(err));

  case WSAEDISCON:
    throw ArchNetworkShutdownException(windowsErrorToString(err));

  case WSAENETRESET:
  case WSAECONNABORTED:
  case WSAECONNRESET:
    throw ArchNetworkDisconnectedException(windowsErrorToString(err));

  case WSAECONNREFUSED:
    throw ArchNetworkConnectionRefusedException(windowsErrorToString(err));

  case WSAEHOSTDOWN:
  case WSAETIMEDOUT:
    throw ArchNetworkTimedOutException(windowsErrorToString(err));

  case WSAHOST_NOT_FOUND:
    throw ArchNetworkNameUnknownException(windowsErrorToString(err));

  case WSANO_DATA:
    throw ArchNetworkNameNoAddressException(windowsErrorToString(err));

  case WSANO_RECOVERY:
    throw ArchNetworkNameFailureException(windowsErrorToString(err));

  case WSATRY_AGAIN:
    throw ArchNetworkNameUnavailableException(windowsErrorToString(err));

  default:
    throw ArchNetworkException(windowsErrorToString(err));
  }
}

[[noreturn]] void ArchNetworkWinsock::throwNameError(int err) const
{
  switch (err) {
  case WSAHOST_NOT_FOUND:
    throw ArchNetworkNameUnknownException(windowsErrorToString(err));

  case WSANO_DATA:
    throw ArchNetworkNameNoAddressException(windowsErrorToString(err));

  case WSANO_RECOVERY:
    throw ArchNetworkNameFailureException(windowsErrorToString(err));

  case WSATRY_AGAIN:
    throw ArchNetworkNameUnavailableException(windowsErrorToString(err));

  default:
    throw ArchNetworkNameException(windowsErrorToString(err));
  }
}
