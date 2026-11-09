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

#ifndef ROMKATV_GITSTATUS_GETOPT_WIN_H_
#define ROMKATV_GITSTATUS_GETOPT_WIN_H_

// Minimal getopt_long() and fnmatch() for native Windows, whose C runtime has neither. Only the
// features that ParseOptions() relies on are implemented: no argument permutation and no
// optional arguments.

#include <cstdio>
#include <cstring>

enum { no_argument = 0, required_argument = 1 };
enum { FNM_NOMATCH = 1 };

struct option {
  const char* name;
  int has_arg;
  int* flag;
  int val;
};

inline char* optarg = nullptr;
inline int optind = 1;

inline int getopt_long(int argc, char* const argv[], const char* shortopts,
                       const struct option* longopts, int* longindex) {
  static const char* next = nullptr;  // the rest of a short option cluster like "-eU"
  (void)longindex;
  optarg = nullptr;

  if (!next || !*next) {
    next = nullptr;
    if (optind >= argc) return -1;
    const char* arg = argv[optind];
    if (arg[0] != '-' || !arg[1]) return -1;
    ++optind;
    if (arg[1] == '-') {
      if (!arg[2]) return -1;  // "--"
      const char* name = arg + 2;
      const char* eq = std::strchr(name, '=');
      size_t len = eq ? static_cast<size_t>(eq - name) : std::strlen(name);
      for (const option* o = longopts; o->name; ++o) {
        if (std::strlen(o->name) != len || std::strncmp(o->name, name, len)) continue;
        if (o->has_arg == required_argument) {
          if (eq) {
            optarg = const_cast<char*>(eq + 1);
          } else if (optind < argc) {
            optarg = argv[optind++];
          } else {
            std::fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], o->name);
            return '?';
          }
        } else if (eq) {
          std::fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], o->name);
          return '?';
        }
        if (o->flag) {
          *o->flag = o->val;
          return 0;
        }
        return o->val;
      }
      std::fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
      return '?';
    }
    next = arg + 1;
  }

  char c = *next++;
  const char* spec = c == ':' ? nullptr : std::strchr(shortopts, c);
  if (!spec) {
    std::fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
    return '?';
  }
  if (spec[1] == ':') {
    if (*next) {
      optarg = const_cast<char*>(next);
    } else if (optind < argc) {
      optarg = argv[optind++];
    } else {
      std::fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
      next = nullptr;
      return '?';
    }
    next = nullptr;
  }
  return c;
}

// Supports '*', '?', '[...]' (with ranges and '!' or '^' negation) and '\' escapes.
inline int fnmatch(const char* pattern, const char* s, int flags) {
  (void)flags;
  for (; *pattern; ++pattern, ++s) {
    switch (*pattern) {
      case '*':
        while (pattern[1] == '*') ++pattern;
        for (;; ++s) {
          if (!fnmatch(pattern + 1, s, 0)) return 0;
          if (!*s) return FNM_NOMATCH;
        }
      case '?':
        if (!*s) return FNM_NOMATCH;
        break;
      case '[': {
        if (!*s) return FNM_NOMATCH;
        const char* p = pattern + 1;
        bool negate = *p == '!' || *p == '^';
        if (negate) ++p;
        bool match = false;
        for (bool first = true; *p && (first || *p != ']'); first = false, ++p) {
          char lo = *p, hi = *p;
          if (p[1] == '-' && p[2] && p[2] != ']') hi = (p += 2)[0];
          if (lo <= *s && *s <= hi) match = true;
        }
        if (*p != ']') {  // unterminated: match '[' literally
          if (*s != '[') return FNM_NOMATCH;
          break;
        }
        if (match == negate) return FNM_NOMATCH;
        pattern = p;
        break;
      }
      case '\\':
        if (pattern[1]) ++pattern;
        [[fallthrough]];
      default:
        if (*pattern != *s) return FNM_NOMATCH;
    }
  }
  return *s ? FNM_NOMATCH : 0;
}

#endif  // ROMKATV_GITSTATUS_GETOPT_WIN_H_
