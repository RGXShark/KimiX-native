// Test for core/hash_map.h (kimix::HashMap, kimix::SmallTreeMap).
// This test covers:
// - try_emplace / emplace / force_emplace insert and find semantics
// - Index key()/value() access, get_index(), find() on missing keys
// - remove() by key, by Index and by Iterator; clear()
// - Const iteration in insertion order, including across growth (reserve)
// - Move construction / assignment (deep move — the source is emptied)
// - String keys with an explicit hasher (kimix::string_hash)
// - The V=void set flavour (HashSetIndex get()/operator*)

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "hash_map_basic_insert_find"_test = [] {
        kimix::HashMap<int, int> map;
        expect(map.empty());
        auto [idx, inserted] = map.try_emplace(1, 10);
        expect(inserted);
        expect(static_cast<bool>(idx));
        expect(eq(idx.key(), 1));
        expect(eq(idx.value(), 10));
        auto [idx2, inserted2] = map.try_emplace(1, 99);
        expect(!inserted2) << "duplicate key must not insert";
        expect(eq(idx2.value(), 10)) << "try_emplace must not overwrite";
        expect(eq(map.size(), size_t{1}));
        expect(static_cast<bool>(map.find(1)));
        expect(!map.find(2));
    };

    "hash_map_emplace_force_emplace"_test = [] {
        kimix::HashMap<int, kimix::string> map;
        auto i = map.emplace(7, "seven");
        expect(eq(i.value(), kimix::string{"seven"}));
        // force_emplace overwrites the value of an existing key.
        auto j = map.force_emplace(7, "SEVEN");
        expect(eq(map.size(), size_t{1}));
        expect(eq(j.value(), kimix::string{"SEVEN"}));
        // ...and inserts a new key like try_emplace does.
        map.force_emplace(8, "eight");
        expect(eq(map.size(), size_t{2}));
    };

    "hash_map_remove"_test = [] {
        kimix::HashMap<int, int> map;
        for (int i = 0; i < 100; ++i) map.try_emplace(i, i * 2);
        expect(eq(map.size(), size_t{100}));
        map.remove(50);
        expect(!map.find(50));
        expect(eq(map.size(), size_t{99}));
        // Remove by Index.
        auto idx = map.find(51);
        expect(static_cast<bool>(idx));
        map.remove(idx);
        expect(!map.find(51));
        // Remove by Iterator.
        map.remove(map.begin());
        expect(eq(map.size(), size_t{97}));
        // Missing key is a no-op.
        map.remove(424242);
        expect(eq(map.size(), size_t{97}));
        map.clear();
        expect(map.empty());
        expect(!map.find(0));
    };

    "hash_map_iteration_insertion_order"_test = [] {
        kimix::HashMap<int, int> map(256);// capacity-hint ctor
        for (int i = 0; i < 64; ++i) map.try_emplace(i, i);
        int expected_key = 0;
        int sum = 0;
        for (auto const &kv : map) {
            expect(eq(kv.first, expected_key));
            sum += kv.second;
            ++expected_key;
        }
        expect(eq(expected_key, 64));
        expect(eq(sum, 64 * 63 / 2));
    };

    "hash_map_growth_and_reserve"_test = [] {
        kimix::HashMap<int, int> map;
        map.reserve(1024);
        expect(ge(map.capacity(), size_t(1024)));
        size_t cap_before = map.capacity();
        for (int i = 0; i < 1000; ++i) map.try_emplace(i, i);
        expect(eq(map.size(), size_t{1000}));
        expect(ge(map.capacity(), cap_before));
        // Every element still findable after growth.
        bool all_found = true;
        for (int i = 0; i < 1000; ++i) {
            auto idx = map.find(i);
            if (!idx || idx.value() != i) all_found = false;
        }
        expect(all_found) << "all 1000 elements must survive rehashing";
        // Insertion order preserved across growth.
        int first_key = map.begin()->first;
        expect(eq(first_key, 0));
    };

    "hash_map_move_semantics"_test = [] {
        kimix::HashMap<int, int> map;
        for (int i = 0; i < 200; ++i) map.try_emplace(i, i + 1);
        kimix::HashMap<int, int> moved{std::move(map)};
        expect(eq(moved.size(), size_t{200}));
        expect(map.empty()) << "the moved-from map must be empty";
        bool all_ok = true;
        for (int i = 0; i < 200; ++i) {
            auto idx = moved.find(i);
            if (!idx || idx.value() != i + 1) all_ok = false;
        }
        expect(all_ok) << "deep move must preserve every element";
        // Move assignment keeps working afterwards.
        kimix::HashMap<int, int> assigned;
        assigned = std::move(moved);
        expect(eq(assigned.size(), size_t{200}));
        expect(static_cast<bool>(assigned.find(199)));
    };

    "hash_map_string_keys"_test = [] {
        kimix::HashMap<kimix::string, int, kimix::string_hash> map;
        map.try_emplace(kimix::string{"alpha"}, 1);
        map.try_emplace(kimix::string{"beta"}, 2);
        expect(eq(map.size(), size_t{2}));
        expect(static_cast<bool>(map.find(kimix::string{"alpha"})));
        expect(eq(map.find(kimix::string{"beta"}).value(), 2));
        map.remove(kimix::string{"alpha"});
        expect(!map.find(kimix::string{"alpha"}));
        expect(eq(map.size(), size_t{1}));
    };

    "hash_map_set_flavour"_test = [] {
        kimix::HashMap<int> set;
        auto idx = set.try_emplace(42);
        expect(idx.second);
        expect(static_cast<bool>(set.find(42)));
        expect(eq(set.find(42).get(), 42));
        expect(eq(*set.find(42), 42));
        expect(eq(set.size(), size_t{1}));
        set.remove(42);
        expect(set.empty());
    };

    "hash_map_get_index"_test = [] {
        kimix::HashMap<int, int> map;
        map.try_emplace(5, 50);
        auto ite = map.begin();
        auto idx = map.get_index(ite);
        expect(static_cast<bool>(idx));
        expect(eq(idx.key(), 5));
        expect(eq(idx.value(), 50));
    };
}
