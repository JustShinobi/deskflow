/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/SocketMultiplexer.h"

#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "base/Log.h"
#include "base/TMethodJob.h"
#include "mt/CondVar.h"
#include "mt/Lock.h"
#include "mt/Mutex.h"
#include "mt/Thread.h"
#include "net/ISocketMultiplexerJob.h"

#include <atomic>
#include <vector>

namespace {
std::atomic<SocketMultiplexer *> s_instance{nullptr};
} // namespace

//
// SocketMultiplexer
//

SocketMultiplexer::SocketMultiplexer()
    : m_mutex(new Mutex),
      m_jobsReady(new CondVar<bool>(m_mutex, false)),
      m_jobListLock(new CondVar<bool>(m_mutex, false)),
      m_jobListLockLocked(new CondVar<bool>(m_mutex, false))
{
  s_instance.store(this, std::memory_order_release);
  // start thread
  auto tMethodJob = new TMethodJob<SocketMultiplexer>(this, &SocketMultiplexer::serviceThread);
  m_thread = new Thread(tMethodJob);
}

SocketMultiplexer::~SocketMultiplexer()
{
  SocketMultiplexer *expected = this;
  s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  m_thread->cancel();
  m_thread->unblockPollSocket();
  m_thread->wait();
  delete m_thread;
  delete m_jobsReady;
  delete m_jobListLock;
  delete m_jobListLockLocked;
  delete m_mutex;

  // clean up jobs
  for (auto i = m_socketJobMap.begin(); i != m_socketJobMap.end(); ++i) {
    delete *(i->second);
  }
}

SocketMultiplexer *SocketMultiplexer::getInstance()
{
  return s_instance.load(std::memory_order_acquire);
}

void SocketMultiplexer::addSocket(ISocket *socket, ISocketMultiplexerJob *job)
{
  assert(socket != nullptr);
  assert(job != nullptr);

  // prevent other threads from locking the job list
  lockJobListLock();

  // break thread out of poll
  m_thread->unblockPollSocket();

  // lock the job list
  lockJobList();

  // insert/replace job
  if (SocketJobMap::iterator i = m_socketJobMap.find(socket); i == m_socketJobMap.end()) {
    // we *must* put the job at the end so the order of jobs in
    // the list continue to match the order of jobs in pfds in
    // serviceThread().
    JobCursor j = m_socketJobs.insert(m_socketJobs.end(), job);
    m_update = true;
    m_socketJobMap.try_emplace(socket, j);
  } else {
    if (JobCursor j = i->second; *j != job) {
      delete *j;
      *j = job;
    }
    m_update = true;
  }

  // unlock the job list
  unlockJobList();
}

void SocketMultiplexer::removeSocket(ISocket *socket)
{
  assert(socket != nullptr);

  // prevent other threads from locking the job list
  lockJobListLock();

  // break thread out of poll
  m_thread->unblockPollSocket();

  // lock the job list
  lockJobList();

  // remove job.  rather than removing it from the map we put nullptr
  // in the list instead so the order of jobs in the list continues
  // to match the order of jobs in pfds in serviceThread().
  if (SocketJobMap::iterator i = m_socketJobMap.find(socket); i != m_socketJobMap.end() && (*(i->second) != nullptr)) {
    delete *(i->second);
    *(i->second) = nullptr;
    m_update = true;
  }

  // unlock the job list
  unlockJobList();
}

[[noreturn]] void SocketMultiplexer::serviceThread(const void *)
{
  std::vector<IArchNetwork::PollEntry> pfds;
  IArchNetwork::PollEntry pfd;

  // service the connections
  for (;;) {
    Thread::testCancel();

    // wait until there are jobs to handle
    {
      Lock lock(m_mutex);
      while (!(bool)*m_jobsReady) {
        m_jobsReady->wait();
      }
    }

    // lock the job list
    lockJobListLock();
    lockJobList();

    // collect poll entries (m_jobListLock is held exclusively by serviceThread here)
    if (m_update) {
      m_update = false;
      pfds.clear();
      pfds.reserve(m_socketJobMap.size());

      for (const ISocketMultiplexerJob *job : m_socketJobs) {
        if (job != nullptr) {
          pfd.m_socket = job->getSocket();
          pfd.m_events = 0;
          if (job->isReadable()) {
            pfd.m_events |= IArchNetwork::PollEventMask::In;
          }
          if (job->isWritable()) {
            pfd.m_events |= IArchNetwork::PollEventMask::Out;
          }
          pfds.push_back(pfd);
        }
      }
    }

    int status;
    try {
      // check for status
      if (!pfds.empty()) {
        status = ARCH->pollSocket(&pfds[0], (int)pfds.size(), -1);
      } else {
        status = 0;
      }
    } catch (ArchNetworkException &e) {
      LOG_WARN("error in socket multiplexer: %s", e.what());
      status = 0;
    }

    if (status != 0) {
      // iterate over socket jobs, invoking each and saving the new job.
      uint32_t i = 0;
      for (auto jobCursor = m_socketJobs.begin(); i < pfds.size() && jobCursor != m_socketJobs.end(); ++jobCursor) {
        if (*jobCursor != nullptr) {
          unsigned short revents = pfds[i].m_revents;
          bool read = ((revents & int(IArchNetwork::PollEventMask::In)) != 0);
          bool write = ((revents & int(IArchNetwork::PollEventMask::Out)) != 0);
          bool error =
              ((revents & (int(IArchNetwork::PollEventMask::Error) | int(IArchNetwork::PollEventMask::Invalid))) != 0);

          ISocketMultiplexerJob *job = *jobCursor;
          if (read || write || error) {
            if (ISocketMultiplexerJob *newJob = job->run(read, write, error); newJob != job) {
              Lock lock(m_mutex);
              delete job;
              *jobCursor = newJob;
              m_update = true;
            }
          }
          ++i;
        }
      }
    }

    // delete any removed socket jobs
    for (auto i = m_socketJobMap.begin(); i != m_socketJobMap.end();) {
      if (*(i->second) == nullptr) {
        m_socketJobs.erase(i->second);
        m_socketJobMap.erase(i++);
        m_update = true;
      } else {
        ++i;
      }
    }

    // unlock the job list
    unlockJobList();
  }
}

void SocketMultiplexer::lockJobListLock()
{
  Lock lock(m_mutex);

  // wait for the lock on the lock
  while (*m_jobListLockLocked) {
    m_jobListLockLocked->wait();
  }

  // take ownership of the lock on the lock
  *m_jobListLockLocked = true;
  m_jobListLockLocker = std::this_thread::get_id();
}

void SocketMultiplexer::lockJobList()
{
  Lock lock(m_mutex);

  // make sure we're the one that called lockJobListLock()
  assert(m_jobListLockLocker == std::this_thread::get_id());

  // wait for the job list lock
  while (*m_jobListLock) {
    m_jobListLock->wait();
  }

  // take ownership of the lock
  *m_jobListLock = true;
  m_jobListLocker = m_jobListLockLocker;
  m_jobListLockLocker = std::thread::id{};

  // release the lock on the lock
  *m_jobListLockLocked = false;
  m_jobListLockLocked->broadcast();
}

void SocketMultiplexer::unlockJobList()
{
  Lock lock(m_mutex);

  // make sure we're the one that called lockJobList()
  assert(m_jobListLocker == std::this_thread::get_id());

  // release the lock
  m_jobListLocker = std::thread::id{};
  *m_jobListLock = false;
  m_jobListLock->signal();

  // set new jobs ready state
  bool isReady = !m_socketJobMap.empty();
  if (*m_jobsReady != isReady) {
    *m_jobsReady = isReady;
    m_jobsReady->signal();
  }
}
