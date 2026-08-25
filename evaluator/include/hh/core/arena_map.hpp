#pragma once
#include "hh/core/id128.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hh {

class ArenaMap {
public:
  struct Ref { std::uint32_t off; std::uint32_t len; };
  struct MemoryBreakdown {
    std::size_t live_entries{0};
    std::size_t slot_capacity{0};
    std::size_t slot_bytes{0};
    std::size_t state_bytes{0};
    std::size_t raw_live_bytes{0};
    std::size_t raw_arena_bytes{0};
    std::size_t garbage_bytes{0};
    std::size_t total_bytes{0};
  };

  Ref intern(const std::string& bytes) {
    Ref r{static_cast<std::uint32_t>(arena_.size()), static_cast<std::uint32_t>(bytes.size())};
    arena_.insert(arena_.end(), bytes.begin(), bytes.end());
    return r;
  }
  void bind(const Id128& id, Ref r) {
    ensure_insert_capacity();
    const std::size_t existing = find_slot(id);
    if (existing != NPOS) {
      slots_[existing].ref = r;
      return;
    }
    insert_new(id, r);
  }
  void bind_bytes(const Id128& id, const std::string& bytes) {
    const std::size_t existing = find_slot(id);
    if (existing != NPOS) {
      const Ref& old = slots_[existing].ref;
      if (old.len == bytes.size() &&
          old.off + old.len <= arena_.size() &&
          std::memcmp(arena_.data() + old.off, bytes.data(), old.len) == 0) {
        return; // already bound to identical key bytes
      }
      garbage_bytes_ += old.len;
      slots_[existing].ref = intern(bytes);
      compact_if_needed();
      return;
    }
    bind(id, intern(bytes));
    compact_if_needed();
  }

  void erase(const Id128& id) {
    const std::size_t index = find_slot(id);
    if (index == NPOS) return;
    garbage_bytes_ += slots_[index].ref.len;
    set_state(index, State::tombstone);
    --size_;
    ++tombstones_;
    compact_if_needed();
  }

  bool lookup(const Id128& id, std::string& out) const {
    const std::size_t index = find_slot(id);
    if (index == NPOS) return false;
    const Ref r = slots_[index].ref;
    out.assign(arena_.data()+r.off, arena_.data()+r.off+r.len);
    return true;
  }

  void clear() {
    // A window reset is also the adaptation boundary. Retaining the largest
    // table ever observed would make a later low-capacity window pay the
    // high-water mark of an earlier one. Keep enough storage for the live set
    // that just completed, which preserves reuse for a stable regime while
    // allowing memory to contract after the controller sizes down.
    const std::size_t target_slots = size_ == 0
        ? MIN_CAPACITY
        : next_power_of_two(std::max(
              MIN_CAPACITY, (size_ * 20 + 16) / 17));
    if (slots_.size() > target_slots) {
      slots_.assign(target_slots, {});
      slots_.shrink_to_fit();
      states_.assign((target_slots + 3) / 4, std::uint8_t{0});
      states_.shrink_to_fit();
    } else {
      std::fill(states_.begin(), states_.end(), std::uint8_t{0});
    }

    const std::size_t target_arena =
        arena_.size() >= garbage_bytes_ ? arena_.size() - garbage_bytes_ : 0;
    if (arena_.capacity() > std::max<std::size_t>(64, target_arena * 2)) {
      std::vector<char> fresh;
      fresh.reserve(target_arena);
      arena_.swap(fresh);
    } else {
      arena_.clear();
    }
    size_ = 0;
    tombstones_ = 0;
    garbage_bytes_ = 0;
  }
  std::size_t size() const { return size_; }
  std::size_t memory_bytes() const {
    return memory_breakdown().total_bytes;
  }
  MemoryBreakdown memory_breakdown() const {
    MemoryBreakdown out;
    out.live_entries = size_;
    out.slot_capacity = slots_.capacity();
    out.slot_bytes = slots_.capacity() * sizeof(Slot);
    out.state_bytes = states_.capacity() * sizeof(std::uint8_t);
    out.raw_live_bytes =
        arena_.size() >= garbage_bytes_ ? arena_.size() - garbage_bytes_ : 0;
    out.raw_arena_bytes = arena_.capacity() * sizeof(char);
    out.garbage_bytes = garbage_bytes_;
    out.total_bytes =
        sizeof(*this) + out.slot_bytes + out.state_bytes + out.raw_arena_bytes;
    return out;
  }

private:
  enum class State : std::uint8_t { empty, occupied, tombstone };
  struct Slot {
    Id128 id{};
    Ref ref{};
  };

  static constexpr std::size_t NPOS = static_cast<std::size_t>(-1);
  static constexpr std::size_t MIN_CAPACITY = 8;

  State state_at(std::size_t index) const {
    const std::size_t byte = index >> 2;
    const std::size_t shift = (index & 3u) * 2u;
    return static_cast<State>((states_[byte] >> shift) & 0x3u);
  }

  static State state_at(
      const std::vector<std::uint8_t>& states,
      std::size_t index) {
    const std::size_t byte = index >> 2;
    const std::size_t shift = (index & 3u) * 2u;
    return static_cast<State>((states[byte] >> shift) & 0x3u);
  }

  void set_state(std::size_t index, State state) {
    const std::size_t byte = index >> 2;
    const std::size_t shift = (index & 3u) * 2u;
    const std::uint8_t mask = static_cast<std::uint8_t>(0x3u << shift);
    states_[byte] = static_cast<std::uint8_t>(
        (states_[byte] & static_cast<std::uint8_t>(~mask))
        | (static_cast<std::uint8_t>(state) << shift));
  }

  static std::size_t next_power_of_two(std::size_t value) {
    std::size_t out = MIN_CAPACITY;
    while (out < value) out <<= 1;
    return out;
  }

  std::size_t find_slot(const Id128& id) const {
    if (slots_.empty()) return NPOS;
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = Id128Hash{}(id) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
      const Slot& slot = slots_[index];
      const State state = state_at(index);
      if (state == State::empty) return NPOS;
      if (state == State::occupied && slot.id == id) return index;
      index = (index + 1) & mask;
    }
    return NPOS;
  }

  void ensure_insert_capacity() {
    if (slots_.empty()) {
      rehash(MIN_CAPACITY);
      return;
    }
    // Include tombstones because they lengthen probe chains. Rebuild in place
    // when live occupancy is still modest; otherwise grow.
    if ((size_ + tombstones_ + 1) * 20 < slots_.size() * 17) return;
    if ((size_ + 1) * 20 < slots_.size() * 17) rehash(slots_.size());
    else rehash(slots_.size() * 2);
  }

  void insert_new(const Id128& id, Ref ref) {
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = Id128Hash{}(id) & mask;
    std::size_t first_tombstone = NPOS;
    for (;;) {
      Slot& slot = slots_[index];
      const State state = state_at(index);
      if (state == State::empty) {
        const std::size_t target =
            first_tombstone == NPOS ? index : first_tombstone;
        Slot& destination = slots_[target];
        if (state_at(target) == State::tombstone) --tombstones_;
        destination.id = id;
        destination.ref = ref;
        set_state(target, State::occupied);
        ++size_;
        return;
      }
      if (state == State::tombstone && first_tombstone == NPOS) {
        first_tombstone = index;
      }
      index = (index + 1) & mask;
    }
  }

  void rehash(std::size_t requested_capacity) {
    std::vector<Slot> old = std::move(slots_);
    std::vector<std::uint8_t> old_states = std::move(states_);
    const std::size_t capacity = next_power_of_two(std::max(
        requested_capacity, (size_ * 20 + 16) / 17));
    slots_.assign(capacity, {});
    states_.assign((capacity + 3) / 4, std::uint8_t{0});
    size_ = 0;
    tombstones_ = 0;
    for (std::size_t index = 0; index < old.size(); ++index) {
      if (state_at(old_states, index) == State::occupied) {
        insert_new(old[index].id, old[index].ref);
      }
    }
  }

  void compact_if_needed() {
    if (garbage_bytes_ == 0 || garbage_bytes_ * 2 <= arena_.size()) return;
    std::vector<char> compact;
    compact.reserve(arena_.size() - garbage_bytes_);
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (state_at(index) != State::occupied) continue;
      auto& ref = slots_[index].ref;
      const std::uint32_t new_off = static_cast<std::uint32_t>(compact.size());
      compact.insert(compact.end(), arena_.begin() + ref.off,
                     arena_.begin() + ref.off + ref.len);
      ref.off = new_off;
    }
    arena_.swap(compact);
    garbage_bytes_ = 0;
  }

  std::vector<char> arena_;
  std::vector<Slot> slots_;
  std::vector<std::uint8_t> states_;
  std::size_t size_{0};
  std::size_t tombstones_{0};
  std::size_t garbage_bytes_{0};
};

} // namespace hh
