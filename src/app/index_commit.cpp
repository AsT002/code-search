#include "codesearch/app/index_commit.hpp"

#include <optional>
#include <utility>

namespace codesearch {

namespace {

void countSkipped(IngestReport& report, SkipReason reason, std::uint64_t bytes) {
    FileCount& count = report.skipped[reason];  // map creates a zero count if missing
    count.files += 1;
    count.bytes += bytes;
}

}  // namespace

CommitIndex indexCommit(const GitRepo& repo, const ObjectId& commit, const FileFilter& filter) {
    IndexBuilder builder;
    IngestReport report;
    BlobStream blobs(repo);

    for (const TreeEntry& entry : repo.listFiles(commit)) {
        if (entry.kind == EntryKind::Symlink) {
            countSkipped(report, SkipReason::Symlink, entry.size);
            continue;
        }
        if (entry.kind == EntryKind::Submodule) {
            countSkipped(report, SkipReason::Submodule, 0);
            continue;
        }

        const std::optional<SkipReason> entryReason = filter.checkEntry(entry.path, entry.size);
        if (entryReason.has_value()) {
            countSkipped(report, entryReason.value(), entry.size);
            continue;
        }

        Blob blob = blobs.read(entry.id, filter.config().maxFileBytes);
        if (blob.status == BlobStatus::Missing) {
            // Skipping it would give results that look complete but aren't.
            throw GitError("the repository is missing the contents of " + entry.path + " (" +
                           entry.id.hex() + ")");
        }
        if (blob.status == BlobStatus::TooLarge) {
            countSkipped(report, SkipReason::TooLarge, blob.size);
            continue;
        }

        const std::optional<SkipReason> contentReason = FileFilter::checkContent(blob.content);
        if (contentReason.has_value()) {
            countSkipped(report, contentReason.value(), blob.size);
            continue;
        }

        report.indexed.files += 1;
        report.indexed.bytes += blob.size;
        builder.add(entry.path, std::move(blob.content));
    }

    return CommitIndex{builder.build(), report};
}

}  // namespace codesearch
