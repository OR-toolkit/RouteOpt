/*
 * Copyright (c) 2025 Zhengzhong (Ricky) You.
 * All rights reserved.
 * Software: RouteOpt
 * License: GPL-3.0
 */

#ifndef ROUTE_OPT_LABEL_LOGGING_HPP
#define ROUTE_OPT_LABEL_LOGGING_HPP
#include "cvrp_pricing_controller.hpp"

namespace RouteOpt::Application::CVRP {
    static_assert(std::is_same_v<res_int, std::int32_t>, "label log stores resources as int32");

    /**
     * Hooks between the labeling code and LabelLogger; every call site is guarded by if constexpr (LABEL_LOG)
     * and none of them changes labels, buckets or the order of extension
     */
    template<bool dir>
    void CVRP_Pricing::beginLabelLogDirection() {
        label_logger.beginDirection(dir);
    }

    template<bool dir>
    void CVRP_Pricing::logInitialLabel(int slot, int vertex, int bj) {
        if (!label_logger.isActive()) return;
        const auto label = all_label + slot;
        const auto &label_list = dir ? label_array_in_forward_sense[vertex][bj] : label_array_in_backward_sense[vertex][bj];
        label_logger.onCreate(dir, slot, NO_SLOT, vertex, 0, bj, LabelArcKind::INITIAL, label->rc,
                              label->res.resources.data(), label->pi.count(), label_list.size());
        label_logger.onInserted(dir, vertex, bj);
    }

    template<bool dir>
    void CVRP_Pricing::logLabelCreated(Label *ki, int j, int bj, LabelArcKind arc_kind) {
        if (!label_logger.isActive()) return;
        const auto new_label = all_label + idx_glo;
        const auto &label_list = dir ? label_array_in_forward_sense[j][bj] : label_array_in_backward_sense[j][bj];
        label_logger.onCreate(dir, idx_glo, ki - all_label, j, ki->end_vertex, bj, arc_kind, new_label->rc,
                              new_label->res.resources.data(), new_label->pi.count(), label_list.size());
    }

    template<bool dir>
    void CVRP_Pricing::logLabelRejected(Label *dominator) {
        if (!label_logger.isActive()) return;
        label_logger.onRejected(dir, idx_glo, dominator - all_label);
    }

    template<bool dir>
    void CVRP_Pricing::logLabelInserted(int j, int bj) {
        if (!label_logger.isActive()) return;
        label_logger.onInserted(dir, j, bj);
    }

    template<bool dir>
    void CVRP_Pricing::logLabelRemoved(Label *kj, int j, int bj) {
        if (!label_logger.isActive()) return;
        label_logger.onRemoved(dir, kj - all_label, j, bj, idx_glo);
    }

    template<bool dir>
    void CVRP_Pricing::logLabelPopped(Label *ki, int i, int b, bool if_suc) {
        if (!label_logger.isActive()) return;
        label_logger.onPopped(dir, ki - all_label, i, b, if_suc,
                              if_suc ? NO_SLOT : static_cast<size_t>(label_pop_dominator - all_label));
    }
}

#endif // ROUTE_OPT_LABEL_LOGGING_HPP
