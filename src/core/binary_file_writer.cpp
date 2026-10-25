/*
 * binary_file_writer.cpp — kimix::BinaryFileWriter + kimix::DeferredBinaryWriter
 * implementation.
 *
 * Compiled by kimix-core's own toolchain (MSVC on Windows); every xnvme call goes
 * through the C ABI of binary_file_stream_xnvme_shim.h (see
 * binary_file_stream_xnvme.h for why <libxnvme.h> cannot be included here).
 *
 * The shim owns both platform transports: xnvme_file_pwrite() elsewhere, and a
 * per-request positioned WriteFile() on Windows, where the shim already issues
 * its own reads for the same reason (the vendored xnvme revision drops the read
 * offset there).
 *
 * DeferredBinaryWriter shares the batching strategy of
 * DeferredBinaryFileStream: a sorted index permutation groups the commands per
 * path — no hash map, no kimix::hash<kimix::string> needed — so each path is
 * opened, written, synced and closed exactly once per commit().
 *
 * Non-throwing, like the rest of the core: the shim reports failure as NULL
 * handles and negative -errno, which become "that command did not complete".
 */
#include "binary_file_writer.h"

#include "binary_file_stream_xnvme_shim.h"
#include "stl/pdqsort.h"

#include <utility>

namespace kimix {

BinaryFileWriter::BinaryFileWriter() noexcept : _dev(nullptr), _pos(0) {}

BinaryFileWriter::BinaryFileWriter(const string &path, bool truncate) noexcept
    : _dev(nullptr), _pos(0) {
    // The shim rejects an empty path as well; don't ask xnvme about it.
    if (path.empty()) {
        return;
    }
    _dev = static_cast<xnvme_dev *>(kimix_xnvme_open_write(path.c_str(), truncate ? 1 : 0));
}

BinaryFileWriter::~BinaryFileWriter() { close(); }

BinaryFileWriter::BinaryFileWriter(BinaryFileWriter &&other) noexcept
    : _dev(std::exchange(other._dev, nullptr)), _pos(std::exchange(other._pos, uint64_t{0})) {}

BinaryFileWriter &BinaryFileWriter::operator=(BinaryFileWriter &&other) noexcept {
    if (this != &other) {
        close();
        _dev = std::exchange(other._dev, nullptr);
        _pos = std::exchange(other._pos, uint64_t{0});
    }
    return *this;
}

size_t BinaryFileWriter::write(const void *buffer, size_t size) noexcept {
    if (!_dev || !buffer || size == 0) {
        return 0;
    }

    long long const offset = static_cast<long long>(_pos);
    if (kimix_xnvme_pwrite(_dev, buffer, size, offset) != 0) {
        return 0;
    }

    _pos += static_cast<uint64_t>(size);
    return size;
}

size_t BinaryFileWriter::write(std::span<const byte> buffer) noexcept {
    return write(buffer.data(), buffer.size());
}

void BinaryFileWriter::set_pos(uint64_t pos) noexcept { _pos = pos; }

uint64_t BinaryFileWriter::position() const noexcept { return _pos; }

bool BinaryFileWriter::sync() noexcept {
    if (!_dev) {
        return false;
    }
    return kimix_xnvme_sync(_dev) == 0;
}

void BinaryFileWriter::close() noexcept {
    if (_dev) {
        kimix_xnvme_stream_close(_dev);
        _dev = nullptr;
    }
    _pos = 0;
}

bool BinaryFileWriter::is_open() const noexcept { return _dev != nullptr; }

BinaryFileWriter::operator bool() const noexcept { return _dev != nullptr; }

DeferredBinaryWriter::DeferredBinaryWriter() noexcept = default;

DeferredBinaryWriter::~DeferredBinaryWriter() = default;

DeferredBinaryWriter::DeferredBinaryWriter(DeferredBinaryWriter &&other) noexcept
    : _commands(std::move(other._commands)) {}

DeferredBinaryWriter &DeferredBinaryWriter::operator=(DeferredBinaryWriter &&other) noexcept {
    if (this != &other) {
        _commands = std::move(other._commands);
    }
    return *this;
}

void DeferredBinaryWriter::add_write(const string &path, uint64_t offset,
                                     vector<std::byte> &&data) {
    // The payload is moved into the command, so it stays alive (and unobservable
    // by the caller) until commit()/clear() releases it.
    _commands.push_back(Command{path, offset, std::move(data)});
}

size_t DeferredBinaryWriter::pending_count() const noexcept { return _commands.size(); }

void DeferredBinaryWriter::clear() noexcept { _commands.clear(); }

size_t DeferredBinaryWriter::commit() {
    if (_commands.empty()) {
        return 0;
    }

    // Indices ordered by path: each distinct path is opened exactly once below.
    vector<size_t> order(_commands.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    kimix::sort(order.begin(), order.end(), [this](size_t lhs, size_t rhs) {
        return _commands[lhs].path < _commands[rhs].path;
    });

    size_t written = 0;
    size_t group = 0;
    while (group < order.size()) {
        const string &path = _commands[order[group]].path;
        size_t next = group;
        while (next < order.size() && _commands[order[next]].path == path) {
            ++next;
        }

        // truncate == 0: a file may already hold content that the offset writes
        // must not destroy.
        auto *dev = static_cast<xnvme_dev *>(kimix_xnvme_open_write(path.c_str(), 0));
        if (dev) {
            for (size_t slot = group; slot < next; ++slot) {
                const Command &command = _commands[order[slot]];
                // Writes straight out of the payload's storage — no copy.
                if (kimix_xnvme_pwrite(dev, command.data.data(), command.data.size(),
                                       static_cast<long long>(command.offset)) == 0) {
                    written += 1;
                }
            }
            // Flush everything this file received before closing it.
            kimix_xnvme_sync(dev);
            kimix_xnvme_stream_close(dev);
        }

        group = next;
    }

    // commit() always consumes the recorded commands, releasing their payloads.
    _commands.clear();

    return written;
}

} // namespace kimix
