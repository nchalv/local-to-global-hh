#include "hh/bench/metrics.hpp"
#include "hh/coord/coordinator.hpp"
#include "hh/core/arena_map.hpp"
#include "hh/core/hash.hpp"
#include "hh/hybrid/hybrid.hpp"
#include "hh/io/reader.hpp"
#include "hh/sizing/sizing.hpp"
#include "hh/sketches/ss.hpp"

#include <zstr.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using hh::Id128;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& message) {
  if (!condition) throw TestFailure(message);
}

void init_hashing() {
  std::array<std::uint8_t, 32> kid{};
  std::array<std::uint8_t, 32> kfp{};
  std::array<std::uint8_t, 32> kidx{};
  for (std::size_t i = 0; i < kid.size(); ++i) {
    kid[i] = static_cast<std::uint8_t>(i + 1);
    kfp[i] = static_cast<std::uint8_t>(101 + i);
    kidx[i] = static_cast<std::uint8_t>(201 + i);
  }
  hh::set_secret_keys(kid, kfp, kidx);
}

Id128 id_for(const std::string& key) {
  return hh::id128_for(key);
}

const hh::GlobalItemLB* find_item(const hh::GlobalResultLB& result, const Id128& id) {
  for (const auto& item : result.items) {
    if (item.id == id) return &item;
  }
  return nullptr;
}

const hh::HybridSizingItem* find_hybrid_sizing_item(
    const std::vector<hh::HybridSizingItem>& items, const Id128& id) {
  for (const auto& item : items) {
    if (item.id == id) return &item;
  }
  return nullptr;
}

void require_same_hybrid_sizing_items(
    const std::vector<hh::HybridSizingItem>& expected,
    const std::vector<hh::HybridSizingItem>& actual) {
  require(expected.size() == actual.size(),
          "coordinated hybrid reducer preserves residual sizing count");
  for (const auto& item : expected) {
    const auto* found = find_hybrid_sizing_item(actual, item.id);
    require(found != nullptr,
            "coordinated hybrid reducer preserves residual sizing ids");
    require(item.est == found->est && item.cert_lb == found->cert_lb &&
                item.cert_ub == found->cert_ub &&
                item.cert_inflation == found->cert_inflation &&
                item.cert_hidden_mass == found->cert_hidden_mass &&
                item.has_cert_components == found->has_cert_components,
            "coordinated hybrid reducer preserves residual sizing certificates");
  }
}

void write_gzip_json(const std::filesystem::path& path, const std::string& json) {
  std::ofstream fout(path, std::ios::binary);
  require(static_cast<bool>(fout), "failed to open gzip fixture for writing: " + path.string());
  zstr::ostream zout(fout);
  zout << json;
}

void test_arena_map_flat_index_lifecycle() {
  hh::ArenaMap map;

  // Construct three identifiers that begin in the same initial eight-slot
  // bucket, then verify that a tombstone does not break the probe chain.
  std::vector<Id128> colliding;
  for (std::size_t i = 0; colliding.size() < 3; ++i) {
    const Id128 id = id_for("arena-collision-" + std::to_string(i));
    if ((hh::Id128Hash{}(id) & 7u) == 3u) colliding.push_back(id);
  }
  map.bind_bytes(colliding[0], "first");
  map.bind_bytes(colliding[1], "second");
  map.bind_bytes(colliding[2], "third");
  map.erase(colliding[0]);

  std::string raw;
  require(map.lookup(colliding[1], raw) && raw == "second",
          "ArenaMap lookup crosses a deleted flat-index slot");
  require(map.lookup(colliding[2], raw) && raw == "third",
          "ArenaMap preserves the end of a collision chain");

  // Force several table growth operations and verify every live reference.
  std::vector<std::pair<Id128, std::string>> inserted;
  inserted.reserve(256);
  for (std::size_t i = 0; i < 256; ++i) {
    std::string key = "arena-rehash-" + std::to_string(i);
    Id128 id = id_for(key);
    map.bind_bytes(id, key);
    inserted.emplace_back(id, std::move(key));
  }
  for (const auto& [id, key] : inserted) {
    require(map.lookup(id, raw) && raw == key,
            "ArenaMap rehash preserves raw-key references");
  }

  // Rebinding creates arena garbage; compaction must update all offsets while
  // retaining only the newest bytes.
  const Id128 rewritten = inserted[17].first;
  map.bind_bytes(rewritten, std::string(256, 'a'));
  map.bind_bytes(rewritten, std::string(256, 'b'));
  map.bind_bytes(rewritten, std::string(256, 'c'));
  require(map.lookup(rewritten, raw) && raw == std::string(256, 'c'),
          "ArenaMap compaction preserves rewritten values");

  const auto memory = map.memory_breakdown();
  require(memory.live_entries == map.size(),
          "ArenaMap memory breakdown reports live entries");
  require(memory.slot_capacity >= memory.live_entries,
          "ArenaMap flat capacity covers all live entries");
  require(memory.slot_bytes > 0 && memory.state_bytes > 0 &&
              memory.raw_arena_bytes >= memory.raw_live_bytes,
          "ArenaMap memory breakdown reports owned buffers");
  require(memory.total_bytes == map.memory_bytes(),
          "ArenaMap exact breakdown matches total accounting");

  map.clear();
  require(map.size() == 0 && !map.lookup(inserted[0].first, raw),
          "ArenaMap clear removes every binding");
  const auto cleared = map.memory_breakdown();
  require(cleared.live_entries == 0 && cleared.raw_live_bytes == 0,
          "ArenaMap clear resets live memory telemetry");
}

void test_space_saving_bounds_and_reconfigure() {
  hh::SpaceSaving ss(3);
  ss.update(id_for("a"), 5);
  ss.update(id_for("b"), 3);
  ss.update(id_for("c"), 2);
  ss.update(id_for("d"), 1);

  const auto snap = ss.snapshot_ex();
  require(snap.N_local == 11, "SpaceSaving tracks local mass");
  require(snap.q_local == 3, "SpaceSaving reports capacity");
  require(snap.candidates.size() <= 3, "SpaceSaving never reports more than q candidates");
  require(snap.has_error_bounds(), "SpaceSaving exports per-item error bounds");

  std::uint64_t counter_sum = 0;
  for (std::size_t i = 0; i < snap.candidates.size(); ++i) {
    require(snap.cand_lb(i) <= snap.candidates[i].est, "candidate lower bound is not above estimate");
    require(snap.errors[i].eps <= snap.candidates[i].est, "candidate epsilon is not above estimate");
    require(snap.cand_lb(i) == snap.candidates[i].est - snap.errors[i].eps,
            "candidate lower bound is derived losslessly from estimate and epsilon");
    counter_sum += snap.candidates[i].est;
  }
  require(counter_sum == snap.N_local, "SpaceSaving counters sum to observed mass");

  ss.reconfigure(2);
  const auto after_resize = ss.snapshot_ex();
  require(after_resize.N_local == 0, "reconfigure starts a fresh window");
  require(after_resize.q_local == 2, "reconfigure updates capacity");
  require(after_resize.candidates.empty(), "reconfigure clears resident candidates");

  ss.update(id_for("a"), 70000);
  const auto wide = ss.snapshot_ex();
  require(wide.N_local == 70000, "SpaceSaving supports counts beyond compact bucket storage");
  require(wide.candidates.size() == 1 && wide.candidates[0].est == 70000,
          "wide bucket values preserve large derived node counts");
}

void test_retained_key_binding_lifecycle() {
  hh::SpaceSaving ss(2);
  hh::ArenaMap map;
  Id128 active_id{};
  const std::string* active_raw = nullptr;
  ss.set_admission_callback([&](const Id128& id) {
    if (active_raw && id == active_id) map.bind_bytes(id, *active_raw);
  });
  ss.set_retirement_callback([&](const Id128& id) { map.erase(id); });

  auto update = [&](const std::string& raw) {
    active_id = id_for(raw);
    active_raw = &raw;
    ss.update(active_id);
    active_raw = nullptr;
  };

  for (int i = 0; i < 100; ++i) update("key-" + std::to_string(i));
  const auto snapshot = ss.snapshot_ex();
  require(map.size() == snapshot.candidates.size(),
          "raw-key dictionary tracks only currently retained Space-Saving ids");
  require(map.size() <= ss.capacity(),
          "raw-key dictionary remains bounded by sketch capacity");
  for (const auto& candidate : snapshot.candidates) {
    std::string raw;
    require(map.lookup(candidate.id, raw),
            "every retained sketch id has a resolvable raw-key binding");
  }
  require(map.memory_bytes() < 4096,
          "retired raw-key bytes are compacted instead of accumulating by window cardinality");
}

void test_space_saving_max_sketch_error_snapshot() {
  const Id128 a = id_for("maxeps-a");
  const Id128 b = id_for("maxeps-b");
  const Id128 c = id_for("maxeps-c");

  hh::SpaceSaving ss(2, /*per_item_eps=*/false);
  ss.update(a, 5);
  ss.update(b, 4);
  ss.update(c, 1);

  const auto snap = ss.snapshot_ex();
  require(!snap.has_error_bounds(), "max-sketch mode omits per-candidate errors");
  require(snap.has_sketch_error_bound(), "max-sketch mode exports one shared error bound");
  require(snap.sketch_eps > 0, "shared sketch error records the max admission error");
  require(snap.errors.empty(), "max-sketch snapshot does not materialize CandErr records");

  hh::ArenaMap m0;
  m0.bind_bytes(a, "maxeps-a");
  m0.bind_bytes(b, "maxeps-b");
  m0.bind_bytes(c, "maxeps-c");
  std::vector<hh::SnapshotEx> snaps{snap};
  std::vector<const hh::ArenaMap*> maps{&m0};
  const auto reduced = hh::Coordinator::reduce_global_with_lb(snaps, 3);
  for (const auto& item : reduced.items) {
    require(item.cert_lb <= item.est, "max-sketch lower bound remains conservative");
    require(item.cert_ub >= item.est, "max-sketch upper bound remains conservative");
  }
}

enum class TestReducer { Hash, Streaming, ParallelStreaming };

hh::GlobalResultLB make_manual_reduction(TestReducer reducer = TestReducer::Hash) {
  const Id128 a = id_for("a");
  const Id128 b = id_for("b");
  const Id128 c = id_for("c");

  hh::SnapshotEx s0;
  s0.N_local = 10;
  s0.q_local = 2;
  s0.candidates = {{a, 6}, {b, 4}};
  s0.errors = {{0}, {0}};

  hh::SnapshotEx s1;
  s1.N_local = 10;
  s1.q_local = 2;
  s1.candidates = {{b, 4}, {c, 6}};
  s1.errors = {{0}, {0}};

  hh::ArenaMap m0;
  hh::ArenaMap m1;
  m0.bind_bytes(a, "a");
  m0.bind_bytes(b, "b");
  m1.bind_bytes(c, "c");

  std::vector<hh::SnapshotEx> snaps{s0, s1};
  std::vector<const hh::ArenaMap*> maps{&m0, &m1};
  if (reducer == TestReducer::Streaming) {
    return hh::Coordinator::reduce_global_streaming_with_lb(snaps, 3);
  }
  if (reducer == TestReducer::ParallelStreaming) {
    return hh::Coordinator::reduce_global_parallel_streaming_with_lb(snaps, 3);
  }
  return hh::Coordinator::reduce_global_with_lb(snaps, 3);
}

void require_same_reduction(const hh::GlobalResultLB& expected,
                            const hh::GlobalResultLB& actual) {
  require(actual.N_global == expected.N_global, "streaming reducer preserves global mass");
  require(actual.threshold == expected.threshold, "streaming reducer preserves threshold");
  require(actual.items.size() == expected.items.size(), "streaming reducer preserves candidate count");
  for (const auto& a : expected.items) {
    const auto* found = find_item(actual, a.id);
    require(found != nullptr, "streaming reducer preserves candidate ids");
    const auto& b = *found;
    require(a.key.empty() && b.key.empty(),
            "reducers keep internal candidate frontiers identifier-only");
    require(a.est == b.est && a.lb == b.lb, "streaming reducer preserves estimates");
    require(a.reporters == b.reporters && std::abs(a.omega - b.omega) < 1e-12,
            "streaming reducer preserves reporter coverage");
    require(a.cert_lb == b.cert_lb && a.cert_ub == b.cert_ub,
            "streaming reducer preserves certificate bounds");
    require(a.resolution_worker == b.resolution_worker,
            "streaming reducer preserves raw-key resolution routes");
    require(a.guaranteed == b.guaranteed, "streaming reducer preserves classification");
  }
}

void test_coordinator_certification_envelope() {
  const auto result = make_manual_reduction();
  require_same_reduction(result, make_manual_reduction(TestReducer::Streaming));
  require_same_reduction(result, make_manual_reduction(TestReducer::ParallelStreaming));
  require(result.N_global == 20, "coordinator sums global mass");
  require(result.threshold == 7, "coordinator uses strict floor(N/n)+1 threshold");
  require(result.items.size() == 3, "coordinator aggregates all reported ids");

  const auto* a = find_item(result, id_for("a"));
  const auto* b = find_item(result, id_for("b"));
  const auto* c = find_item(result, id_for("c"));
  require(a && b && c, "coordinator result contains expected keys");

  require(a->key.empty(),
          "coordinator does not inspect worker key maps during reduction");
  require(a->resolution_worker == 0 && c->resolution_worker == 1,
          "coordinator retains one reporting worker for on-demand resolution");
  require(a->est == 6 && a->cert_lb == 6 && a->cert_ub == 10,
          "coordinator adds non-reporter min counter to upper bound");
  require(!a->guaranteed, "key below certified threshold is not guaranteed");

  require(b->est == 8 && b->cert_lb == 8 && b->cert_ub == 8,
          "coordinator keeps fully reported exact key tight");
  require(b->guaranteed, "key with certified lower bound above threshold is guaranteed");

  require(c->est == 6 && c->cert_lb == 6 && c->cert_ub == 10,
          "coordinator computes symmetric hidden mass case");
}

void test_heavylocker_serial_bucket_merge() {
  const Id128 a = id_for("hl-a");
  const Id128 b = id_for("hl-b");
  const Id128 c = id_for("hl-c");
  const Id128 x = id_for("hl-x");
  const Id128 y = id_for("hl-y");

  hh::HLBucketSnapshot s0;
  s0.N_local = 12;
  s0.w = 2;
  s0.d = 2;
  s0.candidates = {{a, 5, 0}, {b, 3, 0}, {x, 4, 1}};

  hh::HLBucketSnapshot s1;
  s1.N_local = 16;
  s1.w = 2;
  s1.d = 2;
  s1.candidates = {{a, 2, 0}, {c, 6, 0}, {x, 1, 1}, {y, 7, 1}};

  hh::ArenaMap m0;
  hh::ArenaMap m1;
  for (const auto& pair : std::vector<std::pair<Id128, std::string>>{
           {a, "hl-a"}, {b, "hl-b"}, {x, "hl-x"}}) {
    m0.bind_bytes(pair.first, pair.second);
  }
  m1.bind_bytes(c, "hl-c");
  m1.bind_bytes(y, "hl-y");

  const std::vector<hh::HLBucketSnapshot> snaps{s0, s1};
  const std::vector<const hh::ArenaMap*> maps{&m0, &m1};
  hh::ReduceTelemetry telemetry;
  const auto result = hh::Coordinator::reduce_hl_bucketwise(snaps, 4, &telemetry);

  require(result.N_global == 28 && result.threshold == 8,
          "HeavyLocker merge preserves global mass and strict threshold");
  require(result.items.size() == 4, "HeavyLocker merge retains top-d per bucket");
  require(find_item(result, a) && find_item(result, a)->est == 7,
          "HeavyLocker merge sums matching ids within a bucket");
  require(find_item(result, c) && find_item(result, c)->est == 6,
          "HeavyLocker merge retains the second-largest bucket candidate");
  require(find_item(result, b) == nullptr,
          "HeavyLocker merge drops candidates below the bucket top-d frontier");
  require(find_item(result, x) && find_item(result, x)->est == 5,
          "HeavyLocker merge handles a second bucket independently");
  require(find_item(result, y) && find_item(result, y)->est == 7,
          "HeavyLocker merge retains all candidates when a bucket has at most d ids");
  require(telemetry.total_peak_bytes > 0 && telemetry.agg_bytes > 0,
          "HeavyLocker serial reducer reports resident working memory");

  auto wide_snaps = snaps;
  for (auto& snap : wide_snaps) snap.w = 100;
  hh::ReduceTelemetry wide_telemetry;
  const auto wide_result = hh::Coordinator::reduce_hl_bucketwise(
      wide_snaps, 4, &wide_telemetry);
  require(wide_result.items.size() == result.items.size(),
          "empty HeavyLocker buckets do not alter merge output");
  require(wide_telemetry.agg_bytes == telemetry.agg_bytes
              && wide_telemetry.presence_bytes == telemetry.presence_bytes,
          "serial HeavyLocker temporary state does not grow with empty bucket maps");
  require(wide_telemetry.items_bytes > telemetry.items_bytes,
          "HeavyLocker telemetry still accounts for the wider retained output");
}

void test_heavylocker_packed_layout_and_reference_rng() {
  hh::HeavyLocker narrow(2, 3, 0.7, 1.0 / 200.0, 2, 17);
  hh::HeavyLocker wide(3, 3, 0.7, 1.0 / 200.0, 2, 17);
  const std::size_t packed_bucket_bytes =
      3 * (sizeof(Id128) + sizeof(std::uint32_t));
  require(wide.memory_bytes() - narrow.memory_bytes() == packed_bucket_bytes,
          "HeavyLocker worker memory grows by one packed positional bucket");
  hh::HeavyLocker eight(8, 3, 0.7, 1.0 / 200.0, 2, 17);
  hh::HeavyLocker nine(9, 3, 0.7, 1.0 / 200.0, 2, 17);
  require(nine.memory_bytes() - eight.memory_bytes()
              == packed_bucket_bytes + sizeof(std::uint8_t),
          "HeavyLocker stores one packed lock bit per bucket");

  hh::HeavyLockerRand reference_rng(1);
  require(reference_rng.next() == 1804289383u
              && reference_rng.next() == 846930886u
              && reference_rng.next() == 1681692777u,
          "HeavyLocker RNG reproduces glibc rand() after srand(1)");

  hh::HeavyLocker first(1, 1, 0.7, 1.0 / 200.0, 2, 23);
  hh::HeavyLocker second(1, 1, 0.7, 1.0 / 200.0, 2, 23);
  hh::HeavyLocker unrelated(1, 1, 0.7, 1.0 / 200.0, 2, 99);
  for (int i = 0; i < 200; ++i) {
    const Id128 id = id_for("hl-rng-" + std::to_string(i % 17));
    first.update(id, 1);
    unrelated.update(id_for("hl-noise-" + std::to_string(i)), 1);
    second.update(id, 1);
  }
  const auto first_snapshot = first.snapshot_bucketed();
  const auto second_snapshot = second.snapshot_bucketed();
  require(first_snapshot.candidates.size() == second_snapshot.candidates.size(),
          "HeavyLocker per-configuration RNG preserves result size across method mixes");
  for (std::size_t i = 0; i < first_snapshot.candidates.size(); ++i) {
    require(first_snapshot.candidates[i].id == second_snapshot.candidates[i].id
                && first_snapshot.candidates[i].est == second_snapshot.candidates[i].est,
            "HeavyLocker result is independent of unrelated configurations");
  }
}

void test_heavylocker_residual_certificate() {
  const Id128 x = id_for("hl-cert-x");
  const Id128 y = id_for("hl-cert-y");

  // With w=d=1 and RAP seeded as in the reference implementation, the first
  // competing update is rejected and the second replaces x with y while
  // inheriting x's counter. Only y's replacement event is a valid lower bound.
  hh::HeavyLocker certified(
      1, 1, 0.7, 1.0, 2, 1, {}, true);
  certified.update(x, 1);
  certified.update(y, 1);
  certified.update(y, 1);
  const auto snap = certified.snapshot_bucketed();
  require(snap.has_residual_certificate(),
          "certified HeavyLocker exports one residual per bucket");
  require(snap.candidates.size() == 1
              && snap.candidates[0].id == y
              && snap.candidates[0].est == 2
              && snap.candidates[0].lb == 1,
          "RAP inherited count is excluded from the current identity lower bound");
  require(snap.residual_by_bucket.size() == 1
              && snap.residual_by_bucket[0] == 2,
          "bucket residual contains rejected and inherited mass");

  hh::HeavyLocker ordinary(2, 3, 0.7, 1.0 / 200.0, 2, 17);
  hh::HeavyLocker with_certificate(
      2, 3, 0.7, 1.0 / 200.0, 2, 17, {}, true);
  require(with_certificate.memory_bytes() - ordinary.memory_bytes()
              == 2 * 3 * sizeof(std::uint32_t)
                   + 2 * sizeof(std::uint64_t),
          "certificate accounting charges slot lower bounds and bucket mass");

  const Id128 a = id_for("hl-cert-a");
  const Id128 b = id_for("hl-cert-b");
  hh::HLBucketSnapshot s0;
  s0.N_local = 10;
  s0.w = 1;
  s0.d = 1;
  s0.candidates = {{a, 8, 0, 3}};
  s0.residual_by_bucket = {7};

  hh::HLBucketSnapshot s1;
  s1.N_local = 10;
  s1.w = 1;
  s1.d = 1;
  s1.candidates = {{b, 9, 0, 4}};
  s1.residual_by_bucket = {6};

  hh::ArenaMap m0;
  hh::ArenaMap m1;
  m0.bind_bytes(a, "hl-cert-a");
  m1.bind_bytes(b, "hl-cert-b");
  const std::vector<hh::HLBucketSnapshot> snaps{s0, s1};
  const std::vector<const hh::ArenaMap*> maps{&m0, &m1};
  const auto result =
      hh::Coordinator::reduce_hl_bucketwise(snaps, 2);
  require(result.has_completeness_certificate,
          "coordinator recognizes certified HeavyLocker snapshots");
  require(!result.candidate_set_complete
              && result.unseen_mass_ub == 16,
          "discarded merge candidates contribute to the unseen-key bound");
  const auto* retained = find_item(result, b);
  require(retained && retained->cert_lb == 4 && retained->cert_ub == 17,
          "retained HeavyLocker candidates receive deterministic intervals");

  s0.d = 2;
  s0.candidates = {{a, 8, 0, 8}};
  s0.residual_by_bucket = {2};
  s1.d = 2;
  s1.candidates = {{a, 8, 0, 8}};
  s1.residual_by_bucket = {2};
  const auto complete =
      hh::Coordinator::reduce_hl_bucketwise({s0, s1}, 4);
  require(complete.candidate_set_complete
              && complete.unseen_mass_ub == 4
              && complete.threshold == 6,
          "residual below the strict HH threshold certifies completeness");
}

void test_sizing_policy_outputs_are_clamped_and_finite() {
  const hh::sizing::PolicyConfig default_cfg;
  require(default_cfg.censored_control &&
              default_cfg.probe_residual_guard &&
              default_cfg.probe_evidence_window == 2,
          "censored residual-guarded control is the default sizing policy");

  const auto result = make_manual_reduction();

  hh::SnapshotEx s0;
  s0.N_local = 10;
  s0.q_local = 2;
  s0.candidates = {{id_for("a"), 6}, {id_for("b"), 4}};
  s0.errors = {{0}, {0}};

  hh::SnapshotEx s1;
  s1.N_local = 10;
  s1.q_local = 2;
  s1.candidates = {{id_for("b"), 4}, {id_for("c"), 6}};
  s1.errors = {{0}, {0}};

  hh::sizing::PolicyConfig cfg;
  cfg.kind = hh::sizing::PolicyKind::difficulty;
  cfg.n_param = 3;
  cfg.q_cur = 3;
  cfg.q_min = 3;
  cfg.q_max = 40;
  cfg.q_cap = 40;
  cfg.alpha_req = 1.0;
  cfg.epsilon_m = 0.25;

  hh::sizing::PolicyState state;
  const auto first = hh::sizing::next_q_ss(result, {s0, s1}, cfg, &state);
  require(first.q_req >= cfg.n_param, "difficulty policy reports a requirement at least n");
  require(first.q_next >= cfg.q_min && first.q_next <= cfg.q_cap && first.q_next <= cfg.q_max,
          "difficulty policy clamps q_next");
  require(first.q_base >= first.q_req,
          "ambiguity adjustment cannot lower the effective requirement");
  require(first.q_base == first.q_req,
          "ambiguity remains inactive when all resolution demands have negative utility");

  require(first.g_amb == 0.0,
          "non-binding ambiguity reports no actuation lift");
  require(first.q_pred >= cfg.n_param, "difficulty policy exports predictive effective capacity");

  hh::sizing::PolicyConfig max_quantile_cfg = cfg;
  max_quantile_cfg.n_param = 10;
  max_quantile_cfg.q_cur = 10;
  max_quantile_cfg.q_min = 10;
  max_quantile_cfg.alpha_req = 1.0;
  max_quantile_cfg.ambiguity_adjust = false;
  const std::vector<hh::HybridSizingItem> max_quantile_items = {
      {id_for("low-margin"), 110, 100, 120, 10, 5, true},
      {id_for("high-margin"), 110, 100, 130, 30, 20, true},
  };
  const auto max_quantile = hh::sizing::next_q_ss(
      max_quantile_items, 1000, 101, {}, max_quantile_cfg, nullptr);
  require(std::abs(max_quantile.margin_alpha - 0.03) < 1e-12,
          "alpha=1 selects the maximum candidate margin");

  hh::sizing::PolicyConfig residual_cfg = cfg;
  residual_cfg.q_cur = 2;
  residual_cfg.q_min = 2;
  residual_cfg.ambiguity_adjust = false;
  const auto residual_floor = hh::sizing::next_q_ss(result, {s0, s1}, residual_cfg, nullptr);
  require(residual_floor.q_req >= residual_cfg.q_min &&
              residual_floor.q_next >= residual_cfg.q_min,
          "residual-tail difficulty policy uses configured q_min as its capacity floor");

  cfg.alpha_req = 0.5;
  cfg.ambiguity_adjust = false;
  const auto unadjusted = hh::sizing::next_q_ss(result, {s0, s1}, cfg, nullptr);
  require(unadjusted.q_base == unadjusted.q_req,
          "requirement-only ablation keeps q_eff equal to q_req");

  const Id128 cheap_id = id_for("cheap-ambiguity");
  const Id128 costly_id = id_for("costly-ambiguity");
  hh::GlobalResultLB efficiency_result;
  efficiency_result.N_global = 10000;
  efficiency_result.threshold = 1001;
  efficiency_result.items = {
      {cheap_id, "cheap-ambiguity", 1015, 1000, 1, 1.0, 1000, 1030, false},
      {costly_id, "costly-ambiguity", 1001, 0, 1, 1.0, 0, 2000, false},
  };
  hh::SnapshotEx efficiency_snap;
  efficiency_snap.N_local = 10000;
  efficiency_snap.q_local = 10;
  efficiency_snap.candidates = {{cheap_id, 1015}, {costly_id, 1001}};
  efficiency_snap.errors = {{0}, {0}};

  hh::sizing::PolicyConfig efficiency_cfg;
  efficiency_cfg.kind = hh::sizing::PolicyKind::difficulty;
  efficiency_cfg.n_param = 10;
  efficiency_cfg.q_cur = 100;
  efficiency_cfg.q_min = 10;
  efficiency_cfg.q_max = 2000;
  efficiency_cfg.q_cap = 2000;
  efficiency_cfg.alpha_req = 0.95;
  efficiency_cfg.epsilon_m = 0.01;
  efficiency_cfg.ambiguity_adjust = true;

  hh::GlobalResultLB actionable_ambiguity = efficiency_result;
  actionable_ambiguity.items = {
      {cheap_id, "cheap-ambiguity", 1015, 1000, 1, 1.0, 1000, 1030, false},
      {costly_id, "costly-ambiguity", 1001, 0, 1, 1.0, 0, 2000, false},
  };
  hh::SnapshotEx actionable_snap = efficiency_snap;
  actionable_snap.candidates = {{cheap_id, 1015}, {costly_id, 1001}};
  const auto efficient = hh::sizing::next_q_ss(
      actionable_ambiguity, {actionable_snap}, efficiency_cfg, nullptr);
  require(efficient.q_req == 100,
          "zero certificate inflation leaves the baseline at the current sufficient capacity");
  require(efficient.q_base == 151,
          "ambiguity ignores near-threshold ties and serves actionable uncertainty");
  require(std::abs(efficient.g_amb - std::log(151.0 / 100.0)) < 1e-12,
          "ambiguity reports its attributable log-capacity margin");

  hh::SnapshotEx high_error_snap = efficiency_snap;
  high_error_snap.errors = {{100}, {100}};
  hh::sizing::PolicyConfig upward_cfg = efficiency_cfg;
  upward_cfg.ambiguity_adjust = false;
  upward_cfg.censored_control = false;
  upward_cfg.probe_residual_guard = false;
  upward_cfg.q_cur = 40;
  const auto held = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, upward_cfg, nullptr);
  require(!held.service_violation && held.q_up == 0 &&
              held.q_baseline == 40 && held.q_pred == 40,
          "upward-only control holds capacity after a sufficient window");

  upward_cfg.q_cur = 10;
  const auto raised = hh::sizing::next_q_ss(
      efficiency_result, {high_error_snap}, upward_cfg, nullptr);
  require(raised.service_violation && raised.q_up == 100 &&
              raised.q_baseline == 100 && raised.q_pred == 100,
          "a service violation activates origin-aware upward sizing");

  hh::sizing::PolicyConfig probing_cfg = upward_cfg;
  probing_cfg.censored_control = true;
  probing_cfg.probe_evidence_window = 2;
  probing_cfg.q_cur = 40;
  hh::sizing::PolicyState probing_state;
  const auto first_sufficient = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, probing_cfg, &probing_state);
  require(first_sufficient.q_pred == 40 && !first_sufficient.probe_issued,
          "one sufficient observation does not release memory");
  const auto second_sufficient = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, probing_cfg, &probing_state);
  require(second_sufficient.q_pred == 20 &&
              second_sufficient.probe_issued &&
              probing_state.difficulty.probe_active,
          "repeated sufficiency starts a physical geometric probe");

  probing_cfg.q_cur = 20;
  const auto successful_probe = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, probing_cfg, &probing_state);
  require(!successful_probe.service_violation &&
              successful_probe.q_pred == 20 &&
              !probing_state.difficulty.probe_active &&
              probing_state.difficulty.sufficient_upper == 20,
          "a sufficient unguarded probe becomes the retained upper point");

  hh::sizing::PolicyState failed_probe_state;
  probing_cfg.q_cur = 40;
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, probing_cfg, &failed_probe_state);
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, probing_cfg, &failed_probe_state);
  hh::SnapshotEx moderate_error_snap = efficiency_snap;
  moderate_error_snap.errors = {{15}, {15}};
  probing_cfg.q_cur = 20;
  const auto failed_probe = hh::sizing::next_q_ss(
      efficiency_result, {moderate_error_snap}, probing_cfg, &failed_probe_state);
  require(failed_probe.service_violation && failed_probe.probe_failed &&
              failed_probe.q_up == 30 && failed_probe.q_pred == 30 &&
              !failed_probe_state.difficulty.probe_active,
          "an insufficient probe recovers through fresh upward sizing");

  hh::sizing::PolicyConfig guarded_cfg = probing_cfg;
  guarded_cfg.probe_residual_guard = true;
  guarded_cfg.q_cur = 40;
  hh::sizing::PolicyState guarded_state;
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, guarded_cfg, &guarded_state);
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, guarded_cfg, &guarded_state);
  guarded_cfg.q_cur = 20;
  const auto guarded_failure = hh::sizing::next_q_ss(
      efficiency_result, {moderate_error_snap}, guarded_cfg, &guarded_state);
  require(guarded_failure.q_pred == 30 &&
              guarded_failure.probe_failed &&
              guarded_state.difficulty.guarded_demand == 30 &&
              guarded_state.difficulty.probe_residual > 0.0,
          "a failed probe retains its recovered demand");

  guarded_cfg.q_cur = 30;
  const auto first_recovery = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, guarded_cfg, &guarded_state);
  require(first_recovery.q_pred == 30 &&
              !first_recovery.probe_issued &&
              guarded_state.difficulty.guarded_demand == 30,
          "one sufficient recovery window holds the guarded demand");

  hh::SnapshotEx low_error_snap = efficiency_snap;
  low_error_snap.errors = {{5}, {5}};
  const auto confirmed_recovery = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, guarded_cfg, &guarded_state);
  require(confirmed_recovery.q_pred == 30 &&
              !confirmed_recovery.probe_issued &&
              guarded_state.difficulty.guarded_demand == 0,
          "two sufficient recovery windows clear the failed-probe guard");

  const auto first_post_recovery = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, guarded_cfg, &guarded_state);
  require(first_post_recovery.q_pred == 30 &&
              !first_post_recovery.probe_issued,
          "one post-recovery observation does not yet probe");
  const auto guarded_retry = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, guarded_cfg, &guarded_state);
  require(guarded_retry.q_pred == 26 && guarded_retry.probe_issued &&
              guarded_state.difficulty.probe_active,
          "confirmed recovery permits a later real probe");

  guarded_cfg.q_cur = 26;
  const auto guarded_first_success = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, guarded_cfg, &guarded_state);
  require(guarded_first_success.q_pred == 26 &&
              guarded_state.difficulty.probe_active &&
              guarded_state.difficulty.probe_success_streak == 1,
          "one successful guarded probe awaits confirmation");
  const auto guarded_second_success = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, guarded_cfg, &guarded_state);
  require(guarded_second_success.q_pred == 26 &&
              !guarded_state.difficulty.probe_active &&
              guarded_state.difficulty.probe_residual == 0.0,
          "two successful guarded observations accept the lower capacity");

  hh::sizing::PolicyState blocked_comfort_state;
  blocked_comfort_state.difficulty.sufficient_upper = 30;
  blocked_comfort_state.difficulty.sufficient_streak = 1;
  blocked_comfort_state.difficulty.guarded_demand = 30;
  blocked_comfort_state.difficulty.probe_success_streak = 0;
  hh::sizing::PolicyConfig blocked_comfort_cfg = guarded_cfg;
  blocked_comfort_cfg.probe_strategy = hh::sizing::ProbeStrategy::comfort;
  blocked_comfort_cfg.q_cur = 30;
  const auto blocked_comfort = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, blocked_comfort_cfg,
      &blocked_comfort_state);
  require(blocked_comfort.q_pred == 30 &&
              !blocked_comfort.probe_issued,
          "a blocking guard holds capacity instead of forcing a q-minus-one probe");

  hh::sizing::PolicyConfig comfort_cfg = probing_cfg;
  comfort_cfg.probe_strategy = hh::sizing::ProbeStrategy::comfort;
  comfort_cfg.probe_residual_guard = true;
  comfort_cfg.q_cur = 40;
  hh::sizing::PolicyState comfort_state;
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, comfort_cfg, &comfort_state);
  const auto comfort_probe = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, comfort_cfg, &comfort_state);
  require(comfort_probe.q_pred == 35 && comfort_probe.probe_issued,
          "comfort probing uses a capped arithmetic midpoint");

  hh::sizing::PolicyConfig pressure_cfg = probing_cfg;
  pressure_cfg.probe_strategy = hh::sizing::ProbeStrategy::pressure;
  pressure_cfg.probe_residual_guard = true;
  pressure_cfg.q_cur = 40;

  hh::SnapshotEx hold_snap = efficiency_snap;
  hold_snap.errors = {{7}, {7}};
  hh::sizing::PolicyState hold_state;
  (void)hh::sizing::next_q_ss(
      efficiency_result, {hold_snap}, pressure_cfg, &hold_state);
  const auto pressure_hold = hh::sizing::next_q_ss(
      efficiency_result, {hold_snap}, pressure_cfg, &hold_state);
  require(pressure_hold.q_pred == 40 &&
              !pressure_hold.probe_issued &&
              !pressure_hold.service_violation,
          "pressure above one half of the budget vetoes a guarded probe");

  hh::sizing::PolicyState pressure_state;
  const auto pressure_wait = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, pressure_cfg, &pressure_state);
  require(pressure_wait.q_pred == 40 &&
              !pressure_wait.probe_issued,
          "one low-pressure observation does not bypass the temporal guard");
  const auto pressure_probe = hh::sizing::next_q_ss(
      efficiency_result, {low_error_snap}, pressure_cfg, &pressure_state);
  require(pressure_probe.q_pred == 35 &&
              pressure_probe.probe_issued,
          "guarded half-budget pressure uses a capped arithmetic midpoint");

  hh::sizing::PolicyState zero_state;
  (void)hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, pressure_cfg, &zero_state);
  const auto zero_probe = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, pressure_cfg, &zero_state);
  require(zero_probe.q_pred == 35 &&
              zero_probe.probe_issued,
          "a guarded censored zero margin permits only a shallow half-n release");

  pressure_cfg.q_cur = 35;
  const auto pressure_failure = hh::sizing::next_q_ss(
      efficiency_result, {moderate_error_snap}, pressure_cfg, &zero_state);
  require(pressure_failure.probe_failed &&
              pressure_failure.q_pred == 53 &&
              zero_state.difficulty.probe_retry_depth == 3,
          "a failed pressure probe halves the next admissible release depth");

  pressure_cfg.q_cur = 53;
  const auto pressure_recovery = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, pressure_cfg, &zero_state);
  require(pressure_recovery.q_pred == 53 &&
              !pressure_recovery.probe_issued &&
              zero_state.difficulty.guarded_demand == 0,
          "one sufficient recovery observation clears the failed-probe hold");
  const auto pressure_retry_wait = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, pressure_cfg, &zero_state);
  require(pressure_retry_wait.q_pred == 53 &&
              !pressure_retry_wait.probe_issued,
          "a recovered pressure probe again waits for guarded confirmation");
  const auto pressure_retry = hh::sizing::next_q_ss(
      efficiency_result, {efficiency_snap}, pressure_cfg, &zero_state);
  require(pressure_retry.q_pred == 50 &&
              pressure_retry.probe_issued,
          "the next pressure-authorized retry respects the halved failure depth");

  hh::GlobalResultLB resolved_result = efficiency_result;
  resolved_result.items = {
      {cheap_id, "cheap-ambiguity", 110, 0, 1, 1.0, 102, 120, false},
  };
  hh::sizing::PolicyState ambiguity_state;
  ambiguity_state.difficulty.ambiguity_margin = std::log(12.0 / 10.0);
  efficiency_cfg.ambiguity_decay = 0.5;
  const auto persisted = hh::sizing::next_q_ss(
      resolved_result, {efficiency_snap}, efficiency_cfg, &ambiguity_state);
  require(std::abs(persisted.b_q) < 1e-12,
          "ambiguity persistence does not alter the temporal residual bias");
  require(std::abs(persisted.g_amb - 0.5 * std::log(12.0 / 10.0)) < 1e-12 &&
              std::abs(ambiguity_state.difficulty.ambiguity_margin -
                       persisted.g_amb) < 1e-12,
          "unrefreshed ambiguity margin decays by the shared beta");

  cfg.kind = hh::sizing::PolicyKind::fixed;
  cfg.q_cur = 9;
  cfg.q_cap = 4;
  cfg.q_max = 5;
  const auto fixed = hh::sizing::next_q_ss(result, {s0, s1}, cfg, nullptr);
  require(fixed.q_next == 4, "fixed policy honors configured cap");
}

void test_hybrid_exact_head_and_tail_floor() {
  const Id128 hot = id_for("hot");
  const Id128 cold = id_for("cold");

  hh::HybridSS hybrid(1, 1);
  hybrid.seed_head({hot, cold});
  require(hybrid.head_size() == 1, "hybrid respects exact-head capacity while seeding");

  hybrid.update(hot, 5);
  hybrid.update(cold, 3);
  const auto snap = hybrid.snapshot_ex();
  require(snap.N_local == 8, "hybrid tracks combined local mass");
  require(snap.head_mass == 5, "hybrid exact head tracks seeded key exactly");
  require(snap.head_size == 1, "hybrid reports head size");
  require(snap.errors_from == snap.head_size,
          "hybrid stores per-item errors only for the residual suffix");
  require(snap.errors.size() == snap.candidates.size() - snap.head_size,
          "hybrid omits redundant zero errors for exact-head records");
  require(snap.cand_eps(0) == 0 && snap.cand_lb(0) == snap.candidates[0].est,
          "hybrid exact-head prefix derives an exact interval without metadata");

  const auto hot_item = std::find_if(snap.candidates.begin(), snap.candidates.end(),
                                     [&](const hh::Cand& c) { return c.id == hot; });
  require(hot_item != snap.candidates.end() && hot_item->est == 5,
          "hybrid snapshot includes exact head count");

  hybrid.reconfigure(1, 1, 10, 0.25);
  const auto resized = hybrid.snapshot_ex();
  require(resized.q_local >= 8, "hybrid tail capacity is floored by residual discoverability mass");

  hh::HybridSS sparse_head(2, 1);
  std::size_t sparse_admissions = 0;
  sparse_head.set_admission_callback([&](const Id128&) { ++sparse_admissions; });
  sparse_head.seed_head({hot, cold});
  require(sparse_head.head_size() == 2, "hybrid keeps zero-count promoted keys in the local dictionary");
  sparse_head.update(hot, 7);
  sparse_head.update(hot, 1);
  const auto sparse_snap = sparse_head.snapshot_ex();
  require(sparse_admissions == 0,
          "replicated exact-head slots do not bind raw keys in worker-local state");
  require(sparse_snap.head_size == 1, "hybrid emits only nonzero exact-head reports");
  require(sparse_snap.cand_lb(0) == sparse_snap.candidates[0].est
              && sparse_snap.cand_eps(0) == 0,
          "head-only hybrid report exposes an exact interval");
  const auto cold_item = std::find_if(sparse_snap.candidates.begin(), sparse_snap.candidates.end(),
                                      [&](const hh::Cand& c) { return c.id == cold; });
  require(cold_item == sparse_snap.candidates.end(),
          "hybrid suppresses zero-count exact-head reports");

  hh::HybridSS compact_tail(2, 2, /*tail_per_item_eps=*/false);
  compact_tail.seed_head({hot, cold});
  compact_tail.update(hot, 7);
  compact_tail.update(id_for("tail-a"), 4);
  compact_tail.update(id_for("tail-b"), 3);
  compact_tail.update(id_for("tail-c"), 1);
  const auto compact_snap = compact_tail.snapshot_ex();
  require(compact_snap.tail_sorted_by_id,
          "hybrid prepares its residual report in identity order");
  require(std::is_sorted(
              compact_snap.candidates.begin()
                  + static_cast<std::ptrdiff_t>(compact_snap.head_size),
              compact_snap.candidates.end(),
              [](const hh::Cand& a, const hh::Cand& b) {
                return a.id.b < b.id.b;
              }),
          "hybrid residual report satisfies its sorted-tail contract");
  require(!compact_snap.has_error_bounds(), "hybrid compact tail omits per-candidate tail errors");
  require(compact_snap.has_sketch_error_bound(), "hybrid compact tail exports one shared tail error");
  require(compact_snap.sketch_eps_from == compact_snap.head_size,
          "hybrid compact tail applies shared error only after exact-head prefix");

  hh::ArenaMap compact_map;
  compact_map.bind_bytes(hot, "hot");
  std::vector<hh::SnapshotEx> compact_snaps{compact_snap};
  std::vector<const hh::ArenaMap*> compact_maps{&compact_map};
  const auto compact_reduced =
      hh::Coordinator::reduce_global_with_lb(compact_snaps, 10);
  hh::ReduceTelemetry streaming_telemetry;
  const auto compact_streamed = hh::Coordinator::reduce_global_streaming_with_lb(
      compact_snaps, 10, &streaming_telemetry);
  hh::ReduceTelemetry hash_telemetry;
  hh::Coordinator::reduce_global_with_lb(compact_snaps, 10, &hash_telemetry);
  hh::ReduceTelemetry parallel_telemetry;
  const auto compact_parallel = hh::Coordinator::reduce_global_parallel_streaming_with_lb(
      compact_snaps, 10, &parallel_telemetry);
  require_same_reduction(compact_reduced, compact_streamed);
  require_same_reduction(compact_reduced, compact_parallel);

  std::vector<Id128> compact_head_ids{hot, cold};
  std::sort(compact_head_ids.begin(), compact_head_ids.end(), [](const Id128& a, const Id128& b) {
    return a.b < b.b;
  });
  std::vector<std::string> compact_head_keys;
  compact_head_keys.reserve(compact_head_ids.size());
  for (const auto& id : compact_head_ids) {
    compact_head_keys.push_back(id == hot ? "hot" : "cold");
  }
  const auto coordinated = hh::Coordinator::reduce_hybrid_streaming_for_control(
      compact_snaps, 10, /*top_limit=*/10, compact_head_ids);
  const auto coordinated_parallel =
      hh::Coordinator::reduce_hybrid_parallel_streaming_for_control(
          compact_snaps, 10, /*top_limit=*/10, compact_head_ids);
  const auto coordinated_parallel_reused =
      hh::Coordinator::reduce_hybrid_parallel_streaming_for_control(
          compact_snaps, 10, /*top_limit=*/10, compact_head_ids);
  hh::ArenaMap headless_map;
  std::vector<const hh::ArenaMap*> headless_maps{&headless_map};
  const auto coordinated_headless =
      hh::Coordinator::reduce_hybrid_streaming_for_control(
          compact_snaps, 10, /*top_limit=*/10, compact_head_ids);
  const auto* headless_hot = find_item(coordinated_headless.published, hot);
  require(headless_hot && headless_hot->key.empty(),
          "coordinated Hybrid reduction remains identifier-only");
  require_same_reduction(compact_streamed, coordinated.published);
  require_same_reduction(coordinated.published, coordinated_parallel.published);
  require_same_hybrid_sizing_items(
      coordinated.residual_items, coordinated_parallel.residual_items);
  require(coordinated.top_ids == coordinated_parallel.top_ids,
          "parallel coordinated hybrid reducer preserves ranked top ids");
  require(coordinated.top_resolution_workers
              == coordinated_parallel.top_resolution_workers,
          "parallel coordinated hybrid reducer preserves top-id resolution routes");
  require_same_reduction(
      coordinated_parallel.published, coordinated_parallel_reused.published);
  require_same_hybrid_sizing_items(
      coordinated_parallel.residual_items,
      coordinated_parallel_reused.residual_items);
  require(coordinated_parallel.top_ids == coordinated_parallel_reused.top_ids,
          "reused parallel reducer workspace preserves ranked top ids");
  require(coordinated_parallel.top_resolution_workers
              == coordinated_parallel_reused.top_resolution_workers,
          "reused parallel workspace preserves top-id resolution routes");
  require(coordinated_parallel.telemetry.ingress_bytes
              >= coordinated.telemetry.ingress_bytes,
          "parallel coordinated hybrid telemetry accounts active shard ingress");
  require(!coordinated.top_ids.empty() && coordinated.top_ids.front() == hot,
          "coordinated hybrid reducer retains ranked top ids");

  hh::SnapshotEx residual_snap = compact_snap;
  const std::size_t exact_prefix = residual_snap.head_size;
  const bool residual_has_errors = residual_snap.has_error_bounds();
  residual_snap.candidates.erase(
      residual_snap.candidates.begin(),
      residual_snap.candidates.begin() + static_cast<std::ptrdiff_t>(exact_prefix));
  if (residual_has_errors) {
    residual_snap.errors.erase(
        residual_snap.errors.begin(),
        residual_snap.errors.begin() + static_cast<std::ptrdiff_t>(exact_prefix));
  }
  residual_snap.head_size = 0;
  residual_snap.head_mass = 0;
  if (residual_snap.has_sketch_error_bound()) residual_snap.sketch_eps_from = 0;
  const auto expected_residual = hh::Coordinator::reduce_global_streaming_with_lb(
      {residual_snap}, 10);
  require(expected_residual.items.size() == coordinated.residual_items.size(),
          "coordinated residual keeps the expected tail candidates");
  for (const auto& item : expected_residual.items) {
    const auto* found = find_hybrid_sizing_item(coordinated.residual_items, item.id);
    require(found != nullptr && found->est == item.est &&
                found->cert_lb == item.cert_lb && found->cert_ub == item.cert_ub,
            "coordinated residual preserves tail estimates and bounds");
  }
  for (const auto& item : coordinated.residual_items) {
    require(item.has_cert_components,
            "coordinated residual retains exact sizing components");
  }
  auto legacy_residual = expected_residual;
  for (auto& item : legacy_residual.items) {
    item.cert_inflation = 0;
    item.cert_hidden_mass = 0;
    item.has_cert_components = false;
  }
  hh::sizing::PolicyConfig sizing_cfg;
  sizing_cfg.kind = hh::sizing::PolicyKind::difficulty;
  sizing_cfg.n_param = 10;
  sizing_cfg.q_cur = 2;
  sizing_cfg.q_min = 1;
  sizing_cfg.q_cap = 100;
  sizing_cfg.q_max = 100;
  sizing_cfg.alpha_req = 0.95;
  sizing_cfg.epsilon_m = 0.15;
  sizing_cfg.censored_control = false;
  sizing_cfg.ambiguity_adjust = false;
  hh::sizing::PolicyState legacy_state;
  hh::sizing::PolicyState coordinated_state;
  const auto legacy_sizing = hh::sizing::next_q_ss(
      legacy_residual, {residual_snap}, sizing_cfg, &legacy_state);
  const auto coordinated_sizing = hh::sizing::next_q_ss(
      coordinated.residual_items, coordinated.published.N_global,
      coordinated.published.threshold, {}, sizing_cfg, &coordinated_state);
  require(std::abs(legacy_sizing.margin_alpha - coordinated_sizing.margin_alpha) < 1e-12,
          "precomputed residual components preserve the sizing margin");
  require(legacy_sizing.q_next == coordinated_sizing.q_next &&
              legacy_sizing.q_req == coordinated_sizing.q_req,
          "precomputed residual components preserve the sizing decision");
  require(streaming_telemetry.total_peak_bytes > 0,
          "streaming reducer reports modeled resident memory");
  require(hash_telemetry.ingress_bytes > 0 && streaming_telemetry.ingress_bytes > 0,
          "reducers report resident ingress separately from working memory");
  require(parallel_telemetry.ingress_bytes >= streaming_telemetry.ingress_bytes,
          "parallel reducer accounts for all concurrently active merge shards");
  const auto* hot_reduced = find_item(compact_reduced, hot);
  require(hot_reduced && hot_reduced->cert_lb == hot_reduced->est,
          "hybrid compact exact head keeps exact lower bound");

  hh::HybridSS exact_worker(1, 1);
  hh::HybridSS zero_worker(1, 1);
  exact_worker.seed_head({hot});
  zero_worker.seed_head({hot});
  exact_worker.update(hot, 7);
  zero_worker.update(cold, 10);
  hh::ArenaMap exact_map;
  hh::ArenaMap zero_map;
  exact_map.bind_bytes(hot, "hot");
  zero_map.bind_bytes(cold, "cold");
  const std::vector<hh::SnapshotEx> exact_snaps{
      exact_worker.snapshot_ex(), zero_worker.snapshot_ex()};
  const std::vector<const hh::ArenaMap*> exact_maps{&exact_map, &zero_map};
  const std::vector<Id128> exact_head_ids{hot};
  const std::vector<std::string> exact_head_keys{"hot"};
  const auto exact_hash = hh::Coordinator::reduce_global_with_lb(
      exact_snaps, 10, nullptr, false);
  const auto exact_stream = hh::Coordinator::reduce_global_streaming_with_lb(
      exact_snaps, 10, nullptr, false);
  const auto exact_coordinated =
      hh::Coordinator::reduce_hybrid_streaming_for_control(
          exact_snaps, 10, 10, exact_head_ids);
  for (const auto* result :
       {&exact_hash, &exact_stream, &exact_coordinated.published}) {
    const auto* item = find_item(*result, hot);
    require(item && item->est == 7 && item->lb == 7
                && item->cert_lb == 7 && item->cert_ub == 7,
            "missing zero-count head reports do not inflate an exact certificate");
  }
}

void test_hybrid_generation_checked_head_delta() {
  std::vector<Id128> installed{
      id_for("head-a"), id_for("head-b"), id_for("head-c")};
  std::sort(installed.begin(), installed.end(),
            [](const Id128& a, const Id128& b) { return a.b < b.b; });
  const Id128 added = id_for("head-d");

  hh::HybridSS hybrid(3, 1);
  hybrid.install_head(installed, 7);
  require(hybrid.head_generation() == 7,
          "full head install records its generation");
  hybrid.update(installed.front(), 4);
  hybrid.reset_window_preserve_head();

  hh::ExactHeadDelta delta;
  delta.base_generation = 7;
  delta.next_generation = 8;
  delta.removed_slots = {1};
  delta.added_ids = {added};
  require(hybrid.apply_head_delta(delta, 3),
          "matching head generation accepts a delta");
  require(hybrid.head_generation() == 8,
          "accepted head delta advances the generation");

  std::vector<Id128> expected{installed[0], installed[2], added};
  std::sort(expected.begin(), expected.end(),
            [](const Id128& a, const Id128& b) { return a.b < b.b; });
  for (const auto& id : expected) hybrid.update(id, 1);
  const auto snapshot = hybrid.snapshot_ex();
  require(snapshot.head_size == expected.size(),
          "delta-installed head reports every active slot");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(snapshot.candidates[i].id == expected[i],
            "delta installation matches a canonical full dictionary");
  }

  hh::ExactHeadDelta stale = delta;
  stale.next_generation = 9;
  require(!hybrid.apply_head_delta(stale, 3),
          "stale base generation rejects a head delta");
  require(hybrid.head_generation() == 8,
          "rejected head delta preserves the installed generation");

  hybrid.install_head(installed, 9);
  require(hybrid.head_generation() == 9,
          "full checkpoint recovers to an explicit generation");
}

void test_ss_coordinated_controller_projection() {
  const Id128 hot = id_for("ss-control-hot");
  const Id128 warm = id_for("ss-control-warm");
  const Id128 cold = id_for("ss-control-cold");
  hh::SpaceSaving first(2);
  hh::SpaceSaving second(2);
  first.update(hot, 12);
  first.update(warm, 5);
  first.update(cold, 2);
  second.update(hot, 8);
  second.update(warm, 6);
  second.update(cold, 3);

  const std::vector<hh::SnapshotEx> snaps{
      first.snapshot_ex(), second.snapshot_ex()};
  hh::ArenaMap first_map;
  hh::ArenaMap second_map;
  first_map.bind_bytes(hot, "ss-control-hot");
  first_map.bind_bytes(warm, "ss-control-warm");
  first_map.bind_bytes(cold, "ss-control-cold");
  second_map.bind_bytes(hot, "ss-control-hot");
  second_map.bind_bytes(warm, "ss-control-warm");
  second_map.bind_bytes(cold, "ss-control-cold");
  const std::vector<const hh::ArenaMap*> maps{&first_map, &second_map};

  const auto legacy_published = hh::Coordinator::reduce_global_streaming_with_lb(
      snaps, 10);
  const auto legacy_control = hh::Coordinator::reduce_global_streaming_with_lb(
      snaps, 10, nullptr, false);
  const auto coordinated =
      hh::Coordinator::reduce_ss_streaming_for_control(snaps, 10);
  require_same_reduction(legacy_published, coordinated.published);

  std::size_t expected_frontier = 0;
  for (const auto& item : legacy_control.items) {
    if (item.cert_ub < legacy_control.threshold) continue;
    ++expected_frontier;
    const auto* projected =
        find_hybrid_sizing_item(coordinated.residual_items, item.id);
    require(projected && projected->est == item.est
                && projected->cert_lb == item.cert_lb
                && projected->cert_ub == item.cert_ub,
            "single-pass SS controller projection preserves frontier bounds");
  }
  require(coordinated.residual_items.size() == expected_frontier,
          "single-pass SS controller projection retains only K+ union K?");

  hh::sizing::PolicyConfig cfg;
  cfg.kind = hh::sizing::PolicyKind::difficulty;
  cfg.n_param = 10;
  cfg.q_cur = 2;
  cfg.q_min = 1;
  cfg.q_cap = 100;
  cfg.q_max = 100;
  cfg.alpha_req = 0.95;
  cfg.epsilon_m = 0.15;
  cfg.ambiguity_adjust = false;
  hh::sizing::PolicyState legacy_state;
  hh::sizing::PolicyState coordinated_state;
  const auto legacy_decision =
      hh::sizing::next_q_ss(legacy_control, snaps, cfg, &legacy_state);
  const auto coordinated_decision = hh::sizing::next_q_ss(
      coordinated.residual_items, coordinated.published.N_global,
      coordinated.published.threshold, {}, cfg, &coordinated_state);
  require(legacy_decision.q_next == coordinated_decision.q_next
              && legacy_decision.q_req == coordinated_decision.q_req
              && legacy_decision.q_base == coordinated_decision.q_base,
          "single-pass SS projection preserves the controller decision");
}

void test_json_gz_reader_orders_windows_and_partitions() {
  const auto root = std::filesystem::temp_directory_path() / "hh_tests_reader";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  write_gzip_json(root / "window_000002.json.gz", R"({"1":[["late",2]],"0":["early"]})");
  write_gzip_json(root / "window_000010.json.gz", R"({"0":["ten"]})");

  struct Event {
    std::size_t win;
    std::size_t part;
    std::string key;
    int weight;
  };
  std::vector<Event> events;
  const auto emitted = hh::JsonGzNestedReader::read(root.string(), [&](std::size_t w, std::size_t p,
                                                                       const std::string& k, int weight) {
    events.push_back({w, p, k, weight});
  });

  std::filesystem::remove_all(root);

  require(emitted == 3 && events.size() == 3, "reader emits expected event count");
  require(events[0].win == 2 && events[0].part == 0 && events[0].key == "early" && events[0].weight == 1,
          "reader infers window id from filename and orders numeric partitions");
  require(events[1].win == 2 && events[1].part == 1 && events[1].key == "late" && events[1].weight == 2,
          "reader preserves weighted entries");
  require(events[2].win == 10 && events[2].part == 0 && events[2].key == "ten",
          "reader processes directory files in lexical order while preserving parsed ids");
}

void test_error_metrics_include_missed_heavy_hitters() {
  const Id128 retained = id_for("retained-hh");
  const Id128 missed = id_for("missed-hh");
  const Id128 false_positive = id_for("false-positive");

  hh::GlobalResultLB oracle;
  oracle.threshold = 50;
  oracle.N_global = 200;
  oracle.items = {
      hh::GlobalItemLB{.id = retained, .est = 100},
      hh::GlobalItemLB{.id = missed, .est = 80},
      hh::GlobalItemLB{.id = false_positive, .est = 20},
  };

  hh::GlobalResultLB below_threshold;
  below_threshold.threshold = oracle.threshold;
  below_threshold.N_global = oracle.N_global;
  below_threshold.items = {
      hh::GlobalItemLB{.id = retained, .est = 90},
      hh::GlobalItemLB{.id = false_positive, .est = 60},
      hh::GlobalItemLB{.id = missed, .est = 40},
  };
  const auto classified_miss =
      hh::eval_vs_oracle(oracle, below_threshold, std::nullopt);
  require(std::fabs(classified_miss.hh_recall - 0.5) < 1e-12,
          "sub-threshold oracle HH is a false negative");
  require(std::fabs(classified_miss.aae - 25.0) < 1e-12,
          "AAE uses the reported estimate of a sub-threshold oracle HH");
  require(std::fabs(classified_miss.are - 0.3) < 1e-12,
          "ARE uses the reported estimate of a sub-threshold oracle HH");

  hh::GlobalResultLB absent = below_threshold;
  absent.items.pop_back();
  const auto absent_miss = hh::eval_vs_oracle(oracle, absent, std::nullopt);
  require(std::fabs(absent_miss.aae - 45.0) < 1e-12,
          "AAE assigns estimate zero to an absent oracle HH");
  require(std::fabs(absent_miss.are - 0.55) < 1e-12,
          "ARE assigns estimate zero to an absent oracle HH");
}

void run_all_tests() {
  init_hashing();
  test_arena_map_flat_index_lifecycle();
  test_space_saving_bounds_and_reconfigure();
  test_retained_key_binding_lifecycle();
  test_space_saving_max_sketch_error_snapshot();
  test_coordinator_certification_envelope();
  test_heavylocker_serial_bucket_merge();
  test_heavylocker_packed_layout_and_reference_rng();
  test_heavylocker_residual_certificate();
  test_sizing_policy_outputs_are_clamped_and_finite();
  test_hybrid_exact_head_and_tail_floor();
  test_hybrid_generation_checked_head_delta();
  test_ss_coordinated_controller_projection();
  test_json_gz_reader_orders_windows_and_partitions();
  test_error_metrics_include_missed_heavy_hitters();
}

} // namespace

int main() {
  try {
    run_all_tests();
  } catch (const TestFailure& e) {
    std::cerr << "TEST FAILURE: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "UNEXPECTED EXCEPTION: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All hh tests passed\n";
  return EXIT_SUCCESS;
}
