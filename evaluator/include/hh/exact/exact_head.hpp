#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>
#include "hh/core/id128.hpp"

namespace hh {

struct ExactHeadDelta {
  std::uint64_t base_generation{0};
  std::uint64_t next_generation{0};
  std::vector<std::uint32_t> removed_slots;
  std::vector<Id128> added_ids;
};

// Bounded exact counter over a replicated, sorted head dictionary. Every
// worker stores its own dictionary and dense local count array; no shared
// memory is assumed. The coordinator can assign compact report slots from the
// common dictionary order distributed at the window barrier.
class ExactHead {
public:
  explicit ExactHead(std::size_t cap = 0) : cap_(cap) {}

  void set_capacity(std::size_t cap) {
    cap_ = cap;
    if (ids_.size() > cap_) {
      ids_.resize(cap_);
      counts_.resize(cap_);
    }
  }

  std::size_t capacity() const { return cap_; }
  std::size_t size() const { return ids_.size(); }
  std::uint64_t generation() const { return generation_; }

  bool contains(const Id128& id) const { return find_slot(id) != ids_.size(); }

  bool admit_and_add(const Id128& id, std::uint32_t w) {
    const auto it = std::lower_bound(ids_.begin(), ids_.end(), id, id_less);
    if (it != ids_.end() && *it == id) {
      counts_[static_cast<std::size_t>(it - ids_.begin())] += w;
      return true;
    }
    if (ids_.size() >= cap_) return false;
    const auto slot = static_cast<std::size_t>(it - ids_.begin());
    ids_.insert(it, id);
    counts_.insert(counts_.begin() + static_cast<std::ptrdiff_t>(slot), w);
    return true;
  }

  std::uint32_t get(const Id128& id) const {
    const std::size_t slot = find_slot(id);
    return slot == ids_.size() ? 0u : counts_[slot];
  }

  void seed(const std::vector<Id128>& ids) {
    install_full(ids, generation_ + 1);
  }

  void install_full(
      const std::vector<Id128>& ids,
      std::uint64_t next_generation) {
    ids_.clear();
    counts_.clear();
    ids_.reserve(std::min(cap_, ids.size()));
    for (const auto& id : ids) {
      if (ids_.size() >= cap_) break;
      if (std::find(ids_.begin(), ids_.end(), id) == ids_.end()) ids_.push_back(id);
    }
    std::sort(ids_.begin(), ids_.end(), id_less);
    counts_.assign(ids_.size(), 0);
    generation_ = next_generation;
  }

  bool apply_delta(
      const ExactHeadDelta& delta,
      std::size_t next_capacity) {
    if (delta.base_generation != generation_
        || delta.next_generation <= delta.base_generation) {
      return false;
    }

    std::vector<Id128> retained;
    retained.reserve(ids_.size());
    std::size_t removal = 0;
    for (std::size_t slot = 0; slot < ids_.size(); ++slot) {
      if (removal < delta.removed_slots.size()
          && delta.removed_slots[removal] == slot) {
        ++removal;
      } else {
        retained.push_back(ids_[slot]);
      }
    }
    if (removal != delta.removed_slots.size()) return false;

    std::vector<Id128> added = delta.added_ids;
    std::sort(added.begin(), added.end(), id_less);
    if (std::adjacent_find(added.begin(), added.end()) != added.end()) {
      return false;
    }

    std::vector<Id128> next;
    next.reserve(retained.size() + added.size());
    std::merge(
        retained.begin(), retained.end(),
        added.begin(), added.end(),
        std::back_inserter(next), id_less);
    if (std::adjacent_find(next.begin(), next.end()) != next.end()
        || next.size() > next_capacity) {
      return false;
    }

    cap_ = next_capacity;
    ids_.swap(next);
    counts_.assign(ids_.size(), 0);
    generation_ = delta.next_generation;
    return true;
  }

  const std::vector<Id128>& ids() const { return ids_; }
  std::uint32_t count_at(std::size_t slot) const { return counts_[slot]; }

  void clear() {
    ids_.clear();
    counts_.clear();
    generation_ = 0;
  }

  void reset_counts() {
    std::fill(counts_.begin(), counts_.end(), 0);
  }

  std::size_t memory_bytes() const {
    return sizeof(*this)
        + ids_.capacity() * sizeof(Id128)
        + counts_.capacity() * sizeof(std::uint32_t);
  }

private:
  static bool id_less(const Id128& lhs, const Id128& rhs) {
    return lhs.b < rhs.b;
  }

  std::size_t find_slot(const Id128& id) const {
    const auto it = std::lower_bound(ids_.begin(), ids_.end(), id, id_less);
    if (it == ids_.end() || !(*it == id)) return ids_.size();
    return static_cast<std::size_t>(it - ids_.begin());
  }

  std::size_t cap_{0};
  std::vector<Id128> ids_;
  std::vector<std::uint32_t> counts_;
  std::uint64_t generation_{0};
};

} // namespace hh
