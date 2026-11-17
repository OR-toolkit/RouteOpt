/*
 * Copyright (c) 2025 Zhengzhong (Ricky) You.
 * All rights reserved.
 * Software: RouteOpt
 * License: GPL-3.0
 */

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include "label_logger.hpp"

#ifndef ROUTEOPT_GIT_DESCRIBE
#define ROUTEOPT_GIT_DESCRIBE "unknown"
#endif
#ifndef ROUTEOPT_BUILD_TYPE
#define ROUTEOPT_BUILD_TYPE "unknown"
#endif

namespace RouteOpt::Application::CVRP {
    namespace {
        constexpr int LABEL_LOG_FORMAT_VERSION = 1;

        constexpr std::array LABEL_LOG_ENV_KEYS{
            "ROUTEOPT_LABEL_LOG_DIR",
            "ROUTEOPT_LABEL_LOG_INSTANCES",
            "ROUTEOPT_LABEL_LOG_NODES",
            "ROUTEOPT_LABEL_LOG_CALLS",
            "ROUTEOPT_LABEL_LOG_LEVELS",
            "ROUTEOPT_LABEL_LOG_TESTING",
            "ROUTEOPT_LABEL_LOG_CALLS_PER_FILE",
            "ROUTEOPT_LABEL_LOG_MAX_CALLS",
            "ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT",
        };

        struct FieldInfo {
            const char *name;
            const char *numpy_type;
            size_t offset;
            size_t size;
            int count;
            const char *description;
        };

#define LABEL_LOG_FIELD(record, field, type, count, description) \
        FieldInfo{#field, type, offsetof(record, field), sizeof(record::field), count, description}

        constexpr std::array FEATURE_FIELDS{
            LABEL_LOG_FIELD(LabelFeatureRecord, label_id, "<u8", 1, "unique over the whole run"),
            LABEL_LOG_FIELD(LabelFeatureRecord, pred_id, "<u8", 1, "label_id of the predecessor; UINT64_MAX for initial labels"),
            LABEL_LOG_FIELD(LabelFeatureRecord, call_id, "<u4", 1, "pricing call, see calls.csv"),
            LABEL_LOG_FIELD(LabelFeatureRecord, vertex, "<u2", 1, "end vertex of the label"),
            LABEL_LOG_FIELD(LabelFeatureRecord, pred_vertex, "<u2", 1, "end vertex of the predecessor; 0 (depot) for initial labels"),
            LABEL_LOG_FIELD(LabelFeatureRecord, bucket, "<u2", 1, "bucket index at the end vertex"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir, "<u1", 1, "1 forward, 0 backward"),
            LABEL_LOG_FIELD(LabelFeatureRecord, attempt, "<u1", 1, "labeling attempt within the call; increases after an out-of-memory restart"),
            LABEL_LOG_FIELD(LabelFeatureRecord, arc_kind, "<u1", 1, "0 initial, 1 bucket arc, 2 jump arc"),
            LABEL_LOG_FIELD(LabelFeatureRecord, rc, "<f8", 1, "reduced cost"),
            LABEL_LOG_FIELD(LabelFeatureRecord, res, "<i4", NUM_RESOURCE, "resource consumption, scaled by RESOURCE_FACTOR"),
            LABEL_LOG_FIELD(LabelFeatureRecord, n_arcs, "<u2", 1, "arcs in the partial path from the depot"),
            LABEL_LOG_FIELD(LabelFeatureRecord, n_customers, "<u2", 1, "distinct customers in the partial path"),
            LABEL_LOG_FIELD(LabelFeatureRecord, n_repeats, "<u2", 1, "visits to an already visited customer (n_arcs - n_customers)"),
            LABEL_LOG_FIELD(LabelFeatureRecord, ng_mem_size, "<u2", 1, "customers in the ng-memory, including the end vertex"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir_created, "<u4", 1, "labels created so far in this direction and attempt, excluding this one"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir_open, "<u4", 1, "stored labels not yet taken for extension"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir_closed, "<u4", 1, "stored labels already taken for extension"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir_rejected, "<u4", 1, "labels rejected at insertion"),
            LABEL_LOG_FIELD(LabelFeatureRecord, dir_removed, "<u4", 1, "labels removed after insertion"),
            LABEL_LOG_FIELD(LabelFeatureRecord, bucket_stored, "<u4", 1, "labels stored in this label's bucket"),
            LABEL_LOG_FIELD(LabelFeatureRecord, bucket_open, "<u4", 1, "of those, not yet taken for extension"),
            LABEL_LOG_FIELD(LabelFeatureRecord, bucket_closed, "<u4", 1, "of those, already taken for extension"),
        };

        constexpr std::array OUTCOME_FIELDS{
            LABEL_LOG_FIELD(LabelOutcomeRecord, label_id, "<u8", 1, "joins with the feature record"),
            LABEL_LOG_FIELD(LabelOutcomeRecord, dominator_id, "<u8", 1, "label that rejected (0) or removed (1) it; UINT64_MAX otherwise"),
            LABEL_LOG_FIELD(LabelOutcomeRecord, skip_dominator_id, "<u8", 1, "earlier-bucket label that made it skip extension; UINT64_MAX otherwise"),
            LABEL_LOG_FIELD(LabelOutcomeRecord, outcome, "<u1", 1, "0 rejected at insertion, 1 removed by newer, 2 alive at end, 3 discarded by restart"),
            LABEL_LOG_FIELD(LabelOutcomeRecord, ext_state, "<u1", 1, "0 never taken, 1 extended, 2 taken but skipped as dominated"),
        };
#undef LABEL_LOG_FIELD

        template<size_t N>
        constexpr size_t sumFieldSizes(const std::array<FieldInfo, N> &fields) {
            size_t sum = 0;
            for (auto &field: fields) sum += field.size;
            return sum;
        }

        static_assert(sumFieldSizes(FEATURE_FIELDS) == sizeof(LabelFeatureRecord), "feature field table incomplete");
        static_assert(sumFieldSizes(OUTCOME_FIELDS) == sizeof(LabelOutcomeRecord), "outcome field table incomplete");

        std::string localTimeNow() {
            auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            std::ostringstream os;
            os << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z");
            return os.str();
        }

        std::string getEnv(const char *key) {
            const char *value = std::getenv(key);
            return value ? value : "";
        }

        std::vector<std::string> splitList(const std::string &value) {
            std::vector<std::string> items;
            std::stringstream ss(value);
            std::string item;
            while (std::getline(ss, item, ',')) {
                auto beg = item.find_first_not_of(" \t");
                if (beg == std::string::npos) continue;
                auto end = item.find_last_not_of(" \t");
                items.emplace_back(item.substr(beg, end - beg + 1));
            }
            return items;
        }

        long long parseInteger(const std::string &text, const char *key) {
            size_t pos = 0;
            long long value{};
            try {
                value = std::stoll(text, &pos);
            } catch (const std::exception &) {
                pos = 0;
            }
            if (pos == 0 || pos != text.size()) {
                THROW_RUNTIME_ERROR(std::string("label log: invalid integer '") + text + "' in " + key);
            }
            return value;
        }

        // "1,4-9" -> {[1,1],[4,9]}; empty or "all" -> no restriction
        template<typename Range>
        std::vector<Range> parseRanges(const char *key) {
            std::vector<Range> ranges;
            auto value = getEnv(key);
            if (value.empty() || value == "all") return ranges;
            for (auto &item: splitList(value)) {
                auto dash = item.find('-', 1);
                Range range;
                if (dash == std::string::npos) {
                    range.lo = range.hi = parseInteger(item, key);
                } else {
                    range.lo = parseInteger(item.substr(0, dash), key);
                    range.hi = parseInteger(item.substr(dash + 1), key);
                }
                if (range.lo > range.hi) THROW_RUNTIME_ERROR(std::string("label log: empty range in ") + key);
                ranges.emplace_back(range);
            }
            return ranges;
        }

        template<typename Range>
        bool inRanges(const std::vector<Range> &ranges, long long value) {
            if (ranges.empty()) return true;
            return std::any_of(ranges.begin(), ranges.end(),
                               [value](const Range &r) { return r.lo <= value && value <= r.hi; });
        }

        const char *levelName(PRICING_LEVEL level) {
            switch (level) {
                case PRICING_LEVEL::EXACT:
                    return "exact";
                case PRICING_LEVEL::HEAVY:
                    return "heavy";
                case PRICING_LEVEL::LIGHT:
                    return "light";
            }
            return "unknown";
        }

        std::string lowerCase(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        template<size_t N>
        void writeFieldTable(std::ostream &os, const char *section, const char *file_pattern, size_t size,
                             const std::array<FieldInfo, N> &fields) {
            os << "[" << section << "] file=" << file_pattern << " record_size=" << size << "\n";
            os << "# name type offset size count  # description\n";
            for (auto &field: fields) {
                os << field.name << " " << field.numpy_type << " " << field.offset << " " << field.size << " "
                        << field.count << "  # " << field.description << "\n";
            }
            os << "\n";
        }

        template<size_t N>
        void writeNumpyDtype(std::ostream &os, const char *name, const std::array<FieldInfo, N> &fields) {
            os << name << " = [";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) os << ", ";
                os << "('" << fields[i].name << "', '" << fields[i].numpy_type << "'";
                if (fields[i].count != 1 || std::string(fields[i].name) == "res") {
                    os << ", (" << fields[i].count << ",)";
                }
                os << ")";
            }
            os << "]\n";
        }
    }

    LabelLogger::~LabelLogger() {
        features.close();
        outcomes.close();
    }

    void LabelLogger::configure(const std::string &instance_name, const std::string &instance_path,
                                int argc, char *argv[], int dim) {
        // applies even when nothing is logged, so filtered-out instances also stop at the root
        auto stop = getEnv("ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT");
        if (!stop.empty()) stop_after_root = parseInteger(stop, "ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT") != 0;
        if (stop_after_root) std::cout << "label log: the solve stops after the root node" << std::endl;

        auto dir = getEnv("ROUTEOPT_LABEL_LOG_DIR");
        if (dir.empty()) {
            std::cout << "label log: off (ROUTEOPT_LABEL_LOG_DIR is not set)" << std::endl;
            return;
        }

        auto instances = splitList(getEnv("ROUTEOPT_LABEL_LOG_INSTANCES"));
        if (!instances.empty()) {
            auto file_name = std::filesystem::path(instance_path).filename().string();
            if (std::none_of(instances.begin(), instances.end(), [&](const std::string &ins) {
                return ins == instance_name || ins == file_name;
            })) {
                std::cout << "label log: off (instance " << instance_name
                        << " is not in ROUTEOPT_LABEL_LOG_INSTANCES)" << std::endl;
                return;
            }
        }

        node_ranges = parseRanges<Range>("ROUTEOPT_LABEL_LOG_NODES");
        call_ranges = parseRanges<Range>("ROUTEOPT_LABEL_LOG_CALLS");

        auto levels = getEnv("ROUTEOPT_LABEL_LOG_LEVELS");
        if (!levels.empty() && levels != "all") {
            std::fill(std::begin(log_level), std::end(log_level), false);
            for (auto &item: splitList(levels)) {
                auto name = lowerCase(item);
                if (name == "exact") log_level[static_cast<int>(PRICING_LEVEL::EXACT)] = true;
                else if (name == "heavy") log_level[static_cast<int>(PRICING_LEVEL::HEAVY)] = true;
                else if (name == "light") log_level[static_cast<int>(PRICING_LEVEL::LIGHT)] = true;
                else THROW_RUNTIME_ERROR("label log: unknown level '" + item + "' in ROUTEOPT_LABEL_LOG_LEVELS");
            }
        }

        auto testing = getEnv("ROUTEOPT_LABEL_LOG_TESTING");
        if (!testing.empty()) log_testing = parseInteger(testing, "ROUTEOPT_LABEL_LOG_TESTING") != 0;

        auto per_file = getEnv("ROUTEOPT_LABEL_LOG_CALLS_PER_FILE");
        if (!per_file.empty()) calls_per_file = parseInteger(per_file, "ROUTEOPT_LABEL_LOG_CALLS_PER_FILE");
        if (calls_per_file < 1) THROW_RUNTIME_ERROR("label log: ROUTEOPT_LABEL_LOG_CALLS_PER_FILE must be >= 1");

        auto max = getEnv("ROUTEOPT_LABEL_LOG_MAX_CALLS");
        if (!max.empty()) max_calls = parseInteger(max, "ROUTEOPT_LABEL_LOG_MAX_CALLS");

        this->dim = dim;
        words_per_slot = (static_cast<size_t>(dim) + 63) / 64;
        out_dir = dir;

        std::filesystem::create_directories(out_dir);
        if (std::filesystem::exists(std::filesystem::path(out_dir) / "run_config.txt")) {
            THROW_RUNTIME_ERROR("label log: " + out_dir + " already holds a label log; use a new directory");
        }
        writeRunConfig(instance_name, instance_path, argc, argv);
        writeLayout();
        calls_csv.open(std::filesystem::path(out_dir) / "calls.csv", std::ios::trunc);
        if (!calls_csv) THROW_RUNTIME_ERROR("label log: cannot open calls.csv in " + out_dir);
        calls_csv << "call_id,cg_run,node,cg_iter,level,testing,attempts,finished,num_buckets,step_size,"
                "meet_point,meet_point_extensions,n_features,n_outcomes,file_group\n";
        calls_csv.flush();

        enabled = true;
        std::cout << "label log: writing to " << out_dir << std::endl;
    }

    void LabelLogger::resizeSlots(size_t new_num_slots) {
        if (!enabled || new_num_slots <= num_slots) return;
        num_slots = new_num_slots;
        slot_id.reset(new std::uint64_t[num_slots]);
        slot_skip_dominator.reset(new std::uint64_t[num_slots]);
        slot_visited.reset(new std::uint64_t[num_slots * words_per_slot]);
        slot_n_arcs.reset(new std::uint16_t[num_slots]);
        slot_n_repeats.reset(new std::uint16_t[num_slots]);
        slot_ext_state.reset(new LabelExtState[num_slots]);
    }

    void LabelLogger::beginColumnGeneration(int node_idx) {
        ++cg_run;
        this->node_idx = node_idx;
        cg_iter = 0;
    }

    bool LabelLogger::passesFilters(PRICING_LEVEL pricing_level) const {
        if (!log_level[static_cast<int>(pricing_level)]) return false;
        if (in_testing && !log_testing) return false;
        if (!inRanges(node_ranges, node_idx)) return false;
        if (!inRanges(call_ranges, call_id)) return false;
        if (max_calls >= 0 && num_logged_calls >= max_calls) return false;
        return true;
    }

    void LabelLogger::beginCall(PRICING_LEVEL pricing_level, int num_buckets_per_vertex, int step_size,
                                double meet_point) {
        // ids advance on every call, so they stay comparable between runs with different filters
        call_id = next_call_id++;
        active = enabled && passesFilters(pricing_level);
        if (!active) return;

        auto group = num_logged_calls / calls_per_file;
        if (group != open_group) openGroup(group);
        ++num_logged_calls;

        level = pricing_level;
        num_buckets = num_buckets_per_vertex;
        call_step_size = step_size;
        call_meet_point = meet_point;
        attempt = 0;
        started[0] = started[1] = false;
        call_features = call_outcomes = 0;
    }

    void LabelLogger::beginDirection(bool dir) {
        if (!active) return;
        started[dir] = true;
        counters[dir] = {};
        const auto size = static_cast<size_t>(dim) * num_buckets;
        bucket_open[dir].assign(size, 0);
        bucket_closed[dir].assign(size, 0);
    }

    void LabelLogger::abortAttempt() {
        started[0] = started[1] = false;
        ++attempt;
    }

    void LabelLogger::endCall(bool if_finished, size_t num_meet_point_extensions) {
        if (!active) return;
        features.flush();
        outcomes.flush();
        calls_csv << call_id << "," << cg_run << "," << node_idx << "," << cg_iter << "," << levelName(level) << ","
                << in_testing << "," << attempt + 1 << "," << if_finished << "," << num_buckets << ","
                << call_step_size << "," << call_meet_point << "," << num_meet_point_extensions << ","
                << call_features << "," << call_outcomes << "," << open_group << "\n";
        calls_csv.flush();
        if (call_features != call_outcomes) {
            PRINT_WARNING("label log: call " + std::to_string(call_id) + " has " + std::to_string(call_features) +
                " features but " + std::to_string(call_outcomes) + " outcomes");
        }
        active = false;
    }

    void LabelLogger::finishAfterRoot(double root_value, double ub, bool if_root_terminated) {
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
                << "label log: stopped after the root node, root value= " << root_value << " ub= " << ub
                << " root terminated= " << if_root_terminated << std::endl;
        if (!enabled) return;
        features.close();
        outcomes.close();
        calls_csv.close();

        std::ofstream os(std::filesystem::path(out_dir) / "run_config.txt", std::ios::app);
        os << std::setprecision(std::numeric_limits<double>::max_digits10);
        os << "\n# root-only result (ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT)\n";
        os << "end_time = " << localTimeNow() << "\n";
        os << "root_value = " << root_value << "\n";
        os << "ub = " << ub << "\n";
        os << "root_terminated = " << if_root_terminated << "\n";
        os << "logged_calls = " << num_logged_calls << "\n";
    }

    void LabelLogger::openGroup(long long group) {
        std::ostringstream name;
        name << std::setw(6) << std::setfill('0') << group;
        auto base = std::filesystem::path(out_dir);
        features.open((base / ("features_" + name.str() + ".bin")).string());
        outcomes.open((base / ("outcomes_" + name.str() + ".bin")).string());
        open_group = group;
    }

    void LabelLogger::writeRunConfig(const std::string &instance_name, const std::string &instance_path,
                                     int argc, char *argv[]) const {
        std::ofstream os(std::filesystem::path(out_dir) / "run_config.txt");
        if (!os) THROW_RUNTIME_ERROR("label log: cannot write run_config.txt in " + out_dir);


        os << "# RouteOpt label log run configuration\n";
        os << "format_version = " << LABEL_LOG_FORMAT_VERSION << "\n";
        os << "start_time = " << localTimeNow() << "\n";
        os << "instance_name = " << instance_name << "\n";
        os << "instance_path = " << instance_path << "\n";
        os << "command_line =";
        for (int i = 0; i < argc; ++i) os << " " << argv[i];
        os << "\n";
        os << "working_directory = " << std::filesystem::current_path().string() << "\n";
        os << "git_describe_at_configure = " << ROUTEOPT_GIT_DESCRIBE << "\n";
        os << "build_type = " << ROUTEOPT_BUILD_TYPE << "\n";
        os << "dim = " << dim << "\n";
        os << "\n# environment\n";
        for (auto key: LABEL_LOG_ENV_KEYS) {
            const char *value = std::getenv(key);
            os << key << " = " << (value ? value : "(unset)") << "\n";
        }
        os << "\n# compile-time settings\n";
        os << "application_type = " << (app_type == APPLICATION_TYPE::CVRP ? "CVRP" : "VRPTW") << "\n";
        os << "if_symmetry = " << !IF_SYMMETRY_PROHIBIT << "\n";
        os << "ml_type = " << static_cast<int>(ml_type) << "\n";
        os << "IF_USE_STAB = " << IF_USE_STAB << "\n";
        os << "NUM_RESOURCE = " << NUM_RESOURCE << "\n";
        os << "NUM_RESOURCE_CMP_IN_DOMINANCE = " << NUM_RESOURCE_CMP_IN_DOMINANCE << "\n";
        os << "RESOURCE_FACTOR = " << RESOURCE_FACTOR << "\n";
        os << "MAX_NUM_CUSTOMERS = " << MAX_NUM_CUSTOMERS << "\n";
        os << "LABEL_ASSIGN = " << LABEL_ASSIGN << "\n";
        os << "InitialNumBuckets = " << InitialNumBuckets << "\n";
        os << "MAX_NUM_REGENERATE_BUCKET = " << MAX_NUM_REGENERATE_BUCKET << "\n";
        os << "InitialNGSize = " << InitialNGSize << "\n";
        os << "MAX_NG_SIZE = " << MAX_NG_SIZE << "\n";
        os << "RC_TOLERANCE = " << RC_TOLERANCE << "\n";
        os << "HardTimeThresholdInPricing = " << HardTimeThresholdInPricing << "\n";
        os << "\n# record files\n";
        os << "feature_record_size = " << sizeof(LabelFeatureRecord) << "\n";
        os << "outcome_record_size = " << sizeof(LabelOutcomeRecord) << "\n";
        os << "buffer_records = " << LABEL_LOG_BUFFER_RECORDS << "\n";
        os << "layout = layout.txt\n";
    }

    void LabelLogger::writeLayout() const {
        std::ofstream os(std::filesystem::path(out_dir) / "layout.txt");
        if (!os) THROW_RUNTIME_ERROR("label log: cannot write layout.txt in " + out_dir);
        os << "# RouteOpt label log layout, format_version " << LABEL_LOG_FORMAT_VERSION << "\n";
        os << "# Little-endian, packed records (no padding). One feature and one outcome record per label;\n";
        os << "# join them on label_id. Counters in a feature record are read when the label is created,\n";
        os << "# before its own dominance check. UINT64_MAX marks a missing label id.\n";
        os << "# Calls are described in calls.csv; file_group names the files that hold a call's records.\n\n";
        writeFieldTable(os, "feature", "features_<file_group:06d>.bin", sizeof(LabelFeatureRecord), FEATURE_FIELDS);
        writeFieldTable(os, "outcome", "outcomes_<file_group:06d>.bin", sizeof(LabelOutcomeRecord), OUTCOME_FIELDS);
        os << "# numpy: np.fromfile(path, dtype=np.dtype(feature_dtype))\n";
        writeNumpyDtype(os, "feature_dtype", FEATURE_FIELDS);
        writeNumpyDtype(os, "outcome_dtype", OUTCOME_FIELDS);
    }
}
