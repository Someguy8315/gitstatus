// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "request.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <chrono>
#include <thread>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "check.h"
#include "logging.h"
#include "print.h"
#include "serialization.h"

namespace gitstatus {

namespace {

Request ParseRequest(const std::string& s) {
  Request res;
  auto begin = s.begin(), end = s.end(), sep = std::find(begin, end, kFieldSep);
  VERIFY(sep != end) << "Malformed request: " << s;
  res.id.assign(begin, sep);

  begin = sep + 1;
  if (*begin == ':') {
    res.from_dotgit = true;
    ++begin;
  }
  sep = std::find(begin, end, kFieldSep);
  res.dir.assign(begin, sep);
#ifdef _WIN32
  std::replace(res.dir.begin(), res.dir.end(), '\\', '/');
#endif
  if (sep == end) return res;

  begin = sep + 1;
  VERIFY(begin + 1 == end && (*begin == '0' || *begin == '1')) << "Malformed request: " << s;
  res.diff = *begin == '0';
  return res;
}

#ifndef _WIN32
bool IsLockedFd(int fd) {
  CHECK(fd >= 0);
  struct flock flock = {};
  flock.l_type = F_RDLCK;
  flock.l_whence = SEEK_SET;
  CHECK(fcntl(fd, F_GETLK, &flock) != -1) << Errno();
  return flock.l_type != F_UNLCK;
}
#endif

bool IsAlive(int pid) {
#ifdef _WIN32
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return false;
  DWORD res = WaitForSingleObject(h, 0);
  CHECK(CloseHandle(h)) << GetLastError();
  return res == WAIT_TIMEOUT;
#else
  return !kill(pid, 0);
#endif
}

}  // namespace

std::ostream& operator<<(std::ostream& strm, const Request& req) {
  strm << Print(req.id) << " for " << Print(req.dir);
  if (req.from_dotgit) strm << " [from-dotgit]";
  if (!req.diff) strm << " [no-diff]";
  return strm;
}

RequestReader::RequestReader(int fd, int lock_fd, int parent_pid)
    : fd_(fd), lock_fd_(lock_fd), parent_pid_(parent_pid) {
  CHECK(fd != lock_fd);
#ifdef _WIN32
  CHECK(lock_fd < 0);
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  std::thread([this, h] {
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) {
      std::unique_lock<std::mutex> lock(mutex_);
      input_.append(buf, n);
      cv_.notify_one();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    eof_ = true;
    cv_.notify_one();
  }).detach();
#endif
}

int RequestReader::Read(char* buf, int size) {
#ifdef _WIN32
  std::unique_lock<std::mutex> lock(mutex_);
  if (!cv_.wait_for(lock, std::chrono::seconds(1), [&] { return eof_ || !input_.empty(); })) {
    return -1;
  }
  int n = std::min(size, static_cast<int>(input_.size()));
  input_.copy(buf, n);
  input_.erase(0, n);
  return n;
#else
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd_, &fds);
  struct timeval timeout = {.tv_sec = 1};

  int n;
  CHECK((n = select(fd_ + 1, &fds, NULL, NULL, &timeout)) >= 0) << Errno();
  if (n == 0) return -1;
  CHECK((n = read(fd_, buf, size)) >= 0) << Errno();
  return n;
#endif
}

bool RequestReader::ReadRequest(Request& req) {
  auto eol = std::find(read_.begin(), read_.end(), kMsgSep);
  if (eol != read_.end()) {
    std::string msg(read_.begin(), eol);
    read_.erase(read_.begin(), eol + 1);
    req = ParseRequest(msg);
    return true;
  }

  char buf[256];
  while (true) {
    int n = Read(buf, sizeof(buf));
    if (n < 0) {
#ifndef _WIN32
      if (lock_fd_ >= 0 && !IsLockedFd(lock_fd_)) {
        LOG(INFO) << "Lock on fd " << lock_fd_ << " is gone. Exiting.";
        std::exit(0);
      }
#endif
      if (parent_pid_ >= 0 && !IsAlive(parent_pid_)) {
        LOG(INFO) << "Parent process " << parent_pid_ << " is gone. Exiting.";
        std::exit(0);
      }
      req = {};
      return false;
    }

    if (n == 0) {
      LOG(INFO) << "EOF. Exiting.";
      std::exit(0);
    }
    read_.insert(read_.end(), buf, buf + n);
    int eol = std::find(buf, buf + n, kMsgSep) - buf;
    if (eol != n) {
      std::string msg(read_.begin(), read_.end() - (n - eol));
      read_.erase(read_.begin(), read_.begin() + msg.size() + 1);
      req = ParseRequest(msg);
      return true;
    }
  }
}

}  // namespace gitstatus
