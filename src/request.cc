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
#include <windows.h>

#include <io.h>

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
  // libgit2 reports paths with '/'. Normalizing here keeps repo cache keys consistent.
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

}  // namespace

std::ostream& operator<<(std::ostream& strm, const Request& req) {
  strm << Print(req.id) << " for " << Print(req.dir);
  if (req.from_dotgit) strm << " [from-dotgit]";
  if (!req.diff) strm << " [no-diff]";
  return strm;
}

#ifdef _WIN32

RequestReader::RequestReader(int fd, int lock_fd, int parent_pid)
    : fd_(fd), lock_fd_(lock_fd), parent_pid_(parent_pid), input_(std::make_shared<Input>()) {
  CHECK(lock_fd < 0) << "--lock-fd is not supported on Windows";
  if (parent_pid >= 0) {
    // Holding the handle keeps the PID from being reused while we watch it.
    parent_ = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent_pid));
    if (!parent_) {
      LOG(INFO) << "Unable to open parent process " << parent_pid << ". Exiting.";
      std::exit(0);
    }
  }
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  CHECK(h != INVALID_HANDLE_VALUE);
  std::thread([h, input = input_] {
    char buf[4096];
    while (true) {
      DWORD n = 0;
      bool ok = ReadFile(h, buf, sizeof(buf), &n, nullptr);
      std::unique_lock<std::mutex> lock(input->mutex);
      if (!ok || n == 0) {
        input->eof = true;
      } else {
        input->data.insert(input->data.end(), buf, buf + n);
      }
      input->cv.notify_one();
      if (input->eof) return;
    }
  }).detach();
}

bool RequestReader::ReadRequest(Request& req) {
  while (true) {
    auto eol = std::find(read_.begin(), read_.end(), kMsgSep);
    if (eol != read_.end()) {
      std::string msg(read_.begin(), eol);
      read_.erase(read_.begin(), eol + 1);
      req = ParseRequest(msg);
      return true;
    }

    std::unique_lock<std::mutex> lock(input_->mutex);
    if (!input_->cv.wait_for(lock, std::chrono::seconds(1),
                             [&] { return !input_->data.empty() || input_->eof; })) {
      lock.unlock();
      if (parent_ && WaitForSingleObject(parent_, 0) == WAIT_OBJECT_0) {
        LOG(INFO) << "Parent process " << parent_pid_ << " is gone. Exiting.";
        std::exit(0);
      }
      req = {};
      return false;
    }
    if (input_->data.empty()) {
      LOG(INFO) << "EOF. Exiting.";
      std::exit(0);
    }
    read_.insert(read_.end(), input_->data.begin(), input_->data.end());
    input_->data.clear();
  }
}

#else  // _WIN32

RequestReader::RequestReader(int fd, int lock_fd, int parent_pid)
    : fd_(fd), lock_fd_(lock_fd), parent_pid_(parent_pid) {
  CHECK(fd != lock_fd);
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
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    struct timeval timeout = {.tv_sec = 1};

    int n;
    CHECK((n = select(fd_ + 1, &fds, NULL, NULL, &timeout)) >= 0) << Errno();
    if (n == 0) {
      if (lock_fd_ >= 0 && !IsLockedFd(lock_fd_)) {
        LOG(INFO) << "Lock on fd " << lock_fd_ << " is gone. Exiting.";
        std::exit(0);
      }
      if (parent_pid_ >= 0 && kill(parent_pid_, 0)) {
        LOG(INFO) << "Unable to send signal 0 to " << parent_pid_ << ". Exiting.";
        std::exit(0);
      }
      req = {};
      return false;
    }

    CHECK((n = read(fd_, buf, sizeof(buf))) >= 0) << Errno();
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

#endif  // _WIN32

}  // namespace gitstatus
