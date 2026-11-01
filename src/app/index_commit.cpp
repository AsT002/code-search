#include "codesearch/app/index_commit.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace codesearch {

namespace {

void countSkipped(IngestReport& report, SkipReason reason, std::uint64_t bytes) {
    FileCount& count = report.skipped[reason];  // map creates a zero count if missing
    count.files += 1;
    count.bytes += bytes;
}

// About how much content goes to the pool in one task.
constexpr std::size_t kBatchBytes = std::size_t{1} << 20U;  // 1 MiB

// At most this much content can be read from Git but not yet added to the
// index: 2 batches per thread of a full pool. It bounds the extra memory a
// parallel build uses, beyond the index itself.
constexpr std::size_t kMaxBytesInFlight = std::size_t{16} << 20U;  // 16 MiB

// Adds files to an IndexBuilder in the order they're given, while their
// trigrams are worked out on a thread pool. Without a pool, it adds each
// file straight away.
class ParallelAdder {
public:
    ParallelAdder(IndexBuilder& builder, ThreadPool* pool) : builder_(builder), pool_(pool) {}

    // Waits for tasks still running, since they use the batches.
    ~ParallelAdder() {
        std::unique_lock lock(mutex_);
        batchDone_.wait(lock, [this] {
            return std::all_of(inFlight_.begin(), inFlight_.end(), [](const auto& batch) {
                return batch->done;
            });
        });
    }

    ParallelAdder(const ParallelAdder&) = delete;
    ParallelAdder& operator=(const ParallelAdder&) = delete;
    ParallelAdder(ParallelAdder&&) = delete;
    ParallelAdder& operator=(ParallelAdder&&) = delete;

    void add(std::string path, std::string content) {
        if (pool_ == nullptr) {
            builder_.add(std::move(path), std::move(content));
            return;
        }
        if (!current_) {
            current_ = std::make_unique<Batch>();
        }
        current_->bytes += content.size();
        current_->paths.push_back(std::move(path));
        current_->contents.push_back(std::move(content));
        if (current_->bytes >= kBatchBytes) {
            send();
        }
    }

    // Adds everything still waiting. Call it once, after the last add.
    void finish() {
        send();
        while (!inFlight_.empty()) {
            addOldest();
        }
    }

private:
    struct Batch {
        std::vector<std::string> paths;
        std::vector<std::string> contents;
        std::vector<std::vector<Trigram>> trigrams;  // filled in on the pool
        std::size_t bytes = 0;
        std::exception_ptr error;
        bool done = false;  // guarded by mutex_
    };

    // Hands the current batch to the pool. Waits first if too much content
    // is already waiting.
    void send() {
        if (!current_) {
            return;
        }
        while (!inFlight_.empty() && bytesInFlight_ + current_->bytes > kMaxBytesInFlight) {
            addOldest();
        }
        Batch* batch = current_.get();
        inFlight_.push_back(std::move(current_));
        bytesInFlight_ += batch->bytes;
        try {
            pool_->submit([this, batch] {
                try {
                    batch->trigrams.reserve(batch->contents.size());
                    for (const std::string& content : batch->contents) {
                        batch->trigrams.push_back(extractTrigrams(content));
                    }
                } catch (...) {  // a task must not throw; addOldest passes it on
                    batch->error = std::current_exception();
                }
                // Signal under the lock: once it's let go, the adder may be
                // destroyed, and this task must not touch it after that.
                const std::scoped_lock lock(mutex_);
                batch->done = true;
                batchDone_.notify_all();
            });
        } catch (...) {
            // The task never got queued, so the batch would never be done.
            // Take it back out, or the destructor would wait for it forever.
            bytesInFlight_ -= batch->bytes;
            inFlight_.pop_back();
            throw;
        }
    }

    // Waits for the oldest batch and adds its files, in order.
    void addOldest() {
        Batch& batch = *inFlight_.front();
        {
            std::unique_lock lock(mutex_);
            batchDone_.wait(lock, [&] {
                return batch.done;
            });
        }
        if (batch.error) {
            std::rethrow_exception(batch.error);
        }
        for (std::size_t i = 0; i < batch.paths.size(); ++i) {
            builder_.add(std::move(batch.paths[i]), std::move(batch.contents[i]),
                         batch.trigrams[i]);
        }
        bytesInFlight_ -= batch.bytes;
        inFlight_.pop_front();
    }

    IndexBuilder& builder_;
    ThreadPool* pool_;
    std::unique_ptr<Batch> current_;               // being filled, not sent yet
    std::deque<std::unique_ptr<Batch>> inFlight_;  // sent, oldest first
    std::size_t bytesInFlight_ = 0;
    std::mutex mutex_;
    std::condition_variable batchDone_;  // signaled each time a task ends
};

}  // namespace

CommitIndex indexCommit(const GitRepo& repo, const ObjectId& commit, const FileFilter& filter,
                        ThreadPool* pool) {
    IndexBuilder builder;
    IngestReport report;
    BlobStream blobs(repo);
    ParallelAdder adder(builder, pool);

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
        adder.add(entry.path, std::move(blob.content));
    }

    adder.finish();
    return CommitIndex{builder.build(), report};
}

}  // namespace codesearch
