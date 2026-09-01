#pragma once
#include "hh/sketches/isketch.hpp"  // SnapshotEx, Id128
#include "hh/sketches/heavylocker.hpp" // HLBucketSnapshot
#include <vector>
#include <string>
#include <cstdint>

namespace hh {

inline constexpr std::uint32_t kNoResolutionWorker = UINT32_MAX;

struct GlobalItemLB {
  Id128 id;
  std::string key;
  std::uint64_t est{0};
  std::uint64_t lb{0};
  std::uint64_t reporters{0};  // number of partitions that reported this id
  double omega{0.0};           // coverage: mass fraction of reporting partitions (Eq. 1)
  std::uint64_t cert_lb{0};    // certified lower bound (routing-agnostic SS envelope)
  std::uint64_t cert_ub{0};    // certified upper bound (routing-agnostic SS envelope)
  // Exact one-sided certificate components retained for controller sizing.
  // Keeping these two scalars avoids reconstructing per-worker incidence maps
  // after reduction. They are coordinator telemetry, not report fields.
  std::uint64_t cert_inflation{0};
  std::uint64_t cert_hidden_mass{0};
  // One worker that reported this identifier. This routes an on-demand raw-key
  // request without coordinator access to worker-local identity maps.
  std::uint32_t resolution_worker{kNoResolutionWorker};
  bool has_cert_components{false};
  bool guaranteed{false};
};

struct GlobalResultLB {
  std::vector<GlobalItemLB> items;
  std::uint64_t N_global{0};
  std::uint64_t threshold{0};
  // Optional completeness certificate. When present, every key omitted from
  // items has true frequency at most unseen_mass_ub.
  std::uint64_t unseen_mass_ub{0};
  bool has_completeness_certificate{false};
  bool candidate_set_complete{false};
};

// Minimal, precomputed certificate record needed by the residual-tail sizing
// policy. The published result retains the richer GlobalItemLB representation;
// keeping this projection avoids duplicating keys, strings, coverage fields,
// and classification state solely for controller input.
struct HybridSizingItem {
  Id128 id;
  std::uint64_t est{0};
  std::uint64_t cert_lb{0};
  std::uint64_t cert_ub{0};
  std::uint64_t cert_inflation{0};
  std::uint64_t cert_hidden_mass{0};
  bool has_cert_components{false};
};

struct ReduceTelemetry {
  // Maximum report framing concurrently resident while reducing. This is
  // distinct from total report volume, which hh_bench accounts separately.
  std::size_t ingress_bytes{0};
  std::size_t agg_bytes{0};
  std::size_t presence_bytes{0};
  std::size_t items_bytes{0};
  std::size_t total_peak_bytes{0};
};

// A single Hybrid reduction produces the externally visible certified
// frontier, the bounded top set needed to choose the next exact head, and the
// residual-tail frontier needed by the sizing controller. Every local report
// is consumed once; finalized certified negatives outside top_ids are released.
struct HybridControlReduction {
  GlobalResultLB published;
  std::vector<HybridSizingItem> residual_items;
  std::vector<Id128> top_ids;
  std::vector<std::uint32_t> top_resolution_workers;
  ReduceTelemetry telemetry;
};

class Coordinator {
public:
  static std::string id128_hex(const Id128& id);

  // Reducers are identifier-only. Raw-key retrieval belongs to the separately
  // accounted output/promotion resolution protocol.

  // Unified LB-aware reducer (works for all sketches; SS provides lb, others 0)
  static GlobalResultLB reduce_global_with_lb(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      ReduceTelemetry* telemetry = nullptr,
      bool prune_certified_non_hh = true);

  // Result-equivalent streaming reducer. Hybrid exact-head counters use dense
  // aggregation; ordinary SS simply has an empty head. Sorted approximate
  // reports are merged one key at a time so certified negatives need not
  // occupy a full-union hash table.
  static GlobalResultLB reduce_global_streaming_with_lb(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      ReduceTelemetry* telemetry = nullptr,
      bool prune_certified_non_hh = true,
      std::size_t parallelism = 1);

  static GlobalResultLB reduce_global_parallel_streaming_with_lb(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      ReduceTelemetry* telemetry = nullptr,
      bool prune_certified_non_hh = true);

  // Coordinated Hybrid reducer. Compact exact-head slot/count records are
  // resolved against the installed dictionary; residual candidates retain
  // identifiers and error metadata. top_limit is normally n (or 2n for the
  // diagnostic Top2N head policy).
  static HybridControlReduction reduce_hybrid_streaming_for_control(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      std::size_t top_limit,
      const std::vector<Id128>& current_head_ids,
      std::uint64_t current_head_generation,
      std::size_t parallelism = 1);

  // Coordinated plain-SS reduction. This is the headless specialization of
  // the Hybrid reducer: one pass produces both the published certificate
  // frontier and the compact projection consumed by the sizing controller.
  static HybridControlReduction reduce_ss_streaming_for_control(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      std::size_t parallelism = 1);

  static HybridControlReduction reduce_hybrid_parallel_streaming_for_control(
      const std::vector<SnapshotEx>& snaps_ex,
      std::size_t n_param,
      std::size_t top_limit,
      const std::vector<Id128>& current_head_ids,
      std::uint64_t current_head_generation);

  // HeavyLocker paper-style serial bucket merge: aggregate one corresponding
  // bucket across workers, retain top-d, and release its temporary state.
  static GlobalResultLB reduce_hl_bucketwise(
      const std::vector<HLBucketSnapshot>& snaps_hl,
      std::size_t n_param,
      ReduceTelemetry* telemetry = nullptr);
};

} // namespace hh
