/*
 * binary_file_stream_xnvme.cpp — kimix::XnvmeBinaryFileStream implementation.
 *
 * This translation unit is compiled by kimix-core's own toolchain (MSVC on
 * Windows) and talks to xnvme exclusively through the C ABI of
 * binary_file_stream_xnvme_shim.h — it cannot include <libxnvme.h>, because
 * xnvme's public headers use GCC-only constructs (__attribute__((packed)),
 * __packed__, clock_gettime/CLOCK_MONOTONIC) that MSVC rejects. The shim is
 * compiled with the same toolchain as xnvme itself, so the struct layouts and
 * the off_t width are checked where the real headers are visible (see the
 * target comment in src/xmake.lua), and it also owns the Windows workaround for
 * xnvme's offset-less file reads (see _win_pread there).
 *
 * Non-throwing, like the rest of the core: the shim reports failure as NULL /
 * negative -errno, path_from_narrow() through its return value and
 * filesystem::file_size() through an std::error_code.
 */
#include "binary_file_stream_xnvme.h"

#include "binary_file_stream_xnvme_shim.h"
#include "stl/filesystem.h"

#include <system_error>
#include <utility>

namespace kimix {

namespace {

/* Non-throwing file size for length(). path_from_narrow() reports bytes the
   native code page cannot represent and the error_code overload of
   filesystem::file_size() reports I/O failures; both collapse to 0 — the same
   "unknown length" the missing-file case of BinaryFileStream produces — instead
   of terminating a process built without C++ exceptions. */
size_t query_file_size(const string &path) noexcept {
    filesystem::path native_path;
    if (!path_from_narrow(path, native_path)) {
        return 0;
    }
    std::error_code ec;
    auto const size = filesystem::file_size(native_path, ec);
    return ec ? 0 : static_cast<size_t>(size);
}

} // namespace

XnvmeBinaryFileStream::XnvmeBinaryFileStream() noexcept : _dev(nullptr), _pos(0), _length(0) {}

XnvmeBinaryFileStream::XnvmeBinaryFileStream(const string &path) noexcept
    : _dev(nullptr), _pos(0), _length(0) {
    // The shim rejects an empty path as well; don't ask xnvme about it.
    if (path.empty()) {
        return;
    }
    // Opened read-only by the shim (xnvme_opts_default() + rdonly, rdwr cleared).
    _dev = static_cast<xnvme_dev *>(kimix_xnvme_stream_open(path.c_str()));
    if (!_dev) {
        return;
    }
    _pos = 0;
    _length = query_file_size(path);
}

XnvmeBinaryFileStream::XnvmeBinaryFileStream(XnvmeBinaryFileStream &&other) noexcept
    : _dev(std::exchange(other._dev, nullptr))
    , _pos(std::exchange(other._pos, uint64_t{0}))
    , _length(std::exchange(other._length, size_t{0})) {}

XnvmeBinaryFileStream &XnvmeBinaryFileStream::operator=(XnvmeBinaryFileStream &&other) noexcept {
    if (this != &other) {
        close();
        _dev = std::exchange(other._dev, nullptr);
        _pos = std::exchange(other._pos, uint64_t{0});
        _length = std::exchange(other._length, size_t{0});
    }
    return *this;
}

XnvmeBinaryFileStream::~XnvmeBinaryFileStream() { close(); }

size_t XnvmeBinaryFileStream::read(std::span<byte> buffer) noexcept {
    return read(buffer.data(), buffer.size());
}

size_t XnvmeBinaryFileStream::read(void *buffer, size_t size) noexcept {
    if (!_dev || !buffer || size == 0) {
        return 0;
    }
    auto const read_bytes = kimix_xnvme_stream_pread(_dev, buffer, size, static_cast<long long>(_pos));
    if (read_bytes <= 0) {
        return 0;
    }
    _pos += static_cast<uint64_t>(read_bytes);
    return static_cast<size_t>(read_bytes);
}

vector<byte> XnvmeBinaryFileStream::read_all() {
    if (!_dev || _length == 0) {
        return {};
    }
    vector<byte> data(_length);
    auto const read_bytes = kimix_xnvme_stream_pread(_dev, data.data(), _length, 0);
    if (read_bytes <= 0) {
        return {};
    }
    data.resize(static_cast<size_t>(read_bytes));
    _pos = static_cast<uint64_t>(read_bytes);
    return data;
}

void XnvmeBinaryFileStream::set_pos(uint64_t pos) noexcept { _pos = pos; }

uint64_t XnvmeBinaryFileStream::position() const noexcept { return _pos; }

size_t XnvmeBinaryFileStream::length() const noexcept { return _length; }

void XnvmeBinaryFileStream::close() noexcept {
    if (_dev) {
        kimix_xnvme_stream_close(_dev);
        _dev = nullptr;
    }
    _pos = 0;
    _length = 0;
}

bool XnvmeBinaryFileStream::is_open() const noexcept { return _dev != nullptr; }

XnvmeBinaryFileStream::operator bool() const noexcept { return _dev != nullptr; }

} // namespace kimix
