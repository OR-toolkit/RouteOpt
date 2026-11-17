/*
 * Copyright (c) 2025 Zhengzhong (Ricky) You.
 * All rights reserved.
 * Software: RouteOpt
 * License: GPL-3.0
 */

#include "cvrp_pricing_controller.hpp"

namespace RouteOpt::Application::CVRP {
    void CVRP_Pricing::configureLabelLog(const std::string &instance_name, const std::string &instance_path,
                                         int argc, char *argv[]) {
        label_logger.configure(instance_name, instance_path, argc, argv, dim);
        label_logger.resizeSlots(label_assign);
    }

    void CVRP_Pricing::beginLabelLogCall(PRICING_LEVEL pricing_level) {
        label_logger.beginCall(pricing_level, num_buckets_per_vertex, step_size, meet_point_resource_in_bi_dir);
    }

    void CVRP_Pricing::finishLabelLogLabels(LabelOutcome outcome) {
        for (bool dir: {true, false}) {
            if (!label_logger.isStarted(dir)) continue;
            auto label_array = dir ? label_array_in_forward_sense : label_array_in_backward_sense;
            for (int i = 0; i < dim; ++i) {
                for (int b = 0; b < num_buckets_per_vertex; ++b) {
                    for (auto label: label_array[i][b]) label_logger.onFinal(label - all_label, outcome);
                }
            }
        }
    }

    void CVRP_Pricing::abortLabelLogAttempt() {
        if (!label_logger.isActive()) return;
        // must run before reallocateLabel frees the labels
        finishLabelLogLabels(LabelOutcome::DISCARDED_BY_RESTART);
        label_logger.abortAttempt();
    }

    void CVRP_Pricing::endLabelLogCall(bool if_finished) {
        if (!label_logger.isActive()) return;
        finishLabelLogLabels(LabelOutcome::ALIVE_AT_END);
        size_t num_meet_point_extensions = 0;
        if (label_logger.isStarted(true)) {
            for (auto &[arc, labels]: concatenate_labels_in_forward_cg) num_meet_point_extensions += labels.size();
        }
        if (label_logger.isStarted(false)) {
            for (auto &[arc, labels]: concatenate_labels_in_backward_cg) num_meet_point_extensions += labels.size();
        }
        label_logger.endCall(if_finished, num_meet_point_extensions);
    }
}
