/*
 * vertex.h — kimix::graph_db::Vertex: one node of the graph.
 *
 * A Vertex owns
 *   - its identity (_guid),
 *   - an opaque content blob (_content) plus the MD5 cached over it,
 *   - its dependency links, held twice: as GUIDs (the serialized source of
 *     truth that survives a save/load cycle and tolerates cycles) and as
 *     resolved Vertex* sets (the in-memory form used by traversals),
 *   - a per-vertex read/write lock and a dirty flag for incremental save.
 *
 * LOCKING (see graph.h for the full contract)
 *   _mutex (kimix::shared_atomic_mutex, a shared spin lock) guards _content,
 *   _content_hash and _dirty only: content reads take it shared, content writes
 *   take it unique. The dependency vectors/sets are GRAPH structure: they are
 *   guarded by the SHARD lock of this vertex's shard (graph.h —
 *   hash(guid) & (kGraphShardCount - 1)), which a Graph holds while it mutates
 *   them (unique) or reads them (shared). Both the guid lists and the pointer
 *   sets belong to this vertex, hence to this one shard, so the two forms are
 *   always mutually consistent while that lock is held.
 *   The raw depending_guids()/depended_guids()/depending()/depended() accessors
 *   below therefore do NOT lock: they are for a caller that already owns the
 *   structural state — the serializer, which runs under the single-writer rule
 *   ("save never overlaps a structural write"), and Graph's own internals.
 *   Everything else must go through Graph::dependencies()/dependents()/
 *   neighbors(), which take the shard lock themselves.
 *   Lock ordering is always shard-lock before vertex-lock, never the inverse.
 *
 * CACHE-LINE SEPARATION
 *   _mutex is alignas(64): a vertex therefore starts on a cache line and its
 *   lock bitfield + dirty flag occupy a cache line of their own, so writing the
 *   content of vertex A never invalidates the line another thread spins on for
 *   vertex B (no false sharing between adjacent vertices). This makes Vertex
 *   over-aligned (alignof(Vertex) == 64), which is why the class provides
 *   aligned operator new/delete below — the plain mimalloc allocation inherited
 *   from kimix::IOperatorNewBase only guarantees 16-byte alignment.
 *
 * CONTENT MODEL — TODO
 *   _content is deliberately an opaque, already-serialized byte blob. The target
 *   model is the typed property graph of graph_db.md §1: a PropertyValue tagged
 *   union (int64 / double / string / blob / int64[] / double[]), a label set and
 *   a (label, external_id) primary key. That design is intentionally NOT done
 *   here: the container, the codec and the lock discipline must work before the
 *   property model is chosen, and every consumer above this layer only needs a
 *   stable blob with a content hash.
 *
 * Example:
 *   kimix::graph_db::Vertex v;                       // fresh guid, empty content
 *   v.set_content(bytes);                            // unique vertex lock
 *   kimix::vector<std::byte> copy = v.content_copy(); // shared vertex lock
 *   kimix::MD5 const hash = v.content_hash();        // shared vertex lock
 */
#pragma once

#include "graph_db_api.h"

#include <core/guid.h>
#include <core/md5.h>
#include <core/memory.h>
#include <core/shared_atomic_mutex.h>
#include <core/stl/memory.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace kimix::graph_db {

// Cache line the per-vertex atomic state is padded to (see the class comment).
inline constexpr size_t kCacheLineSize = 64;

/*
 * RAII guards for kimix::shared_atomic_mutex. The core type has no lock_guard
 * adapter and cannot be used with std::lock_guard's deduction, so the two
 * adapters below exist: they are time_pedantic-free, never throw, and are the
 * only way the graph_db code takes a vertex lock.
 */
struct KIMIX_GRAPH_DB_API VertexUniqueLock {
    explicit VertexUniqueLock(kimix::shared_atomic_mutex &mutex) noexcept
        : _mutex(mutex) {
        _mutex.lock();
    }
    VertexUniqueLock(VertexUniqueLock const &) = delete;
    VertexUniqueLock &operator=(VertexUniqueLock const &) = delete;
    ~VertexUniqueLock() noexcept {
        _mutex.unlock();
    }

private:
    kimix::shared_atomic_mutex &_mutex;
};

struct KIMIX_GRAPH_DB_API VertexSharedLock {
    explicit VertexSharedLock(kimix::shared_atomic_mutex &mutex) noexcept
        : _mutex(mutex) {
        _mutex.lock_shared();
    }
    VertexSharedLock(VertexSharedLock const &) = delete;
    VertexSharedLock &operator=(VertexSharedLock const &) = delete;
    ~VertexSharedLock() noexcept {
        _mutex.unlock_shared();
    }

private:
    kimix::shared_atomic_mutex &_mutex;
};

class Graph;

/*
 * One graph node. Move-only (the lock is pinned), created through
 * Graph::create_vertex / Graph::add_vertex or directly on the stack/in a
 * unique_ptr by the tests and the later deserializer.
 */
class KIMIX_GRAPH_DB_API Vertex : public kimix::IOperatorNewBase {
    friend class Graph;

public:
    Vertex() noexcept;
    explicit Vertex(kimix::Guid const &guid) noexcept;
    Vertex(kimix::Guid const &guid,
           kimix::span<const std::byte> initial_content);
    Vertex(Vertex const &) = delete;
    Vertex &operator=(Vertex const &) = delete;
    Vertex(Vertex &&) = delete;
    Vertex &operator=(Vertex &&) = delete;
    ~Vertex() noexcept = default;

    // --- identity -----------------------------------------------------------
    [[nodiscard]] kimix::Guid const &guid() const noexcept { return _guid; }
    void set_guid(kimix::Guid const &guid) noexcept { _guid = guid; }

    // --- content (vertex lock: shared read / unique write) ------------------
    // Copy of the content. Takes the vertex lock in SHARED mode, so any number
    // of readers may run while a writer waits for its unique lock.
    void content(kimix::vector<std::byte> &out) const;
    [[nodiscard]] kimix::vector<std::byte> content_copy() const;
    [[nodiscard]] size_t content_size() const;
    // MD5 cached at the last content write (recomputed under the unique lock).
    [[nodiscard]] kimix::MD5 content_hash() const;
    // Replace the content, recompute the cached hash and mark the vertex dirty.
    void set_content(kimix::span<const std::byte> content);
    // Recompute the cached hash from the current content and mark it dirty.
    // Used by the codec/loader after a record is materialized.
    void touch();
    void mark_dirty() noexcept { _dirty.store(true, std::memory_order_release); }
    void clear_dirty() noexcept { _dirty.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_dirty() const noexcept {
        return _dirty.load(std::memory_order_acquire);
    }

    // --- dependencies (GRAPH structure: caller holds this vertex's SHARD lock,
    // or runs under the single-writer rule; see the LOCKING comment above) ----
    // Epoch-2 sizing hook: reserve room for `outgoing` + `incoming` edges in the
    // four containers those edges live in. A bulk builder (the loader's link
    // pass, Graph::connect's caller) knows its vertex's final degree up front, so
    // reserving once removes the per-edge growth reallocation - and the 16-slot
    // minimum `kimix::unordered_set`, `kimix::HashMap` and `kimix::vector`
    // each start with. Never shrinks: it only ever raises capacity, so calling it
    // with a smaller count than the current capacity is a no-op, and it never
    // touches the elements. Same locking rule as the accessors above: graph
    // structure, so caller holds this vertex's SHARD lock (unique).
    void reserve_links(size_t outgoing, size_t incoming);
    // GUIDs this vertex depends on (outgoing edges), serialized form.
    [[nodiscard]] kimix::vector<kimix::Guid> const &depending_guids() const noexcept {
        return _depending_guids;
    }
    // GUIDs of the vertices that depend on this one (reverse edges).
    [[nodiscard]] kimix::vector<kimix::Guid> const &depended_guids() const noexcept {
        return _depended_guids;
    }
    [[nodiscard]] size_t depending_count() const noexcept { return _depending_guids.size(); }
    [[nodiscard]] size_t depended_count() const noexcept { return _depended_guids.size(); }
    [[nodiscard]] bool depends_on(kimix::Guid const &guid) const noexcept;
    // Resolved in-memory edges, as copies (the pointer sets themselves stay
    // private so only a Graph can grow or shrink them).
    [[nodiscard]] kimix::vector<Vertex *> depending() const;
    [[nodiscard]] kimix::vector<Vertex *> depended() const;

    // --- locking (exposed for callers that already own a Vertex*, e.g. the
    // serializer, which wants a long-lived content snapshot) -----------------
    void lock_shared() const noexcept { _mutex.lock_shared(); }
    void unlock_shared() const noexcept { _mutex.unlock_shared(); }
    void lock_unique() noexcept { _mutex.lock(); }
    void unlock_unique() noexcept { _mutex.unlock(); }

    // Per-vertex lock, exposed for the tests' cache-line sanity checks.
    [[nodiscard]] kimix::shared_atomic_mutex const &mutex() const noexcept { return _mutex; }

    // --- aligned allocation -------------------------------------------------
    // Vertex is alignas(kCacheLineSize), i.e. over-aligned, so the compiler
    // selects the align_val_t overloads of operator new/delete for every
    // `new Vertex` / `std::make_unique<Vertex>()`; the plain forms are declared
    // too so no path falls back to the global aligned operators (which are not
    // mimalloc). Both flavours route through mi_malloc_aligned / mi_free, the
    // same heap family kimix::IOperatorNewBase uses.
    static void *operator new(size_t size) noexcept;
    static void *operator new(size_t size, std::align_val_t align) noexcept;
    static void operator delete(void *ptr) noexcept;
    static void operator delete(void *ptr, size_t size) noexcept;
    static void operator delete(void *ptr, std::align_val_t align) noexcept;
    static void operator delete(void *ptr, size_t size, std::align_val_t align) noexcept;

private:
    // Graph-only mutators of the serialized dep lists. A Graph calls these
    // while holding the graph unique lock and this vertex's unique lock.
    void _add_depending(Vertex *other);
    // Insert-if-absent in ONE hash operation (the probe and the insert are the
    // same lookup), returning whether the edge was new. This is what
    // Graph::_connect_resolved_locked uses, so an "edge already present" costs one
    // hash lookup instead of a find() plus an insert().
    bool _add_depending_if_absent(Vertex *other);
    void _remove_depending(Vertex *other);
    void _add_depended(Vertex *other);
    void _remove_depended(Vertex *other);
    // Drop every edge of this vertex without touching the neighbours (the
    // cascade in Graph::remove_vertex does the neighbour side).
    void _clear_links() noexcept;

    kimix::Guid _guid;
    kimix::MD5 _content_hash;             // guarded by _mutex
    kimix::vector<std::byte> _content;    // guarded by _mutex
    kimix::vector<kimix::Guid> _depending_guids; // graph lock
    kimix::vector<kimix::Guid> _depended_guids;  // graph lock
    kimix::unordered_set<Vertex *> _depending;   // graph lock + vertex locks
    kimix::unordered_set<Vertex *> _depended;    // graph lock + vertex locks

    // Dedicated cache line: the lock bitfield and the dirty flag live alone so
    // neighbouring vertices never invalidate each other's line (see the class
    // comment). The remaining members above are cold relative to this line.
    alignas(kCacheLineSize) mutable kimix::shared_atomic_mutex _mutex;
    std::atomic<bool> _dirty{true};
};

} // namespace kimix::graph_db