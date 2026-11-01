#pragma once

#include <filesystem>
#include <string>

#include "codesearch/index/memory_index.hpp"
#include "codesearch/index/shard_writer.hpp"
#include "codesearch/repo/registry.hpp"
#include "codesearch/repo/workspace.hpp"

namespace codesearch {

// The steps of publishing a new index file. The caller holds the workspace
// lock the whole time and runs them in this order:
//
//   1. writeToTmp       build the file in tmp/ and check it
//   2. moveIntoIndexes  rename it into indexes/; nothing points to it yet
//   3. save the registry, which now points to it (RegistryStore::save)
//   4. cleanUp          delete whatever the registry doesn't use
//
// If the process dies at any point, the registry still points to complete
// files, and the next cleanUp deletes the leftovers.

// Writes `index` to tmp/<fileName>, then opens it to check that it holds the
// commit and file count we expect. Returns its path.
std::filesystem::path writeToTmp(const Workspace& workspace, const MemoryIndex& index,
                                 const ShardInfo& info, const std::string& fileName);

// Moves a file from tmp/ into indexes/, keeping its name.
void moveIntoIndexes(const Workspace& workspace, const std::filesystem::path& tmpFile);

// Deletes builds left in tmp/, index files `registry` doesn't name, clones
// of repos it doesn't list, and temp files a killed save left next to
// registry.json. Only names code-search makes are deleted (like
// "r_1c2d3e4f-g0001.idx" and "r_1c2d3e4f.git"); anything else in the
// folders is left alone. It first checks the three folders are real folders inside
// the workspace (Workspace::checkFolders). It only deletes things directly
// inside them, and checks that after following symlinks. A symlink is deleted
// itself, never what it points to.
void cleanUp(const Workspace& workspace, const Registry& registry);

// True if indexes/ or repos/ hold an index file or clone code-search made.
// Other files there (like .DS_Store) don't count; cleanup never deletes them.
bool holdsIndexesOrClones(const Workspace& workspace);

}  // namespace codesearch
