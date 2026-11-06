// POSIX names that gitstatusd uses and the Windows CRT lacks. Force-included by CMakeLists.txt.

#ifndef ROMKATV_GITSTATUS_COMPAT_WINDOWS_H_
#define ROMKATV_GITSTATUS_COMPAT_WINDOWS_H_

#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef ptrdiff_t ssize_t;
typedef unsigned int mode_t;

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#define S_IFLNK 0120000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

#endif  // ROMKATV_GITSTATUS_COMPAT_WINDOWS_H_
