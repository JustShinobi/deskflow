/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SecureListenSocket.h"

#include "SecureSocket.h"
#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "common/Settings.h"
#include "net/SocketMultiplexer.h"

//
// SecureListenSocket
//

SecureListenSocket::SecureListenSocket(
    IEventQueue *events, SocketMultiplexer *socketMultiplexer, IArchNetwork::AddressFamily family,
    SecurityLevel securityLevel
)
    : TCPListenSocket(events, socketMultiplexer, family),
      m_securityLevel{securityLevel}

{
  // do nothing
}

std::unique_ptr<IDataSocket> SecureListenSocket::accept()
{
  std::unique_ptr<SecureSocket> secureSocket;
  ArchNetAddress remoteAddr = nullptr;
  try {
    ArchSocket accepted = ARCH->acceptSocket(socket(), &remoteAddr);
    std::string peerIp;
    if (remoteAddr != nullptr) {
      peerIp = ARCH->addrToString(remoteAddr);
      ARCH->closeAddr(remoteAddr);
      remoteAddr = nullptr;
    }
    secureSocket = std::make_unique<SecureSocket>(events(), socketMultiplexer(), accepted, m_securityLevel);
    secureSocket->setPeerAddressString(std::move(peerIp));
    secureSocket->initSsl(true);

    setListeningJob();

    // default location of the TLS cert file in users dir
    if (!secureSocket->loadCertificate(Settings::value(Settings::Security::Certificate).toString())) {
      return nullptr;
    }

    secureSocket->secureAccept();

    return secureSocket;
  } catch (ArchNetworkException &) {
    if (remoteAddr != nullptr) {
      ARCH->closeAddr(remoteAddr);
    }
    if (secureSocket) {
      setListeningJob();
    }
    return nullptr;
  } catch (std::exception &ex) {
    if (remoteAddr != nullptr) {
      ARCH->closeAddr(remoteAddr);
    }
    if (secureSocket) {
      setListeningJob();
    }
    throw ex;
  }
}
