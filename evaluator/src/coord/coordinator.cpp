#include "hh/coord/coordinator.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hh {

namespace {

// Logical coordinator result record under the common 128-bit fingerprint
// contract. Raw-key strings and std::vector padding belong to the evaluator,
// not to the distributed algorithm's required state.
constexpr std::size_t kLogicalGlobalItemBytes =
    sizeof(Id128)                    // fingerprint
    + 7 * sizeof(std::uint64_t)      // estimates, bounds, coverage components
    + sizeof(double)                 // reporting-mass coverage
    + sizeof(std::uint32_t)          // raw-key resolution worker
    + 2 * sizeof(std::uint8_t);      // component and guaranteed flags

constexpr std::size_t kLogicalCertificateRecordBytes =
    sizeof(Id128) + 2 * sizeof(std::uint32_t); // id, estimate, epsilon

// A generation-based fixed worker pool avoids creating reducer threads for
// every window. Calls are serialized because the coordinator owns one logical
// reduction at a time; worker tasks within a call remain fully parallel.
class ReducerThreadPool {
public:
  ~ReducerThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      ++generation_;
    }
    start_cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  template <class Fn>
  void parallel_for(std::size_t worker_count, std::size_t task_count, Fn&& fn) {
    if (task_count == 0) return;
    worker_count = std::max<std::size_t>(
        1, std::min(worker_count, task_count));
    if (worker_count == 1) {
      for (std::size_t task = 0; task < task_count; ++task) fn(task);
      return;
    }

    std::lock_guard<std::mutex> run_lock(run_mutex_);
    ensure_workers(worker_count);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = std::forward<Fn>(fn);
      task_count_ = task_count;
      next_task_.store(0, std::memory_order_relaxed);
      active_workers_ = worker_count;
      remaining_workers_ = worker_count;
      error_ = nullptr;
      ++generation_;
    }
    start_cv_.notify_all();

    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [&] { return remaining_workers_ == 0; });
    task_ = {};
    if (error_) std::rethrow_exception(error_);
  }

private:
  void ensure_workers(std::size_t count) {
    while (workers_.size() < count) {
      const std::size_t index = workers_.size();
      workers_.emplace_back([this, index] { worker_loop(index); });
    }
  }

  void worker_loop(std::size_t index) {
    std::size_t observed_generation = 0;
    while (true) {
      std::function<void(std::size_t)> task;
      std::size_t task_count = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        start_cv_.wait(lock, [&] {
          return stopping_ || generation_ != observed_generation;
        });
        if (stopping_) return;
        observed_generation = generation_;
        if (index >= active_workers_) continue;
        task = task_;
        task_count = task_count_;
      }

      try {
        while (true) {
          const std::size_t item =
              next_task_.fetch_add(1, std::memory_order_relaxed);
          if (item >= task_count) break;
          task(item);
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_) error_ = std::current_exception();
        next_task_.store(task_count, std::memory_order_relaxed);
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--remaining_workers_ == 0) done_cv_.notify_one();
      }
    }
  }

  std::mutex run_mutex_;
  std::mutex mutex_;
  std::condition_variable start_cv_;
  std::condition_variable done_cv_;
  std::vector<std::thread> workers_;
  std::function<void(std::size_t)> task_;
  std::atomic<std::size_t> next_task_{0};
  std::size_t task_count_{0};
  std::size_t active_workers_{0};
  std::size_t remaining_workers_{0};
  std::size_t generation_{0};
  bool stopping_{false};
  std::exception_ptr error_;
};

ReducerThreadPool& reducer_thread_pool() {
  static ReducerThreadPool pool;
  return pool;
}

std::vector<unsigned> balanced_prefix_boundaries(
    const std::array<std::size_t, 256>& histogram,
    std::size_t shard_count) {
  shard_count = std::max<std::size_t>(
      1, std::min<std::size_t>(256, shard_count));
  std::array<std::size_t, 257> cumulative{};
  for (std::size_t prefix = 0; prefix < histogram.size(); ++prefix) {
    cumulative[prefix + 1] = cumulative[prefix] + histogram[prefix];
  }

  std::vector<unsigned> boundaries(shard_count + 1, 0);
  boundaries.back() = 256;
  const std::size_t total = cumulative.back();
  for (std::size_t shard = 1; shard < shard_count; ++shard) {
    const std::size_t target = total * shard / shard_count;
    const std::size_t minimum = boundaries[shard - 1] + 1;
    const std::size_t maximum = 256 - (shard_count - shard);
    const auto begin = cumulative.begin()
        + static_cast<std::ptrdiff_t>(minimum);
    const auto end = cumulative.begin()
        + static_cast<std::ptrdiff_t>(maximum + 1);
    const auto it = std::lower_bound(begin, end, target);
    boundaries[shard] = static_cast<unsigned>(
        std::min<std::size_t>(
            maximum,
            std::max<std::size_t>(
                minimum,
                static_cast<std::size_t>(it - cumulative.begin()))));
  }
  return boundaries;
}

struct HybridRankedId {
  Id128 id;
  std::uint64_t est{0};
  std::uint32_t resolution_worker{kNoResolutionWorker};
};

struct HybridShardWorkspace {
  std::vector<GlobalItemLB> published;
  std::vector<HybridSizingItem> residual_items;
  std::vector<HybridRankedId> ranked;
  std::vector<std::size_t> ends;

  void clear() {
    published.clear();
    residual_items.clear();
    ranked.clear();
    ends.clear();
  }
};

std::mutex hybrid_workspace_mutex;
std::vector<HybridShardWorkspace> hybrid_workspaces;

} // namespace

std::string Coordinator::id128_hex(const Id128& id) {
  static const char* HEX = "0123456789abcdef";
  std::string s; s.resize(32);
  for (int i = 0; i < 16; ++i) {
    const unsigned v = id.b[i];
    s[2 * i + 0] = HEX[(v >> 4) & 0xF];
    s[2 * i + 1] = HEX[v & 0xF];
  }
  return s;
}

GlobalResultLB Coordinator::reduce_global_with_lb(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    ReduceTelemetry* telemetry,
    bool prune_certified_non_hh)
{
  GlobalResultLB R{};
  if (n_param == 0) return R;
  if (telemetry) *telemetry = ReduceTelemetry{};
  if (telemetry) {
    constexpr std::size_t kHeaderBytes = 32;
    constexpr std::size_t kRecordBytes = kLogicalCertificateRecordBytes;
    telemetry->ingress_bytes = snaps_ex.size() * (kHeaderBytes + kRecordBytes);
  }

  // 1) N_global and strict HH threshold:
  //    p(k) > 1/n  <=>  f(k) >= floor(N/n) + 1.
  for (const auto& s : snaps_ex) R.N_global += s.N_local;
  R.threshold = (R.N_global / n_param) + 1;

  // 2) Aggregate per id
  struct Agg {
    std::uint64_t est=0, lb=0, reporters=0;
    double mass_reporters=0.0;
    double min_counter_reporters=0.0;
    std::uint64_t eps_sum=0;      // sum of per-item eps over reporters (SS only)
    std::uint32_t resolution_worker{kNoResolutionWorker};
    bool has_ss_reporter{false};  // true if reported by at least one SS partition
    bool globally_exact{false};   // Hybrid exact-head dictionary entry
  };
  std::unordered_map<Id128, Agg, Id128Hash> agg;
  agg.reserve(1024);

  // Keep per-partition mass and SS min-counter terms c_i.
  std::vector<double> part_mass; part_mass.reserve(snaps_ex.size());
  std::vector<double> min_counter_by_part; min_counter_by_part.reserve(snaps_ex.size());
  for (const auto& s : snaps_ex) part_mass.push_back(static_cast<double>(s.N_local));

  double total_min_counter_mass = 0.0;
  std::size_t transient_presence_peak_bytes = 0;
  for (std::size_t idx=0; idx<snaps_ex.size(); ++idx) {
    const auto& s = snaps_ex[idx];
    // Space-Saving min-counter proxy c_i:
    // - if summary not full (|S_i| < q_i): c_i = 0
    // - else c_i = min retained counter
    double c_i = 0.0;
    const std::size_t exact_prefix =
        std::min(s.head_size, s.candidates.size());
    const std::size_t tail_size = s.candidates.size() - exact_prefix;
    if (s.q_local > 0 && tail_size >= s.q_local) {
      std::uint32_t mn = UINT32_MAX;
      for (std::size_t ci = exact_prefix; ci < s.candidates.size(); ++ci) {
        mn = std::min(mn, s.candidates[ci].est);
      }
      c_i = (mn == UINT32_MAX) ? 0.0 : static_cast<double>(mn);
    }
    min_counter_by_part.push_back(c_i);
    total_min_counter_mass += c_i;

    const bool has_bounds = s.has_error_bounds();
    const bool has_sketch_bound = s.has_sketch_error_bound();
    for (std::size_t ci = 0; ci < s.candidates.size(); ++ci) {
      const auto& c = s.candidates[ci];
      auto& a = agg[c.id];
      if (a.resolution_worker == kNoResolutionWorker) {
        a.resolution_worker = static_cast<std::uint32_t>(idx);
      }
      a.est += c.est;
      if (ci < s.head_size) {
        a.globally_exact = true;
        a.lb += c.est;
      } else if (has_bounds || (has_sketch_bound && ci >= s.sketch_eps_from)) {
        a.lb += s.cand_lb(ci);
        a.eps_sum += s.cand_eps(ci);
      }
    }

    // Deduplicate reporter accounting per partition. The set is intentionally
    // transient: the reducer only needs per-key reporter mass and reporter
    // min-counter mass, not a stored partition-by-key incidence matrix.
    std::unordered_set<Id128, Id128Hash> pres;
    pres.reserve(s.candidates.size()*2);
    for (const auto& c : s.candidates) pres.insert(c.id);
    transient_presence_peak_bytes = std::max(
        transient_presence_peak_bytes,
        sizeof(pres) + pres.bucket_count() * sizeof(void*) + pres.size() * sizeof(Id128));
    for (const auto& id : pres) {
      auto& a = agg[id];
      a.reporters += 1;
      a.mass_reporters += part_mass[idx];
      a.min_counter_reporters += c_i;
      if (s.q_local > 0 && !a.globally_exact) a.has_ss_reporter = true;
    }
  }

  if (telemetry) {
    std::size_t agg_bytes = 0;
    agg_bytes += sizeof(agg);
    agg_bytes += agg.bucket_count() * sizeof(void*);
    agg_bytes += agg.size() * sizeof(decltype(agg)::value_type);

    std::size_t presence_bytes = 0;
    presence_bytes += sizeof(part_mass) + part_mass.capacity() * sizeof(decltype(part_mass)::value_type);
    presence_bytes += sizeof(min_counter_by_part)
                   + min_counter_by_part.capacity() * sizeof(decltype(min_counter_by_part)::value_type);
    presence_bytes += transient_presence_peak_bytes;

    telemetry->agg_bytes = agg_bytes;
    telemetry->presence_bytes = presence_bytes;
    telemetry->total_peak_bytes = std::max(telemetry->total_peak_bytes, agg_bytes + presence_bytes);
  }

  // 3) Build identifier-only results. Raw keys are resolved only for query
  // output by the explicitly accounted resolution protocol in the harness.
  std::vector<GlobalItemLB> items; items.reserve(agg.size());
  for (const auto& kv : agg) {
    GlobalItemLB gi;
    gi.id  = kv.first;
    gi.est = kv.second.est;
    gi.lb  = kv.second.lb;
    gi.reporters = kv.second.reporters;
    gi.omega = (R.N_global > 0) ? (kv.second.mass_reporters / static_cast<double>(R.N_global)) : 0.0;

    // Routing-agnostic SS certification envelope:
    //   f_hat - sum_{reporters} eps_i <= f <= f_hat + sum_{non-reporters} c_j
    if (kv.second.globally_exact) {
      gi.lb = gi.est;
      gi.cert_lb = gi.est;
      gi.cert_ub = gi.est;
      gi.guaranteed = gi.est >= R.threshold;
    } else if (kv.second.has_ss_reporter) {
      const double inflation = static_cast<double>(kv.second.eps_sum);
      const double hidden_non_reporter_mass =
          std::max(0.0, total_min_counter_mass - kv.second.min_counter_reporters);
      const double cert_ub_d = static_cast<double>(gi.est) + hidden_non_reporter_mass;
      if (prune_certified_non_hh && cert_ub_d < static_cast<double>(R.threshold)) {
        continue;
      }

      const double cert_lb_d = static_cast<double>(gi.est) - inflation;
      gi.cert_lb = cert_lb_d > 0.0 ? static_cast<std::uint64_t>(cert_lb_d) : 0ull;
      gi.cert_ub = static_cast<std::uint64_t>(std::ceil(std::max(0.0, cert_ub_d)));
      gi.cert_inflation = kv.second.eps_sum;
      gi.cert_hidden_mass = static_cast<std::uint64_t>(hidden_non_reporter_mass);
      gi.has_cert_components = true;
      gi.guaranteed = (gi.cert_lb >= R.threshold);
    } else {
      // Fallback for non-SS sketches (no per-item epsilon / c_i telemetry contract).
      gi.cert_lb = gi.lb;
      gi.cert_ub = gi.est;
      gi.guaranteed = (gi.lb >= R.threshold);
    }

    gi.resolution_worker = kv.second.resolution_worker;
    items.push_back(std::move(gi));
  }

  if (telemetry) {
    const std::size_t items_bytes =
        sizeof(items) + items.capacity() * kLogicalGlobalItemBytes;
    telemetry->items_bytes = items_bytes;
    telemetry->total_peak_bytes = std::max(
        telemetry->total_peak_bytes,
        telemetry->agg_bytes + telemetry->presence_bytes + telemetry->items_bytes);
  }

  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){
    if (a.est != b.est) return a.est > b.est;
    return a.id.b < b.id.b;
  });

  R.items = std::move(items);
  return R;
}

GlobalResultLB Coordinator::reduce_global_streaming_with_lb(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    ReduceTelemetry* telemetry,
    bool prune_certified_non_hh,
    std::size_t parallelism)
{
  GlobalResultLB R{};
  if (n_param == 0) return R;
  if (telemetry) *telemetry = ReduceTelemetry{};

  const auto id_less = [](const Id128& a, const Id128& b) {
    return a.b < b.b;
  };

  for (const auto& s : snaps_ex) R.N_global += s.N_local;
  R.threshold = (R.N_global / n_param) + 1;

  struct Agg {
    std::uint64_t est{0};
    std::uint64_t lb{0};
    std::uint64_t reporters{0};
    double mass_reporters{0.0};
    double min_counter_reporters{0.0};
    std::uint64_t eps_sum{0};
    std::uint32_t resolution_worker{kNoResolutionWorker};
    bool has_ss_reporter{false};
    bool globally_exact{false};
  };
  struct Record {
    Id128 id;
    std::uint32_t est{0};
    std::uint32_t eps{0};
    enum class Bound : std::uint8_t { unknown, exact, epsilon } bound{Bound::unknown};
  };

  std::vector<double> part_mass(snaps_ex.size(), 0.0);
  std::vector<double> min_counter_by_part(snaps_ex.size(), 0.0);
  double total_min_counter_mass = 0.0;
  for (std::size_t pi = 0; pi < snaps_ex.size(); ++pi) {
    const auto& s = snaps_ex[pi];
    part_mass[pi] = static_cast<double>(s.N_local);
    const std::size_t exact_prefix =
        std::min(s.head_size, s.candidates.size());
    const std::size_t tail_size = s.candidates.size() - exact_prefix;
    if (s.q_local > 0 && tail_size >= s.q_local) {
      std::uint32_t mn = UINT32_MAX;
      for (std::size_t ci = exact_prefix; ci < s.candidates.size(); ++ci) {
        mn = std::min(mn, s.candidates[ci].est);
      }
      min_counter_by_part[pi] =
          (mn == UINT32_MAX) ? 0.0 : static_cast<double>(mn);
    }
    total_min_counter_mass += min_counter_by_part[pi];
  }

  // The promoted exact-head dictionary is shared state. Build its compact,
  // sorted slot order once, then aggregate active counters into dense slots.
  std::vector<Id128> head_ids;
  for (const auto& s : snaps_ex) {
    const std::size_t head_size = std::min(s.head_size, s.candidates.size());
    for (std::size_t ci = 0; ci < head_size; ++ci) {
      head_ids.push_back(s.candidates[ci].id);
    }
  }
  std::sort(head_ids.begin(), head_ids.end(), id_less);
  head_ids.erase(std::unique(head_ids.begin(), head_ids.end()), head_ids.end());
  // The dictionary is shared state, so retained coordinator memory should be
  // proportional to unique head slots, not to repeated active-slot reports.
  // Compact after reconstruction in this in-process benchmark.
  std::vector<Id128>(head_ids).swap(head_ids);
  std::vector<Agg> head_agg(head_ids.size());
  // These vectors emulate reports sorted independently at the workers. Their
  // storage is input-side preparation and is intentionally excluded from the
  // modeled coordinator working set below.
  std::vector<std::vector<Record>> tails(snaps_ex.size());
  std::array<std::size_t, 256> prefix_histogram{};

  auto add_value = [](Agg& a, const Record& r) {
    a.est += r.est;
    if (r.bound == Record::Bound::exact) {
      a.lb += r.est;
    } else if (r.bound == Record::Bound::epsilon) {
      a.lb += r.est >= r.eps ? (r.est - r.eps) : 0u;
    }
    a.eps_sum += r.eps;
  };
  auto head_slot = [&](const Id128& id) -> std::size_t {
    const auto it = std::lower_bound(head_ids.begin(), head_ids.end(), id, id_less);
    if (it == head_ids.end() || !(*it == id)) return head_ids.size();
    return static_cast<std::size_t>(it - head_ids.begin());
  };

  for (std::size_t pi = 0; pi < snaps_ex.size(); ++pi) {
    const auto& s = snaps_ex[pi];
    const bool has_bounds = s.has_error_bounds();
    const bool has_sketch_bound = s.has_sketch_error_bound();
    const std::size_t exact_prefix = std::min(s.head_size, s.candidates.size());
    std::vector<std::size_t> reported_head_slots;
    reported_head_slots.reserve(exact_prefix);
    auto& tail = tails[pi];
    tail.reserve(s.candidates.size() - exact_prefix);

    for (std::size_t ci = 0; ci < s.candidates.size(); ++ci) {
      const auto& c = s.candidates[ci];
      Record record{c.id, c.est};
      if (!has_bounds && has_sketch_bound && ci < s.sketch_eps_from) {
        record.bound = Record::Bound::exact;
      } else if (has_bounds || (has_sketch_bound && ci >= s.sketch_eps_from)) {
        record.eps = s.cand_eps(ci);
        record.bound = Record::Bound::epsilon;
      }

      const std::size_t slot = head_slot(c.id);
      if (ci < exact_prefix || slot < head_ids.size()) {
        // The second condition handles a defensive mixed head/tail report of
        // the same shared-head id without changing generic reducer semantics.
        add_value(head_agg[slot], record);
        if (head_agg[slot].resolution_worker == kNoResolutionWorker) {
          head_agg[slot].resolution_worker = static_cast<std::uint32_t>(pi);
        }
        head_agg[slot].globally_exact = true;
        reported_head_slots.push_back(slot);
      } else {
        tail.push_back(record);
        ++prefix_histogram[record.id.b[0]];
      }
    }

    std::sort(reported_head_slots.begin(), reported_head_slots.end());
    reported_head_slots.erase(
        std::unique(reported_head_slots.begin(), reported_head_slots.end()),
        reported_head_slots.end());
    for (const std::size_t slot : reported_head_slots) {
      auto& a = head_agg[slot];
      ++a.reporters;
      a.mass_reporters += part_mass[pi];
      a.min_counter_reporters += min_counter_by_part[pi];
      if (s.q_local > 0 && !a.globally_exact) a.has_ss_reporter = true;
    }
  }

  parallelism = std::max<std::size_t>(1, parallelism);
  const std::size_t sort_workers = std::min(parallelism, tails.size());
  auto sort_worker = [&](std::size_t worker) {
    for (std::size_t pi = worker; pi < tails.size(); pi += sort_workers) {
      auto& tail = tails[pi];
      std::sort(tail.begin(), tail.end(), [&](const Record& a, const Record& b) {
        return id_less(a.id, b.id);
      });
    }
  };
  if (sort_workers > 1) {
    reducer_thread_pool().parallel_for(
        sort_workers, sort_workers, sort_worker);
  } else if (sort_workers == 1) {
    sort_worker(0);
  }

  std::vector<GlobalItemLB> items;
  items.reserve(head_ids.size());
  auto make_item = [&](const Id128& id, const Agg& a) -> std::optional<GlobalItemLB> {
    GlobalItemLB gi;
    gi.id = id;
    gi.est = a.est;
    gi.lb = a.lb;
    gi.reporters = a.reporters;
    gi.resolution_worker = a.resolution_worker;
    gi.omega = R.N_global > 0
        ? a.mass_reporters / static_cast<double>(R.N_global)
        : 0.0;

    if (a.globally_exact) {
      gi.lb = gi.est;
      gi.cert_lb = gi.est;
      gi.cert_ub = gi.est;
      gi.guaranteed = gi.est >= R.threshold;
    } else if (a.has_ss_reporter) {
      const double hidden_non_reporter_mass =
          std::max(0.0, total_min_counter_mass - a.min_counter_reporters);
      const double cert_ub_d = static_cast<double>(gi.est) + hidden_non_reporter_mass;
      if (prune_certified_non_hh && cert_ub_d < static_cast<double>(R.threshold)) {
        return std::nullopt;
      }
      const double cert_lb_d = static_cast<double>(gi.est) - static_cast<double>(a.eps_sum);
      gi.cert_lb = cert_lb_d > 0.0 ? static_cast<std::uint64_t>(cert_lb_d) : 0ull;
      gi.cert_ub = static_cast<std::uint64_t>(std::ceil(std::max(0.0, cert_ub_d)));
      gi.cert_inflation = a.eps_sum;
      gi.cert_hidden_mass = static_cast<std::uint64_t>(hidden_non_reporter_mass);
      gi.has_cert_components = true;
      gi.guaranteed = gi.cert_lb >= R.threshold;
    } else {
      gi.cert_lb = gi.lb;
      gi.cert_ub = gi.est;
      gi.guaranteed = gi.lb >= R.threshold;
    }

    return gi;
  };

  for (std::size_t slot = 0; slot < head_ids.size(); ++slot) {
    if (auto item = make_item(head_ids[slot], head_agg[slot])) {
      items.push_back(std::move(*item));
    }
  }

  struct Cursor { std::size_t part; std::size_t index; };
  struct CursorGreater {
    const std::vector<std::vector<Record>>* streams;
    bool operator()(const Cursor& a, const Cursor& b) const {
      return (*streams)[b.part][b.index].id.b < (*streams)[a.part][a.index].id.b;
    }
  };
  std::size_t total_tail_records = 0;
  for (const auto& tail : tails) total_tail_records += tail.size();
  const std::size_t shard_count = std::min({
      std::size_t{256}, parallelism, std::max<std::size_t>(1, total_tail_records)});
  const auto shard_boundaries =
      balanced_prefix_boundaries(prefix_histogram, shard_count);
  auto range_position = [&](const std::vector<Record>& stream, unsigned boundary) {
    if (boundary == 0) return std::size_t{0};
    if (boundary >= 256) return stream.size();
    Id128 key{};
    key.b[0] = static_cast<std::uint8_t>(boundary);
    const auto it = std::lower_bound(
        stream.begin(), stream.end(), key,
        [&](const Record& record, const Id128& value) {
          return id_less(record.id, value);
        });
    return static_cast<std::size_t>(it - stream.begin());
  };
  auto merge_shard = [&](std::size_t shard) {
    std::vector<GlobalItemLB> shard_items;
    const unsigned lower = shard_boundaries[shard];
    const unsigned upper = shard_boundaries[shard + 1];
    std::vector<std::size_t> ends(tails.size(), 0);
    std::priority_queue<Cursor, std::vector<Cursor>, CursorGreater>
        queue(CursorGreater{&tails});
    for (std::size_t pi = 0; pi < tails.size(); ++pi) {
      const std::size_t begin = range_position(tails[pi], lower);
      ends[pi] = range_position(tails[pi], upper);
      if (begin < ends[pi]) queue.push(Cursor{pi, begin});
    }

    while (!queue.empty()) {
      const Id128 id = tails[queue.top().part][queue.top().index].id;
      Agg a;
      while (!queue.empty()) {
        const Cursor cursor = queue.top();
        const auto& stream = tails[cursor.part];
        if (!(stream[cursor.index].id == id)) break;
        queue.pop();

        std::size_t next = cursor.index;
        while (next < ends[cursor.part] && stream[next].id == id) {
          add_value(a, stream[next]);
          ++next;
        }
        ++a.reporters;
        if (a.resolution_worker == kNoResolutionWorker) {
          a.resolution_worker = static_cast<std::uint32_t>(cursor.part);
        }
        a.mass_reporters += part_mass[cursor.part];
        a.min_counter_reporters += min_counter_by_part[cursor.part];
        if (snaps_ex[cursor.part].q_local > 0) a.has_ss_reporter = true;
        if (next < ends[cursor.part]) queue.push(Cursor{cursor.part, next});
      }
      if (auto item = make_item(id, a)) shard_items.push_back(std::move(*item));
    }
    return shard_items;
  };

  if (shard_count > 1) {
    std::vector<std::vector<GlobalItemLB>> shard_results(shard_count);
    reducer_thread_pool().parallel_for(
        shard_count, shard_count,
        [&](std::size_t shard) {
          shard_results[shard] = merge_shard(shard);
        });
    for (auto& shard_items : shard_results) {
      items.insert(items.end(),
                   std::make_move_iterator(shard_items.begin()),
                   std::make_move_iterator(shard_items.end()));
    }
  } else {
    auto shard_items = merge_shard(0);
    items.insert(items.end(),
                 std::make_move_iterator(shard_items.begin()),
                 std::make_move_iterator(shard_items.end()));
  }

  if (telemetry) {
    const std::size_t dense_head_bytes =
        sizeof(head_ids) + head_ids.capacity() * sizeof(Id128)
        + sizeof(head_agg) + head_agg.capacity() * sizeof(Agg);
    const std::size_t merge_bytes =
        shard_count * (sizeof(std::priority_queue<Cursor, std::vector<Cursor>, CursorGreater>)
                       + snaps_ex.size() * (sizeof(Cursor) + sizeof(std::size_t)))
        + sizeof(part_mass) + part_mass.capacity() * sizeof(double)
        + sizeof(min_counter_by_part) + min_counter_by_part.capacity() * sizeof(double);
    const std::size_t items_bytes =
        sizeof(items) + items.capacity() * kLogicalGlobalItemBytes;
    telemetry->agg_bytes = dense_head_bytes + sizeof(Agg);
    constexpr std::size_t kHeaderBytes = 32;
    constexpr std::size_t kRecordBytes = kLogicalCertificateRecordBytes;
    telemetry->ingress_bytes =
        snaps_ex.size() * kHeaderBytes
        + shard_count * snaps_ex.size() * kRecordBytes;
    telemetry->presence_bytes = merge_bytes;
    telemetry->items_bytes = items_bytes;
    telemetry->total_peak_bytes =
        telemetry->agg_bytes + telemetry->presence_bytes
        + telemetry->items_bytes * (shard_count > 1 ? 2 : 1);
  }

  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    if (a.est != b.est) return a.est > b.est;
    return a.id.b < b.id.b;
  });
  R.items = std::move(items);
  return R;
}

HybridControlReduction Coordinator::reduce_hybrid_streaming_for_control(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    std::size_t top_limit,
    const std::vector<Id128>& current_head_ids,
    std::uint64_t current_head_generation,
    std::size_t parallelism)
{
  HybridControlReduction out{};
  if (n_param == 0) return out;

  const auto id_less = [](const Id128& a, const Id128& b) {
    return a.b < b.b;
  };
  const auto result_less = [](const GlobalItemLB& a, const GlobalItemLB& b) {
    if (a.est != b.est) return a.est > b.est;
    return a.id.b < b.id.b;
  };
  for (const auto& snapshot : snaps_ex) {
    out.published.N_global += snapshot.N_local;
  }
  out.published.threshold = (out.published.N_global / n_param) + 1;

  struct Record {
    Id128 id;
    std::uint32_t est{0};
    std::uint32_t eps{0};
    enum class Bound : std::uint8_t { unknown, exact, epsilon } bound{Bound::unknown};
  };
  struct Agg {
    std::uint64_t est{0};
    std::uint64_t lb{0};
    std::uint64_t reporters{0};
    double mass_reporters{0.0};
    double min_counter_reporters{0.0};
    double residual_min_counter_reporters{0.0};
    std::uint64_t eps_sum{0};
    std::uint32_t resolution_worker{kNoResolutionWorker};
    bool has_ss_reporter{false};
  };
  // Exact-head reports use a shared dictionary. They need the same published
  // envelope as before, but do not need per-slot tail-only fields such as an
  // error sum or residual minimum-counter coverage.
  struct HeadAgg {
    std::uint64_t est{0};
    std::uint64_t reporters{0};
    double mass_reporters{0.0};
    std::uint32_t resolution_worker{kNoResolutionWorker};
  };

  std::vector<double> part_mass(snaps_ex.size(), 0.0);
  std::vector<double> full_min_counter(snaps_ex.size(), 0.0);
  std::vector<double> residual_min_counter(snaps_ex.size(), 0.0);
  double full_min_total = 0.0;
  double residual_min_total = 0.0;
  std::vector<HeadAgg> head_agg(current_head_ids.size());
  std::vector<std::vector<Record>> tails(snaps_ex.size());
  std::array<std::size_t, 256> prefix_histogram{};

  for (std::size_t pi = 0; pi < snaps_ex.size(); ++pi) {
    const auto& snapshot = snaps_ex[pi];
    if (snapshot.head_generation != current_head_generation) {
      throw std::runtime_error(
          "hybrid report generation does not match installed dictionary");
    }
    part_mass[pi] = static_cast<double>(snapshot.N_local);

    const std::size_t residual_size = snapshot.candidates.size();
    if (snapshot.q_local > 0 && residual_size >= snapshot.q_local) {
      std::uint32_t minimum = UINT32_MAX;
      for (const auto& candidate : snapshot.candidates) {
        minimum = std::min(minimum, candidate.est);
      }
      if (minimum != UINT32_MAX) residual_min_counter[pi] = minimum;
    }
    // Exact-head counters do not contribute approximation error. The full
    // published envelope and residual controller envelope therefore share the
    // approximate-tail minimum-counter term.
    full_min_counter[pi] = residual_min_counter[pi];
    full_min_total += full_min_counter[pi];
    residual_min_total += residual_min_counter[pi];

    const bool has_bounds = snapshot.has_error_bounds();
    const bool has_sketch_bound = snapshot.has_sketch_error_bound();
    auto& tail = tails[pi];
    tail.reserve(snapshot.candidates.size());
    std::vector<std::size_t> reported_head_slots;
    reported_head_slots.reserve(snapshot.head_records.size());
    for (const auto& head_record : snapshot.head_records) {
      const std::size_t slot = head_record.slot;
      if (slot >= current_head_ids.size()) {
        throw std::runtime_error(
            "hybrid exact-head report contains an invalid dictionary slot");
      }
      head_agg[slot].est += head_record.count;
      if (head_agg[slot].resolution_worker == kNoResolutionWorker) {
        head_agg[slot].resolution_worker = static_cast<std::uint32_t>(pi);
      }
      reported_head_slots.push_back(slot);
    }
    for (std::size_t ci = 0; ci < snapshot.candidates.size(); ++ci) {
      const auto& candidate = snapshot.candidates[ci];
      Record record{candidate.id, candidate.est};
      if (!has_bounds && has_sketch_bound && ci < snapshot.sketch_eps_from) {
        record.bound = Record::Bound::exact;
      } else if (has_bounds || (has_sketch_bound && ci >= snapshot.sketch_eps_from)) {
        record.eps = snapshot.cand_eps(ci);
        record.bound = Record::Bound::epsilon;
      }
      // Installed head keys are routed exclusively through head_records, so no
      // dictionary membership test is required for residual records.
      tail.push_back(record);
      ++prefix_histogram[record.id.b[0]];
    }
    std::sort(reported_head_slots.begin(), reported_head_slots.end());
    reported_head_slots.erase(
        std::unique(reported_head_slots.begin(), reported_head_slots.end()),
        reported_head_slots.end());
    for (const std::size_t slot : reported_head_slots) {
        auto& aggregate = head_agg[slot];
        ++aggregate.reporters;
        aggregate.mass_reporters += part_mass[pi];
    }
  }

  parallelism = std::max<std::size_t>(1, parallelism);
  const bool tails_presorted = std::all_of(
      snaps_ex.begin(), snaps_ex.end(),
      [](const SnapshotEx& snapshot) {
        return snapshot.tail_sorted_by_id;
      });
  const std::size_t sort_workers =
      tails_presorted ? 0 : std::min(parallelism, tails.size());
  auto sort_worker = [&](std::size_t worker) {
    for (std::size_t pi = worker; pi < tails.size(); pi += sort_workers) {
      auto& tail = tails[pi];
      std::sort(tail.begin(), tail.end(), [&](const Record& a, const Record& b) {
        return id_less(a.id, b.id);
      });
    }
  };
  if (sort_workers > 1) {
    reducer_thread_pool().parallel_for(
        sort_workers, sort_workers, sort_worker);
  } else if (sort_workers == 1) {
    sort_worker(0);
  }

  auto add_record = [](Agg& aggregate, const Record& record) {
    aggregate.est += record.est;
    if (record.bound == Record::Bound::exact) {
      aggregate.lb += record.est;
    } else if (record.bound == Record::Bound::epsilon) {
      aggregate.lb += record.est >= record.eps ? (record.est - record.eps) : 0u;
    }
    aggregate.eps_sum += record.eps;
  };
  struct Envelope {
    std::uint64_t cert_lb{0};
    std::uint64_t cert_ub{0};
    std::uint64_t inflation{0};
    std::uint64_t hidden_mass{0};
    bool has_components{false};
  };
  auto make_envelope = [&](const Agg& aggregate,
                           double minimum_total,
                           double minimum_reporters) {
    Envelope envelope;
    if (aggregate.has_ss_reporter) {
      const double hidden = std::max(0.0, minimum_total - minimum_reporters);
      const double lower =
          static_cast<double>(aggregate.est) - aggregate.eps_sum;
      envelope.cert_lb = lower > 0.0
          ? static_cast<std::uint64_t>(lower)
          : 0;
      envelope.cert_ub = static_cast<std::uint64_t>(
          std::ceil(std::max(0.0, static_cast<double>(aggregate.est) + hidden)));
      envelope.inflation = aggregate.eps_sum;
      envelope.hidden_mass = static_cast<std::uint64_t>(hidden);
      envelope.has_components = true;
    } else {
      envelope.cert_lb = aggregate.lb;
      envelope.cert_ub = aggregate.est;
    }
    return envelope;
  };
  auto make_item = [&](const Id128& id,
                       const Agg& aggregate,
                       const Envelope& envelope) {
    GlobalItemLB item;
    item.id = id;
    item.est = aggregate.est;
    item.lb = aggregate.lb;
    item.reporters = aggregate.reporters;
    item.resolution_worker = aggregate.resolution_worker;
    item.omega = out.published.N_global > 0
        ? aggregate.mass_reporters /
              static_cast<double>(out.published.N_global)
        : 0.0;
    item.cert_lb = envelope.cert_lb;
    item.cert_ub = envelope.cert_ub;
    item.cert_inflation = envelope.inflation;
    item.cert_hidden_mass = envelope.hidden_mass;
    item.has_cert_components = envelope.has_components;
    item.guaranteed = item.cert_lb >= out.published.threshold;
    return item;
  };

  using RankedId = HybridRankedId;
  const auto ranked_better = [](const RankedId& a, const RankedId& b) {
    if (a.est != b.est) return a.est > b.est;
    return a.id.b < b.id.b;
  };
  struct BetterPriority {
    decltype(ranked_better)* better;
    bool operator()(const RankedId& a, const RankedId& b) const {
      return (*better)(a, b);
    }
  };
  using TopHeap = std::priority_queue<
      RankedId, std::vector<RankedId>, BetterPriority>;
  struct Cursor { std::size_t part; std::size_t index; };
  struct CursorGreater {
    const std::vector<std::vector<Record>>* streams;
    bool operator()(const Cursor& a, const Cursor& b) const {
      return (*streams)[b.part][b.index].id.b <
             (*streams)[a.part][a.index].id.b;
    }
  };
  std::size_t total_records = 0;
  for (const auto& tail : tails) total_records += tail.size();
  const std::size_t shard_count = std::min({
      std::size_t{256}, parallelism, std::max<std::size_t>(1, total_records)});
  const auto shard_boundaries =
      balanced_prefix_boundaries(prefix_histogram, shard_count);
  auto range_position = [&](const std::vector<Record>& stream, unsigned boundary) {
    if (boundary == 0) return std::size_t{0};
    if (boundary >= 256) return stream.size();
    Id128 key{};
    key.b[0] = static_cast<std::uint8_t>(boundary);
    const auto it = std::lower_bound(
        stream.begin(), stream.end(), key,
        [&](const Record& record, const Id128& value) {
          return id_less(record.id, value);
        });
    return static_cast<std::size_t>(it - stream.begin());
  };

  struct ShardResult {
    std::vector<GlobalItemLB> published;
    std::vector<HybridSizingItem> residual_items;
    std::vector<RankedId> ranked;
  };
  auto process_aggregate = [&](const Id128& id,
                               const Agg& aggregate,
                               bool include_residual,
                               auto& top_heap,
                               std::vector<GlobalItemLB>& published,
                               std::vector<HybridSizingItem>& residual_items) {
    const Envelope full_envelope = make_envelope(
        aggregate, full_min_total, aggregate.min_counter_reporters);
    const Envelope residual_envelope = include_residual
        ? make_envelope(
              aggregate, residual_min_total,
              aggregate.residual_min_counter_reporters)
        : Envelope{};
    const bool keep_published =
        full_envelope.cert_ub >= out.published.threshold;
    const bool keep_residual = include_residual
        && residual_envelope.cert_ub >= out.published.threshold;

    if (top_limit > 0) {
      bool enters_top = top_heap.size() < top_limit;
      if (!enters_top) {
        const auto& worst = top_heap.top();
        if (aggregate.est > worst.est) {
          enters_top = true;
        } else if (aggregate.est == worst.est) {
          const RankedId candidate{
              id, aggregate.est, aggregate.resolution_worker};
          enters_top = ranked_better(candidate, worst);
        }
      }
      if (enters_top) {
        RankedId candidate{id, aggregate.est, aggregate.resolution_worker};
        if (top_heap.size() == top_limit) top_heap.pop();
        top_heap.push(std::move(candidate));
      }
    }

    if (keep_published || keep_residual) {
      if (keep_published) {
        published.push_back(make_item(id, aggregate, full_envelope));
      }
      if (keep_residual) {
        residual_items.push_back(HybridSizingItem{
            id,
            aggregate.est,
            residual_envelope.cert_lb,
            residual_envelope.cert_ub,
            residual_envelope.inflation,
            residual_envelope.hidden_mass,
            residual_envelope.has_components,
        });
      }
    }
  };
  auto merge_shard_into = [&](std::size_t shard,
                              auto& top_heap,
                              std::vector<GlobalItemLB>& published,
                              std::vector<HybridSizingItem>& residual_items,
                              std::vector<std::size_t>& ends) {
    const unsigned lower = shard_boundaries[shard];
    const unsigned upper = shard_boundaries[shard + 1];
    ends.assign(tails.size(), 0);
    std::priority_queue<Cursor, std::vector<Cursor>, CursorGreater>
        queue(CursorGreater{&tails});
    for (std::size_t pi = 0; pi < tails.size(); ++pi) {
      const std::size_t begin = range_position(tails[pi], lower);
      ends[pi] = range_position(tails[pi], upper);
      if (begin < ends[pi]) queue.push(Cursor{pi, begin});
    }

    while (!queue.empty()) {
      const Id128 id = tails[queue.top().part][queue.top().index].id;
      Agg aggregate;
      while (!queue.empty()) {
        const Cursor cursor = queue.top();
        const auto& stream = tails[cursor.part];
        if (!(stream[cursor.index].id == id)) break;
        queue.pop();

        std::size_t next = cursor.index;
        while (next < ends[cursor.part] && stream[next].id == id) {
          add_record(aggregate, stream[next]);
          ++next;
        }
        ++aggregate.reporters;
        if (aggregate.resolution_worker == kNoResolutionWorker) {
          aggregate.resolution_worker = static_cast<std::uint32_t>(cursor.part);
        }
        aggregate.mass_reporters += part_mass[cursor.part];
        aggregate.min_counter_reporters += full_min_counter[cursor.part];
        aggregate.residual_min_counter_reporters +=
            residual_min_counter[cursor.part];
        if (snaps_ex[cursor.part].q_local > 0) {
          aggregate.has_ss_reporter = true;
        }
        if (next < ends[cursor.part]) queue.push(Cursor{cursor.part, next});
      }
      process_aggregate(
          id, aggregate, true, top_heap, published, residual_items);
    }
  };
  auto merge_shard = [&](std::size_t shard, HybridShardWorkspace& result) {
    result.clear();
    TopHeap top_heap(BetterPriority{&ranked_better});
    merge_shard_into(
        shard, top_heap, result.published, result.residual_items, result.ends);
    result.ranked.reserve(top_heap.size());
    while (!top_heap.empty()) {
      result.ranked.push_back(top_heap.top());
      top_heap.pop();
    }
  };
  auto merge_head_into = [&](auto& top_heap,
                             std::vector<GlobalItemLB>& published,
                             std::vector<HybridSizingItem>& residual_items) {
    for (std::size_t slot = 0; slot < current_head_ids.size(); ++slot) {
      if (head_agg[slot].reporters == 0) continue;
      Agg aggregate;
      aggregate.est = head_agg[slot].est;
      aggregate.lb = head_agg[slot].est;
      aggregate.reporters = head_agg[slot].reporters;
      aggregate.mass_reporters = head_agg[slot].mass_reporters;
      aggregate.resolution_worker = head_agg[slot].resolution_worker;
      process_aggregate(current_head_ids[slot], aggregate, false, top_heap,
                        published, residual_items);
    }
  };

  std::size_t reusable_workspace_bytes = 0;
  std::unique_lock<std::mutex> workspace_lock;
  if (shard_count == 1) {
    // A serial reduction has a single global order.  Feed both the exact head
    // and tail into one bounded rank heap, avoiding duplicate shard and final
    // top-n materializations.
    TopHeap top_heap(BetterPriority{&ranked_better});
    merge_head_into(top_heap, out.published.items, out.residual_items);
    std::vector<std::size_t> ends;
    merge_shard_into(
        0, top_heap, out.published.items, out.residual_items, ends);
    std::vector<RankedId> ranked_ids;
    ranked_ids.reserve(top_heap.size());
    while (!top_heap.empty()) {
      ranked_ids.push_back(top_heap.top());
      top_heap.pop();
    }
    std::sort(ranked_ids.begin(), ranked_ids.end(), ranked_better);
    out.top_ids.reserve(ranked_ids.size());
    out.top_resolution_workers.reserve(ranked_ids.size());
    for (const auto& ranked : ranked_ids) {
      out.top_ids.push_back(ranked.id);
      out.top_resolution_workers.push_back(ranked.resolution_worker);
    }
  } else {
    workspace_lock = std::unique_lock<std::mutex>(hybrid_workspace_mutex);
    if (hybrid_workspaces.size() < shard_count) {
      hybrid_workspaces.resize(shard_count);
    }
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
      hybrid_workspaces[shard].clear();
    }

    ShardResult head_result;
    TopHeap head_top_heap(BetterPriority{&ranked_better});
    merge_head_into(
        head_top_heap, head_result.published, head_result.residual_items);
    head_result.ranked.reserve(head_top_heap.size());
    while (!head_top_heap.empty()) {
      head_result.ranked.push_back(head_top_heap.top());
      head_top_heap.pop();
    }

    reducer_thread_pool().parallel_for(
        shard_count, shard_count,
        [&](std::size_t shard) {
          merge_shard(shard, hybrid_workspaces[shard]);
        });

    TopHeap top_heap(BetterPriority{&ranked_better});
    auto consume_result = [&](auto& result) {
      out.published.items.insert(
          out.published.items.end(),
          std::make_move_iterator(result.published.begin()),
          std::make_move_iterator(result.published.end()));
      out.residual_items.insert(
          out.residual_items.end(),
          std::make_move_iterator(result.residual_items.begin()),
          std::make_move_iterator(result.residual_items.end()));
      for (auto& ranked : result.ranked) {
        if (top_heap.size() < top_limit) {
          top_heap.push(std::move(ranked));
        } else if (ranked_better(ranked, top_heap.top())) {
          top_heap.pop();
          top_heap.push(std::move(ranked));
        }
      }
    };
    consume_result(head_result);
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
      auto& result = hybrid_workspaces[shard];
      consume_result(result);
      reusable_workspace_bytes +=
          result.published.capacity() * kLogicalGlobalItemBytes
          + result.residual_items.capacity() * sizeof(HybridSizingItem)
          + result.ranked.capacity()
              * (sizeof(Id128) + sizeof(std::uint64_t) + sizeof(std::uint32_t))
          + result.ends.capacity() * sizeof(std::size_t);
      result.clear();
    }

    std::vector<RankedId> ranked_ids;
    ranked_ids.reserve(top_heap.size());
    while (!top_heap.empty()) {
      ranked_ids.push_back(top_heap.top());
      top_heap.pop();
    }
    std::sort(ranked_ids.begin(), ranked_ids.end(), ranked_better);
    out.top_ids.reserve(ranked_ids.size());
    out.top_resolution_workers.reserve(ranked_ids.size());
    for (const auto& ranked : ranked_ids) {
      out.top_ids.push_back(ranked.id);
      out.top_resolution_workers.push_back(ranked.resolution_worker);
    }
  }
  std::sort(out.published.items.begin(), out.published.items.end(), result_less);
  std::sort(out.residual_items.begin(), out.residual_items.end(),
            [&](const HybridSizingItem& a, const HybridSizingItem& b) {
              if (a.est != b.est) return a.est > b.est;
              return a.id.b < b.id.b;
            });

  constexpr std::size_t kHeaderBytes = 32;
  const std::size_t dense_head_bytes =
      sizeof(head_agg) + head_agg.capacity() * sizeof(HeadAgg);
  const std::size_t ranking_bytes = shard_count == 1
      ? sizeof(TopHeap) + top_limit *
          (sizeof(Id128) + sizeof(std::uint64_t) + sizeof(std::uint32_t))
      : (shard_count + 2) * top_limit *
          (sizeof(Id128) + sizeof(std::uint64_t) + sizeof(std::uint32_t));
  const std::size_t merge_bytes =
      shard_count *
          (sizeof(std::priority_queue<Cursor, std::vector<Cursor>, CursorGreater>)
           + tails.size() * (sizeof(Cursor) + sizeof(std::size_t)))
      + sizeof(part_mass) + part_mass.capacity() * sizeof(double)
      + sizeof(full_min_counter) + full_min_counter.capacity() * sizeof(double)
      + sizeof(residual_min_counter) + residual_min_counter.capacity() * sizeof(double)
      + ranking_bytes;
  const std::size_t retained_bytes =
      sizeof(out.published.items)
      + out.published.items.capacity() * kLogicalGlobalItemBytes
      + sizeof(out.residual_items)
      + out.residual_items.capacity() * sizeof(HybridSizingItem)
      + sizeof(out.top_ids) + out.top_ids.capacity() * sizeof(Id128)
      + sizeof(out.top_resolution_workers)
      + out.top_resolution_workers.capacity() * sizeof(std::uint32_t)
      + sizeof(current_head_ids)
      + current_head_ids.capacity() * sizeof(Id128);
  out.telemetry.ingress_bytes =
      snaps_ex.size() * kHeaderBytes;
  for (const auto& snapshot : snaps_ex) {
    out.telemetry.ingress_bytes +=
        snapshot.head_records.size() * sizeof(HeadRecord)
        + snapshot.candidates.size() * kLogicalCertificateRecordBytes;
  }
  out.telemetry.agg_bytes = dense_head_bytes + sizeof(Agg);
  out.telemetry.presence_bytes = merge_bytes;
  out.telemetry.items_bytes = retained_bytes;
  out.telemetry.total_peak_bytes =
      out.telemetry.agg_bytes + merge_bytes
      + retained_bytes
      + (shard_count > 1
             ? std::max(retained_bytes, reusable_workspace_bytes)
             : 0);
  return out;
}

HybridControlReduction Coordinator::reduce_ss_streaming_for_control(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    std::size_t parallelism)
{
  static const std::vector<Id128> no_head_ids;
  return reduce_hybrid_streaming_for_control(
      snaps_ex, n_param, /*top_limit=*/0, no_head_ids,
      /*current_head_generation=*/0, parallelism);
}

HybridControlReduction Coordinator::reduce_hybrid_parallel_streaming_for_control(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    std::size_t top_limit,
    const std::vector<Id128>& current_head_ids,
    std::uint64_t current_head_generation)
{
  const std::size_t parallelism = std::max<std::size_t>(
      1, std::min<std::size_t>(
             8, static_cast<std::size_t>(std::thread::hardware_concurrency())));
  return reduce_hybrid_streaming_for_control(
      snaps_ex, n_param, top_limit, current_head_ids,
      current_head_generation, parallelism);
}

GlobalResultLB Coordinator::reduce_global_parallel_streaming_with_lb(
    const std::vector<SnapshotEx>& snaps_ex,
    std::size_t n_param,
    ReduceTelemetry* telemetry,
    bool prune_certified_non_hh)
{
  const std::size_t parallelism =
      std::max<std::size_t>(
          1, std::min<std::size_t>(
                 8, static_cast<std::size_t>(std::thread::hardware_concurrency())));
  return reduce_global_streaming_with_lb(
      snaps_ex, n_param, telemetry, prune_certified_non_hh, parallelism);
}

GlobalResultLB Coordinator::reduce_hl_bucketwise(
    const std::vector<HLBucketSnapshot>& snaps_hl,
    std::size_t n_param,
    ReduceTelemetry* telemetry)
{
  GlobalResultLB R{};
  if (n_param == 0) return R;
  if (telemetry) *telemetry = ReduceTelemetry{};
  if (snaps_hl.empty()) return R;
  if (telemetry) {
    constexpr std::size_t kHeaderBytes = 32;
    constexpr std::size_t kCellBytes = sizeof(Id128) + sizeof(std::uint32_t);
    const bool carries_residuals = std::all_of(
        snaps_hl.begin(), snaps_hl.end(),
        [](const HLBucketSnapshot& s) {
          return s.has_residual_certificate();
        });
    telemetry->ingress_bytes =
        snaps_hl.size()
        * (kHeaderBytes + kCellBytes
           + (carries_residuals
                  ? sizeof(std::uint32_t) + sizeof(std::uint64_t)
                  : 0));
  }

  for (const auto& s : snaps_hl) R.N_global += s.N_local;
  R.threshold = (R.N_global / n_param) + 1;

  std::size_t w = 0;
  std::size_t d = 0;
  for (const auto& s : snaps_hl) {
    if (s.w == 0 || s.d == 0) continue;
    if (w == 0) w = s.w;
    if (d == 0) d = s.d;
    w = std::min(w, s.w);
    d = std::min(d, s.d);
  }
  if (w == 0 || d == 0) return R;
  const bool has_residual_certificate = std::all_of(
      snaps_hl.begin(), snaps_hl.end(), [&](const HLBucketSnapshot& s) {
        return s.w == w && s.has_residual_certificate();
      });
  R.has_completeness_certificate = has_residual_certificate;
  std::uint64_t unseen_mass_ub = 0;

  // HeavyLocker reports candidates in bucket order. Follow the paper's merge
  // literally: aggregate one corresponding bucket across workers, retain its
  // top-d entries, then release the temporary Stream Summary before advancing.
  std::vector<std::size_t> cursors(snaps_hl.size(), 0);
  std::vector<GlobalItemLB> items;
  items.reserve(w * d);

  const std::size_t cursor_bytes =
      sizeof(cursors) + cursors.capacity() * sizeof(decltype(cursors)::value_type);
  const std::size_t kMergedCellBytes =
      sizeof(Id128) + sizeof(std::uint32_t)
      + (has_residual_certificate ? sizeof(std::uint32_t) : 0);
  const std::size_t merged_table_bytes =
      w * d * kMergedCellBytes + (w + 7) / 8;
  std::size_t bucket_map_peak_bytes = 0;
  std::size_t scratch_peak_bytes = 0;
  std::size_t total_peak_bytes = cursor_bytes + merged_table_bytes;

  for (std::size_t bi = 0; bi < w; ++bi) {
    struct HLAggregate {
      std::uint64_t est{0};
      std::uint64_t lb{0};
      std::uint32_t resolution_worker{kNoResolutionWorker};
    };
    std::unordered_map<Id128, HLAggregate, Id128Hash> mp;
    mp.reserve(snaps_hl.size() * d);
    std::uint64_t bucket_residual = 0;
    for (std::size_t si = 0; si < snaps_hl.size(); ++si) {
      if (has_residual_certificate) {
        bucket_residual += snaps_hl[si].residual_by_bucket[bi];
      }
      const auto& candidates = snaps_hl[si].candidates;
      auto& cursor = cursors[si];
      while (cursor < candidates.size() && candidates[cursor].bucket < bi) ++cursor;
      while (cursor < candidates.size() && candidates[cursor].bucket == bi) {
        auto& aggregate = mp[candidates[cursor].id];
        if (aggregate.resolution_worker == kNoResolutionWorker) {
          aggregate.resolution_worker = static_cast<std::uint32_t>(si);
        }
        aggregate.est += candidates[cursor].est;
        aggregate.lb += candidates[cursor].lb;
        ++cursor;
      }
    }
    if (has_residual_certificate) {
      // A key absent from every current slot in this bucket can account for at
      // most all mass not assigned to current candidate lower bounds.
      unseen_mass_ub = std::max(unseen_mass_ub, bucket_residual);
    }
    if (mp.empty()) continue;

    std::vector<std::pair<Id128, HLAggregate>> vec;
    vec.reserve(mp.size());
    for (const auto& kv : mp) vec.push_back(kv);
    scratch_peak_bytes = std::max(
        scratch_peak_bytes,
        sizeof(vec) + vec.capacity() * sizeof(decltype(vec)::value_type));

    std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b){
      if (a.second.est != b.second.est) return a.second.est > b.second.est;
      return a.first.b < b.first.b;
    });

    const std::size_t keep = std::min<std::size_t>(d, vec.size());
    for (std::size_t i = 0; i < vec.size(); ++i) {
      const std::uint64_t cert_ub = has_residual_certificate
          ? std::min<std::uint64_t>(
                R.N_global, vec[i].second.lb + bucket_residual)
          : vec[i].second.est;
      if (i >= keep) {
        if (has_residual_certificate) {
          // The paper-style top-d bucket merge omits this reported identity.
          // Include its candidate-specific bound in the global omitted-key
          // certificate.
          unseen_mass_ub = std::max(unseen_mass_ub, cert_ub);
        }
        continue;
      }
      GlobalItemLB gi;
      gi.id = vec[i].first;
      gi.est = vec[i].second.est;
      gi.lb = has_residual_certificate ? vec[i].second.lb : 0;
      gi.reporters = 0;
      gi.omega = 0.0;
      gi.cert_lb = gi.lb;
      gi.cert_ub = cert_ub;
      gi.resolution_worker = vec[i].second.resolution_worker;
      gi.guaranteed =
          has_residual_certificate && gi.cert_lb >= R.threshold;

      items.push_back(std::move(gi));
    }

    const std::size_t bucket_map_bytes =
        sizeof(mp) + mp.bucket_count() * sizeof(void*)
        + mp.size() * sizeof(decltype(mp)::value_type);
    const std::size_t scratch_bytes =
        sizeof(vec) + vec.capacity() * sizeof(decltype(vec)::value_type);
    bucket_map_peak_bytes = std::max(bucket_map_peak_bytes, bucket_map_bytes);
    scratch_peak_bytes = std::max(scratch_peak_bytes, scratch_bytes);
    total_peak_bytes = std::max(
        total_peak_bytes,
        cursor_bytes + bucket_map_bytes + scratch_bytes + merged_table_bytes);
  }
  if (has_residual_certificate) {
    R.unseen_mass_ub = unseen_mass_ub;
    R.candidate_set_complete = unseen_mass_ub < R.threshold;
  }

  if (telemetry) {
    // The reference implementation keeps the merged w-by-d table compact and
    // materializes only entries that pass Query(). GlobalItemLB is an evaluator
    // adapter and is deliberately excluded from HeavyLocker's modeled state.
    const std::size_t reported_hh_count = static_cast<std::size_t>(std::count_if(
        items.begin(), items.end(), [&](const GlobalItemLB& item) {
          return item.est >= R.threshold;
        }));
    const std::size_t query_output_bytes =
        sizeof(std::vector<HLBucketCand>) + reported_hh_count * kMergedCellBytes;
    telemetry->agg_bytes = bucket_map_peak_bytes;
    telemetry->presence_bytes = cursor_bytes + scratch_peak_bytes;
    telemetry->items_bytes = merged_table_bytes + query_output_bytes;
    telemetry->total_peak_bytes = std::max(
        total_peak_bytes, merged_table_bytes + query_output_bytes);
  }

  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){
    if (a.est != b.est) return a.est > b.est;
    return a.id.b < b.id.b;
  });

  R.items = std::move(items);
  return R;
}

} // namespace hh
