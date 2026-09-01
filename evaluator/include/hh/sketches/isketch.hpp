#pragma once
#include "hh/core/id128.hpp"
#include <cstdint>
#include <vector>
#include <functional>

namespace hh {

// Basic candidate
struct Cand {
  Id128 id;
  std::uint32_t est;
};

struct Snapshot {
  std::uint64_t N_local{0};
  std::vector<Cand> candidates;
};

// Optional per-candidate overestimation metadata. The lower bound is derived
// as max(0, est - eps), so retaining both values would be redundant.
struct CandErr {
  std::uint32_t eps;
};
static_assert(sizeof(CandErr) == sizeof(std::uint32_t));

// Compact exact-head wire record. The slot is interpreted against the shared,
// generation-checked head dictionary; inactive slots are simply omitted.
struct HeadRecord {
  std::uint32_t slot;
  std::uint32_t count;
};
static_assert(sizeof(HeadRecord) == 2 * sizeof(std::uint32_t));

struct SnapshotEx {
  std::uint64_t N_local{0};
  // Hybrid uses compact slot/count head records and identifier-bearing residual
  // candidates. Other sketches leave head_records empty.
  std::vector<HeadRecord> head_records;
  std::uint64_t head_generation{0};
  std::vector<Cand> candidates;
  // Optional suffix-aligned per-candidate errors.
  std::vector<CandErr> errors;
  std::size_t errors_from{0};
  // Optional compact SS error contract. If errors is empty and sketch_eps > 0,
  // candidates at indices >= sketch_eps_from share this overestimation bound.
  std::uint32_t sketch_eps{0};
  std::size_t sketch_eps_from{0};
  bool has_sketch_eps{false};
  std::size_t q_local{0};       // capacity (entries) if applicable (SS), else 0
  // Optional hybrid telemetry; non-hybrid sketches leave it at zero.
  std::uint64_t head_mass{0};
  // Compatibility for generic identifier-bearing exact prefixes. HybridSS
  // emits compact head_records instead and leaves this at zero.
  std::size_t head_size{0};
  // Hybrid residual candidates are ordered by Id128, allowing an m-way merge
  // without sorting reports at the coordinator.
  bool tail_sorted_by_id{false};

  bool has_error_bounds() const {
    return !errors.empty() && errors_from <= candidates.size()
        && errors.size() == candidates.size() - errors_from;
  }
  bool has_sketch_error_bound() const {
    return errors.empty() && has_sketch_eps && sketch_eps_from <= candidates.size();
  }
  std::uint32_t cand_lb(std::size_t i) const {
    if (i < head_size) return candidates[i].est;
    if (has_sketch_error_bound() && i >= sketch_eps_from) {
      const auto est = candidates[i].est;
      return est >= sketch_eps ? (est - sketch_eps) : 0u;
    }
    if (has_error_bounds()) {
      const auto est = candidates[i].est;
      const auto eps = i < errors_from ? 0u : errors[i - errors_from].eps;
      return est >= eps ? (est - eps) : 0u;
    }
    return 0u;
  }
  std::uint32_t cand_eps(std::size_t i) const {
    if (i < head_size) return 0u;
    if (has_sketch_error_bound() && i >= sketch_eps_from) return sketch_eps;
    if (!has_error_bounds() || i < errors_from) return 0u;
    return errors[i - errors_from].eps;
  }
};

class ISketch {
public:
  using AdmitFn = std::function<void(const Id128&)>;
  using RetireFn = std::function<void(const Id128&)>;

  virtual ~ISketch() = default;
  virtual void update(const Id128& id, int weight = 1) = 0;
  virtual Snapshot snapshot() const = 0;
  virtual void reset_window() = 0;

  // Rich snapshot; default builds from snapshot() without error metadata.
  virtual SnapshotEx snapshot_ex() const {
    Snapshot s = snapshot();
    SnapshotEx x; x.N_local = s.N_local; x.q_local = 0; x.candidates = std::move(s.candidates);
    return x;
  }

  // Admission callback (fires when an unmonitored id becomes resident). Default: noop.
  virtual void set_admission_callback(AdmitFn) {}
  // Retirement callback (fires when a resident id is evicted). Default: noop.
  virtual void set_retirement_callback(RetireFn) {}
  virtual void reconfigure(std::size_t /*new_capacity*/) {}
  // Best-effort memory usage of sketch state (bytes). Implementations may return 0 if unknown.
  virtual std::size_t memory_bytes() const { return 0; }
};

} // namespace hh
