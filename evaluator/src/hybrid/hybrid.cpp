#include "hh/hybrid/hybrid.hpp"
#include <algorithm>
#include <cmath>

namespace hh {

HybridSS::HybridSS(std::size_t q_e, std::size_t q_a, bool tail_per_item_eps)
  : head_(q_e),
    tail_(std::make_unique<SpaceSaving>(std::max<std::size_t>(q_a,1), tail_per_item_eps)),
    tail_per_item_eps_(tail_per_item_eps) {
  // tail admission callback will be wired when/if user sets on_admit_
}

void HybridSS::reconfigure(std::size_t q_e, std::size_t q_a, std::size_t n_param, double head_mass_frac) {
  head_.set_capacity(q_e);
  const double p_e = (head_mass_frac < 0.0)
      ? head_mass_fraction()
      : std::clamp(head_mass_frac, 0.0, 1.0);
  const std::size_t residual_floor = (n_param == 0)
      ? 1
      : std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(n_param) * (1.0 - p_e))));
  const std::size_t q_tail = std::max<std::size_t>(std::max<std::size_t>(q_a, residual_floor), 1);

  // rebuild tail to enforce capacity precisely
  auto new_tail = std::make_unique<SpaceSaving>(q_tail, tail_per_item_eps_);
  if (on_admit_) new_tail->set_admission_callback(on_admit_);
  if (on_retire_) new_tail->set_retirement_callback(on_retire_);
  tail_.swap(new_tail);
}

void HybridSS::update(const Id128& id, int w) {
  if (w <= 0) return;
  N_local_ += static_cast<std::uint64_t>(w);

  if (head_.contains(id)) {
    // The promoted head is a shared, coordinator-issued dictionary.  Workers
    // retain its Id128 slots and counts, but not a duplicate raw-key entry in
    // their ArenaMap; the coordinator retains that key material once.
    head_.admit_and_add(id, static_cast<std::uint32_t>(w));
    return;
  }

  // Not in head: send to tail (head membership is pre-seeded by caller)
  tail_->update(id, w);
}

Snapshot HybridSS::snapshot() const {
  Snapshot s{};
  s.N_local = N_local_;
  Snapshot ts = tail_->snapshot();
  s.candidates.reserve(head_.size() + ts.candidates.size());
  // Active head entries first (exact). Seeded head keys with zero local mass
  // are shared dictionary slots and do not need to be reported.
  for (std::size_t slot = 0; slot < head_.size(); ++slot) {
    const auto count = head_.count_at(slot);
    if (count == 0) continue;
    s.candidates.push_back({head_.ids()[slot], count});
  }
  // then tail
  for (const auto& c : ts.candidates) s.candidates.push_back(c);
  return s;
}

SnapshotEx HybridSS::snapshot_ex() const {
  SnapshotEx sx{};
  sx.N_local = N_local_;
  sx.q_local = tail_ ? tail_->capacity() : 0;
  sx.head_mass = head_mass();

  // Active head entries: exact => lb=est=count. Seeded head keys with zero
  // local mass are omitted; the coordinator infers absent head slots as zero.
  sx.candidates.reserve(head_.size() + tail_->capacity());
  for (std::size_t slot = 0; slot < head_.size(); ++slot) {
    const auto count = head_.count_at(slot);
    if (count == 0) continue;
    sx.candidates.push_back({head_.ids()[slot], count});
  }
  sx.head_size = sx.candidates.size();
  // Append directly from the resident tail. Sorting only compact node indices
  // avoids materializing a second full candidate/error record array.
  tail_->append_snapshot_ex(sx, /*sort_by_id=*/true);
  sx.tail_sorted_by_id = true;
  return sx;
}

void HybridSS::reset_window() {
  N_local_ = 0;
  head_.clear();
  tail_->reset_window();
}

std::size_t HybridSS::memory_bytes() const {
  std::size_t bytes = sizeof(*this);
  bytes += head_.memory_bytes() - sizeof(ExactHead);
  if (tail_) bytes += tail_->memory_bytes();
  return bytes;
}

void HybridSS::seed_head(const std::vector<Id128>& ids) {
  head_.seed(ids);
}

void HybridSS::install_head(
    const std::vector<Id128>& ids,
    std::uint64_t next_generation) {
  head_.install_full(ids, next_generation);
}

bool HybridSS::apply_head_delta(
    const ExactHeadDelta& delta,
    std::size_t next_capacity) {
  return head_.apply_delta(delta, next_capacity);
}

void HybridSS::reset_window_preserve_head() {
  N_local_ = 0;
  head_.reset_counts();
  tail_->reset_window();
}

std::uint64_t HybridSS::head_generation() const {
  return head_.generation();
}

std::size_t HybridSS::head_size() const { return head_.size(); }
std::size_t HybridSS::head_capacity() const { return head_.capacity(); }

std::uint64_t HybridSS::head_mass() const {
  std::uint64_t sum = 0;
  for (std::size_t slot = 0; slot < head_.size(); ++slot) sum += head_.count_at(slot);
  return sum;
}

double HybridSS::head_mass_fraction() const {
  if (N_local_ == 0) return 0.0;
  return static_cast<double>(head_mass()) / static_cast<double>(N_local_);
}

} // namespace hh
