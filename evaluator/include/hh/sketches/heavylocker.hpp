#pragma once
#include "hh/sketches/isketch.hpp"
#include "hh/core/hash.hpp"
#include "hh/sketches/lossy_strategy.hpp"
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <string>
#include <cstring>
#include <memory>

namespace hh {

struct HLBucketCand {
  Id128 id{};
  std::uint32_t est{0};
  std::uint32_t bucket{0};
  // Events observed while this identity occupied its current slot. Unlike
  // est, this excludes counter mass inherited through lossy replacement.
  std::uint32_t lb{0};
};

struct HLBucketSnapshot {
  std::uint64_t N_local{0};
  std::size_t w{0};
  std::size_t d{0};
  std::vector<HLBucketCand> candidates;
  // Optional deterministic mass not assigned to current candidate lower
  // bounds, indexed by bucket. Empty for ordinary HeavyLocker.
  std::vector<std::uint64_t> residual_by_bucket;

  bool has_residual_certificate() const {
    return w > 0 && residual_by_bucket.size() == w;
  }
};

// Reentrant reproduction of glibc rand() after srand(seed). HeavyLocker's
// reference harness seeds one generator per method, shared by all workers.
class HeavyLockerRand {
public:
  explicit HeavyLockerRand(std::uint32_t seed = 1);
  std::uint32_t next();

private:
  std::array<std::uint32_t, 31> state_{};
  std::size_t position_{0};
};

// Single-thread HeavyLocker with LossyStrategy admission.
// - 1 hash → 1 bucket (width = w_)
// - d_ slots per bucket, lightly ordered by count (single swap per update)
// - bucket full → apply Lossy strategy to smallest slot iff
//   smallest < N_local * (theta_phi_ * lock_L_)
class HeavyLocker : public ISketch {
public:
  // lossy_mode: 0=MinusOne, 1=HeavyKeeper, 2=RAP (default), 3=USS)
  HeavyLocker(std::size_t w, std::size_t d, double lock_L, double theta_phi,
              int lossy_mode = 2, std::uint32_t random_seed = 1,
              std::shared_ptr<HeavyLockerRand> random_stream = {},
              bool certify_residual = false);

  void update(const Id128& id, int weight) override;
  Snapshot snapshot() const override;
  SnapshotEx snapshot_ex() const override;
  HLBucketSnapshot snapshot_bucketed() const;
  void reset_window() override;
  std::size_t memory_bytes() const override;
  void set_admission_callback(AdmitFn cb) override { on_admit_ = std::move(cb); }
  void set_retirement_callback(RetireFn cb) override { on_retire_ = std::move(cb); }

private:
  struct Slot {
    Id128 id{};
    std::uint32_t cnt{0};
  };

  std::size_t w_{0};
  std::size_t d_{0};
  double lock_L_{0.7};       // L from the paper (default 0.7)
  double theta_phi_{0.01};   // theta = 1/n_param (set by caller)
  std::uint64_t N_local_{0};
  // The authors use one dense bucket array. Counter value zero denotes an
  // empty cell; bucket position is therefore implicit in memory and reports.
  std::vector<Slot> table_;            // exactly w_ * d_ cells
  std::vector<std::uint8_t> lock_bits_; // bit-packed: one lock bit per bucket
  // Allocated only by hl-cert=on. Slot lower bounds are kept separately so
  // ordinary HeavyLocker retains the authors' packed cell layout.
  std::vector<std::uint32_t> lower_bounds_;
  std::vector<std::uint64_t> bucket_mass_;

  Lossy::Context lossy_;
  std::shared_ptr<HeavyLockerRand> rng_;
  AdmitFn on_admit_{};
  RetireFn on_retire_{};

  std::size_t index_of(const Id128& id) const;
  Slot* bucket_slots(std::size_t bucket) { return table_.data() + bucket * d_; }
  const Slot* bucket_slots(std::size_t bucket) const { return table_.data() + bucket * d_; }
  static void bubble_up(Slot* slots, std::uint32_t* lower_bounds,
                        std::size_t depth, std::size_t j);

  static std::string id128_to_bytes(const Id128& id) {
    return std::string(reinterpret_cast<const char*>(id.b.data()), 16);
  }
  static Id128 bytes_to_id128(const std::string& bytes) {
    Id128 id{};
    if (!bytes.empty()) {
      const std::size_t n = std::min<std::size_t>(bytes.size(), id.b.size());
      std::memcpy(id.b.data(), bytes.data(), n);
    }
    return id;
  }
};

} // namespace hh
