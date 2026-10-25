/*
 * deferred_binary_file_stream.cpp — kimix::DeferredBinaryFileStream
 * implementation.
 *
 * This translation unit is compiled by kimix-core's own toolchain (MSVC on
 * Windows) and talks to xnvme exclusively through the C ABI of
 * binary_file_stream_xnvme_shim.h — it cannot include <libxnvme.h>, because
 * xnvme's public headers use GCC-only constructs (see the header comment in
 * binary_file_stream_xnvme.h). The shim owns both platform transports of a
 * batched commit: a single xnvme command-queue on Linux (whose only enabled
 * async backend for a regular file is xnvme's emulated one, so the commands are
 * completed serially while the queue is drained) and a sequence of positioned
 * ReadFile() calls on Windows (where xnvme's own positioned read path is
 * broken — see _win_pread in the shim). Neither transport runs the reads
 * concurrently; the batching is about grouping and amortizing, not parallelism.
 *
 * This file's whole job is the GROUPING: one batched reader per distinct path,
 * so a file is opened, read and closed exactly once per commit() no matter how
 * many positioned reads were recorded for it. The grouping is done through a
 * sorted index permutation rather than a hash map: it keeps the command order
 * irrelevant, needs no hashing of kimix::string (which has no kimix::hash
 * specialization) and stays cheap for the large batches this class exists for.
 *
 * Non-throwing, like the rest of the core: the shim reports failure through NULL
 * handles and negative -errno, both of which become "that command did not
 * complete" — never an exception or a process abort.
 */
#include "deferred_binary_file_stream.h"

#include "binary_file_stream_xnvme_shim.h"
#include "stl/pdqsort.h"

#include <utility>

namespace kimix {

DeferredBinaryFileStream::DeferredBinaryFileStream() noexcept = default;

DeferredBinaryFileStream::~DeferredBinaryFileStream() = default;

DeferredBinaryFileStream::DeferredBinaryFileStream(DeferredBinaryFileStream &&other) noexcept
    : _commands(std::move(other._commands)) {}

DeferredBinaryFileStream &DeferredBinaryFileStream::operator=(
    DeferredBinaryFileStream &&other) noexcept {
    if (this != &other) {
        _commands = std::move(other._commands);
    }
    return *this;
}

void DeferredBinaryFileStream::add_read(const string &path, uint64_t offset, size_t size,
                                       void *dst) {
    _commands.push_back(Command{path, offset, size, dst});
}

size_t DeferredBinaryFileStream::pending_count() const noexcept { return _commands.size(); }

void DeferredBinaryFileStream::clear() noexcept { _commands.clear(); }

size_t DeferredBinaryFileStream::commit() {
    if (_commands.empty()) {
        return 0;
    }

    // Indices ordered by path: every path's commands end up adjacent, so each
    // distinct path is opened exactly once below.
    vector<size_t> order(_commands.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    kimix::sort(order.begin(), order.end(), [this](size_t lhs, size_t rhs) {
        return _commands[lhs].path < _commands[rhs].path;
    });

    size_t completed = 0;
    size_t group = 0;
    while (group < order.size()) {
        const string &path = _commands[order[group]].path;
        size_t next = group;
        while (next < order.size() && _commands[order[next]].path == path) {
            ++next;
        }

        // One reader (= one read-only open) for every command of this path. A
        // NULL reader means the path could not be opened; its commands count as
        // failures and the remaining groups still run.
        auto *reader = kimix_xnvme_breader_open(path.c_str());
        if (reader) {
            for (size_t slot = group; slot < next; ++slot) {
                const Command &command = _commands[order[slot]];
                // A rejected record (NULL 'dst') simply never completes.
                kimix_xnvme_breader_add(reader, command.dst, command.offset, command.size);
            }
            completed += kimix_xnvme_breader_commit(reader);
            kimix_xnvme_breader_close(reader);
        }

        group = next;
    }

    // commit() always consumes the recorded commands.
    _commands.clear();

    return completed;
}

} // namespace kimix
