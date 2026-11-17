/*
 * Copyright (c) 2025 Zhengzhong (Ricky) You.
 * All rights reserved.
 * Software: RouteOpt
 * License: GPL-3.0
 */

#ifndef ROUTE_OPT_LABEL_LOGGER_HPP
#define ROUTE_OPT_LABEL_LOGGER_HPP
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "cvrp_macro.hpp"
#include "pricing_macro.hpp"

namespace RouteOpt::Application::CVRP {
    /**
     * Read-only record of every label created in column-generation pricing, for offline dominance analysis.
     * The layout of both record types is written to layout.txt next to the data files.
     */
    static_assert(std::endian::native == std::endian::little, "label log files are written little-endian");

    enum class LabelOutcome : std::uint8_t {
        REJECTED_AT_INSERTION = 0,
        REMOVED_BY_NEWER = 1,
        ALIVE_AT_END = 2,
        DISCARDED_BY_RESTART = 3
    };

    enum class LabelExtState : std::uint8_t {
        NEVER_POPPED = 0,
        EXTENDED = 1,
        SKIPPED_DOMINATED = 2
    };

    enum class LabelArcKind : std::uint8_t {
        INITIAL = 0,
        BUCKET_ARC = 1,
        JUMP_ARC = 2
    };

    constexpr std::uint64_t NO_LABEL_ID = UINT64_MAX;
    constexpr size_t NO_SLOT = SIZE_MAX;
    constexpr size_t LABEL_LOG_BUFFER_RECORDS = 1 << 16;

#pragma pack(push, 1)
    struct LabelFeatureRecord {
        std::uint64_t label_id;
        std::uint64_t pred_id;
        std::uint32_t call_id;
        std::uint16_t vertex;
        std::uint16_t pred_vertex;
        std::uint16_t bucket;
        std::uint8_t dir;
        std::uint8_t attempt;
        std::uint8_t arc_kind;
        double rc;
        std::int32_t res[NUM_RESOURCE];
        std::uint16_t n_arcs;
        std::uint16_t n_customers;
        std::uint16_t n_repeats;
        std::uint16_t ng_mem_size;
        std::uint32_t dir_created;
        std::uint32_t dir_open;
        std::uint32_t dir_closed;
        std::uint32_t dir_rejected;
        std::uint32_t dir_removed;
        std::uint32_t bucket_stored;
        std::uint32_t bucket_open;
        std::uint32_t bucket_closed;
    };

    struct LabelOutcomeRecord {
        std::uint64_t label_id;
        std::uint64_t dominator_id;
        std::uint64_t skip_dominator_id;
        std::uint8_t outcome;
        std::uint8_t ext_state;
    };
#pragma pack(pop)

    /**
     * Fixed-capacity buffer written to a binary file in whole chunks, then reused.
     * Uses std::FILE: libc++ std::ofstream::write splits large chunks into many small locked writes.
     */
    template<typename Record>
    class RecordBuffer {
    public:
        RecordBuffer() = default;

        RecordBuffer(const RecordBuffer &) = delete;

        RecordBuffer &operator=(const RecordBuffer &) = delete;

        ~RecordBuffer() { close(); }

        void open(const std::string &path) {
            close();
            out = std::fopen(path.c_str(), "wb");
            if (!out) THROW_RUNTIME_ERROR("label log: cannot open " + path);
            std::setvbuf(out, nullptr, _IONBF, 0);
            records.reserve(LABEL_LOG_BUFFER_RECORDS);
        }

        void push(const Record &record) {
            records.push_back(record);
            if (records.size() == LABEL_LOG_BUFFER_RECORDS) flush();
        }

        void flush() {
            if (!out || records.empty()) return;
            if (std::fwrite(records.data(), sizeof(Record), records.size(), out) != records.size()) {
                THROW_RUNTIME_ERROR("label log: write failed");
            }
            records.clear();
        }

        void close() {
            if (!out) return;
            flush();
            std::fclose(out);
            out = nullptr;
        }

    private:
        std::FILE *out{};
        std::vector<Record> records{};
    };

    class LabelLogger {
    public:
        LabelLogger() = default;

        LabelLogger(const LabelLogger &) = delete;

        LabelLogger &operator=(const LabelLogger &) = delete;

        ~LabelLogger();

        void configure(const std::string &instance_name, const std::string &instance_path,
                       int argc, char *argv[], int dim);

        void resizeSlots(size_t num_slots);

        void beginColumnGeneration(int node_idx);

        void setIteration(int iter) { cg_iter = iter; }

        void setTesting(bool if_testing) { in_testing = if_testing; }

        void beginCall(PRICING_LEVEL pricing_level, int num_buckets_per_vertex, int step_size, double meet_point);

        void beginDirection(bool dir);

        void abortAttempt();

        void endCall(bool if_finished, size_t num_meet_point_extensions);

        [[nodiscard]] bool isActive() const { return active; }

        [[nodiscard]] bool ifStopAfterRoot() const { return stop_after_root; }

        void finishAfterRoot(double root_value, double ub, bool if_root_terminated);

        [[nodiscard]] bool isStarted(bool dir) const { return started[dir]; }

        void onCreate(bool dir, size_t slot, size_t pred_slot, int vertex, int pred_vertex, int bucket,
                      LabelArcKind arc_kind, double rc, const std::int32_t *res, size_t ng_mem_size,
                      size_t bucket_stored);

        void onRejected(bool dir, size_t slot, size_t dominator_slot);

        void onInserted(bool dir, int vertex, int bucket);

        void onRemoved(bool dir, size_t slot, int vertex, int bucket, size_t dominator_slot);

        void onPopped(bool dir, size_t slot, int vertex, int bucket, bool if_extended, size_t skip_dominator_slot);

        void onFinal(size_t slot, LabelOutcome outcome);

    private:
        struct DirCounters {
            std::uint32_t created{};
            std::uint32_t open{};
            std::uint32_t closed{};
            std::uint32_t rejected{};
            std::uint32_t removed{};
        };

        struct Range {
            long long lo{};
            long long hi{};
        };

        //settings
        bool enabled{};
        std::string out_dir{};
        std::vector<Range> node_ranges{};
        std::vector<Range> call_ranges{};
        bool log_level[3]{true, true, true};
        bool log_testing{true};
        long long calls_per_file{1};
        long long max_calls{-1};
        bool stop_after_root{};
        int dim{};
        size_t words_per_slot{};

        //run state
        std::uint64_t next_label_id{};
        std::uint32_t next_call_id{};
        long long num_logged_calls{};
        long long open_group{-1};
        int cg_run{-1};
        int node_idx{-1};
        int cg_iter{};
        bool in_testing{};
        std::ofstream calls_csv{};
        RecordBuffer<LabelFeatureRecord> features{};
        RecordBuffer<LabelOutcomeRecord> outcomes{};

        //call state
        bool active{};
        std::uint32_t call_id{};
        PRICING_LEVEL level{};
        int num_buckets{};
        int call_step_size{};
        double call_meet_point{};
        std::uint8_t attempt{};
        bool started[2]{};
        std::uint64_t call_features{};
        std::uint64_t call_outcomes{};
        DirCounters counters[2]{};
        std::vector<std::uint32_t> bucket_open[2]{};
        std::vector<std::uint32_t> bucket_closed[2]{};

        //per label slot, indexed like all_label; default-initialized so untouched slots cost no memory
        size_t num_slots{};
        std::unique_ptr<std::uint64_t[]> slot_id{};
        std::unique_ptr<std::uint64_t[]> slot_skip_dominator{};
        std::unique_ptr<std::uint64_t[]> slot_visited{};
        std::unique_ptr<std::uint16_t[]> slot_n_arcs{};
        std::unique_ptr<std::uint16_t[]> slot_n_repeats{};
        std::unique_ptr<LabelExtState[]> slot_ext_state{};

        [[nodiscard]] size_t bucketIndex(int vertex, int bucket) const {
            return static_cast<size_t>(vertex) * num_buckets + bucket;
        }

        [[nodiscard]] bool passesFilters(PRICING_LEVEL pricing_level) const;

        void openGroup(long long group);

        void writeOutcome(size_t slot, LabelOutcome outcome, std::uint64_t dominator_id);

        void writeRunConfig(const std::string &instance_name, const std::string &instance_path,
                            int argc, char *argv[]) const;

        void writeLayout() const;
    };

    inline void LabelLogger::onCreate(bool dir, size_t slot, size_t pred_slot, int vertex, int pred_vertex,
                                      int bucket, LabelArcKind arc_kind, double rc, const std::int32_t *res,
                                      size_t ng_mem_size, size_t bucket_stored) {
        auto &c = counters[dir];
        auto visited = slot_visited.get() + slot * words_per_slot;
        const auto word = static_cast<size_t>(vertex) / 64;
        const auto bit = std::uint64_t{1} << (vertex % 64);
        std::uint16_t n_arcs = 1, n_repeats = 0;
        std::uint64_t pred_id = NO_LABEL_ID;
        if (pred_slot == NO_SLOT) {
            std::fill_n(visited, words_per_slot, 0);
        } else {
            std::copy_n(slot_visited.get() + pred_slot * words_per_slot, words_per_slot, visited);
            n_arcs = slot_n_arcs[pred_slot] + 1;
            n_repeats = slot_n_repeats[pred_slot] + ((visited[word] & bit) != 0);
            pred_id = slot_id[pred_slot];
        }
        visited[word] |= bit;

        const auto id = next_label_id++;
        slot_id[slot] = id;
        slot_n_arcs[slot] = n_arcs;
        slot_n_repeats[slot] = n_repeats;
        slot_ext_state[slot] = LabelExtState::NEVER_POPPED;
        slot_skip_dominator[slot] = NO_LABEL_ID;

        const auto idx = bucketIndex(vertex, bucket);
        LabelFeatureRecord record{};
        record.label_id = id;
        record.pred_id = pred_id;
        record.call_id = call_id;
        record.vertex = static_cast<std::uint16_t>(vertex);
        record.pred_vertex = static_cast<std::uint16_t>(pred_vertex);
        record.bucket = static_cast<std::uint16_t>(bucket);
        record.dir = dir;
        record.attempt = attempt;
        record.arc_kind = static_cast<std::uint8_t>(arc_kind);
        record.rc = rc;
        std::copy_n(res, NUM_RESOURCE, record.res);
        record.n_arcs = n_arcs;
        record.n_customers = static_cast<std::uint16_t>(n_arcs - n_repeats);
        record.n_repeats = n_repeats;
        record.ng_mem_size = static_cast<std::uint16_t>(ng_mem_size);
        record.dir_created = c.created;
        record.dir_open = c.open;
        record.dir_closed = c.closed;
        record.dir_rejected = c.rejected;
        record.dir_removed = c.removed;
        record.bucket_stored = static_cast<std::uint32_t>(bucket_stored);
        record.bucket_open = bucket_open[dir][idx];
        record.bucket_closed = bucket_closed[dir][idx];
        features.push(record);
        ++c.created;
        ++call_features;
    }

    inline void LabelLogger::writeOutcome(size_t slot, LabelOutcome outcome, std::uint64_t dominator_id) {
        outcomes.push({
            slot_id[slot], dominator_id, slot_skip_dominator[slot], static_cast<std::uint8_t>(outcome),
            static_cast<std::uint8_t>(slot_ext_state[slot])
        });
        ++call_outcomes;
    }

    inline void LabelLogger::onRejected(bool dir, size_t slot, size_t dominator_slot) {
        ++counters[dir].rejected;
        writeOutcome(slot, LabelOutcome::REJECTED_AT_INSERTION, slot_id[dominator_slot]);
    }

    inline void LabelLogger::onInserted(bool dir, int vertex, int bucket) {
        ++counters[dir].open;
        ++bucket_open[dir][bucketIndex(vertex, bucket)];
    }

    inline void LabelLogger::onRemoved(bool dir, size_t slot, int vertex, int bucket, size_t dominator_slot) {
        auto &c = counters[dir];
        const auto idx = bucketIndex(vertex, bucket);
        if (slot_ext_state[slot] == LabelExtState::NEVER_POPPED) {
            --c.open;
            --bucket_open[dir][idx];
        } else {
            --c.closed;
            --bucket_closed[dir][idx];
        }
        ++c.removed;
        writeOutcome(slot, LabelOutcome::REMOVED_BY_NEWER, slot_id[dominator_slot]);
    }

    inline void LabelLogger::onPopped(bool dir, size_t slot, int vertex, int bucket, bool if_extended,
                                      size_t skip_dominator_slot) {
        auto &c = counters[dir];
        const auto idx = bucketIndex(vertex, bucket);
        if (if_extended) {
            slot_ext_state[slot] = LabelExtState::EXTENDED;
        } else {
            slot_ext_state[slot] = LabelExtState::SKIPPED_DOMINATED;
            slot_skip_dominator[slot] = slot_id[skip_dominator_slot];
        }
        --c.open;
        ++c.closed;
        --bucket_open[dir][idx];
        ++bucket_closed[dir][idx];
    }

    inline void LabelLogger::onFinal(size_t slot, LabelOutcome outcome) {
        writeOutcome(slot, outcome, NO_LABEL_ID);
    }
}

#endif // ROUTE_OPT_LABEL_LOGGER_HPP
