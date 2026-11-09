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

#include "timer.h"

#ifdef _WIN32
#include <windows.h>

#include <chrono>
#include <cstdint>
#else
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#endif

#include <cmath>
#include <limits>

#include "check.h"
#include "logging.h"

namespace gitstatus {

namespace {

#ifdef _WIN32

double CpuTimeMs() {
  auto ToMs = [](const FILETIME& ft) {
    return 1e-4 * static_cast<double>((std::uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
  };
  FILETIME creation, exit, kernel, user;
  CHECK(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) << GetLastError();
  return ToMs(user) + ToMs(kernel);
}

double WallTimeMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#else  // _WIN32

double CpuTimeMs() {
  auto ToMs = [](const timeval& tv) { return 1e3 * tv.tv_sec + 1e-3 * tv.tv_usec; };
  rusage usage = {};
  CHECK(getrusage(RUSAGE_SELF, &usage) == 0) << Errno();
  return ToMs(usage.ru_utime) + ToMs(usage.ru_stime);
}

double WallTimeMs() {
  // An attempt to call clock_gettime on an ancient version of MacOS fails at runtime.
  // It's possible to detect the presence of clock_gettime at runtime but I don't have
  // an ancient MacOS to test the code. Hence this.
#ifdef __APPLE__
  return std::numeric_limits<double>::quiet_NaN();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return 1e3 * ts.tv_sec + 1e-6 * ts.tv_nsec;
#endif
}

#endif  // _WIN32

}  // namespace

void Timer::Start() {
  cpu_ = CpuTimeMs();
  wall_ = WallTimeMs();
}

void Timer::Report(const char* msg) {
  double cpu = CpuTimeMs() - cpu_;
  if (std::isnan(wall_)) {
    LOG(INFO) << "Timing for: " << msg << ": " << cpu << "ms cpu";
  } else {
    double wall = WallTimeMs() - wall_;
    LOG(INFO) << "Timing for: " << msg << ": " << cpu << "ms cpu, " << wall << "ms wall";
  }
  Start();
}

}  // namespace gitstatus
