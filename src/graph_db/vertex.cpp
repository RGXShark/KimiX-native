/*
 * vertex.cpp — content access, dependency bookkeeping and aligned allocation
 * for kimix::graph_db::Vertex. See vertex.h for the API contract, the lock
 * discipline and the cache-line argument.
 *
 * Every method here assumes the caller already holds the right lock: the
 * content accessors take the vertex lock themselves, while the dependency
 * mutators are called by Graph under the shard UNIQUE lock of this vertex's
 * shard (documented on each private helper in vertex.h). Nothing in this file
 * aborts, throws or allocates on a failure path.
 */
#include "vertex.h"

#include <mimalloc.h>

#include <algorithm>
#include <utility>

namespace kimix::graph_db {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Vertex::Vertex() noexcept
    : _guid{true} {
}

Vertex::Vertex(kimix::Guid const &guid) noexcept
    : _guid(guid) {
}

Vertex::Vertex(kimix::Guid const &guid,
               kimix::span<const std::byte> initial_content)
    : _guid(guid) {
    set_content(initial_content);
}

// ---------------------------------------------------------------------------
// Content (vertex lock: shared for reads, unique for writes)
// ---------------------------------------------------------------------------

void Vertex::content(kimix::vector<std::byte> &out) const {
    VertexSharedLock guard(const_cast<kimix::shared_atomic_mutex &>(_mutex));
    out = _content;
}

kimix::vector<std::byte> Vertex::content_copy() const {
    VertexSharedLock guard(const_cast<kimix::shared_atomic_mutex &>(_mutex));
    return _content;
}

size_t Vertex::content_size() const {
    VertexSharedLock guard(const_cast<kimix::shared_atomic_mutex &>(_mutex));
    return _content.size();
}

kimix::MD5 Vertex::content_hash() const {
    VertexSharedLock guard(const_cast<kimix::shared_atomic_mutex &>(_mutex));
    return _content_hash;
}

void Vertex::set_content(kimix::span<const std::byte> content) {
    VertexUniqueLock guard(_mutex);
    _content.assign(content.begin(), content.end());
    _content_hash = kimix::MD5{kimix::span<uint8_t const>{
        reinterpret_cast<uint8_t const *>(_content.data()), _content.size()}};
    _dirty.store(true, std::memory_order_release);
}

void Vertex::touch() {
    VertexUniqueLock guard(_mutex);
    _content_hash = kimix::MD5{kimix::span<uint8_t const>{
        reinterpret_cast<uint8_t const *>(_content.data()), _content.size()}};
    _dirty.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------

bool Vertex::depends_on(kimix::Guid const &guid) const noexcept {
    return std::find(_depending_guids.begin(), _depending_guids.end(), guid) !=
           _depending_guids.end();
}

void Vertex::reserve_links(size_t outgoing, size_t incoming) {
    // Only ever grows: each reserve() is itself a no-op below the current
    // capacity, so a second call with a smaller count cannot shrink a container
    // or move an element out of it.
    _depending.reserve(outgoing);
    _depending_guids.reserve(outgoing);
    _depended.reserve(incoming);
    _depended_guids.reserve(incoming);
}

kimix::vector<Vertex *> Vertex::depending() const {
    kimix::vector<Vertex *> out;
    out.reserve(_depending.size());
    for (auto *vertex : _depending) {
        out.push_back(vertex);
    }
    return out;
}

kimix::vector<Vertex *> Vertex::depended() const {
    kimix::vector<Vertex *> out;
    out.reserve(_depended.size());
    for (auto *vertex : _depended) {
        out.push_back(vertex);
    }
    return out;
}

void Vertex::_add_depending(Vertex *other) {
    if (other == nullptr) {
        return;
    }
    if (_depending.insert(other).second) {
        _depending_guids.push_back(other->guid());
    }
}

bool Vertex::_add_depending_if_absent(Vertex *other) {
    if (other == nullptr) {
        return false;
    }
    if (!_depending.insert(other).second) {
        return false;
    }
    _depending_guids.push_back(other->guid());
    return true;
}

void Vertex::_remove_depending(Vertex *other) {
    if (other == nullptr) {
        return;
    }
    if (_depending.erase(other) == 0) {
        return;
    }
    auto const &guid = other->guid();
    auto it = std::find(_depending_guids.begin(), _depending_guids.end(), guid);
    if (it != _depending_guids.end()) {
        _depending_guids.erase(it);
    }
}

void Vertex::_add_depended(Vertex *other) {
    if (other == nullptr) {
        return;
    }
    if (_depended.insert(other).second) {
        _depended_guids.push_back(other->guid());
    }
}

void Vertex::_remove_depended(Vertex *other) {
    if (other == nullptr) {
        return;
    }
    if (_depended.erase(other) == 0) {
        return;
    }
    auto const &guid = other->guid();
    auto it = std::find(_depended_guids.begin(), _depended_guids.end(), guid);
    if (it != _depended_guids.end()) {
        _depended_guids.erase(it);
    }
}

void Vertex::_clear_links() noexcept {
    _depending.clear();
    _depended.clear();
    _depending_guids.clear();
    _depended_guids.clear();
}

// ---------------------------------------------------------------------------
// Over-aligned allocation (alignof(Vertex) == kCacheLineSize)
// ---------------------------------------------------------------------------

void *Vertex::operator new(size_t size) noexcept {
    return mi_malloc_aligned(size, kCacheLineSize);
}

void *Vertex::operator new(size_t size, std::align_val_t align) noexcept {
    auto const requested = static_cast<size_t>(align);
    return mi_malloc_aligned(size, requested > kCacheLineSize ? requested : kCacheLineSize);
}

void Vertex::operator delete(void *ptr) noexcept {
    // mi_free releases aligned blocks allocated by mi_malloc_aligned too.
    mi_free(ptr);
}

void Vertex::operator delete(void *ptr, size_t) noexcept {
    mi_free(ptr);
}

void Vertex::operator delete(void *ptr, std::align_val_t) noexcept {
    mi_free(ptr);
}

void Vertex::operator delete(void *ptr, size_t, std::align_val_t) noexcept {
    mi_free(ptr);
}

} // namespace kimix::graph_db