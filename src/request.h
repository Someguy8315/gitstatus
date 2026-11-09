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

#ifndef ROMKATV_GITSTATUS_REQUEST_H_
#define ROMKATV_GITSTATUS_REQUEST_H_

#include <deque>
#include <ostream>
#include <string>

#ifdef _WIN32
#include <condition_variable>
#include <memory>
#include <mutex>
#endif

namespace gitstatus {

struct Request {
  std::string id;
  std::string dir;
  bool from_dotgit = false;
  bool diff = true;
};

std::ostream& operator<<(std::ostream& strm, const Request& req);

class RequestReader {
 public:
  RequestReader(int fd, int lock_fd, int parent_pid);
  bool ReadRequest(Request& req);

 private:
  int fd_;
  int lock_fd_;
  int parent_pid_;
  std::deque<char> read_;

#ifdef _WIN32
  // Native Windows cannot select() on pipes. A detached thread does blocking reads into this
  // buffer and ReadRequest() waits for data with a timeout instead.
  struct Input {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<char> data;
    bool eof = false;
  };
  std::shared_ptr<Input> input_;
  // Handle of the parent process (HANDLE), or null.
  void* parent_ = nullptr;
#endif
};

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_REQUEST_H_
