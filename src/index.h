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

#ifndef ROMKATV_GITSTATUS_INDEX_H_
#define ROMKATV_GITSTATUS_INDEX_H_

#include <git2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "change_journal.h"
#include "dir.h"
#include "options.h"
#include "stat.h"
#include "string_view.h"
#include "tribool.h"

namespace gitstatus {

struct RepoCaps {
  RepoCaps(git_repository* repo, git_index* index);

  bool trust_filemode;
  bool has_symlinks;
  bool case_sensitive;
  bool precompose_unicode;
};

// A file whose content matched its index entry although its stat didn't. gitstatusd never writes
// the index, so without remembering this such files would be hashed again on every query.
struct VerifiedFile {
  Stat st;
  git_oid id;
  uint32_t mode;
};

// Keyed by index path.
using VerifiedFiles = std::unordered_map<std::string, VerifiedFile>;

// A tracked regular file whose stat differs from its index entry.
struct StatCandidate {
  const git_index_entry* entry;
  Stat st;
};

struct ScanOpts {
  bool include_untracked;
  Tribool untracked_cache;
  // Not dirty candidates while their stat and index entry stay the same. Can be null.
  const VerifiedFiles* verified;
};

struct IndexDir {
  explicit IndexDir(Arena* arena) : files(arena), subdirs(arena) {}

  StringView path;
  StringView basename;
  size_t depth = 0;
  Stat st = {};
  WithArena<std::vector<const git_index_entry*>> files;
  WithArena<std::vector<StringView>> subdirs;

  Arena arena;
  std::vector<const char*> unmatched;

#ifdef _WIN32
  // The last listing, reused while the change journal shows nothing changed in the directory.
  DirId id;
  bool listed = false;
  bool changed = true;
  // Dirty candidates other than the modified files: new, deleted and unreadable.
  std::vector<const char*> candidates;
  // Index entries whose stat differs, with that stat.
  std::vector<StatCandidate> modified;
#endif
};

class Index {
 public:
  Index(git_repository* repo, git_index* index);

  // Stat candidates are also appended to `stat_candidates`.
  std::vector<const char*> GetDirtyCandidates(const ScanOpts& opts,
                                              std::vector<StatCandidate>& stat_candidates);

 private:
  size_t InitDirs(git_index* index);
  void InitSplits(size_t total_weight);
#ifdef _WIN32
  // Sets `changed` on the directories that need listing.
  void MarkChangedDirs(const std::wstring& root, const ScanOpts& opts);
#endif

  Arena arena_;
  WithArena<std::vector<IndexDir*>> dirs_;
  WithArena<std::vector<size_t>> splits_;
  git_index* git_index_;
  const char* root_dir_;
  RepoCaps caps_;
#ifdef _WIN32
  ChangeJournal journal_;
  // Listed directories by file ID.
  std::unordered_multimap<uint64_t, IndexDir*> dirs_by_id_;
  bool include_untracked_ = false;
#endif
};

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_GIT_H_
