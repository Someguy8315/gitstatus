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
  // Reads up to size bytes into buf. Returns -1 if no data arrives for a second, 0 on EOF.
  int Read(char* buf, int size);

  int fd_;
  int lock_fd_;
  int parent_pid_;
  std::deque<char> read_;

#ifdef _WIN32
  // Windows cannot wait for an anonymous pipe with a timeout, so a thread reads it into input_.
  std::mutex mutex_;
  std::condition_variable cv_;
  std::string input_;
  bool eof_ = false;
#endif
};

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_REQUEST_H_
