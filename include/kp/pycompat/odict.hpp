// An insertion-ordered map with Python dict semantics.
//
// std::map iterates in KEY order and std::unordered_map in no order; a Python dict iterates in
// INSERTION order, and that order is load-bearing here more often than it looks: risklimit folds
// q*p in the positions dict's order (summation order is part of the answer), `json.dumps` writes
// keys in it, and "first market that ..." loops pick by it. Anything ported from a dict that is
// ever iterated uses this.
//
// Semantics copied from dict: assigning an existing key replaces the value IN PLACE (position is
// kept); erase removes it; update() is a sequence of assignments in the other map's order.
#pragma once
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kp::pycompat {

template <class K, class V, class Hash = std::hash<K>>
class ODict {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    V& operator[](const K& k) {
        auto it = idx_.find(k);
        if (it != idx_.end()) return items_[it->second].second;
        idx_.emplace(k, items_.size());
        items_.emplace_back(k, V{});
        return items_.back().second;
    }
    void set(const K& k, V v) { (*this)[k] = std::move(v); }

    const V* find(const K& k) const {
        auto it = idx_.find(k);
        return it == idx_.end() ? nullptr : &items_[it->second].second;
    }
    V* find(const K& k) {
        auto it = idx_.find(k);
        return it == idx_.end() ? nullptr : &items_[it->second].second;
    }
    bool contains(const K& k) const { return idx_.count(k) != 0; }

    bool erase(const K& k) {
        auto it = idx_.find(k);
        if (it == idx_.end()) return false;
        const std::size_t pos = it->second;
        idx_.erase(it);
        items_.erase(items_.begin() + (std::ptrdiff_t)pos);
        for (std::size_t i = pos; i < items_.size(); ++i) idx_[items_[i].first] = i;
        return true;
    }

    void update(const ODict& o) { for (const auto& kv : o) (*this)[kv.first] = kv.second; }
    void clear() { items_.clear(); idx_.clear(); }

    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

private:
    std::vector<value_type> items_;
    std::unordered_map<K, std::size_t, Hash> idx_;
};

}  // namespace kp::pycompat
