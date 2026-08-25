// apps/hh_bench.cpp
// Batch benchmarking harness: replays the same nested JSON stream for multiple methods,
// treats the oracle pass as ground truth, and reports per-window plus aggregate accuracy
// metrics (HH precision/recall, AAE/ARE, optional top-k overlap) used in the paper’s plots.
#include "hh/io/reader.hpp"
#include "hh/core/hash.hpp"
#include "hh/core/arena_map.hpp"
#include "hh/sketches/isketch.hpp"
#include "hh/sketches/ss.hpp"
#include "hh/sketches/heavylocker.hpp"
#include "hh/sketches/chk.hpp"
#include "hh/oracle/oracle_all.hpp"
#include "hh/hybrid/hybrid.hpp"
#include "hh/coord/coordinator.hpp"

#include "hh/bench/metrics.hpp"
#include "hh/sizing/sizing.hpp"   // centralized adaptive sizing

#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <sstream>

using namespace hh;

struct Part {
  std::unique_ptr<ISketch> sketch;
  ArenaMap amap;
  Id128 active_id{};
  const std::string* active_raw{nullptr};

  void begin_update(const Id128& id, const std::string& raw) {
    active_id = id;
    active_raw = &raw;
  }
  void end_update() { active_raw = nullptr; }
  void bind_active(const Id128& id) {
    if (active_raw && id == active_id) amap.bind_bytes(id, *active_raw);
  }
};

static void wire_key_lifecycle(ISketch& sketch, Part& part) {
  sketch.set_admission_callback([&part](const Id128& id) { part.bind_active(id); });
  sketch.set_retirement_callback([&part](const Id128& id) { part.amap.erase(id); });
}

struct MethodGroup {
  std::string name;
  std::string label; // unique for output (allows multiple of same method)
  std::vector<Part> parts; // size m
};

struct DifficultyVerification {
  bool valid{false};
  double tilde_qpred{NAN};
  std::size_t q_planned{0};
};

struct KeyResolutionStats {
  std::size_t request_bytes{0};
  std::size_t reply_bytes{0};
  std::size_t coordinator_peak_bytes{0};
  std::size_t resolved_keys{0};
};

struct CertificationMetrics {
  std::size_t candidate_count{0};
  double candidate_hh_recall{0.0};
  std::size_t cert_pos_count{0};
  double cert_pos_precision{0.0};
  double cert_pos_mass{0.0};
  std::size_t ambiguous_count{0};
  double ambiguous_mass{0.0};
  std::size_t cert_neg_count{0};
  double cert_neg_mass{0.0};
  double interval_width_avg{0.0};
  double interval_width_amb_avg{0.0};
  double interval_width_max{0.0};
};

struct CsvRow {
  std::size_t window{0};
  std::string method;
  std::string method_type;
  std::uint64_t N_global{0};
  std::uint64_t threshold{0};

  double hh_precision{0.0};
  double hh_recall{0.0};
  double hh_f1{0.0};
  double aae{0.0};
  double are{0.0};
  bool has_topk{false};
  double topk_overlap{0.0};

  std::size_t q_current{0};
  std::size_t q_next{0};
  std::size_t q_head_current{0};
  std::size_t q_tail_current{0};
  std::size_t q_head_next{0};
  std::size_t q_tail_next{0};
  std::size_t head_overlap_count{0};
  std::size_t head_added_count{0};
  std::size_t head_removed_count{0};
  std::size_t head_full_update_bytes{0};
  std::size_t head_delta_update_bytes{0};
  double head_delta_ratio{NAN};
  int head_delta_selected{-1};
  double head_update_apply_ms{NAN};
  double margin_alpha{NAN};
  int service_violation{-1};
  std::size_t q_up{0};
  std::size_t q_baseline{0};
  int probe_issued{-1};
  int probe_failed{-1};
  std::size_t q_req_unclipped{0};
  std::size_t q_req{0};
  std::size_t q_eff_replay{0};
  std::size_t q_eff_pred{0};
  double q_eff_pred_tilde{NAN};
  int miss_req{-1};
  int miss_eff{-1};
  double over_req{NAN};
  double over_eff{NAN};
  double calib_bias{NAN};

  CertificationMetrics cert;
  int completeness_certified{-1};
  std::uint64_t unseen_mass_ub{0};
  double unseen_ub_over_threshold{NAN};

  double mem_worker_algo_kib{0.0};
  double mem_worker_key_kib{0.0};
  double mem_worker_total_kib{0.0};
  // Worker-to-coordinator report traffic. Kept under the historical name for
  // CSV compatibility; this is not bidirectional communication.
  double report_volume_kib{0.0};
  // Total coordinator-to-worker traffic, including key-resolution requests.
  double control_volume_kib{0.0};
  // Exact-head dictionary frames and recovery snapshots only.
  double head_update_volume_kib{0.0};
  double total_communication_kib{0.0};
  double key_request_volume_kib{0.0};
  double key_reply_volume_kib{0.0};
  std::size_t key_resolution_count{0};
  double mem_coord_ingress_kib{0.0};
  double mem_coord_work_kib{0.0};
  double mem_coord_control_peak_kib{0.0};
  double mem_coord_resolution_peak_kib{0.0};
  double mem_coord_peak_kib{0.0};

  std::uint64_t update_events{0};
  double update_ms{0.0};
  double update_mops{0.0};
  double report_prepare_ms{0.0};
  double reduce_ms{0.0};
  double control_ms{0.0};
  double aggregation_ms{0.0};
};

static std::size_t logical_result_bytes(const GlobalResultLB& result) {
  constexpr std::size_t kLogicalGlobalItemBytes =
      sizeof(Id128) + 7 * sizeof(std::uint64_t) + sizeof(double)
      + sizeof(std::uint32_t)
      + 2 * sizeof(std::uint8_t);
  return sizeof(result) + result.items.capacity() * kLogicalGlobalItemBytes;
}

class KeyResolutionModel {
public:
  explicit KeyResolutionModel(std::size_t workers)
      : reply_key_lengths_by_worker_(workers) {}

  void seed_known(const std::vector<Id128>& ids,
                  const std::vector<std::string>& keys) {
    const std::size_t count = std::min(ids.size(), keys.size());
    known_.reserve(known_.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
      known_.emplace(ids[i], keys[i]);
    }
  }

  std::string resolve(const MethodGroup& group, const Id128& id,
                      std::uint32_t reporting_worker) {
    if (const auto it = known_.find(id); it != known_.end()) {
      return it->second;
    }
    if (const auto it = fetched_.find(id); it != fetched_.end()) {
      return it->second;
    }

    if (reporting_worker == kNoResolutionWorker
        || reporting_worker >= group.parts.size()) {
      throw std::runtime_error(
          "published identifier has no valid reporting-worker route");
    }
    std::string raw;
    if (!group.parts[reporting_worker].amap.lookup(id, raw)) {
      throw std::runtime_error(
          "reporting worker cannot resolve its published identifier");
    }
    fetched_.emplace(id, raw);
    reply_key_lengths_by_worker_[reporting_worker].push_back(raw.size());
    return raw;
  }

  std::string resolve_output(const MethodGroup& group, const Id128& id,
                             std::uint32_t reporting_worker) {
    output_ids_.insert(id);
    return resolve(group, id, reporting_worker);
  }

  KeyResolutionStats finish() const {
    constexpr std::size_t kHeaderBytes = 32;
    constexpr std::size_t kIdBytes = sizeof(Id128);
    constexpr std::size_t kLengthBytes = sizeof(std::uint32_t);
    constexpr std::size_t kPublishedRecordBytes =
        sizeof(Id128) + 2 * sizeof(std::uint32_t);

    KeyResolutionStats out;
    std::size_t largest_frame = 0;
    for (const auto& lengths : reply_key_lengths_by_worker_) {
      if (lengths.empty()) continue;
      const std::size_t request =
          kHeaderBytes + lengths.size() * kIdBytes;
      std::size_t reply = kHeaderBytes;
      for (const std::size_t length : lengths) {
        reply += kIdBytes + kLengthBytes + length;
      }
      out.request_bytes += request;
      out.reply_bytes += reply;
      out.resolved_keys += lengths.size();
      largest_frame = std::max({largest_frame, request, reply});
    }

    // Resolution follows query selection. Only the common published
    // (identifier,count) list remains resident while one worker response is
    // decoded and emitted at a time; method-specific certificate state is
    // charged separately to reduction or control.
    out.coordinator_peak_bytes =
        sizeof(output_ids_)
        + output_ids_.size() * kPublishedRecordBytes
        + largest_frame;
    return out;
  }

private:
  std::unordered_map<Id128, std::string, Id128Hash> known_;
  std::unordered_map<Id128, std::string, Id128Hash> fetched_;
  std::unordered_set<Id128, Id128Hash> output_ids_;
  std::vector<std::vector<std::size_t>> reply_key_lengths_by_worker_;
};

static CertificationMetrics certification_metrics(const GlobalResultLB& oracle,
                                                   const GlobalResultLB& method) {
  CertificationMetrics cm{};
  cm.candidate_count = method.items.size();

  std::unordered_set<Id128, Id128Hash> oracle_hh;
  oracle_hh.reserve(oracle.items.size() * 2 + 8);
  for (const auto& it : oracle.items) {
    if (it.est >= oracle.threshold) oracle_hh.insert(it.id);
  }

  std::size_t candidate_tp = 0;
  std::size_t cert_pos_tp = 0;
  double width_sum = 0.0;
  double width_amb_sum = 0.0;
  const double N = static_cast<double>(method.N_global ? method.N_global : 1);

  for (const auto& it : method.items) {
    const bool is_oracle_hh = oracle_hh.find(it.id) != oracle_hh.end();
    if (is_oracle_hh) ++candidate_tp;

    const double mass = static_cast<double>(it.est) / N;
    const double width =
        static_cast<double>(it.cert_ub >= it.cert_lb ? it.cert_ub - it.cert_lb : 0) / N;
    width_sum += width;
    cm.interval_width_max = std::max(cm.interval_width_max, width);

    if (it.cert_lb >= method.threshold) {
      ++cm.cert_pos_count;
      cm.cert_pos_mass += mass;
      if (is_oracle_hh) ++cert_pos_tp;
    } else if (it.cert_ub < method.threshold) {
      ++cm.cert_neg_count;
      cm.cert_neg_mass += mass;
    } else {
      ++cm.ambiguous_count;
      cm.ambiguous_mass += mass;
      width_amb_sum += width;
    }
  }

  cm.candidate_hh_recall = oracle_hh.empty()
      ? 1.0
      : static_cast<double>(candidate_tp) / static_cast<double>(oracle_hh.size());
  cm.cert_pos_precision = cm.cert_pos_count == 0
      ? 1.0
      : static_cast<double>(cert_pos_tp) / static_cast<double>(cm.cert_pos_count);
  cm.interval_width_avg = cm.candidate_count == 0
      ? 0.0
      : width_sum / static_cast<double>(cm.candidate_count);
  cm.interval_width_amb_avg = cm.ambiguous_count == 0
      ? 0.0
      : width_amb_sum / static_cast<double>(cm.ambiguous_count);
  return cm;
}

static std::string csv_escape(const std::string& s) {
  bool quote = s.find_first_of(",\"\n\r") != std::string::npos;
  if (!quote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static void write_csv_header(std::ostream& os) {
  os << "window,method,method_type,N_global,threshold,"
     << "hh_precision,hh_recall,hh_f1,aae,are,topk_overlap,"
     << "q_current,q_next,q_head_current,q_tail_current,q_head_next,q_tail_next,"
     << "head_overlap_count,head_added_count,head_removed_count,"
     << "head_full_update_bytes,head_delta_update_bytes,head_delta_ratio,"
     << "head_delta_selected,head_update_apply_ms,"
     << "margin_alpha,service_violation,q_up,q_baseline,"
     << "probe_issued,probe_failed,"
     << "q_req_unclipped,q_req,q_eff_replay,q_eff_pred,q_eff_pred_tilde,"
     << "miss_req,miss_eff,over_req,over_eff,"
     << "calib_bias,"
     << "candidate_count,candidate_hh_recall,cert_pos_count,cert_pos_precision,cert_pos_mass,"
     << "ambiguous_count,ambiguous_mass,cert_neg_count,cert_neg_mass,"
     << "interval_width_avg,interval_width_amb_avg,interval_width_max,"
     << "completeness_certified,unseen_mass_ub,unseen_ub_over_threshold,"
     << "mem_worker_algo_kib,mem_worker_key_kib,mem_worker_total_kib,"
     << "report_volume_kib,control_volume_kib,head_update_volume_kib,"
     << "total_communication_kib,"
     << "key_request_volume_kib,key_reply_volume_kib,key_resolution_count,"
     << "mem_coord_ingress_kib,mem_coord_work_kib,mem_coord_control_peak_kib,"
     << "mem_coord_resolution_peak_kib,mem_coord_peak_kib,"
     << "update_events,update_ms,update_mops,report_prepare_ms,reduce_ms,control_ms,aggregation_ms\n";
}

static void write_csv_row(std::ostream& os, const CsvRow& r) {
  auto num = [](double v) {
    if (!std::isfinite(v)) return std::string{};
    std::ostringstream ss;
    ss << std::setprecision(10) << v;
    return ss.str();
  };
  auto opt_size = [](std::size_t v) {
    return v == 0 ? std::string{} : std::to_string(v);
  };
  auto opt_int = [](int v) {
    return v < 0 ? std::string{} : std::to_string(v);
  };

  os << r.window << ','
     << csv_escape(r.method) << ','
     << csv_escape(r.method_type) << ','
     << r.N_global << ','
     << r.threshold << ','
     << num(r.hh_precision) << ','
     << num(r.hh_recall) << ','
     << num(r.hh_f1) << ','
     << num(r.aae) << ','
     << num(r.are) << ','
     << (r.has_topk ? num(r.topk_overlap) : std::string{}) << ','
     << opt_size(r.q_current) << ','
     << opt_size(r.q_next) << ','
     << opt_size(r.q_head_current) << ','
     << opt_size(r.q_tail_current) << ','
     << opt_size(r.q_head_next) << ','
     << opt_size(r.q_tail_next) << ','
     << r.head_overlap_count << ','
     << r.head_added_count << ','
     << r.head_removed_count << ','
     << r.head_full_update_bytes << ','
     << r.head_delta_update_bytes << ','
     << num(r.head_delta_ratio) << ','
     << opt_int(r.head_delta_selected) << ','
     << num(r.head_update_apply_ms) << ','
     << num(r.margin_alpha) << ','
     << opt_int(r.service_violation) << ','
     << opt_size(r.q_up) << ','
     << opt_size(r.q_baseline) << ','
     << opt_int(r.probe_issued) << ','
     << opt_int(r.probe_failed) << ','
     << r.q_req_unclipped << ','
     << opt_size(r.q_req) << ','
     << opt_size(r.q_eff_replay) << ','
     << opt_size(r.q_eff_pred) << ','
     << num(r.q_eff_pred_tilde) << ','
     << opt_int(r.miss_req) << ','
     << opt_int(r.miss_eff) << ','
     << num(r.over_req) << ','
     << num(r.over_eff) << ','
     << num(r.calib_bias) << ','
     << r.cert.candidate_count << ','
     << num(r.cert.candidate_hh_recall) << ','
     << r.cert.cert_pos_count << ','
     << num(r.cert.cert_pos_precision) << ','
     << num(r.cert.cert_pos_mass) << ','
     << r.cert.ambiguous_count << ','
     << num(r.cert.ambiguous_mass) << ','
     << r.cert.cert_neg_count << ','
     << num(r.cert.cert_neg_mass) << ','
     << num(r.cert.interval_width_avg) << ','
     << num(r.cert.interval_width_amb_avg) << ','
     << num(r.cert.interval_width_max) << ','
     << opt_int(r.completeness_certified) << ','
     << (r.completeness_certified < 0
             ? std::string{}
             : std::to_string(r.unseen_mass_ub)) << ','
     << num(r.unseen_ub_over_threshold) << ','
     << num(r.mem_worker_algo_kib) << ','
     << num(r.mem_worker_key_kib) << ','
     << num(r.mem_worker_total_kib) << ','
     << num(r.report_volume_kib) << ','
     << num(r.control_volume_kib) << ','
     << num(r.head_update_volume_kib) << ','
     << num(r.total_communication_kib) << ','
     << num(r.key_request_volume_kib) << ','
     << num(r.key_reply_volume_kib) << ','
     << r.key_resolution_count << ','
     << num(r.mem_coord_ingress_kib) << ','
     << num(r.mem_coord_work_kib) << ','
     << num(r.mem_coord_control_peak_kib) << ','
     << num(r.mem_coord_resolution_peak_kib) << ','
     << num(r.mem_coord_peak_kib) << ','
     << r.update_events << ','
     << num(r.update_ms) << ','
     << num(r.update_mops) << ','
     << num(r.report_prepare_ms) << ','
     << num(r.reduce_ms) << ','
     << num(r.control_ms) << ','
     << num(r.aggregation_ms) << '\n';
}

// --- CLI policy/state ---
enum class Policy { Difficulty, Static };
enum class TailPolicy { Static, Difficulty };
enum class HeadPolicy {
  TopNFrontier,
  Frontier,
  Confirmed,
  TopN,
  Top2N,
  Threshold,
};
enum class ReducerMode { Hash, Streaming, ParallelStreaming };
enum class DifficultyMode { Predictive, ReactiveReq, ReactiveEff };

struct Args {
  std::string path;
  std::size_t m{0};
  std::size_t n_param{0};
  std::size_t memKiB{0};
  std::string methods_csv;     // must include 'oracle' (reference)
  std::string csv_out;         // optional per-window CSV output
  std::optional<std::size_t> topk;
  std::size_t reducer_workers{8}; // parallel reducer worker tasks

  // adaptive sizing knobs (for SS)
  Policy policy{Policy::Difficulty};
  double r{0.10};              // retained for compatibility with older method specs
  double alpha_req{0.98};      // difficulty: quantile for q_Req^t
  // difficulty policy knobs
  double delta_m{0.2};
  std::size_t probe_evidence_window{2};
  double ambiguity_decay{0.5};
  bool symmetric_relaxation{false};
  bool censored_control{true};
  bool probe_residual_guard{true};
  hh::sizing::ProbeStrategy probe_strategy{hh::sizing::ProbeStrategy::bracket};
  double probe_pressure_gate{0.5};
  bool ambiguity_adjust{true};
  double epsilon_m{0.03};
  DifficultyMode diff_mode{DifficultyMode::Predictive};
  bool ss_per_item_eps{true}; // SS epsilon mode: false=max-sketch, true=per-item

  // HeavyLocker parameters
  std::size_t hl_d{6};
  double hl_L{0.7};
  int hl_lossy{2}; // 0=MinusOne, 1=HeavyKeeper, 2=RAP, 3=USS
  std::size_t hl_w{0}; // optional fixed width (0 => auto from memory)
  bool hl_certify{false};
  bool hl_adaptive{false}; // certificate failure advances one width-ladder rung

  TailPolicy tail_policy{TailPolicy::Static}; // hybrid tail sizing
  std::size_t tail_q_factor{1};               // hybrid static tail: q_a = tail_q_factor * n_param
  HeadPolicy head_policy{HeadPolicy::TopNFrontier}; // hybrid head seeding
  std::size_t q_factor{1};                    // static SS: q = q_factor * n_param
};

struct MethodCfg {
  std::string name;
  Policy policy;
  double r;
  double alpha_req;
  double delta_m;
  std::size_t probe_evidence_window;
  double ambiguity_decay;
  bool symmetric_relaxation;
  bool censored_control;
  bool probe_residual_guard;
  hh::sizing::ProbeStrategy probe_strategy;
  double probe_pressure_gate;
  bool ambiguity_adjust;
  double epsilon_m;
  DifficultyMode diff_mode;
  bool ss_per_item_eps;
  std::size_t hl_d;
  double hl_L;
  int hl_lossy;
  std::size_t hl_w;
  bool hl_certify;
  bool hl_adaptive;
  TailPolicy tail_policy;
  std::size_t tail_q_factor;
  HeadPolicy head_policy;
  ReducerMode reducer;
  std::size_t q_factor;
  double head_delta_eta{0.80};
  std::size_t head_checkpoint_interval{32};
};

static bool parse_args(int argc, char** argv, Args& a) {
  if (argc < 6) {
    std::cerr << "usage: " << argv[0]
              << " <stream.json.gz> <m> <n_param> <memKiB_each> <methods_csv>"
              << " [--csv-out FILE] [--topk K] [--policy difficulty|static] [--alpha-req A]"
              << " [--reducer-workers P]"
              << " [--delta-m D (deprecated, ignored)]"
              << " [--symmetric-relaxation on|off]"
              << " [--downward-probing on|off]"
              << " [--probe-residual-guard on|off]"
              << " [--probe-strategy bracket|comfort|pressure]"
              << " [--probe-pressure-gate RHO]"
              << " [--amb-adjust on|off]"
              << " [--epsilon-m EPS] [--diff-mode predictive|reactive-req|reactive-eff]"
              << " [--ss-eps per-item|max-sketch]"
              << " [--hl-d D] [--hl-L L] [--hl-lossy MODE] [--hl-w W]"
              << " [method options for hl: hl-cert=on|off hl-adaptive=on|off]"
              << " [--hyb-tail n|2n|difficulty] [--hyb-head topn|frontier|topn-frontier|confirmed|top2n|threshold]"
              << " [method options for hybrid: head-delta-eta=ETA head-checkpoint=WINDOWS]"
              << " [method option for ss/hybrid: reducer=hash|streaming|parallel-streaming] [--q kN]\n";
    return false;
  }
  a.path       = argv[1];
  a.m          = std::stoull(argv[2]);
  a.n_param    = std::stoull(argv[3]);
  a.memKiB     = std::stoull(argv[4]);
  a.methods_csv= argv[5];

  for (int i=6; i<argc; ++i) {
    std::string flag = argv[i];
  auto need = [&](int more){ if (i+more >= argc){ std::cerr<<"missing arg for "<<flag<<"\n"; std::exit(2);} };
  if (flag == "--csv-out") { need(1); a.csv_out = argv[++i]; }
  else if (flag == "--topk") { need(1); a.topk = std::stoull(argv[++i]); }
  else if (flag == "--reducer-workers") {
    need(1);
    a.reducer_workers = std::max<std::size_t>(1, std::stoull(argv[++i]));
  }
  else if (flag == "--policy") {
    need(1); std::string v = argv[++i];
    if (v=="difficulty") a.policy = Policy::Difficulty;
    else if (v=="static") a.policy = Policy::Static;
    else { std::cerr<<"unknown policy "<<v<<"\n"; return false; }
  } else if (flag == "--alpha-req") {
    need(1); a.alpha_req = std::stod(argv[++i]); if (a.alpha_req<=0 || a.alpha_req>1) a.alpha_req=0.98;
  } else if (flag == "--delta-m") {
    need(1); a.delta_m = std::clamp(std::stod(argv[++i]), 0.0, 1.0);
  } else if (flag == "--symmetric-relaxation") {
    need(1);
    std::string v = argv[++i];
    if (v == "on" || v == "true" || v == "1") a.symmetric_relaxation = true;
    else if (v == "off" || v == "false" || v == "0") a.symmetric_relaxation = false;
    else { std::cerr<<"--symmetric-relaxation must be on|off\n"; return false; }
  } else if (flag == "--censored-control" || flag == "--downward-probing") {
    need(1);
    std::string v = argv[++i];
    if (v == "on" || v == "true" || v == "1") a.censored_control = true;
    else if (v == "off" || v == "false" || v == "0") a.censored_control = false;
    else { std::cerr<<"--downward-probing must be on|off\n"; return false; }
  } else if (flag == "--probe-residual-guard") {
    need(1);
    std::string v = argv[++i];
    if (v == "on" || v == "true" || v == "1") a.probe_residual_guard = true;
    else if (v == "off" || v == "false" || v == "0") a.probe_residual_guard = false;
    else { std::cerr<<"--probe-residual-guard must be on|off\n"; return false; }
  } else if (flag == "--probe-strategy") {
    need(1);
    std::string v = argv[++i];
    if (v == "bracket") a.probe_strategy = hh::sizing::ProbeStrategy::bracket;
    else if (v == "comfort") a.probe_strategy = hh::sizing::ProbeStrategy::comfort;
    else if (v == "pressure") a.probe_strategy = hh::sizing::ProbeStrategy::pressure;
    else { std::cerr<<"--probe-strategy must be bracket|comfort|pressure\n"; return false; }
  } else if (flag == "--probe-pressure-gate") {
    need(1); a.probe_pressure_gate = std::clamp(std::stod(argv[++i]), 0.0, 1.0);
  } else if (flag == "--amb-adjust") {
    need(1);
    std::string v = argv[++i];
    if (v == "on" || v == "true" || v == "1") a.ambiguity_adjust = true;
    else if (v == "off" || v == "false" || v == "0") a.ambiguity_adjust = false;
    else { std::cerr<<"--amb-adjust must be on|off\n"; return false; }
  } else if (flag == "--epsilon-m") {
    need(1); a.epsilon_m = std::max(1e-12, std::stod(argv[++i]));
  } else if (flag == "--diff-mode") {
    need(1);
    std::string v = argv[++i];
    if (v == "predictive") a.diff_mode = DifficultyMode::Predictive;
    else if (v == "reactive-req") a.diff_mode = DifficultyMode::ReactiveReq;
    else if (v == "reactive-eff") a.diff_mode = DifficultyMode::ReactiveEff;
    else { std::cerr<<"--diff-mode must be predictive|reactive-req|reactive-eff\n"; return false; }
  } else if (flag == "--ss-eps") {
    need(1); std::string v = argv[++i];
    if      (v=="max-sketch") a.ss_per_item_eps = false;
    else if (v=="per-item") a.ss_per_item_eps = true;
    else { std::cerr<<"--ss-eps must be per-item|max-sketch\n"; return false; }
  } else if (flag == "--hl-d") {
    need(1); a.hl_d = std::max<std::size_t>(1, std::stoull(argv[++i]));
  } else if (flag == "--hl-L") {
    need(1); a.hl_L = std::stod(argv[++i]);
  } else if (flag == "--hl-lossy") {
    need(1); a.hl_lossy = std::stoi(argv[++i]);
  } else if (flag == "--hl-w") {
    need(1); a.hl_w = std::stoull(argv[++i]);
  } else if (flag == "--hyb-tail") {
    need(1); std::string v = argv[++i];
    if (!v.empty() && v.back()=='n') {
      v.pop_back();
      a.tail_policy = TailPolicy::Static;
      a.tail_q_factor = v.empty() ? 1 : std::stoull(v);
      if (a.tail_q_factor == 0) a.tail_q_factor = 1;
    } else if (v=="difficulty") {
      a.tail_policy = TailPolicy::Difficulty;
    } else { std::cerr<<"--hyb-tail must be n|2n|difficulty\n"; return false; }
  } else if (flag == "--q") {
    need(1); std::string v = argv[++i];
    if (!v.empty() && v.back()=='n') v.pop_back();
    a.q_factor = v.empty() ? 1 : std::stoull(v);
    if (a.q_factor == 0) a.q_factor = 1;
  } else if (flag == "--hyb-head") {
    need(1); std::string v = argv[++i];
    if      (v=="topn-frontier") a.head_policy = HeadPolicy::TopNFrontier;
    else if (v=="frontier")  a.head_policy = HeadPolicy::Frontier;
    else if (v=="confirmed") a.head_policy = HeadPolicy::Confirmed;
    else if (v=="topn")      a.head_policy = HeadPolicy::TopN;
    else if (v=="top2n")     a.head_policy = HeadPolicy::Top2N;
    else if (v=="threshold") a.head_policy = HeadPolicy::Threshold;
    else { std::cerr<<"--hyb-head must be topn|frontier|topn-frontier|confirmed|top2n|threshold\n"; return false; }
  }
}

  if (a.m==0 || a.n_param==0) { std::cerr<<"m and n_param must be >0\n"; return false; }
  return true;
}

// --- method builders ---
static MethodGroup make_method(
    const MethodCfg& spec,
    std::size_t m,
    std::size_t bytes_per_part,
    std::size_t n_param,
    std::shared_ptr<HeavyLockerRand> hl_random_stream = {}) {
  MethodGroup G; G.name = spec.name; G.parts.resize(m);

  if (spec.name == "oracle") {
    for (std::size_t i=0; i<m; ++i) {
      auto ex = std::make_unique<OracleAll>();
      Part* part = &G.parts[i];
      wire_key_lifecycle(*ex, *part);
      G.parts[i].sketch = std::move(ex);
    }
  } else if (spec.name == "ss") {
    // initial q0 = n (cap to memory): ~32B per SS counter
    const std::size_t q_cap = std::max<std::size_t>(std::size_t(1), bytes_per_part / 32);
    const std::size_t q0 = std::min<std::size_t>(n_param, q_cap); // initial guess; will be set per policy
    for (std::size_t i=0; i<m; ++i) {
      auto ss = std::make_unique<SpaceSaving>(q0, spec.ss_per_item_eps);
      Part* part = &G.parts[i];
      wire_key_lifecycle(*ss, *part);
      G.parts[i].sketch = std::move(ss);
    }
  } else if (spec.name == "hl") {
    // HeavyLocker (paper-inspired static sizing)
    const std::size_t d = std::max<std::size_t>(1, spec.hl_d);
    const double L = spec.hl_L;
    const double theta = 1.0 / static_cast<double>(n_param); // θ = 1/n
    const int lossy_mode = spec.hl_lossy;
    constexpr std::size_t SLOT_BYTES = sizeof(Id128) + sizeof(std::uint32_t);
    std::size_t w = spec.hl_w;
    if (w == 0) {
      w = std::max<std::size_t>(1, bytes_per_part / (d * SLOT_BYTES + 1));
    }
    // Match the authors' srand(1) placement: one random stream is shared by
    // every worker in this HL configuration. Different configurations receive
    // independent copies so their results do not depend on benchmark ordering.
    auto random_stream = hl_random_stream
        ? std::move(hl_random_stream)
        : std::make_shared<HeavyLockerRand>(1);
    for (std::size_t i = 0; i < G.parts.size(); ++i) {
      auto& p = G.parts[i];
      auto hl = std::make_unique<HeavyLocker>(
          w, d, L, theta, lossy_mode, 1, random_stream, spec.hl_certify);
      Part* part = &p;
      wire_key_lifecycle(*hl, *part);
      p.sketch = std::move(hl);
    }
  } else if (spec.name == "chk") {
    // CHK: ~86B per bucket across 2 tables (empirical)
    const std::size_t B = std::max<std::size_t>(1, bytes_per_part / 86);
    for (auto& p : G.parts) {
      auto chk = std::make_unique<CHK>(B, /*L*/16, /*decay*/1.08);
      Part* part = &p;
      wire_key_lifecycle(*chk, *part);
      p.sketch = std::move(chk);
    }
  } else if (spec.name == "hybrid") {
    // Rough split: half memory to head (24B/entry), half to tail (32B/entry)
    const std::size_t head_bytes = bytes_per_part / 2;
    const std::size_t tail_bytes = bytes_per_part - head_bytes;
    const std::size_t q_e = std::max<std::size_t>(1, head_bytes / 24);
    const std::size_t q_a = std::max<std::size_t>(n_param, tail_bytes / 32); // residual discoverability
    for (auto& p : G.parts) {
      auto hy = std::make_unique<HybridSS>(q_e, q_a, spec.ss_per_item_eps);
      Part* part = &p;
      wire_key_lifecycle(*hy, *part);
      p.sketch = std::move(hy);
    }
  }

  return G;
}

static GlobalResultLB reduce_global(const MethodGroup& G, std::size_t n_param) {
  std::vector<SnapshotEx> snaps; snaps.reserve(G.parts.size());
  for (const auto& p : G.parts) {
    snaps.push_back(p.sketch->snapshot_ex());
  }
  return Coordinator::reduce_global_with_lb(snaps, n_param);
}

// helper: rebuild ss group with a new q across partitions
static void rebuild_ss_group(MethodGroup& G, std::size_t q_new, bool per_item_eps) {
  for (auto& p : G.parts) {
    auto ss = std::make_unique<SpaceSaving>(q_new, per_item_eps);
    Part* part = &p;
    wire_key_lifecycle(*ss, *part);
    p.sketch = std::move(ss);
  }
}

static std::vector<std::size_t> hl_width_ladder(std::size_t n_param) {
  // Existing evaluation points: 0.08n, 0.16n, 0.32n, 0.64n, and 1.28n.
  // Integer arithmetic keeps the ladder reproducible for every n.
  std::vector<std::size_t> widths;
  widths.reserve(5);
  for (const std::size_t numerator : {2u, 4u, 8u, 16u, 32u}) {
    const std::size_t width =
        std::max<std::size_t>(1, (n_param * numerator + 24) / 25);
    if (widths.empty() || widths.back() != width) widths.push_back(width);
  }
  return widths;
}

static std::size_t next_hl_width(
    const std::vector<std::size_t>& ladder,
    std::size_t current) {
  const auto it = std::upper_bound(ladder.begin(), ladder.end(), current);
  return it == ladder.end() ? current : *it;
}

static void rebuild_hl_group(
    MethodGroup& group,
    const MethodCfg& spec,
    std::size_t n_param,
    std::size_t width,
    const std::shared_ptr<HeavyLockerRand>& random_stream) {
  const std::size_t depth = std::max<std::size_t>(1, spec.hl_d);
  const double theta = 1.0 / static_cast<double>(n_param);
  for (auto& part : group.parts) {
    auto hl = std::make_unique<HeavyLocker>(
        width, depth, spec.hl_L, theta, spec.hl_lossy, 1,
        random_stream, /*certify_residual=*/true);
    wire_key_lifecycle(*hl, part);
    part.sketch = std::move(hl);
  }
}

int main(int argc, char** argv) {
  Args A;
  if (!parse_args(argc, argv, A)) return 1;

  std::ofstream csv_file;
  if (!A.csv_out.empty()) {
    csv_file.open(A.csv_out);
    if (!csv_file) {
      std::cerr << "ERROR: could not open CSV output file '" << A.csv_out << "'\n";
      return 5;
    }
    write_csv_header(csv_file);
  }

  set_secret_keys({0},{1},{2});
  const std::size_t bytes_per_part = A.memKiB * 1024;

  // Parse method list
  auto known_method = [&](const std::string& nm){
    return nm == "oracle" || nm == "hl" || nm == "chk" || nm == "hybrid" || nm == "ss";
  };
  std::vector<MethodCfg> methods;
  {
    MethodCfg defaults{"", A.policy, A.r, A.alpha_req,
                      A.delta_m, A.probe_evidence_window, A.ambiguity_decay,
                      A.symmetric_relaxation,
                      A.censored_control,
                      A.probe_residual_guard,
                      A.probe_strategy,
                      A.probe_pressure_gate,
                      A.ambiguity_adjust,
                      A.epsilon_m, A.diff_mode, A.ss_per_item_eps,
                      A.hl_d, A.hl_L, A.hl_lossy, A.hl_w, A.hl_certify,
                      A.hl_adaptive,
                      A.tail_policy, A.tail_q_factor, A.head_policy,
                      ReducerMode::Hash, A.q_factor};
    std::string tok;
    bool in_bracket = false;
    for (char c : A.methods_csv) {
      if (c == ',' && !in_bracket) {
        if (!tok.empty()) { // flush token
          // parse token
          std::string name = tok;
          std::string opts;
          auto lb = tok.find('[');
          if (lb != std::string::npos) {
            name = tok.substr(0, lb);
            auto rb = tok.find(']', lb);
            if (rb == std::string::npos || rb != tok.size()-1) {
              std::cerr << "Malformed method entry: " << tok << "\n";
              return 4;
            }
            opts = tok.substr(lb+1, rb-lb-1);
          }
          if (!known_method(name)) {
            std::cerr << "ERROR: unknown method '" << name << "'. Allowed: oracle, ss, hl, chk, hybrid.\n";
            return 4;
          }
          MethodCfg cfg = defaults;
          cfg.name = name;
          if (!opts.empty()) {
            std::stringstream ss(opts);
            std::string kv;
            while (ss >> kv) {
              auto eq = kv.find('=');
              if (eq == std::string::npos) { std::cerr << "Malformed option '"<<kv<<"' in " << tok << "\n"; return 4; }
              auto key = kv.substr(0, eq);
              auto val = kv.substr(eq+1);
              if (key == "policy") {
                if (val=="difficulty") cfg.policy = Policy::Difficulty;
                else if (val=="static") cfg.policy = Policy::Static;
                else { std::cerr<<"unknown policy "<<val<<"\n"; return 4; }
              } else if (key == "r") {
                cfg.r = std::stod(val); if (!(cfg.r>0 && cfg.r<1)) cfg.r=defaults.r;
              } else if (key == "alpha-req") {
                cfg.alpha_req = std::stod(val); if (cfg.alpha_req<=0 || cfg.alpha_req>1) cfg.alpha_req=defaults.alpha_req;
              } else if (key == "delta-m") {
                cfg.delta_m = std::clamp(std::stod(val), 0.0, 1.0);
              } else if (key == "symmetric-relaxation") {
                if (val == "on" || val == "true" || val == "1") cfg.symmetric_relaxation = true;
                else if (val == "off" || val == "false" || val == "0") cfg.symmetric_relaxation = false;
                else { std::cerr<<"symmetric-relaxation must be on|off\n"; return 4; }
              } else if (key == "censored-control" || key == "downward-probing") {
                if (val == "on" || val == "true" || val == "1") cfg.censored_control = true;
                else if (val == "off" || val == "false" || val == "0") cfg.censored_control = false;
                else { std::cerr<<"downward-probing must be on|off\n"; return 4; }
              } else if (key == "probe-residual-guard") {
                if (val == "on" || val == "true" || val == "1") cfg.probe_residual_guard = true;
                else if (val == "off" || val == "false" || val == "0") cfg.probe_residual_guard = false;
                else { std::cerr<<"probe-residual-guard must be on|off\n"; return 4; }
              } else if (key == "probe-strategy") {
                if (val == "bracket") cfg.probe_strategy = hh::sizing::ProbeStrategy::bracket;
                else if (val == "comfort") cfg.probe_strategy = hh::sizing::ProbeStrategy::comfort;
                else if (val == "pressure") cfg.probe_strategy = hh::sizing::ProbeStrategy::pressure;
                else { std::cerr<<"probe-strategy must be bracket|comfort|pressure\n"; return 4; }
              } else if (key == "probe-pressure-gate") {
                cfg.probe_pressure_gate = std::clamp(std::stod(val), 0.0, 1.0);
              } else if (key == "amb-adjust") {
                if (val == "on" || val == "true" || val == "1") cfg.ambiguity_adjust = true;
                else if (val == "off" || val == "false" || val == "0") cfg.ambiguity_adjust = false;
                else { std::cerr<<"amb-adjust must be on|off\n"; return 4; }
              } else if (key == "epsilon-m") {
                cfg.epsilon_m = std::max(1e-12, std::stod(val));
              } else if (key == "diff-mode") {
                if (val == "predictive") cfg.diff_mode = DifficultyMode::Predictive;
                else if (val == "reactive-req") cfg.diff_mode = DifficultyMode::ReactiveReq;
                else if (val == "reactive-eff") cfg.diff_mode = DifficultyMode::ReactiveEff;
                else { std::cerr<<"diff-mode must be predictive|reactive-req|reactive-eff\n"; return 4; }
              } else if (key == "ss-eps") {
                if      (val=="max-sketch") cfg.ss_per_item_eps = false;
                else if (val=="per-item") cfg.ss_per_item_eps = true;
                else { std::cerr<<"ss-eps must be per-item|max-sketch\n"; return 4; }
              } else if (key == "hyb-tail") {
                if (!val.empty() && val.back()=='n') {
                  val.pop_back();
                  cfg.tail_policy = TailPolicy::Static;
                  cfg.tail_q_factor = val.empty() ? 1 : std::stoull(val);
                  if (cfg.tail_q_factor == 0) cfg.tail_q_factor = 1;
                } else if (val=="difficulty") {
                  cfg.tail_policy = TailPolicy::Difficulty;
                } else { std::cerr<<"hyb-tail must be n|2n|difficulty\n"; return 4; }
              } else if (key == "q") {
                std::string v = val;
                if (!v.empty() && v.back()=='n') v.pop_back();
                cfg.q_factor = v.empty() ? 1 : std::stoull(v);
                if (cfg.q_factor == 0) cfg.q_factor = 1;
              } else if (key == "hyb-head") {
                if      (val=="topn-frontier") cfg.head_policy = HeadPolicy::TopNFrontier;
                else if (val=="frontier")  cfg.head_policy = HeadPolicy::Frontier;
                else if (val=="confirmed") cfg.head_policy = HeadPolicy::Confirmed;
                else if (val=="topn")      cfg.head_policy = HeadPolicy::TopN;
                else if (val=="top2n")     cfg.head_policy = HeadPolicy::Top2N;
                else if (val=="threshold") cfg.head_policy = HeadPolicy::Threshold;
                else { std::cerr<<"hyb-head must be topn|frontier|topn-frontier|confirmed|top2n|threshold\n"; return 4; }
              } else if (key == "reducer") {
                if (val == "hash") cfg.reducer = ReducerMode::Hash;
                else if (val == "streaming") cfg.reducer = ReducerMode::Streaming;
                else if (val == "parallel-streaming") cfg.reducer = ReducerMode::ParallelStreaming;
                else { std::cerr<<"reducer must be hash|streaming|parallel-streaming\n"; return 4; }
              } else if (key == "head-delta-eta") {
                cfg.head_delta_eta = std::clamp(std::stod(val), 0.0, 1.0);
              } else if (key == "head-checkpoint") {
                cfg.head_checkpoint_interval =
                    std::max<std::size_t>(1, std::stoull(val));
              } else if (key == "hl-d") {
                cfg.hl_d = std::max<std::size_t>(1, std::stoull(val));
              } else if (key == "hl-L") {
                cfg.hl_L = std::stod(val);
              } else if (key == "hl-lossy") {
                cfg.hl_lossy = std::stoi(val);
              } else if (key == "hl-w") {
                cfg.hl_w = std::stoull(val);
              } else if (key == "hl-cert") {
                if (val == "on" || val == "true" || val == "1") cfg.hl_certify = true;
                else if (val == "off" || val == "false" || val == "0") cfg.hl_certify = false;
                else { std::cerr<<"hl-cert must be on|off\n"; return 4; }
              } else if (key == "hl-adaptive") {
                if (val == "on" || val == "true" || val == "1") cfg.hl_adaptive = true;
                else if (val == "off" || val == "false" || val == "0") cfg.hl_adaptive = false;
                else { std::cerr<<"hl-adaptive must be on|off\n"; return 4; }
              } else {
                std::cerr << "Unknown method option '" << key << "'\n";
                return 4;
              }
            }
          }
          methods.push_back(cfg);
          tok.clear();
        }
      } else {
        if (c == '[') in_bracket = true;
        if (c == ']') in_bracket = false;
        tok.push_back(c);
      }
    }
  if (!tok.empty()) {
      std::string name = tok;
      std::string opts;
      auto lb = tok.find('[');
      if (lb != std::string::npos) {
        name = tok.substr(0, lb);
        auto rb = tok.find(']', lb);
        if (rb == std::string::npos || rb != tok.size()-1) {
          std::cerr << "Malformed method entry: " << tok << "\n";
          return 4;
        }
        opts = tok.substr(lb+1, rb-lb-1);
      }
      if (!known_method(name)) {
        std::cerr << "ERROR: unknown method '" << name << "'. Allowed: oracle, ss, hl, chk, hybrid.\n";
        return 4;
      }
      MethodCfg cfg = defaults;
      cfg.name = name;
      if (!opts.empty()) {
        std::stringstream ss(opts);
        std::string kv;
        while (ss >> kv) {
          auto eq = kv.find('=');
          if (eq == std::string::npos) { std::cerr << "Malformed option '"<<kv<<"' in " << tok << "\n"; return 4; }
          auto key = kv.substr(0, eq);
          auto val = kv.substr(eq+1);
          if (key == "policy") {
            if (val=="difficulty") cfg.policy = Policy::Difficulty;
            else if (val=="static") cfg.policy = Policy::Static;
            else { std::cerr<<"unknown policy "<<val<<"\n"; return 4; }
          } else if (key == "r") {
            cfg.r = std::stod(val); if (!(cfg.r>0 && cfg.r<1)) cfg.r=defaults.r;
          } else if (key == "alpha-req") {
            cfg.alpha_req = std::stod(val); if (cfg.alpha_req<=0 || cfg.alpha_req>1) cfg.alpha_req=defaults.alpha_req;
          } else if (key == "delta-m") {
            cfg.delta_m = std::clamp(std::stod(val), 0.0, 1.0);
          } else if (key == "symmetric-relaxation") {
            if (val == "on" || val == "true" || val == "1") cfg.symmetric_relaxation = true;
            else if (val == "off" || val == "false" || val == "0") cfg.symmetric_relaxation = false;
            else { std::cerr<<"symmetric-relaxation must be on|off\n"; return 4; }
          } else if (key == "censored-control" || key == "downward-probing") {
            if (val == "on" || val == "true" || val == "1") cfg.censored_control = true;
            else if (val == "off" || val == "false" || val == "0") cfg.censored_control = false;
            else { std::cerr<<"downward-probing must be on|off\n"; return 4; }
          } else if (key == "probe-residual-guard") {
            if (val == "on" || val == "true" || val == "1") cfg.probe_residual_guard = true;
            else if (val == "off" || val == "false" || val == "0") cfg.probe_residual_guard = false;
            else { std::cerr<<"probe-residual-guard must be on|off\n"; return 4; }
          } else if (key == "probe-strategy") {
            if (val == "bracket") cfg.probe_strategy = hh::sizing::ProbeStrategy::bracket;
            else if (val == "comfort") cfg.probe_strategy = hh::sizing::ProbeStrategy::comfort;
            else if (val == "pressure") cfg.probe_strategy = hh::sizing::ProbeStrategy::pressure;
            else { std::cerr<<"probe-strategy must be bracket|comfort|pressure\n"; return 4; }
          } else if (key == "probe-pressure-gate") {
            cfg.probe_pressure_gate = std::clamp(std::stod(val), 0.0, 1.0);
          } else if (key == "amb-adjust") {
            if (val == "on" || val == "true" || val == "1") cfg.ambiguity_adjust = true;
            else if (val == "off" || val == "false" || val == "0") cfg.ambiguity_adjust = false;
            else { std::cerr<<"amb-adjust must be on|off\n"; return 4; }
          } else if (key == "epsilon-m") {
            cfg.epsilon_m = std::max(1e-12, std::stod(val));
          } else if (key == "diff-mode") {
            if (val == "predictive") cfg.diff_mode = DifficultyMode::Predictive;
            else if (val == "reactive-req") cfg.diff_mode = DifficultyMode::ReactiveReq;
            else if (val == "reactive-eff") cfg.diff_mode = DifficultyMode::ReactiveEff;
            else { std::cerr<<"diff-mode must be predictive|reactive-req|reactive-eff\n"; return 4; }
          } else if (key == "ss-eps") {
            if      (val=="max-sketch") cfg.ss_per_item_eps = false;
            else if (val=="per-item") cfg.ss_per_item_eps = true;
            else { std::cerr<<"ss-eps must be per-item|max-sketch\n"; return 4; }
          } else if (key == "hyb-tail") {
            if (!val.empty() && val.back()=='n') {
              val.pop_back();
              cfg.tail_policy = TailPolicy::Static;
              cfg.tail_q_factor = val.empty() ? 1 : std::stoull(val);
              if (cfg.tail_q_factor == 0) cfg.tail_q_factor = 1;
            } else if (val=="difficulty") {
              cfg.tail_policy = TailPolicy::Difficulty;
            } else { std::cerr<<"hyb-tail must be n|2n|difficulty\n"; return 4; }
          } else if (key == "q") {
            std::string v = val;
            if (!v.empty() && v.back()=='n') v.pop_back();
            cfg.q_factor = v.empty() ? 1 : std::stoull(v);
            if (cfg.q_factor == 0) cfg.q_factor = 1;
          } else if (key == "hyb-head") {
            if      (val=="topn-frontier") cfg.head_policy = HeadPolicy::TopNFrontier;
            else if (val=="frontier")  cfg.head_policy = HeadPolicy::Frontier;
            else if (val=="confirmed") cfg.head_policy = HeadPolicy::Confirmed;
            else if (val=="topn")      cfg.head_policy = HeadPolicy::TopN;
            else if (val=="top2n")     cfg.head_policy = HeadPolicy::Top2N;
            else if (val=="threshold") cfg.head_policy = HeadPolicy::Threshold;
            else { std::cerr<<"hyb-head must be topn|frontier|topn-frontier|confirmed|top2n|threshold\n"; return 4; }
          } else if (key == "reducer") {
            if (val == "hash") cfg.reducer = ReducerMode::Hash;
            else if (val == "streaming") cfg.reducer = ReducerMode::Streaming;
            else if (val == "parallel-streaming") cfg.reducer = ReducerMode::ParallelStreaming;
            else { std::cerr<<"reducer must be hash|streaming|parallel-streaming\n"; return 4; }
          } else if (key == "head-delta-eta") {
            cfg.head_delta_eta = std::clamp(std::stod(val), 0.0, 1.0);
          } else if (key == "head-checkpoint") {
            cfg.head_checkpoint_interval =
                std::max<std::size_t>(1, std::stoull(val));
          } else if (key == "hl-d") {
            cfg.hl_d = std::max<std::size_t>(1, std::stoull(val));
          } else if (key == "hl-L") {
            cfg.hl_L = std::stod(val);
          } else if (key == "hl-lossy") {
            cfg.hl_lossy = std::stoi(val);
          } else if (key == "hl-w") {
            cfg.hl_w = std::stoull(val);
          } else if (key == "hl-cert") {
            if (val == "on" || val == "true" || val == "1") cfg.hl_certify = true;
            else if (val == "off" || val == "false" || val == "0") cfg.hl_certify = false;
            else { std::cerr<<"hl-cert must be on|off\n"; return 4; }
          } else if (key == "hl-adaptive") {
            if (val == "on" || val == "true" || val == "1") cfg.hl_adaptive = true;
            else if (val == "off" || val == "false" || val == "0") cfg.hl_adaptive = false;
            else { std::cerr<<"hl-adaptive must be on|off\n"; return 4; }
          } else {
            std::cerr << "Unknown method option '" << key << "'\n";
            return 4;
          }
        }
      }
      methods.push_back(cfg);
    }
  }
  if (methods.empty()) {
    std::cerr << "No methods parsed from '" << A.methods_csv << "'\n";
    return 4;
  }
  for (auto& method : methods) {
    if (method.name == "hl" && method.hl_adaptive) {
      // Adaptation is driven exclusively by the deterministic residual
      // certificate, so certified bookkeeping is mandatory.
      method.hl_certify = true;
    }
  }

  // Build human-readable labels (especially for SS) so the output reflects differentiating args
  std::vector<std::string> labels(methods.size());
  std::size_t max_label_len = 0;
  {
    // First handle SS with minimal distinguishing prefixes
    std::vector<std::size_t> ss_idxs;
    std::unordered_map<std::size_t, std::vector<std::string>> ss_components;
    auto policy_str = [](Policy p) {
      switch (p) {
        case Policy::Difficulty: return std::string("policy=difficulty");
        case Policy::Static: return std::string("policy=static");
      }
      return std::string("policy=unknown");
    };
    auto q_str = [](std::size_t qf) {
      if (qf <= 1) return std::string("q=n");
      return std::string("q=") + std::to_string(qf) + "n";
    };
    auto dbl = [](double v) {
      std::ostringstream oss;
      oss << std::setprecision(3) << std::fixed << v;
      return oss.str();
    };
    auto head_str = [](HeadPolicy p) {
      switch (p) {
        case HeadPolicy::TopNFrontier: return std::string("topn-frontier");
        case HeadPolicy::Frontier: return std::string("frontier");
        case HeadPolicy::Confirmed: return std::string("confirmed");
        case HeadPolicy::TopN: return std::string("topn");
        case HeadPolicy::Top2N: return std::string("top2n");
        case HeadPolicy::Threshold: return std::string("threshold");
      }
      return std::string("unknown");
    };
    for (std::size_t i = 0; i < methods.size(); ++i) {
      if (methods[i].name != "ss") continue;
      ss_idxs.push_back(i);
      std::vector<std::string> comps;
      comps.push_back(policy_str(methods[i].policy));
      comps.push_back(q_str(methods[i].q_factor));
      comps.push_back(std::string("eps=") + (methods[i].ss_per_item_eps ? "per-item" : "max-sketch"));
      comps.push_back(std::string("reducer=")
                      + (methods[i].reducer == ReducerMode::Streaming ? "streaming"
                         : methods[i].reducer == ReducerMode::ParallelStreaming
                             ? "parallel-streaming" : "hash"));
      switch (methods[i].policy) {
        case Policy::Difficulty:
          comps.push_back("alphaReq=" + dbl(methods[i].alpha_req));
          comps.push_back("epsilonM=" + dbl(methods[i].epsilon_m));
          comps.push_back(std::string("mode=") + (methods[i].diff_mode == DifficultyMode::Predictive ? "predictive"
                                                   : (methods[i].diff_mode == DifficultyMode::ReactiveReq ? "reactive-req" : "reactive-eff")));
          comps.push_back(std::string("amb=") + (methods[i].ambiguity_adjust ? "on" : "off"));
          break;
        case Policy::Static:
          break;
      }
      ss_components[i] = std::move(comps);
    }

    // Choose minimal prefix that distinguishes each SS config
    std::unordered_map<std::string, int> base_counts;
    for (auto idx : ss_idxs) {
      const auto& comps = ss_components[idx];
      std::string chosen = comps.empty() ? std::string{} : comps.front();
      for (std::size_t L = 1; L <= comps.size(); ++L) {
        std::ostringstream oss;
        for (std::size_t k = 0; k < L; ++k) {
          if (k) oss << ' ';
          oss << comps[k];
        }
        auto possible_label = oss.str();
        bool unique = true;
        for (auto other : ss_idxs) {
          if (other == idx) continue;
          const auto& ocomps = ss_components[other];
          if (ocomps.size() < L) continue;
          std::ostringstream oss2;
          for (std::size_t k = 0; k < L; ++k) {
            if (k) oss2 << ' ';
            oss2 << ocomps[k];
          }
          if (oss2.str() == possible_label) { unique = false; break; }
        }
        if (unique) { chosen = possible_label; break; }
        chosen = possible_label; // fallback to longest if none unique
      }
      auto base = std::string("ss[") + chosen + "]";
      int& cnt = base_counts[base];
      std::string final = base;
      if (cnt > 0) final += "#" + std::to_string(cnt);
      ++cnt;
      labels[idx] = std::move(final);
    }

    // Hybrid labels expose the promoted-head policy, because hybrid ablations
    // often run multiple head selectors in one benchmark invocation.
    std::unordered_map<std::string, int> hybrid_counts;
    for (std::size_t i = 0; i < methods.size(); ++i) {
      if (methods[i].name != "hybrid") continue;
      auto base = std::string("hybrid[head=") + head_str(methods[i].head_policy);
      if (methods[i].reducer == ReducerMode::Streaming) base += " reducer=streaming";
      if (methods[i].reducer == ReducerMode::ParallelStreaming) base += " reducer=parallel-streaming";
      if (methods[i].head_delta_eta <= 0.0) base += " head-update=full";
      base += "]";
      int& cnt = hybrid_counts[base];
      labels[i] = base;
      if (cnt > 0) labels[i] += "#" + std::to_string(cnt);
      ++cnt;
    }

    // Other methods keep original unique numbering
    std::unordered_map<std::string, int> label_count;
    for (std::size_t i = 0; i < methods.size(); ++i) {
      if (!labels[i].empty()) continue; // already set (ss)
      const std::string base =
          methods[i].name == "hl" && methods[i].hl_adaptive
          ? "hl[adaptive-certified]"
          : methods[i].name == "hl" && methods[i].hl_certify
              ? "hl[certified]"
          : methods[i].name;
      int cnt = label_count[base]++;
      labels[i] = (cnt == 0) ? base : (base + "#" + std::to_string(cnt));
    }

    for (const auto& lb : labels) {
      if (lb.size() > max_label_len) max_label_len = lb.size();
    }
  }



  // Build groups
  std::vector<MethodGroup> groups;
  std::vector<std::size_t> ss_q_caps(methods.size(), 0);
  std::vector<std::size_t> ss_q_curs(methods.size(), 0);
  std::vector<std::size_t> hl_width_curs(methods.size(), 0);
  std::vector<std::vector<std::size_t>> hl_width_ladders(methods.size());
  std::vector<std::shared_ptr<HeavyLockerRand>> hl_random_streams(methods.size());
  std::vector<std::size_t> hybrid_qe(methods.size(), 0), hybrid_qa(methods.size(), 0);
  std::vector<std::vector<Id128>> hybrid_installed_heads(methods.size());
  std::vector<std::vector<std::string>> hybrid_installed_head_keys(methods.size());
  std::vector<std::uint64_t> hybrid_head_generations(methods.size(), 0);
  std::vector<sizing::PolicyState> policy_states(methods.size());
  std::vector<DifficultyVerification> pending_verify(methods.size());
  constexpr std::size_t HYB_HEAD_SLOT_BYTES = 24;
  constexpr std::size_t HYB_TAIL_SLOT_BYTES = 32;
  auto hybrid_tail_cap_for_head = [&](std::size_t q_e) -> std::size_t {
    const std::size_t head_bytes = q_e * HYB_HEAD_SLOT_BYTES;
    if (head_bytes >= bytes_per_part) return 1;
    return std::max<std::size_t>(std::size_t{1}, (bytes_per_part - head_bytes) / HYB_TAIL_SLOT_BYTES);
  };
  groups.reserve(methods.size());
  for (std::size_t idx=0; idx<methods.size(); ++idx) {
    const auto& spec = methods[idx];
    MethodCfg build_spec = spec;
    if (spec.name == "hl") {
      hl_width_ladders[idx] = hl_width_ladder(A.n_param);
      if (spec.hl_w != 0) {
        hl_width_curs[idx] = spec.hl_w;
      } else if (spec.hl_adaptive) {
        hl_width_curs[idx] = hl_width_ladders[idx].front();
      } else {
        constexpr std::size_t slot_bytes =
            sizeof(Id128) + sizeof(std::uint32_t);
        const std::size_t depth = std::max<std::size_t>(1, spec.hl_d);
        hl_width_curs[idx] = std::max<std::size_t>(
            1, bytes_per_part / (depth * slot_bytes + 1));
      }
      build_spec.hl_w = hl_width_curs[idx];
      hl_random_streams[idx] = std::make_shared<HeavyLockerRand>(1);
    }
    auto g = make_method(
        build_spec, A.m, bytes_per_part, A.n_param,
        hl_random_streams[idx]);
    g.label = labels[idx];
    groups.push_back(std::move(g));
    if (spec.name == "ss") {
      const std::size_t cap = std::max<std::size_t>(std::size_t(1), bytes_per_part / 32);
      ss_q_caps[idx] = cap;
      ss_q_curs[idx] = std::min<std::size_t>(A.n_param * spec.q_factor, cap);
    } else if (spec.name == "hybrid") {
      hybrid_qe[idx] = 0;          // no prior head knowledge in the first window
      const std::size_t cap = hybrid_tail_cap_for_head(hybrid_qe[idx]);
      ss_q_caps[idx] = cap; // reuse cap helper
      if (spec.tail_policy == TailPolicy::Static) {
        hybrid_qa[idx] = A.n_param * std::max<std::size_t>(1, spec.tail_q_factor);
      } else {
        hybrid_qa[idx] = A.n_param;  // start tail at n
      }
      hybrid_qa[idx] = std::max<std::size_t>(A.n_param, std::min<std::size_t>(hybrid_qa[idx], cap));
      // Reconfigure the freshly-built hybrid sketches to enforce the intended q_e/q_a
      for (auto& p : groups.back().parts) {
        auto hy = std::make_unique<HybridSS>(hybrid_qe[idx], hybrid_qa[idx], spec.ss_per_item_eps);
        Part* part = &p;
        wire_key_lifecycle(*hy, *part);
        hy->reconfigure(hybrid_qe[idx], hybrid_qa[idx], A.n_param, /*head_mass_frac=*/0.0);
        p.sketch = std::move(hy);
      }
    }
  }

  // Must include oracle reference
  bool has_oracle = false;
  for (auto& g : groups) if (g.name == "oracle") { has_oracle = true; break; }
  if (!has_oracle) {
    std::cerr << "ERROR: methods must include 'oracle' for reference.\n";
    return 3;
  }


  // Per-method per-window metrics
  std::unordered_map<std::string, std::vector<WindowMetrics>> perwin;
  std::unordered_map<std::string, double> mem_algo_equiv_per_part_sum;
  std::unordered_map<std::string, double> mem_key_equiv_per_part_sum;
  std::unordered_map<std::string, double> mem_worker_equiv_per_part_sum;
  std::unordered_map<std::string, double> report_volume_flat_sum;
  std::unordered_map<std::string, double> control_volume_flat_sum;
  std::unordered_map<std::string, double> total_communication_flat_sum;
  std::unordered_map<std::string, double> key_request_volume_flat_sum;
  std::unordered_map<std::string, double> key_reply_volume_flat_sum;
  std::unordered_map<std::string, double> key_resolution_count_sum;
  std::unordered_map<std::string, double> mem_coord_ingress_flat_sum;
  std::unordered_map<std::string, double> mem_coord_work_flat_sum;
  std::unordered_map<std::string, double> mem_coord_control_peak_flat_sum;
  std::unordered_map<std::string, double> mem_coord_resolution_peak_flat_sum;
  std::unordered_map<std::string, double> mem_coord_peak_flat_sum;
  std::unordered_map<std::string, std::size_t> miss_req_cnt;
  std::unordered_map<std::string, std::size_t> miss_eff_cnt;
  std::unordered_map<std::string, std::size_t> miss_den_cnt;
  std::unordered_map<std::string, std::size_t> completeness_cert_cnt;
  std::unordered_map<std::string, std::size_t> completeness_pass_cnt;
  std::unordered_map<std::string, double> unseen_ratio_sum;
  std::vector<std::uint64_t> update_events_by_group(groups.size(), 0);
  std::vector<double> update_ns_by_group(groups.size(), 0.0);
  const int labelw = static_cast<int>(max_label_len);

  // Ingestion state
  std::size_t curr_win = 0; bool first = false;
  std::size_t windows_flushed = 0;

  auto flush = [&](std::size_t win_idx){
    // End-of-window routine: evaluate all methods, print metrics, decide next window sizes, then reset/reseed state.
    // reduce each method
    GlobalResultLB oracleR{};
    std::vector<GlobalResultLB> Rs(groups.size());
    std::vector<std::optional<HybridControlReduction>> hybrid_control(groups.size());
    std::vector<std::optional<HybridControlReduction>> ss_control(groups.size());
    std::vector<ReduceTelemetry> coord_mem_by_group(groups.size());
    std::vector<std::size_t> report_volume_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> control_volume_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> head_update_volume_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> key_request_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> key_reply_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> key_resolution_count_by_group(groups.size(), 0);
    std::vector<std::size_t> coord_ingress_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> coord_peak_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> coord_control_peak_bytes_by_group(groups.size(), 0);
    std::vector<std::size_t> coord_resolution_peak_bytes_by_group(groups.size(), 0);
    std::vector<double> report_prepare_ms_by_group(groups.size(), 0.0);
    std::vector<double> reduce_ms_by_group(groups.size(), 0.0);
    std::vector<double> control_ms_by_group(groups.size(), 0.0);
    std::vector<CsvRow> csv_rows(groups.size());

    // also keep snapshots for SS/hybrid policy (avoids recompute)
    std::vector<std::vector<SnapshotEx>> snaps_by_group(groups.size());

    for (std::size_t gi=0; gi<groups.size(); ++gi) {
      const auto& g = groups[gi];
      std::vector<SnapshotEx> snaps;
      std::vector<HLBucketSnapshot> snaps_hl;
      const bool all_hl_bucketed = (g.name == "hl");
      if (all_hl_bucketed) snaps_hl.resize(g.parts.size());
      else snaps.resize(g.parts.size());
      // Worker reports are independent. Report the distributed critical path
      // (the slowest worker), rather than the
      // sum induced by replaying m workers serially in this harness. Execution
      // remains serial here for deterministic benchmarking; only the reported
      // latency follows the distributed worker model.
      std::vector<double> worker_prepare_ms(g.parts.size(), 0.0);
      for (std::size_t pi = 0; pi < g.parts.size(); ++pi) {
        const auto worker_t0 = std::chrono::steady_clock::now();
        if (all_hl_bucketed) {
          auto* hl =
              dynamic_cast<const HeavyLocker*>(g.parts[pi].sketch.get());
          if (!hl) {
            throw std::runtime_error(
                "internal error: HeavyLocker method does not contain HeavyLocker sketches");
          }
          snaps_hl[pi] = hl->snapshot_bucketed();
        } else {
          snaps[pi] = g.parts[pi].sketch->snapshot_ex();
        }
        const auto worker_t1 = std::chrono::steady_clock::now();
        worker_prepare_ms[pi] = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                worker_t1 - worker_t0).count()) / 1.0e6;
      }
      report_prepare_ms_by_group[gi] = worker_prepare_ms.empty()
          ? 0.0
          : *std::max_element(
                worker_prepare_ms.begin(), worker_prepare_ms.end());
      snaps_by_group[gi] = std::move(snaps);

      GlobalResultLB R;
      if (all_hl_bucketed && snaps_hl.size() == g.parts.size()) {
        constexpr std::size_t kReportHeaderBytes = 32;
        constexpr std::size_t kHLCellBytes = sizeof(Id128) + sizeof(std::uint32_t);
        std::size_t report_bytes = snaps_hl.size() * kReportHeaderBytes;
        for (const auto& s : snaps_hl) {
          // HeavyLocker work() exports the dense bucket table. Bucket identity
          // is positional, so no per-cell bucket index is transmitted.
          report_bytes += s.w * s.d * kHLCellBytes;
          if (s.has_residual_certificate()) {
            report_bytes += s.w * s.d * sizeof(std::uint32_t)
                          + s.w * sizeof(std::uint64_t);
          }
        }
        report_volume_bytes_by_group[gi] = report_bytes;
        const auto reduce_t0 = std::chrono::steady_clock::now();
        R = Coordinator::reduce_hl_bucketwise(
            snaps_hl, A.n_param, &coord_mem_by_group[gi]);
        const auto reduce_t1 = std::chrono::steady_clock::now();
        reduce_ms_by_group[gi] =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                reduce_t1 - reduce_t0).count()) / 1.0e6;
      } else {
        const auto& sx = snaps_by_group[gi];
        constexpr std::size_t kReportHeaderBytes = 32;
        constexpr std::size_t kCandidateBytes = sizeof(Id128) + sizeof(std::uint32_t);
        // The lower bound is derived as estimate - epsilon, so transmitting it
        // in addition to epsilon would charge a redundant field.
        constexpr std::size_t kErrorBytes = sizeof(std::uint32_t);
        constexpr std::size_t kHybridHeadRecordBytes =
            sizeof(std::uint32_t) + sizeof(std::uint32_t);
        std::size_t report_bytes = 0;
        for (const auto& s : sx) {
          std::size_t worker_report_bytes = kReportHeaderBytes;
          if (g.name == "hybrid" &&
              methods[gi].reducer != ReducerMode::Hash) {
            // Hybrid exact-head membership is shared state between workers and
            // the coordinator. Head entries therefore need only transmit a
            // compact slot identifier plus the exact count; full Id128 keys are
            // still used internally by this benchmark to execute the merge.
            const std::size_t head_records = std::min(s.head_size, s.candidates.size());
            const std::size_t tail_records = s.candidates.size() - head_records;
            worker_report_bytes += head_records * kHybridHeadRecordBytes
                                 + tail_records * kCandidateBytes;
            if (s.has_error_bounds()) {
              worker_report_bytes += tail_records * kErrorBytes;
            } else if (s.has_sketch_error_bound()) {
              worker_report_bytes += kErrorBytes;
            }
          } else {
            worker_report_bytes += s.candidates.size() * kCandidateBytes;
            if (s.has_error_bounds()) {
              worker_report_bytes += s.candidates.size() * kErrorBytes;
            } else if (s.has_sketch_error_bound()) {
              worker_report_bytes += kErrorBytes;
            }
          }
          report_bytes += worker_report_bytes;
        }
        report_volume_bytes_by_group[gi] = report_bytes;
        const auto reduce_t0 = std::chrono::steady_clock::now();
        // Hybrid and adaptive SS use one coordinated pass for both their
        // published result and controller telemetry. Static SS retains the
        // selected diagnostic reducer because it has no controller projection.
        const bool streaming_reducer =
            g.name == "hybrid"
            || (g.name == "ss" && methods[gi].reducer != ReducerMode::Hash);
        if (g.name == "hybrid") {
          const std::size_t top_limit =
              methods[gi].head_policy == HeadPolicy::Top2N
              ? 2 * A.n_param
              : A.n_param;
          auto coordinated = Coordinator::reduce_hybrid_streaming_for_control(
              snaps_by_group[gi], A.n_param, top_limit,
              hybrid_installed_heads[gi],
              methods[gi].reducer == ReducerMode::ParallelStreaming
                  ? A.reducer_workers
                  : 1);
          coord_mem_by_group[gi] = coordinated.telemetry;
          R = std::move(coordinated.published);
          hybrid_control[gi] = std::move(coordinated);
        } else if (g.name == "ss"
                   && methods[gi].policy == Policy::Difficulty) {
          auto coordinated = Coordinator::reduce_ss_streaming_for_control(
              snaps_by_group[gi], A.n_param,
              methods[gi].reducer == ReducerMode::ParallelStreaming
                  ? A.reducer_workers
                  : 1);
          coord_mem_by_group[gi] = coordinated.telemetry;
          R = std::move(coordinated.published);
          ss_control[gi] = std::move(coordinated);
        } else if (streaming_reducer) {
          if (methods[gi].reducer == ReducerMode::ParallelStreaming) {
            R = Coordinator::reduce_global_streaming_with_lb(
                snaps_by_group[gi], A.n_param, &coord_mem_by_group[gi],
                true, A.reducer_workers);
          } else {
            R = Coordinator::reduce_global_streaming_with_lb(
                snaps_by_group[gi], A.n_param, &coord_mem_by_group[gi]);
          }
        } else {
          R = Coordinator::reduce_global_with_lb(
              snaps_by_group[gi], A.n_param, &coord_mem_by_group[gi]);
        }
        const auto reduce_t1 = std::chrono::steady_clock::now();
        reduce_ms_by_group[gi] =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                reduce_t1 - reduce_t0).count()) / 1.0e6;
      }
      coord_ingress_bytes_by_group[gi] = coord_mem_by_group[gi].ingress_bytes;
      coord_peak_bytes_by_group[gi] =
          coord_ingress_bytes_by_group[gi] + coord_mem_by_group[gi].total_peak_bytes;
      if (g.name == "oracle") oracleR = R;
      Rs[gi] = std::move(R);
    }

    // Resolve only identifiers returned by the HH query. Internal candidate
    // and certificate frontiers remain identifier-only. A real deployment
    // batches requests by the selected reporting worker; this harness performs
    // the same lookup locally and accounts the request/reply frames below.
    std::vector<std::unique_ptr<KeyResolutionModel>> key_resolution(groups.size());
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (groups[gi].name == "oracle") continue;
      auto model = std::make_unique<KeyResolutionModel>(groups[gi].parts.size());
      if (groups[gi].name == "hybrid") {
        model->seed_known(
            hybrid_installed_heads[gi],
            hybrid_installed_head_keys[gi]);
      }
      for (auto& item : Rs[gi].items) {
        if (item.est >= Rs[gi].threshold) {
          item.key = model->resolve_output(
              groups[gi], item.id, item.resolution_worker);
        }
      }
      key_resolution[gi] = std::move(model);
    }

    // Evaluate vs oracle and print window header (print q for SS)
    std::cout << "Window " << win_idx << ":\n";
    auto sketch_mem_kib_for_group = [&](const MethodGroup& g) -> double {
      if (g.parts.empty()) return 0.0;
      double sum_bytes = 0.0;
      std::size_t cnt = 0;
      for (const auto& p : g.parts) {
        if (!p.sketch) continue;
        sum_bytes += static_cast<double>(p.sketch->memory_bytes());
        ++cnt;
      }
      if (cnt == 0) return 0.0;
      return (sum_bytes / static_cast<double>(cnt)) / 1024.0;
    };
    auto binding_mem_kib_for_group = [&](const MethodGroup& g) -> double {
      if (g.parts.empty()) return 0.0;
      double sum_bytes = 0.0;
      std::size_t cnt = 0;
      for (const auto& p : g.parts) {
        sum_bytes += static_cast<double>(p.amap.memory_bytes());
        ++cnt;
      }
      if (cnt == 0) return 0.0;
      return (sum_bytes / static_cast<double>(cnt)) / 1024.0;
    };

    // The first processed window is a warmup/adaptation seed. Do not infer this
    // from the numeric window ID, because input IDs are labels and may not start
    // at zero for externally supplied traces.
    const bool include_in_summary = (windows_flushed > 0);
    for (std::size_t gi=0; gi<Rs.size(); ++gi) {
      if (groups[gi].name == "oracle") continue;
      // if (groups[gi].name == "hybrid") {
      //   // One-time debug: compare hybrid vs oracle HH counts for the first window
      //   static bool dbg_hybrid_checked = false;
      //   if (!dbg_hybrid_checked) {
      //     dbg_hybrid_checked = true;
      //     std::unordered_map<Id128, std::uint64_t, Id128Hash> hyb_map;
      //     for (const auto& it : Rs[gi].items) hyb_map[it.id] = it.est;

      //     bool any = false;
      //     for (const auto& it : oracleR.items) {
      //       auto hm = hyb_map.find(it.id);
      //       if (hm == hyb_map.end() || hm->second != it.est) {
      //         if (!any) std::cerr << "[hybrid dbg] window " << win_idx << " HH diffs vs oracle:\n";
      //         any = true;
      //         std::cerr << "  id=" << Coordinator::id128_hex(it.id)
      //                   << " oracle=" << it.est
      //                   << " hybrid=" << (hm == hyb_map.end() ? 0 : hm->second) << "\n";
      //       }
      //     }
      //     if (!any) std::cerr << "[hybrid dbg] window " << win_idx << " hybrid matches oracle for all HH.\n";
      //   }
      // }
      auto wm = eval_vs_oracle(oracleR, Rs[gi], A.topk);
      if (include_in_summary) {
        perwin[groups[gi].label].push_back(wm);
        if (Rs[gi].has_completeness_certificate) {
          ++completeness_cert_cnt[groups[gi].label];
          if (Rs[gi].candidate_set_complete) {
            ++completeness_pass_cnt[groups[gi].label];
          }
          if (Rs[gi].threshold > 0) {
            unseen_ratio_sum[groups[gi].label] +=
                static_cast<double>(Rs[gi].unseen_mass_ub)
                / static_cast<double>(Rs[gi].threshold);
          }
        }
      }
      std::cout << "  [" << std::left << std::setw(labelw) << groups[gi].label << "] " << std::right
                << "HH precision=" << std::fixed << std::setprecision(3)
                << wm.hh_precision << " recall=" << wm.hh_recall
                << " F1=" << wm.hh_f1
                << " AAE=" << std::setprecision(3) << wm.aae
                << " ARE=" << std::setprecision(3) << (wm.are * 100.0) << "%";
      if (groups[gi].name == "ss") std::cout << " q=" << ss_q_curs[gi];
      if (groups[gi].name == "hybrid") std::cout << " head=" << hybrid_qe[gi] << " tail=" << hybrid_qa[gi];
      if (groups[gi].name == "hl") std::cout << " w=" << hl_width_curs[gi];
      const double sketch_mem_kib_win = sketch_mem_kib_for_group(groups[gi]);
      const double binding_mem_kib_win = binding_mem_kib_for_group(groups[gi]);
      const double algo_equiv_kib_win = sketch_mem_kib_win;
      const double key_equiv_kib_win = binding_mem_kib_win;
      const double worker_equiv_kib_win = algo_equiv_kib_win + key_equiv_kib_win;
      const double report_volume_flat_kib_win =
          static_cast<double>(report_volume_bytes_by_group[gi]) / 1024.0;
      const double coord_ingress_flat_kib_win =
          static_cast<double>(coord_ingress_bytes_by_group[gi]) / 1024.0;
      const double coord_work_flat_kib_win =
          static_cast<double>(coord_mem_by_group[gi].total_peak_bytes) / 1024.0;
      const double coord_peak_flat_kib_win =
          static_cast<double>(coord_peak_bytes_by_group[gi]) / 1024.0;
      std::cout << "\tmem(wAlgo/wKey/wTotal)≈"
                << std::fixed << std::setprecision(2)
                << algo_equiv_kib_win << "/" << key_equiv_kib_win << "/" << worker_equiv_kib_win
                << " KiB"
                << "\ttelemetryUp≈" << report_volume_flat_kib_win << " KiB"
                << "\tmem(cIngress/cReducePeak)≈"
                << coord_ingress_flat_kib_win << "/" << coord_peak_flat_kib_win
                << " KiB";
      if (wm.topk_overlap) {
        std::cout << " topk_overlap=" << std::setprecision(3) << *wm.topk_overlap;
      }
      if (Rs[gi].has_completeness_certificate) {
        const double ratio = Rs[gi].threshold == 0
            ? 0.0
            : static_cast<double>(Rs[gi].unseen_mass_ub)
                / static_cast<double>(Rs[gi].threshold);
        std::cout << " complete="
                  << (Rs[gi].candidate_set_complete ? "yes" : "no")
                  << " unseenUB/T=" << std::setprecision(3) << ratio;
      }
      std::cout << "\n";
      if (!A.csv_out.empty()) {
        auto& row = csv_rows[gi];
        row.window = win_idx;
        row.method = groups[gi].label;
        row.method_type = groups[gi].name;
        row.N_global = Rs[gi].N_global;
        row.threshold = Rs[gi].threshold;
        row.hh_precision = wm.hh_precision;
        row.hh_recall = wm.hh_recall;
        row.hh_f1 = wm.hh_f1;
        row.aae = wm.aae;
        row.are = wm.are;
        if (wm.topk_overlap) {
          row.has_topk = true;
          row.topk_overlap = *wm.topk_overlap;
        }
        if (groups[gi].name == "ss") row.q_current = ss_q_curs[gi];
        else if (groups[gi].name == "hybrid") {
          row.q_current = hybrid_qe[gi] + hybrid_qa[gi];
          row.q_head_current = hybrid_qe[gi];
          row.q_tail_current = hybrid_qa[gi];
        } else if (groups[gi].name == "hl") {
          // q_current/q_next are generic capacity columns. For HeavyLocker
          // they carry bucket width w; method_type disambiguates the unit.
          row.q_current = hl_width_curs[gi];
        }
        row.cert = certification_metrics(oracleR, Rs[gi]);
        if (Rs[gi].has_completeness_certificate) {
          row.completeness_certified =
              Rs[gi].candidate_set_complete ? 1 : 0;
          row.unseen_mass_ub = Rs[gi].unseen_mass_ub;
          row.unseen_ub_over_threshold = Rs[gi].threshold == 0
              ? 0.0
              : static_cast<double>(Rs[gi].unseen_mass_ub)
                  / static_cast<double>(Rs[gi].threshold);
        }
        row.mem_worker_algo_kib = algo_equiv_kib_win;
        row.mem_worker_key_kib = key_equiv_kib_win;
        row.mem_worker_total_kib = worker_equiv_kib_win;
        row.report_volume_kib = report_volume_flat_kib_win;
        row.mem_coord_ingress_kib = coord_ingress_flat_kib_win;
        row.mem_coord_work_kib = coord_work_flat_kib_win;
        row.mem_coord_peak_kib = coord_peak_flat_kib_win;
        row.update_events = update_events_by_group[gi];
        row.update_ms = update_ns_by_group[gi] / 1.0e6;
        row.update_mops = row.update_ms > 0.0
            ? (static_cast<double>(row.update_events) / (row.update_ms / 1000.0)) / 1.0e6
            : 0.0;
        row.report_prepare_ms = report_prepare_ms_by_group[gi];
        row.reduce_ms = reduce_ms_by_group[gi];
      }
      // accumulate memory metrics per partition for averaging
      if (include_in_summary) {
        mem_algo_equiv_per_part_sum[groups[gi].label] += algo_equiv_kib_win;
        mem_key_equiv_per_part_sum[groups[gi].label] += key_equiv_kib_win;
        mem_worker_equiv_per_part_sum[groups[gi].label] += worker_equiv_kib_win;
        report_volume_flat_sum[groups[gi].label] += report_volume_flat_kib_win;
      }
    }

    // Precompute hybrid seed + head mass fraction (min across partitions) and sizing rule for each hybrid
    std::vector<Id128> hybrid_seed_ids;
    std::vector<std::string> hybrid_seed_keys;
    for (std::size_t gi=0; gi<groups.size(); ++gi) {
      if (groups[gi].name != "hybrid") continue;
      const auto control_t0 = std::chrono::steady_clock::now();
      double hybrid_head_frac_min = 0.0;
      const auto& snaps_for_hybrid = snaps_by_group[gi];
      if (!snaps_for_hybrid.empty()) {
        double min_frac = 1.0;
        for (const auto& s : snaps_for_hybrid) {
          if (s.N_local == 0) { min_frac = 0.0; break; }
          const double frac = static_cast<double>(s.head_mass) / static_cast<double>(s.N_local);
          min_frac = std::min(min_frac, frac);
        }
        hybrid_head_frac_min = min_frac;
      }

      const auto& hyb_cfg = methods[gi];
      // Head seed from previous-window telemetry.
      hybrid_seed_ids.clear();
      if (!hybrid_control[gi]) {
        // Preserve the hash reducer as a diagnostic/output option. Its
        // controller still uses the coordinated streaming pass so the policy
        // semantics remain identical across reducer implementations.
        const std::size_t top_limit =
            hyb_cfg.head_policy == HeadPolicy::Top2N
            ? 2 * A.n_param
            : A.n_param;
        hybrid_control[gi] = Coordinator::reduce_hybrid_streaming_for_control(
            snaps_for_hybrid, A.n_param, top_limit,
            hybrid_installed_heads[gi]);
      }
      auto& coordinated = *hybrid_control[gi];
      const std::size_t topk =
          std::min<std::size_t>(A.n_param, coordinated.top_ids.size());
      const std::size_t top2k =
          std::min<std::size_t>(2 * A.n_param, coordinated.top_ids.size());
      std::vector<Id128> top_members(
          coordinated.top_ids.begin(), coordinated.top_ids.begin() + topk);
      std::sort(top_members.begin(), top_members.end(), [](const Id128& a, const Id128& b) {
        return a.b < b.b;
      });
      const auto in_top = [&](const Id128& id) {
        return std::binary_search(
            top_members.begin(), top_members.end(), id,
            [](const Id128& a, const Id128& b) { return a.b < b.b; });
      };
      if (hyb_cfg.head_policy == HeadPolicy::TopN) {
        hybrid_seed_ids.insert(
            hybrid_seed_ids.end(), coordinated.top_ids.begin(),
            coordinated.top_ids.begin() + topk);
      } else if (hyb_cfg.head_policy == HeadPolicy::Top2N) {
        hybrid_seed_ids.insert(
            hybrid_seed_ids.end(), coordinated.top_ids.begin(),
            coordinated.top_ids.begin() + top2k);
      } else {
        if (hyb_cfg.head_policy == HeadPolicy::TopNFrontier) {
          hybrid_seed_ids.insert(
              hybrid_seed_ids.end(), coordinated.top_ids.begin(),
              coordinated.top_ids.begin() + topk);
        }
        for (const auto& item : Rs[gi].items) {
          const bool confirmed = item.cert_lb >= Rs[gi].threshold;
          if (hyb_cfg.head_policy == HeadPolicy::Frontier ||
              hyb_cfg.head_policy == HeadPolicy::TopNFrontier ||
              (hyb_cfg.head_policy == HeadPolicy::Confirmed && confirmed) ||
              (hyb_cfg.head_policy == HeadPolicy::Threshold && !in_top(item.id))) {
            hybrid_seed_ids.push_back(item.id);
          }
        }
      }
      // ExactHead installs a sorted unique dictionary. Canonicalize here so
      // capacity, worker memory, CSV telemetry, and control traffic all use
      // the exact number of entries that will actually be deployed.
      std::sort(hybrid_seed_ids.begin(), hybrid_seed_ids.end(), [](const Id128& a, const Id128& b) {
        return a.b < b.b;
      });
      hybrid_seed_ids.erase(
          std::unique(hybrid_seed_ids.begin(), hybrid_seed_ids.end()),
          hybrid_seed_ids.end());

      // Build a generation-checked transition from the installed dictionary
      // to the next promoted head.
      std::size_t head_overlap_count = 0;
      ExactHeadDelta head_delta;
      head_delta.base_generation = hybrid_head_generations[gi];
      head_delta.next_generation = head_delta.base_generation + 1;
      {
        std::size_t current = 0;
        std::size_t next = 0;
        const auto& installed = hybrid_installed_heads[gi];
        while (current < installed.size() && next < hybrid_seed_ids.size()) {
          if (installed[current] == hybrid_seed_ids[next]) {
            ++head_overlap_count;
            ++current;
            ++next;
          } else if (installed[current].b < hybrid_seed_ids[next].b) {
            head_delta.removed_slots.push_back(
                static_cast<std::uint32_t>(current));
            ++current;
          } else {
            head_delta.added_ids.push_back(hybrid_seed_ids[next]);
            ++next;
          }
        }
        while (current < installed.size()) {
          head_delta.removed_slots.push_back(
              static_cast<std::uint32_t>(current++));
        }
        while (next < hybrid_seed_ids.size()) {
          head_delta.added_ids.push_back(hybrid_seed_ids[next++]);
        }
      }
      const std::size_t head_added_count =
          hybrid_seed_ids.size() - head_overlap_count;
      const std::size_t head_removed_count =
          hybrid_installed_heads[gi].size() - head_overlap_count;
      // Modeled wire format: generation metadata, framing/check fields, and
      // two 32-bit entry counts. Full and delta updates share this header.
      constexpr std::size_t kHeadUpdateHeaderBytes =
          32 + 2 * sizeof(std::uint32_t);
      constexpr std::size_t kHeadRemovalSlotBytes = sizeof(std::uint32_t);
      const std::size_t head_full_update_bytes =
          kHeadUpdateHeaderBytes
          + hybrid_seed_ids.size() * sizeof(Id128);
      const std::size_t head_delta_update_bytes =
          kHeadUpdateHeaderBytes
          + head_removed_count * kHeadRemovalSlotBytes
          + head_added_count * sizeof(Id128);
      const double head_delta_max_ratio = methods[gi].head_delta_eta;
      const std::uint64_t head_checkpoint_interval =
          methods[gi].head_checkpoint_interval;
      const double head_delta_ratio =
          head_full_update_bytes == 0
          ? 1.0
          : static_cast<double>(head_delta_update_bytes)
              / static_cast<double>(head_full_update_bytes);
      const bool head_checkpoint =
          head_delta.base_generation == 0
          || (head_delta.next_generation % head_checkpoint_interval) == 0;
      const bool head_delta_selected =
          !head_checkpoint && head_delta_ratio <= head_delta_max_ratio;
      if (!A.csv_out.empty()) {
        csv_rows[gi].head_overlap_count = head_overlap_count;
        csv_rows[gi].head_added_count = head_added_count;
        csv_rows[gi].head_removed_count = head_removed_count;
        csv_rows[gi].head_full_update_bytes = head_full_update_bytes;
        csv_rows[gi].head_delta_update_bytes = head_delta_update_bytes;
        csv_rows[gi].head_delta_ratio = head_delta_ratio;
        csv_rows[gi].head_delta_selected = head_delta_selected ? 1 : 0;
      }

      struct ResolutionRoute {
        Id128 id;
        std::uint32_t worker{kNoResolutionWorker};
      };
      std::vector<ResolutionRoute> resolution_routes;
      resolution_routes.reserve(
          coordinated.top_ids.size() + Rs[gi].items.size());
      if (coordinated.top_ids.size()
          != coordinated.top_resolution_workers.size()) {
        throw std::runtime_error(
            "hybrid top identifiers and resolution routes are misaligned");
      }
      for (std::size_t i = 0; i < coordinated.top_ids.size(); ++i) {
        resolution_routes.push_back(ResolutionRoute{
            coordinated.top_ids[i], coordinated.top_resolution_workers[i]});
      }
      for (const auto& item : Rs[gi].items) {
        resolution_routes.push_back(
            ResolutionRoute{item.id, item.resolution_worker});
      }
      std::sort(
          resolution_routes.begin(), resolution_routes.end(),
          [](const ResolutionRoute& lhs, const ResolutionRoute& rhs) {
            if (lhs.id.b != rhs.id.b) return lhs.id.b < rhs.id.b;
            return lhs.worker < rhs.worker;
          });
      resolution_routes.erase(
          std::unique(
              resolution_routes.begin(), resolution_routes.end(),
              [](const ResolutionRoute& lhs, const ResolutionRoute& rhs) {
                return lhs.id == rhs.id;
              }),
          resolution_routes.end());
      const auto resolution_worker_for = [&](const Id128& id) {
        const auto it = std::lower_bound(
            resolution_routes.begin(), resolution_routes.end(), id,
            [](const ResolutionRoute& route, const Id128& value) {
              return route.id.b < value.b;
            });
        if (it == resolution_routes.end() || !(it->id == id)) {
          throw std::runtime_error(
              "new Hybrid head identifier has no reporting-worker route");
        }
        return it->worker;
      };

      hybrid_seed_keys.clear();
      hybrid_seed_keys.reserve(hybrid_seed_ids.size());
      const auto resolve_promoted_key = [&](const Id128& id) {
        if (!key_resolution[gi]) {
          throw std::runtime_error(
              "hybrid key-resolution state was not initialized");
        }
        return key_resolution[gi]->resolve(
            groups[gi], id, resolution_worker_for(id));
      };
      std::size_t installed_slot = 0;
      for (const auto& id : hybrid_seed_ids) {
        while (installed_slot < hybrid_installed_heads[gi].size()
               && hybrid_installed_heads[gi][installed_slot].b < id.b) {
          ++installed_slot;
        }
        if (installed_slot < hybrid_installed_heads[gi].size()
            && hybrid_installed_heads[gi][installed_slot] == id) {
          hybrid_seed_keys.push_back(
              std::move(hybrid_installed_head_keys[gi][installed_slot]));
        } else {
          hybrid_seed_keys.push_back(resolve_promoted_key(id));
        }
      }
      const auto dictionary_storage_bytes = [](
          const std::vector<Id128>& ids,
          const std::vector<std::string>& keys) {
        std::size_t bytes = sizeof(ids) + ids.capacity() * sizeof(Id128)
            + sizeof(keys) + keys.capacity() * sizeof(std::string);
        for (const auto& key : keys) bytes += key.capacity();
        return bytes;
      };
      const auto delta_storage_bytes = [&]() {
        return sizeof(head_delta)
            + head_delta.removed_slots.capacity() * sizeof(std::uint32_t)
            + head_delta.added_ids.capacity() * sizeof(Id128);
      };
      const std::size_t common_control_bytes =
          logical_result_bytes(Rs[gi])
          + sizeof(coordinated.residual_items)
          + coordinated.residual_items.capacity() * sizeof(HybridSizingItem)
          + sizeof(coordinated.top_ids)
          + coordinated.top_ids.capacity() * sizeof(Id128)
          + sizeof(coordinated.top_resolution_workers)
          + coordinated.top_resolution_workers.capacity()
                * sizeof(std::uint32_t)
          + sizeof(resolution_routes)
          + resolution_routes.capacity()
                * sizeof(ResolutionRoute);
      // Both dictionary containers and the delta coexist while the transition
      // is assembled. Overlapping raw strings are moved, but all live vector
      // allocations still contribute to the transient coordinator peak.
      coord_control_peak_bytes_by_group[gi] = std::max(
          coord_control_peak_bytes_by_group[gi],
          common_control_bytes
              + dictionary_storage_bytes(
                    hybrid_installed_heads[gi], hybrid_installed_head_keys[gi])
              + dictionary_storage_bytes(hybrid_seed_ids, hybrid_seed_keys)
              + delta_storage_bytes());

      // The transition frame now contains every reference to the old slots
      // needed by workers. Release the superseded coordinator dictionary;
      // overlapping raw-key strings were moved into the next dictionary.
      std::vector<Id128>().swap(hybrid_installed_heads[gi]);
      std::vector<std::string>().swap(hybrid_installed_head_keys[gi]);

      // The coordinated reducer has already filtered the current exact-head
      // prefix while consuming each merged key. Residual items are precisely
      // K_+ U K_?, with exact inflation and hidden-mass components attached.
      const auto& residual_items = coordinated.residual_items;
      const std::size_t retained_control_bytes =
          common_control_bytes
          + dictionary_storage_bytes(
                hybrid_installed_heads[gi], hybrid_installed_head_keys[gi])
          + dictionary_storage_bytes(hybrid_seed_ids, hybrid_seed_keys)
          + delta_storage_bytes();
      coord_control_peak_bytes_by_group[gi] = std::max(
          coord_control_peak_bytes_by_group[gi], retained_control_bytes);

      hybrid_qe[gi] = hybrid_seed_ids.size(); // q_e = |E^{t+1}|
      const std::size_t tail_cap = hybrid_tail_cap_for_head(hybrid_qe[gi]);
      // Tail sizing: policy with head coverage deduction
      const double P_E_min = std::clamp(hybrid_head_frac_min, 0.0, 1.0); // min head mass fraction across partitions
      const std::size_t tail_floor = std::max<std::size_t>(
          std::size_t{1},
          static_cast<std::size_t>(std::ceil(static_cast<double>(A.n_param) * (1.0 - P_E_min))));
      if (hyb_cfg.tail_policy == TailPolicy::Static) {
        // Static: keep tail fixed to n_param (clamped to memory cap) instead of adapting with head coverage
        const std::size_t q_static = A.n_param * std::max<std::size_t>(1, hyb_cfg.tail_q_factor);
        hybrid_qa[gi] = std::max<std::size_t>(tail_floor, std::min<std::size_t>(q_static, tail_cap));
      } else {
        sizing::PolicyConfig cfg_h;
        cfg_h.n_param = A.n_param;
        cfg_h.r = hyb_cfg.r;
        cfg_h.alpha_req = hyb_cfg.alpha_req;
        cfg_h.delta_m = hyb_cfg.delta_m;
        cfg_h.probe_evidence_window = hyb_cfg.probe_evidence_window;
        cfg_h.ambiguity_decay = hyb_cfg.ambiguity_decay;
        cfg_h.symmetric_relaxation = hyb_cfg.symmetric_relaxation;
        cfg_h.censored_control = hyb_cfg.censored_control;
        cfg_h.probe_residual_guard = hyb_cfg.probe_residual_guard;
        cfg_h.probe_strategy = hyb_cfg.probe_strategy;
        cfg_h.probe_pressure_gate = hyb_cfg.probe_pressure_gate;
        cfg_h.ambiguity_adjust = hyb_cfg.ambiguity_adjust;
        cfg_h.epsilon_m = hyb_cfg.epsilon_m;
        cfg_h.q_cur = hybrid_qa[gi];
        cfg_h.q_min = tail_floor;
        cfg_h.q_cap = tail_cap;
        cfg_h.q_max = tail_cap;
        coord_control_peak_bytes_by_group[gi] = std::max(
            coord_control_peak_bytes_by_group[gi],
            retained_control_bytes + sizeof(cfg_h) + sizeof(policy_states[gi]));
        switch (hyb_cfg.tail_policy) {
          case TailPolicy::Difficulty: cfg_h.kind = sizing::PolicyKind::difficulty; break;
          case TailPolicy::Static: cfg_h.kind = sizing::PolicyKind::fixed; break;
        }
        // Certificate components were attached during the coordinated merge,
        // so sizing does not need copied residual snapshots, full result
        // entries, or per-worker incidence maps.
        auto res_h = sizing::next_q_ss(
            residual_items, Rs[gi].N_global, Rs[gi].threshold,
            {}, cfg_h, &policy_states[gi]);
        const std::size_t q_req_resid = std::max<std::size_t>(
            tail_floor,
            res_h.q_req);
        const std::size_t q_eff_resid = std::max<std::size_t>(
            tail_floor,
            res_h.q_base);
        const std::size_t q_req_fair = std::min(q_req_resid, tail_cap);
        const std::size_t q_eff_fair = std::min(q_eff_resid, tail_cap);
        if (include_in_summary) {
          miss_den_cnt[groups[gi].label] += 1;
          if (hybrid_qa[gi] < q_req_fair) miss_req_cnt[groups[gi].label] += 1;
          if (hybrid_qa[gi] < q_eff_fair) miss_eff_cnt[groups[gi].label] += 1;
        }
        if (hyb_cfg.tail_policy == TailPolicy::Difficulty) {
          std::size_t q_next_mode = res_h.q_next;
          if (hyb_cfg.diff_mode == DifficultyMode::ReactiveReq) q_next_mode = std::max<std::size_t>(cfg_h.q_min, res_h.q_req);
          else if (hyb_cfg.diff_mode == DifficultyMode::ReactiveEff) q_next_mode = std::max<std::size_t>(cfg_h.q_min, res_h.q_base);
          if (cfg_h.q_cap != std::size_t(-1)) q_next_mode = std::min(q_next_mode, cfg_h.q_cap);
          q_next_mode = std::max(q_next_mode, cfg_h.q_min);
          if (cfg_h.q_max != std::size_t(-1)) q_next_mode = std::min(q_next_mode, cfg_h.q_max);
          res_h.q_next = q_next_mode;
        }
        if (hyb_cfg.tail_policy == TailPolicy::Difficulty && pending_verify[gi].valid) {
          const std::size_t q_req_resid = std::max<std::size_t>(
              tail_floor,
              res_h.q_req);
          const std::size_t q_eff_resid = std::max<std::size_t>(
              tail_floor,
              res_h.q_base);
          const std::size_t q_pred_eff_resid = std::max<std::size_t>(
              tail_floor,
              static_cast<std::size_t>(std::llround(pending_verify[gi].tilde_qpred)));
          const bool service_ok = (q_req_resid <= pending_verify[gi].q_planned);
          const bool control_ok = (q_eff_resid <= pending_verify[gi].q_planned);
          const bool calib_eff_ok = (q_eff_resid <= q_pred_eff_resid);
          std::cout << "  [" << std::left << std::setw(labelw) << groups[gi].label << "] " << std::right
                    << "verify: qBaseline_resid=" << q_req_resid
                    << " qActuation_resid=" << q_eff_resid
                    << " | pred(qActuation_resid<=" << q_pred_eff_resid
                    << ", q=" << pending_verify[gi].q_planned << ")"
                    << " | service=" << (service_ok ? "OK" : "FAIL")
                    << " control=" << (control_ok ? "OK" : "FAIL")
                    << " calibEff=" << (calib_eff_ok ? "OK" : "FAIL") << "\n";
        }
        if (hyb_cfg.tail_policy == TailPolicy::Difficulty) {
          pending_verify[gi].valid = true;
          pending_verify[gi].tilde_qpred = res_h.q_pred_tilde;
        }
        if (!A.csv_out.empty()) {
          auto& row = csv_rows[gi];
          row.q_req = q_req_resid;
          row.q_req_unclipped = res_h.q_req_unclipped;
          row.margin_alpha = res_h.margin_alpha;
          row.service_violation = res_h.service_violation ? 1 : 0;
          row.q_up = res_h.q_up;
          row.q_baseline = res_h.q_baseline;
          row.probe_issued = res_h.probe_issued ? 1 : 0;
          row.probe_failed = res_h.probe_failed ? 1 : 0;
          row.q_eff_replay = q_eff_resid;
          row.q_eff_pred = res_h.q_pred;
          row.q_eff_pred_tilde = res_h.q_pred_tilde;
          row.miss_req = hybrid_qa[gi] < q_req_fair ? 1 : 0;
          row.miss_eff = hybrid_qa[gi] < q_eff_fair ? 1 : 0;
          row.over_req = q_req_resid == 0 ? NAN : static_cast<double>(hybrid_qa[gi]) / static_cast<double>(q_req_resid);
          row.over_eff = q_eff_resid == 0 ? NAN : static_cast<double>(hybrid_qa[gi]) / static_cast<double>(q_eff_resid);
          row.calib_bias = res_h.b_q;
        }
        // Tail controller already operates on residual telemetry; deploy q_next directly.
        hybrid_qa[gi] = std::max<std::size_t>(tail_floor, std::min<std::size_t>(res_h.q_next, tail_cap));
        if (hyb_cfg.tail_policy == TailPolicy::Difficulty) {
          // q_planned must match the actually deployed residual tail size.
          pending_verify[gi].q_planned = hybrid_qa[gi];
        }
        if (hyb_cfg.tail_policy == TailPolicy::Difficulty) {
          std::cout << "  [" << std::left << std::setw(labelw) << groups[gi].label << "] "
                    << std::right << "hyb_tail difficulty:"
                    << " mode=" << (hyb_cfg.diff_mode == DifficultyMode::Predictive ? "predictive"
                                    : (hyb_cfg.diff_mode == DifficultyMode::ReactiveReq ? "reactive-req" : "reactive-eff"))
                    << " marginAlpha=" << std::setprecision(6) << res_h.margin_alpha
                    << " violation=" << (res_h.service_violation ? "yes" : "no")
                    << " qUp=" << res_h.q_up
                    << " qBaseline=" << res_h.q_baseline
                    << " qActuation=" << res_h.q_base
                    << " probe=" << (res_h.probe_issued ? "issued"
                                     : (res_h.probe_failed ? "failed" : "none"))
                    << "\n";
        }
      }
      if (!A.csv_out.empty()) {
        csv_rows[gi].q_next = hybrid_qe[gi] + hybrid_qa[gi];
        csv_rows[gi].q_head_next = hybrid_qe[gi];
        csv_rows[gi].q_tail_next = hybrid_qa[gi];
      }
      // The exact-head slot encoding is valid only after every worker receives
      // the same ordered dictionary for the next window. The coordinator owns
      // one immutable serialized dictionary and streams that same frame to all
      // workers; workers still install independent local copies. Charge every
      // transmission, but never model m coordinator-resident copies.
      const auto& head_control_frame = hybrid_seed_ids;
      std::size_t head_control_bytes =
          head_delta_selected
          ? head_delta_update_bytes
          : head_full_update_bytes;
      control_volume_bytes_by_group[gi] =
          groups[gi].parts.size() * head_control_bytes;
      head_update_volume_bytes_by_group[gi] =
          control_volume_bytes_by_group[gi];
      const auto control_t1 = std::chrono::steady_clock::now();
      control_ms_by_group[gi] =
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(control_t1 - control_t0).count()) / 1.0e6;
      if (!A.csv_out.empty()) csv_rows[gi].control_ms = control_ms_by_group[gi];
      // Retain each worker's dictionary across the window boundary and apply
      // the same generation-checked update. A stale worker falls back to a
      // full checkpoint and both transmissions are charged.
      std::uint64_t head_apply_ns = 0;
      for (auto& p : groups[gi].parts) {
        auto* hy = dynamic_cast<HybridSS*>(p.sketch.get());
        if (!hy) {
          throw std::runtime_error("hybrid group contains a non-hybrid sketch");
        }
        hy->reset_window_preserve_head();
        p.amap.clear();
        bool installed = false;
        if (head_delta_selected) {
          const auto apply_t0 = std::chrono::steady_clock::now();
          installed = hy->apply_head_delta(head_delta, hybrid_qe[gi]);
          const auto apply_t1 = std::chrono::steady_clock::now();
          head_apply_ns +=
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      apply_t1 - apply_t0)
                      .count());
        }
        if (!installed) {
          if (head_delta_selected) {
            control_volume_bytes_by_group[gi] += head_full_update_bytes;
            head_update_volume_bytes_by_group[gi] += head_full_update_bytes;
          }
          hy->reconfigure(
              hybrid_qe[gi], hybrid_qa[gi],
              A.n_param, hybrid_head_frac_min);
          const auto apply_t0 = std::chrono::steady_clock::now();
          hy->install_head(
              head_control_frame, head_delta.next_generation);
          const auto apply_t1 = std::chrono::steady_clock::now();
          head_apply_ns +=
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      apply_t1 - apply_t0)
                      .count());
        } else {
          hy->reconfigure(
              hybrid_qe[gi], hybrid_qa[gi],
              A.n_param, hybrid_head_frac_min);
        }
      }
      if (!A.csv_out.empty()) {
        csv_rows[gi].head_update_apply_ms =
            static_cast<double>(head_apply_ns) / 1.0e6;
      }
      hybrid_installed_heads[gi].swap(hybrid_seed_ids);
      hybrid_installed_head_keys[gi].swap(hybrid_seed_keys);
      hybrid_head_generations[gi] = head_delta.next_generation;
    }

    for (std::size_t gi=0; gi<groups.size(); ++gi) {
      if (groups[gi].name != "ss") continue;
      const auto& ss_cfg = methods[gi];
      if (ss_cfg.policy == Policy::Static) {
        if (!A.csv_out.empty()) csv_rows[gi].q_next = ss_q_curs[gi];
        for (auto& p : groups[gi].parts) {
          p.sketch->reset_window();
          p.amap.clear();
        }
        rebuild_ss_group(groups[gi], ss_q_curs[gi], methods[gi].ss_per_item_eps);
        continue;
      }

      const auto control_t0 = std::chrono::steady_clock::now();
      sizing::PolicyConfig cfg{};
      cfg.kind = sizing::PolicyKind::difficulty;
      cfg.n_param = A.n_param;
      cfg.r = ss_cfg.r;
      cfg.alpha_req = ss_cfg.alpha_req;
      cfg.delta_m = ss_cfg.delta_m;
      cfg.probe_evidence_window = ss_cfg.probe_evidence_window;
      cfg.ambiguity_decay = ss_cfg.ambiguity_decay;
      cfg.symmetric_relaxation = ss_cfg.symmetric_relaxation;
      cfg.censored_control = ss_cfg.censored_control;
      cfg.probe_residual_guard = ss_cfg.probe_residual_guard;
      cfg.probe_strategy = ss_cfg.probe_strategy;
      cfg.probe_pressure_gate = ss_cfg.probe_pressure_gate;
      cfg.ambiguity_adjust = ss_cfg.ambiguity_adjust;
      cfg.epsilon_m = ss_cfg.epsilon_m;
      cfg.q_cur = ss_q_curs[gi];
      cfg.q_cap = ss_q_caps[gi];
      cfg.q_min = A.n_param;
      cfg.q_max = ss_q_caps[gi];

      if (!ss_control[gi]) {
        throw std::runtime_error(
            "adaptive SS is missing its coordinated controller projection");
      }
      const auto& residual_items = ss_control[gi]->residual_items;
      coord_control_peak_bytes_by_group[gi] = std::max(
          coord_control_peak_bytes_by_group[gi],
          logical_result_bytes(Rs[gi])
              + sizeof(residual_items)
              + residual_items.capacity() * sizeof(HybridSizingItem)
              + sizeof(cfg) + sizeof(policy_states[gi]));
      auto res = sizing::next_q_ss(
          residual_items, Rs[gi].N_global, Rs[gi].threshold,
          {}, cfg, &policy_states[gi]);
      if (include_in_summary && ss_cfg.policy == Policy::Difficulty) {
        const std::size_t q_req_fair = std::min(res.q_req, ss_q_caps[gi]);
        const std::size_t q_eff_fair = std::min(res.q_base, ss_q_caps[gi]);
        miss_den_cnt[groups[gi].label] += 1;
        if (ss_q_curs[gi] < q_req_fair) miss_req_cnt[groups[gi].label] += 1;
        if (ss_q_curs[gi] < q_eff_fair) miss_eff_cnt[groups[gi].label] += 1;
      }
      if (cfg.kind == sizing::PolicyKind::difficulty) {
        std::size_t q_next_mode = res.q_next;
        if (ss_cfg.diff_mode == DifficultyMode::ReactiveReq) q_next_mode = std::max<std::size_t>(cfg.q_min, res.q_req);
        else if (ss_cfg.diff_mode == DifficultyMode::ReactiveEff) q_next_mode = std::max<std::size_t>(cfg.q_min, res.q_base);
        if (cfg.q_cap != std::size_t(-1)) q_next_mode = std::min(q_next_mode, cfg.q_cap);
        q_next_mode = std::max(q_next_mode, cfg.q_min);
        if (cfg.q_max != std::size_t(-1)) q_next_mode = std::min(q_next_mode, cfg.q_max);
        res.q_next = q_next_mode;
      }
      if (ss_cfg.policy == Policy::Difficulty && pending_verify[gi].valid) {
        const bool service_ok = (res.q_req <= pending_verify[gi].q_planned);
        const bool control_ok = (res.q_base <= pending_verify[gi].q_planned);
        const bool calib_eff_ok = (res.q_base <= static_cast<std::size_t>(std::llround(pending_verify[gi].tilde_qpred)));
        std::cout << "  [" << std::left << std::setw(labelw) << groups[gi].label << "] " << std::right
                  << "verify: qBaseline=" << res.q_req
                  << " qActuation=" << res.q_base
                  << " | pred(qActuation<=" << static_cast<std::size_t>(std::llround(pending_verify[gi].tilde_qpred))
                  << ", q=" << pending_verify[gi].q_planned << ")"
                  << " | service=" << (service_ok ? "OK" : "FAIL")
                  << " control=" << (control_ok ? "OK" : "FAIL")
                  << " calibEff=" << (calib_eff_ok ? "OK" : "FAIL") << "\n";
      }
      if (ss_cfg.policy == Policy::Difficulty) {
        pending_verify[gi].valid = true;
        pending_verify[gi].tilde_qpred = res.q_pred_tilde;
        pending_verify[gi].q_planned = res.q_next;
      }

      if (!A.csv_out.empty()) {
        auto& row = csv_rows[gi];
        row.q_next = res.q_next;
        if (ss_cfg.policy == Policy::Difficulty) {
          const std::size_t q_req_fair = std::min(res.q_req, ss_q_caps[gi]);
          const std::size_t q_eff_fair = std::min(res.q_base, ss_q_caps[gi]);
          row.q_req = res.q_req;
          row.q_req_unclipped = res.q_req_unclipped;
          row.margin_alpha = res.margin_alpha;
          row.service_violation = res.service_violation ? 1 : 0;
          row.q_up = res.q_up;
          row.q_baseline = res.q_baseline;
          row.probe_issued = res.probe_issued ? 1 : 0;
          row.probe_failed = res.probe_failed ? 1 : 0;
          row.q_eff_replay = res.q_base;
          row.q_eff_pred = res.q_pred;
          row.q_eff_pred_tilde = res.q_pred_tilde;
          row.miss_req = ss_q_curs[gi] < q_req_fair ? 1 : 0;
          row.miss_eff = ss_q_curs[gi] < q_eff_fair ? 1 : 0;
          row.over_req = res.q_req == 0 ? NAN : static_cast<double>(ss_q_curs[gi]) / static_cast<double>(res.q_req);
          row.over_eff = res.q_base == 0 ? NAN : static_cast<double>(ss_q_curs[gi]) / static_cast<double>(res.q_base);
          row.calib_bias = res.b_q;
        }
      }

      std::cout << std::fixed;
      std::cout << "  [" << std::left << std::setw(labelw) << groups[gi].label << "] " << std::right
                << "q_next=" << res.q_next << " (policy=";
      switch (cfg.kind) {
        case sizing::PolicyKind::difficulty:
          {
          std::cout << "difficulty, alphaReq=" << ss_cfg.alpha_req
                    << ", ambAdjust=" << (ss_cfg.ambiguity_adjust ? "on" : "off")
                    << ", mode=" << (ss_cfg.diff_mode == DifficultyMode::Predictive ? "predictive"
                                     : (ss_cfg.diff_mode == DifficultyMode::ReactiveReq ? "reactive-req" : "reactive-eff"))
                    << ", marginAlpha=" << std::setprecision(6) << res.margin_alpha
                    << ", violation=" << (res.service_violation ? "yes" : "no")
                    << ", qUp=" << res.q_up
                    << ", qBaseline=" << res.q_baseline
                    << ", qActuation=" << res.q_base
                    << ", probe=" << (res.probe_issued ? "issued"
                                       : (res.probe_failed ? "failed" : "none"));
          break;
          }
        default: std::cout << "fixed";
      }
      std::cout << ")\n";

      const auto control_t1 = std::chrono::steady_clock::now();
      control_ms_by_group[gi] =
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(control_t1 - control_t0).count()) / 1.0e6;
      if (!A.csv_out.empty()) csv_rows[gi].control_ms = control_ms_by_group[gi];

      // Reset and rebuild only this SS group
      const std::size_t next_q = res.q_next;
      constexpr std::size_t kControlHeaderBytes = 32;
      constexpr std::size_t kCapacityBytes = sizeof(std::uint32_t);
      control_volume_bytes_by_group[gi] = groups[gi].parts.size()
          * (kControlHeaderBytes + kCapacityBytes);
      ss_q_curs[gi] = next_q;
      for (auto& p : groups[gi].parts) {
        p.sketch->reset_window();
        p.amap.clear();
      }
      rebuild_ss_group(groups[gi], ss_q_curs[gi], methods[gi].ss_per_item_eps);
    }

    // Certified adaptive HL moves upward by one established width-ladder rung
    // when the realized window cannot certify candidate completeness. A pass
    // holds width: this first controller intentionally has no speculative
    // downward release policy.
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (groups[gi].name != "hl" || !methods[gi].hl_adaptive) continue;
      const auto control_t0 = std::chrono::steady_clock::now();
      const std::size_t width_current = hl_width_curs[gi];
      const bool service_ok =
          Rs[gi].has_completeness_certificate
          && Rs[gi].candidate_set_complete;
      const std::size_t width_next = service_ok
          ? width_current
          : next_hl_width(hl_width_ladders[gi], width_current);
      hl_width_curs[gi] = width_next;

      constexpr std::size_t kControlHeaderBytes = 32;
      constexpr std::size_t kWidthBytes = sizeof(std::uint32_t);
      control_volume_bytes_by_group[gi] =
          groups[gi].parts.size() * (kControlHeaderBytes + kWidthBytes);
      coord_control_peak_bytes_by_group[gi] = std::max(
          coord_control_peak_bytes_by_group[gi],
          sizeof(width_current) + sizeof(width_next)
              + hl_width_ladders[gi].capacity() * sizeof(std::size_t));

      if (!A.csv_out.empty()) csv_rows[gi].q_next = width_next;
      std::cout << "  [" << std::left << std::setw(labelw)
                << groups[gi].label << "] " << std::right
                << "w_next=" << width_next
                << " (certificate="
                << (service_ok
                        ? "pass, hold"
                        : width_next > width_current
                            ? "fail, advance"
                            : "fail, ladder ceiling")
                << ")\n";

      for (auto& part : groups[gi].parts) {
        part.sketch->reset_window();
        part.amap.clear();
      }
      if (width_next != width_current) {
        rebuild_hl_group(
            groups[gi], methods[gi], A.n_param, width_next,
            hl_random_streams[gi]);
      }
      const auto control_t1 = std::chrono::steady_clock::now();
      control_ms_by_group[gi] =
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              control_t1 - control_t0).count()) / 1.0e6;
      if (!A.csv_out.empty()) csv_rows[gi].control_ms = control_ms_by_group[gi];
    }

    // Reset non-SS/non-hybrid state. Adaptive HL was reset or rebuilt above.
    for (std::size_t gi=0; gi<groups.size(); ++gi) {
      if (groups[gi].name == "ss" || groups[gi].name == "hybrid") continue;
      if (groups[gi].name == "hl" && methods[gi].hl_adaptive) continue;
      for (auto& p : groups[gi].parts) {
        p.sketch->reset_window();
        p.amap.clear();
      }
    }

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (groups[gi].name == "oracle" || !key_resolution[gi]) continue;
      const auto stats = key_resolution[gi]->finish();
      key_request_bytes_by_group[gi] = stats.request_bytes;
      key_reply_bytes_by_group[gi] = stats.reply_bytes;
      key_resolution_count_by_group[gi] = stats.resolved_keys;
      coord_resolution_peak_bytes_by_group[gi] =
          stats.coordinator_peak_bytes;
    }

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (groups[gi].name == "oracle") continue;
      const double key_request_kib =
          static_cast<double>(key_request_bytes_by_group[gi]) / 1024.0;
      const double key_reply_kib =
          static_cast<double>(key_reply_bytes_by_group[gi]) / 1024.0;
      const double upstream_kib =
          static_cast<double>(report_volume_bytes_by_group[gi]
                              + key_reply_bytes_by_group[gi]) / 1024.0;
      const double downstream_kib =
          static_cast<double>(control_volume_bytes_by_group[gi]
                              + key_request_bytes_by_group[gi]) / 1024.0;
      const double head_update_kib =
          static_cast<double>(head_update_volume_bytes_by_group[gi]) / 1024.0;
      const double total_communication_kib =
          static_cast<double>(report_volume_bytes_by_group[gi]
                              + control_volume_bytes_by_group[gi]
                              + key_request_bytes_by_group[gi]
                              + key_reply_bytes_by_group[gi]) / 1024.0;
      const double coord_ingress_kib =
          static_cast<double>(coord_ingress_bytes_by_group[gi]) / 1024.0;
      const double coord_reduce_work_kib =
          static_cast<double>(coord_mem_by_group[gi].total_peak_bytes) / 1024.0;
      const double coord_control_peak_kib =
          static_cast<double>(coord_control_peak_bytes_by_group[gi]) / 1024.0;
      const double coord_resolution_peak_kib =
          static_cast<double>(coord_resolution_peak_bytes_by_group[gi]) / 1024.0;
      coord_peak_bytes_by_group[gi] = std::max(
          {coord_peak_bytes_by_group[gi],
           coord_control_peak_bytes_by_group[gi],
           coord_resolution_peak_bytes_by_group[gi]});
      const double coord_peak_kib =
          static_cast<double>(coord_peak_bytes_by_group[gi]) / 1024.0;
      if (!A.csv_out.empty()) {
        auto& row = csv_rows[gi];
        row.report_volume_kib = upstream_kib;
        row.control_volume_kib = downstream_kib;
        row.head_update_volume_kib = head_update_kib;
        row.total_communication_kib = total_communication_kib;
        row.key_request_volume_kib = key_request_kib;
        row.key_reply_volume_kib = key_reply_kib;
        row.key_resolution_count = key_resolution_count_by_group[gi];
        row.mem_coord_ingress_kib = coord_ingress_kib;
        row.mem_coord_work_kib = coord_reduce_work_kib;
        row.mem_coord_control_peak_kib = coord_control_peak_kib;
        row.mem_coord_resolution_peak_kib = coord_resolution_peak_kib;
        row.mem_coord_peak_kib = coord_peak_kib;
        row.aggregation_ms = row.report_prepare_ms + row.reduce_ms + row.control_ms;
        write_csv_row(csv_file, row);
      }
      if (include_in_summary) {
        // report/control retain their historical names but now represent all
        // upstream/downstream traffic, including raw-key resolution.
        report_volume_flat_sum[groups[gi].label] +=
            key_reply_kib;
        control_volume_flat_sum[groups[gi].label] += downstream_kib;
        total_communication_flat_sum[groups[gi].label] += total_communication_kib;
        key_request_volume_flat_sum[groups[gi].label] += key_request_kib;
        key_reply_volume_flat_sum[groups[gi].label] += key_reply_kib;
        key_resolution_count_sum[groups[gi].label] +=
            static_cast<double>(key_resolution_count_by_group[gi]);
        mem_coord_ingress_flat_sum[groups[gi].label] += coord_ingress_kib;
        mem_coord_work_flat_sum[groups[gi].label] += coord_reduce_work_kib;
        mem_coord_control_peak_flat_sum[groups[gi].label] += coord_control_peak_kib;
        mem_coord_resolution_peak_flat_sum[groups[gi].label] +=
            coord_resolution_peak_kib;
        mem_coord_peak_flat_sum[groups[gi].label] += coord_peak_kib;
      }
    }

    std::fill(update_events_by_group.begin(), update_events_by_group.end(), 0);
    std::fill(update_ns_by_group.begin(), update_ns_by_group.end(), 0.0);
    ++windows_flushed;
  };

  // Ingest once, feed all methods identically
  JsonGzNestedReader::read(A.path, [&](std::size_t win, std::size_t part, const std::string& raw, int w){
    const std::size_t pidx = part % A.m;
    if (!first) { curr_win = win; first = true; }
    if (win != curr_win) { flush(curr_win); curr_win = win; }

    const Id128 id = id128_for(raw);

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      // Raw bytes are exposed only for this synchronous update. Admission and
      // retirement callbacks keep the persistent dictionary bounded to the
      // keys currently resident in the sketch.
      auto& g = groups[gi];
      g.parts[pidx].begin_update(id, raw);
      const auto t0 = std::chrono::steady_clock::now();
      g.parts[pidx].sketch->update(id, w);
      const auto t1 = std::chrono::steady_clock::now();
      g.parts[pidx].end_update();
      update_ns_by_group[gi] +=
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      update_events_by_group[gi] +=
          static_cast<std::uint64_t>(std::max(w, 0));
    }
  });

  if (first) flush(curr_win);

  // Aggregate & print summary
  std::cout << "\n=== Summary vs ORACLE (HH by default"
            << (A.topk ? ", plus top-k overlap" : "") << ") ===\n";
  std::cout << "(up/down/total include on-demand raw-key resolution; worker memory is per-partition mean; coordinator peak=max(reduction, control, resolution))\n";

  auto fmt = [](double v, int prec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << v;
    return oss.str();
  };

  // stable order based on declared groups (excluding oracle), to ensure all methods are reported
  std::unordered_set<std::string> seen;
  struct SummaryRow {
    std::string label, win, prec, rec, f1, aae, are, miss_req, miss_eff, topk;
    std::string mem_algo_eq, mem_key_eq, mem_worker_eq;
    std::string report_volume, control_volume, total_communication;
    std::string key_request, key_reply, key_count;
    std::string mem_coord_ingress, mem_coord_work, mem_coord_control;
    std::string mem_coord_resolution, mem_coord_peak;
    std::string completeness_rate, unseen_ratio;
  };
  std::vector<SummaryRow> rows;
  std::size_t w_label = max_label_len, w_win = 0, w_prec = 0, w_rec = 0, w_f1 = 0, w_aae = 0, w_are = 0, w_mreq = 0, w_meff = 0, w_topk = 0;
  std::size_t w_mem_aeq = 0, w_mem_keq = 0, w_mem_weq = 0;
  std::size_t w_report = 0, w_control = 0, w_total_comm = 0;
  std::size_t w_key_req = 0, w_key_reply = 0, w_key_count = 0;
  std::size_t w_mem_ci = 0, w_mem_cw = 0, w_mem_cc = 0;
  std::size_t w_mem_cr = 0, w_mem_cp = 0;

  for (const auto& g : groups) {
    if (g.name == "oracle") continue;
    if (!seen.insert(g.label).second) continue;
    auto it = perwin.find(g.label);
    if (it == perwin.end()) continue;
    auto ag = mean_of(it->second);

    SummaryRow r;
    r.label = g.label;
    r.win   = std::to_string(ag.windows);
    r.prec  = fmt(ag.hh_precision_avg, 3);
    r.rec   = fmt(ag.hh_recall_avg, 3);
    r.f1    = fmt(ag.hh_f1_avg, 3);
    r.aae   = fmt(ag.aae_avg, 3);
    r.are   = fmt(ag.are_avg * 100.0, 3);
    auto mem_ae_it = mem_algo_equiv_per_part_sum.find(g.label);
    auto mem_ke_it = mem_key_equiv_per_part_sum.find(g.label);
    auto mem_we_it = mem_worker_equiv_per_part_sum.find(g.label);
    auto report_it = report_volume_flat_sum.find(g.label);
    auto control_it = control_volume_flat_sum.find(g.label);
    auto total_comm_it = total_communication_flat_sum.find(g.label);
    auto key_request_it = key_request_volume_flat_sum.find(g.label);
    auto key_reply_it = key_reply_volume_flat_sum.find(g.label);
    auto key_count_it = key_resolution_count_sum.find(g.label);
    auto mem_ci_it = mem_coord_ingress_flat_sum.find(g.label);
    auto mem_cwe_it = mem_coord_work_flat_sum.find(g.label);
    auto mem_cce_it = mem_coord_control_peak_flat_sum.find(g.label);
    auto mem_cre_it = mem_coord_resolution_peak_flat_sum.find(g.label);
    auto mem_cpe_it = mem_coord_peak_flat_sum.find(g.label);
    if (ag.windows > 0) {
      const double avg_ae = (mem_ae_it != mem_algo_equiv_per_part_sum.end()) ? (mem_ae_it->second / static_cast<double>(ag.windows)) : 0.0;
      const double avg_ke = (mem_ke_it != mem_key_equiv_per_part_sum.end()) ? (mem_ke_it->second / static_cast<double>(ag.windows)) : 0.0;
      r.mem_algo_eq = fmt(avg_ae, 2);
      r.mem_key_eq = fmt(avg_ke, 2);
      if (mem_we_it != mem_worker_equiv_per_part_sum.end()) r.mem_worker_eq = fmt(mem_we_it->second / static_cast<double>(ag.windows), 2);
      if (report_it != report_volume_flat_sum.end()) r.report_volume = fmt(report_it->second / static_cast<double>(ag.windows), 2);
      if (control_it != control_volume_flat_sum.end()) r.control_volume = fmt(control_it->second / static_cast<double>(ag.windows), 2);
      if (total_comm_it != total_communication_flat_sum.end()) r.total_communication = fmt(total_comm_it->second / static_cast<double>(ag.windows), 2);
      if (key_request_it != key_request_volume_flat_sum.end()) r.key_request = fmt(key_request_it->second / static_cast<double>(ag.windows), 2);
      if (key_reply_it != key_reply_volume_flat_sum.end()) r.key_reply = fmt(key_reply_it->second / static_cast<double>(ag.windows), 2);
      if (key_count_it != key_resolution_count_sum.end()) r.key_count = fmt(key_count_it->second / static_cast<double>(ag.windows), 1);
      if (mem_ci_it != mem_coord_ingress_flat_sum.end()) r.mem_coord_ingress = fmt(mem_ci_it->second / static_cast<double>(ag.windows), 2);
      if (mem_cwe_it != mem_coord_work_flat_sum.end()) r.mem_coord_work = fmt(mem_cwe_it->second / static_cast<double>(ag.windows), 2);
      if (mem_cce_it != mem_coord_control_peak_flat_sum.end()) r.mem_coord_control = fmt(mem_cce_it->second / static_cast<double>(ag.windows), 2);
      if (mem_cre_it != mem_coord_resolution_peak_flat_sum.end()) r.mem_coord_resolution = fmt(mem_cre_it->second / static_cast<double>(ag.windows), 2);
      if (mem_cpe_it != mem_coord_peak_flat_sum.end()) r.mem_coord_peak = fmt(mem_cpe_it->second / static_cast<double>(ag.windows), 2);
    }
    if (ag.topk_overlap_avg) {
      r.topk = fmt(*ag.topk_overlap_avg, 3);
    }
    auto den_it = miss_den_cnt.find(g.label);
    if (den_it != miss_den_cnt.end() && den_it->second > 0) {
      const double miss_req = 100.0 * static_cast<double>(miss_req_cnt[g.label]) / static_cast<double>(den_it->second);
      const double miss_eff = 100.0 * static_cast<double>(miss_eff_cnt[g.label]) / static_cast<double>(den_it->second);
      r.miss_req = fmt(miss_req, 2) + "%";
      r.miss_eff = fmt(miss_eff, 2) + "%";
    }
    auto cert_it = completeness_cert_cnt.find(g.label);
    if (cert_it != completeness_cert_cnt.end() && cert_it->second > 0) {
      const double den = static_cast<double>(cert_it->second);
      r.completeness_rate =
          fmt(100.0 * static_cast<double>(completeness_pass_cnt[g.label]) / den, 2)
          + "%";
      r.unseen_ratio = fmt(unseen_ratio_sum[g.label] / den, 3);
    }

    w_win   = std::max(w_win,   r.win.size());
    w_prec  = std::max(w_prec,  r.prec.size());
    w_rec   = std::max(w_rec,   r.rec.size());
    w_f1    = std::max(w_f1,    r.f1.size());
    w_aae   = std::max(w_aae,   r.aae.size());
    w_are   = std::max(w_are,   r.are.size());
    w_mreq  = std::max(w_mreq,  r.miss_req.size());
    w_meff  = std::max(w_meff,  r.miss_eff.size());
    w_mem_aeq = std::max(w_mem_aeq, r.mem_algo_eq.size());
    w_mem_keq = std::max(w_mem_keq, r.mem_key_eq.size());
    w_mem_weq = std::max(w_mem_weq, r.mem_worker_eq.size());
    w_report = std::max(w_report, r.report_volume.size());
    w_control = std::max(w_control, r.control_volume.size());
    w_total_comm = std::max(w_total_comm, r.total_communication.size());
    w_key_req = std::max(w_key_req, r.key_request.size());
    w_key_reply = std::max(w_key_reply, r.key_reply.size());
    w_key_count = std::max(w_key_count, r.key_count.size());
    w_mem_ci = std::max(w_mem_ci, r.mem_coord_ingress.size());
    w_mem_cw = std::max(w_mem_cw, r.mem_coord_work.size());
    w_mem_cc = std::max(w_mem_cc, r.mem_coord_control.size());
    w_mem_cr = std::max(w_mem_cr, r.mem_coord_resolution.size());
    w_mem_cp = std::max(w_mem_cp, r.mem_coord_peak.size());
    w_topk  = std::max(w_topk,  r.topk.size());

    rows.push_back(std::move(r));
  }

  for (const auto& r : rows) {
    std::cout << std::left << std::setw(w_label) << r.label << std::right
              << "\twindows=" << std::setw(w_win) << r.win
              << "\tHH: precision=" << std::setw(w_prec) << r.prec
              << " recall=" << std::setw(w_rec) << r.rec
              << " F1=" << std::setw(w_f1) << r.f1
              << "\tAAE=" << std::setw(w_aae) << r.aae
              << "\tARE=" << std::setw(w_are) << r.are << "%";
    if (!r.topk.empty()) {
      std::cout << "\ttopK_overlap=" << std::setw(w_topk) << r.topk;
    }
    if (!r.mem_worker_eq.empty()) {
      std::cout << "\tmem(wAlgo/wKey/wTotal)≈"
                << std::setw(w_mem_aeq) << r.mem_algo_eq << "/"
                << std::setw(w_mem_keq) << r.mem_key_eq << "/"
                << std::setw(w_mem_weq) << r.mem_worker_eq << " KiB"
                << "\tcomm(up/down/total)≈"
                << std::setw(w_report) << r.report_volume << "/"
                << std::setw(w_control) << r.control_volume << "/"
                << std::setw(w_total_comm) << r.total_communication << " KiB"
                << "\tkeyResolve(req/reply/count)≈"
                << std::setw(w_key_req) << r.key_request << "/"
                << std::setw(w_key_reply) << r.key_reply << "/"
                << std::setw(w_key_count) << r.key_count
                << "\tmem(cIngress/cReduceWork/cControlPeak/cResolvePeak/cPeak)≈"
                << std::setw(w_mem_ci) << r.mem_coord_ingress << "/"
                << std::setw(w_mem_cw) << r.mem_coord_work << "/"
                << std::setw(w_mem_cc) << r.mem_coord_control << "/"
                << std::setw(w_mem_cr) << r.mem_coord_resolution << "/"
                << std::setw(w_mem_cp) << r.mem_coord_peak << " KiB";
    }
    if (!r.miss_req.empty()) {
      std::cout << "\tmissReq=" << std::setw(w_mreq) << r.miss_req
                << " missEff=" << std::setw(w_meff) << r.miss_eff;
    }
    if (!r.completeness_rate.empty()) {
      std::cout << "\tcertComplete=" << r.completeness_rate
                << " avgUnseenUB/T=" << r.unseen_ratio;
    }
    std::cout << "\n";
  }

  return 0;
}
