#ifndef ROMKATV_GITSTATUS_STAT_H_
#define ROMKATV_GITSTATUS_STAT_H_

#include <sys/stat.h>

#ifdef _WIN32
#include <cstdint>
#include <ctime>
#endif

namespace gitstatus {

#ifdef _WIN32

// The subset of struct stat that gitstatusd needs, filled from Win32 file information.
// st_ino is always zero, matching the indexes written by Git for Windows.
struct Stat {
  struct timespec st_mtim;
  std::int64_t st_size;
  std::uint32_t st_ino;
  mode_t st_mode;
};

#else

using Stat = struct stat;

#endif

inline const struct timespec& MTim(const Stat& s) {
#ifdef __APPLE__
  return s.st_mtimespec;
#else
  return s.st_mtim;
#endif
}

inline bool StatEq(const Stat& x, const Stat& y) {
  return MTim(x).tv_sec == MTim(y).tv_sec && MTim(x).tv_nsec == MTim(y).tv_nsec &&
         x.st_size == y.st_size && x.st_ino == y.st_ino && x.st_mode == y.st_mode;
}

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_STAT_H_
