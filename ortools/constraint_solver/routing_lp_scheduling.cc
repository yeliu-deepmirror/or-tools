// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/constraint_solver/routing_lp_scheduling.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "ortools/base/logging.h"
#include "ortools/base/mathutil.h"
#include "ortools/constraint_solver/constraint_solver.h"
#include "ortools/constraint_solver/routing.h"
#include "ortools/constraint_solver/routing_parameters.pb.h"
#include "ortools/glop/parameters.pb.h"
#include "ortools/graph/min_cost_flow.h"
#include "ortools/util/saturated_arithmetic.h"
#include "ortools/util/sorted_interval_list.h"

namespace operations_research {

namespace {

// The following sets of parameters give the fastest response time without
// impacting solutions found negatively.
glop::GlopParameters GetGlopParametersForLocalLP() {
  glop::GlopParameters parameters;
  parameters.set_use_dual_simplex(true);
  parameters.set_use_preprocessing(false);
  return parameters;
}

glop::GlopParameters GetGlopParametersForGlobalLP() {
  glop::GlopParameters parameters;
  parameters.set_use_dual_simplex(true);
  return parameters;
}

bool GetCumulBoundsWithOffset(const RoutingDimension& dimension,
                              int64_t node_index, int64_t cumul_offset,
                              int64_t* lower_bound, int64_t* upper_bound) {
  DCHECK(lower_bound != nullptr);
  DCHECK(upper_bound != nullptr);

  const IntVar& cumul_var = *dimension.CumulVar(node_index);
  *upper_bound = cumul_var.Max();
  if (*upper_bound < cumul_offset) {
    return false;
  }

  const int64_t first_after_offset =
      std::max(dimension.GetFirstPossibleGreaterOrEqualValueForNode(
                   node_index, cumul_offset),
               cumul_var.Min());
  DCHECK_LT(first_after_offset, std::numeric_limits<int64_t>::max());
  *lower_bound = CapSub(first_after_offset, cumul_offset);
  DCHECK_GE(*lower_bound, 0);

  if (*upper_bound == std::numeric_limits<int64_t>::max()) {
    return true;
  }
  *upper_bound = CapSub(*upper_bound, cumul_offset);
  DCHECK_GE(*upper_bound, *lower_bound);
  return true;
}

int64_t GetFirstPossibleValueForCumulWithOffset(
    const RoutingDimension& dimension, int64_t node_index,
    int64_t lower_bound_without_offset, int64_t cumul_offset) {
  return CapSub(
      dimension.GetFirstPossibleGreaterOrEqualValueForNode(
          node_index, CapAdd(lower_bound_without_offset, cumul_offset)),
      cumul_offset);
}

int64_t GetLastPossibleValueForCumulWithOffset(
    const RoutingDimension& dimension, int64_t node_index,
    int64_t upper_bound_without_offset, int64_t cumul_offset) {
  return CapSub(
      dimension.GetLastPossibleLessOrEqualValueForNode(
          node_index, CapAdd(upper_bound_without_offset, cumul_offset)),
      cumul_offset);
}

// Finds the pickup/delivery pairs of nodes on a given vehicle's route.
// Returns the vector of visited pair indices, and stores the corresponding
// pickup/delivery indices in visited_pickup_delivery_indices_for_pair_.
// NOTE: Supposes that visited_pickup_delivery_indices_for_pair is correctly
// sized and initialized to {-1, -1} for all pairs.
void StoreVisitedPickupDeliveryPairsOnRoute(
    const RoutingDimension& dimension, int vehicle,
    const std::function<int64_t(int64_t)>& next_accessor,
    std::vector<int>* visited_pairs,
    std::vector<std::pair<int64_t, int64_t>>*
        visited_pickup_delivery_indices_for_pair) {
  // visited_pickup_delivery_indices_for_pair must be all {-1, -1}.
  DCHECK_EQ(visited_pickup_delivery_indices_for_pair->size(),
            dimension.model()->GetPickupAndDeliveryPairs().size());
  DCHECK(std::all_of(visited_pickup_delivery_indices_for_pair->begin(),
                     visited_pickup_delivery_indices_for_pair->end(),
                     [](std::pair<int64_t, int64_t> p) {
                       return p.first == -1 && p.second == -1;
                     }));
  visited_pairs->clear();
  if (!dimension.HasPickupToDeliveryLimits()) {
    return;
  }
  const RoutingModel& model = *dimension.model();

  int64_t node_index = model.Start(vehicle);
  while (!model.IsEnd(node_index)) {
    const std::vector<std::pair<int, int>>& pickup_index_pairs =
        model.GetPickupIndexPairs(node_index);
    const std::vector<std::pair<int, int>>& delivery_index_pairs =
        model.GetDeliveryIndexPairs(node_index);
    if (!pickup_index_pairs.empty()) {
      // The current node is a pickup. We verify that it belongs to a single
      // pickup index pair and that it's not a delivery, and store the index.
      DCHECK(delivery_index_pairs.empty());
      DCHECK_EQ(pickup_index_pairs.size(), 1);
      (*visited_pickup_delivery_indices_for_pair)[pickup_index_pairs[0].first]
          .first = node_index;
      visited_pairs->push_back(pickup_index_pairs[0].first);
    } else if (!delivery_index_pairs.empty()) {
      // The node is a delivery. We verify that it belongs to a single
      // delivery pair, and set the limit with its pickup if one has been
      // visited for this pair.
      DCHECK_EQ(delivery_index_pairs.size(), 1);
      const int pair_index = delivery_index_pairs[0].first;
      std::pair<int64_t, int64_t>& pickup_delivery_index =
          (*visited_pickup_delivery_indices_for_pair)[pair_index];
      if (pickup_delivery_index.first < 0) {
        // This case should not happen, as a delivery must have its pickup
        // on the route, but we ignore it here.
        node_index = next_accessor(node_index);
        continue;
      }
      pickup_delivery_index.second = node_index;
    }
    node_index = next_accessor(node_index);
  }
}

}  // namespace

// LocalDimensionCumulOptimizer

LocalDimensionCumulOptimizer::LocalDimensionCumulOptimizer(
    const RoutingDimension* dimension,
    RoutingSearchParameters::SchedulingSolver solver_type)
    : optimizer_core_(dimension, /*use_precedence_propagator=*/false) {
  // Using one solver per vehicle in the hope that if routes don't change this
  // will be faster.
  const int vehicles = dimension->model()->vehicles();
  solver_.resize(vehicles);
  switch (solver_type) {
    case RoutingSearchParameters::SCHEDULING_GLOP: {
      const glop::GlopParameters parameters = GetGlopParametersForLocalLP();
      for (int vehicle = 0; vehicle < vehicles; ++vehicle) {
        // TODO(user): Instead of passing false, detect if the relaxation
        // will always violate the MIPL constraints.
        solver_[vehicle] =
            std::make_unique<RoutingGlopWrapper>(false, parameters);
      }
      break;
    }
    case RoutingSearchParameters::SCHEDULING_CP_SAT: {
      for (int vehicle = 0; vehicle < vehicles; ++vehicle) {
        solver_[vehicle] = std::make_unique<RoutingCPSatWrapper>();
      }
      break;
    }
    default:
      LOG(DFATAL) << "Unrecognized solver type: " << solver_type;
  }
}

DimensionSchedulingStatus LocalDimensionCumulOptimizer::ComputeRouteCumulCost(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    int64_t* optimal_cost) {
  return optimizer_core_.OptimizeSingleRoute(vehicle, next_accessor, {},
                                             solver_[vehicle].get(), nullptr,
                                             nullptr, optimal_cost, nullptr);
}

DimensionSchedulingStatus
LocalDimensionCumulOptimizer::ComputeRouteCumulCostWithoutFixedTransits(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    int64_t* optimal_cost_without_transits) {
  int64_t cost = 0;
  int64_t transit_cost = 0;
  const DimensionSchedulingStatus status = optimizer_core_.OptimizeSingleRoute(
      vehicle, next_accessor, {}, solver_[vehicle].get(), nullptr, nullptr,
      &cost, &transit_cost);
  if (status != DimensionSchedulingStatus::INFEASIBLE &&
      optimal_cost_without_transits != nullptr) {
    *optimal_cost_without_transits = CapSub(cost, transit_cost);
  }
  return status;
}

std::vector<DimensionSchedulingStatus> LocalDimensionCumulOptimizer::
    ComputeRouteCumulCostsForResourcesWithoutFixedTransits(
        int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
        const std::function<int64_t(int64_t, int64_t)>& transit_accessor,
        const std::vector<RoutingModel::ResourceGroup::Resource>& resources,
        const std::vector<int>& resource_indices, bool optimize_vehicle_costs,
        std::vector<int64_t>* optimal_costs_without_transits,
        std::vector<std::vector<int64_t>>* optimal_cumuls,
        std::vector<std::vector<int64_t>>* optimal_breaks) {
  return optimizer_core_.OptimizeSingleRouteWithResources(
      vehicle, next_accessor, transit_accessor, {}, resources, resource_indices,
      optimize_vehicle_costs, solver_[vehicle].get(),
      optimal_costs_without_transits, optimal_cumuls, optimal_breaks);
}

DimensionSchedulingStatus LocalDimensionCumulOptimizer::ComputeRouteCumuls(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    std::vector<int64_t>* optimal_cumuls,
    std::vector<int64_t>* optimal_breaks) {
  return optimizer_core_.OptimizeSingleRoute(
      vehicle, next_accessor, solver_[vehicle].get(), optimal_cumuls,
      optimal_breaks, nullptr, nullptr);
}

DimensionSchedulingStatus
LocalDimensionCumulOptimizer::ComputePackedRouteCumuls(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    const RoutingModel::ResourceGroup::Resource* resource,
    std::vector<int64_t>* packed_cumuls, std::vector<int64_t>* packed_breaks) {
  return optimizer_core_.OptimizeAndPackSingleRoute(
      vehicle, next_accessor, resource, solver_[vehicle].get(), packed_cumuls,
      packed_breaks);
}

const int CumulBoundsPropagator::kNoParent = -2;
const int CumulBoundsPropagator::kParentToBePropagated = -1;

CumulBoundsPropagator::CumulBoundsPropagator(const RoutingDimension* dimension)
    : dimension_(*dimension), num_nodes_(2 * dimension->cumuls().size()) {
  outgoing_arcs_.resize(num_nodes_);
  node_in_queue_.resize(num_nodes_, false);
  tree_parent_node_of_.resize(num_nodes_, kNoParent);
  propagated_bounds_.resize(num_nodes_);
  visited_pickup_delivery_indices_for_pair_.resize(
      dimension->model()->GetPickupAndDeliveryPairs().size(), {-1, -1});
}

void CumulBoundsPropagator::AddArcs(int first_index, int second_index,
                                    int64_t offset) {
  // Add arc first_index + offset <= second_index
  outgoing_arcs_[PositiveNode(first_index)].push_back(
      {PositiveNode(second_index), offset});
  AddNodeToQueue(PositiveNode(first_index));
  // Add arc -second_index + transit <= -first_index
  outgoing_arcs_[NegativeNode(second_index)].push_back(
      {NegativeNode(first_index), offset});
  AddNodeToQueue(NegativeNode(second_index));
}

bool CumulBoundsPropagator::InitializeArcsAndBounds(
    const std::function<int64_t(int64_t)>& next_accessor,
    int64_t cumul_offset) {
  propagated_bounds_.assign(num_nodes_, std::numeric_limits<int64_t>::min());

  for (std::vector<ArcInfo>& arcs : outgoing_arcs_) {
    arcs.clear();
  }

  RoutingModel* const model = dimension_.model();
  std::vector<int64_t>& lower_bounds = propagated_bounds_;

  for (int vehicle = 0; vehicle < model->vehicles(); vehicle++) {
    const std::function<int64_t(int64_t, int64_t)>& transit_accessor =
        dimension_.transit_evaluator(vehicle);

    int node = model->Start(vehicle);
    while (true) {
      int64_t cumul_lb, cumul_ub;
      if (!GetCumulBoundsWithOffset(dimension_, node, cumul_offset, &cumul_lb,
                                    &cumul_ub)) {
        return false;
      }
      lower_bounds[PositiveNode(node)] = cumul_lb;
      if (cumul_ub < std::numeric_limits<int64_t>::max()) {
        lower_bounds[NegativeNode(node)] = -cumul_ub;
      }

      if (model->IsEnd(node)) {
        break;
      }

      const int next = next_accessor(node);
      const int64_t transit = transit_accessor(node, next);
      const IntVar& slack_var = *dimension_.SlackVar(node);
      // node + transit + slack_var == next
      // Add arcs for node + transit + slack_min <= next
      AddArcs(node, next, CapAdd(transit, slack_var.Min()));
      if (slack_var.Max() < std::numeric_limits<int64_t>::max()) {
        // Add arcs for node + transit + slack_max >= next.
        AddArcs(next, node, CapSub(-slack_var.Max(), transit));
      }

      node = next;
    }

    // Add vehicle span upper bound: end - span_ub <= start.
    const int64_t span_ub = dimension_.GetSpanUpperBoundForVehicle(vehicle);
    if (span_ub < std::numeric_limits<int64_t>::max()) {
      AddArcs(model->End(vehicle), model->Start(vehicle), -span_ub);
    }

    // Set pickup/delivery limits on route.
    std::vector<int> visited_pairs;
    StoreVisitedPickupDeliveryPairsOnRoute(
        dimension_, vehicle, next_accessor, &visited_pairs,
        &visited_pickup_delivery_indices_for_pair_);
    for (int pair_index : visited_pairs) {
      const int64_t pickup_index =
          visited_pickup_delivery_indices_for_pair_[pair_index].first;
      const int64_t delivery_index =
          visited_pickup_delivery_indices_for_pair_[pair_index].second;
      visited_pickup_delivery_indices_for_pair_[pair_index] = {-1, -1};

      DCHECK_GE(pickup_index, 0);
      if (delivery_index < 0) {
        // We didn't encounter a delivery for this pickup.
        continue;
      }

      const int64_t limit = dimension_.GetPickupToDeliveryLimitForPair(
          pair_index, model->GetPickupIndexPairs(pickup_index)[0].second,
          model->GetDeliveryIndexPairs(delivery_index)[0].second);
      if (limit < std::numeric_limits<int64_t>::max()) {
        // delivery_cumul - limit  <= pickup_cumul.
        AddArcs(delivery_index, pickup_index, -limit);
      }
    }
  }

  for (const RoutingDimension::NodePrecedence& precedence :
       dimension_.GetNodePrecedences()) {
    const int first_index = precedence.first_node;
    const int second_index = precedence.second_node;
    if (lower_bounds[PositiveNode(first_index)] ==
            std::numeric_limits<int64_t>::min() ||
        lower_bounds[PositiveNode(second_index)] ==
            std::numeric_limits<int64_t>::min()) {
      // One of the nodes is unperformed, so the precedence rule doesn't apply.
      continue;
    }
    AddArcs(first_index, second_index, precedence.offset);
  }

  return true;
}

bool CumulBoundsPropagator::UpdateCurrentLowerBoundOfNode(int node,
                                                          int64_t new_lb,
                                                          int64_t offset) {
  const int cumul_var_index = node / 2;

  if (node == PositiveNode(cumul_var_index)) {
    // new_lb is a lower bound of the cumul of variable 'cumul_var_index'.
    propagated_bounds_[node] = GetFirstPossibleValueForCumulWithOffset(
        dimension_, cumul_var_index, new_lb, offset);
  } else {
    // -new_lb is an upper bound of the cumul of variable 'cumul_var_index'.
    const int64_t new_ub = CapSub(0, new_lb);
    propagated_bounds_[node] =
        CapSub(0, GetLastPossibleValueForCumulWithOffset(
                      dimension_, cumul_var_index, new_ub, offset));
  }

  // Test that the lower/upper bounds do not cross each other.
  const int64_t cumul_lower_bound =
      propagated_bounds_[PositiveNode(cumul_var_index)];

  const int64_t negated_cumul_upper_bound =
      propagated_bounds_[NegativeNode(cumul_var_index)];

  return CapAdd(negated_cumul_upper_bound, cumul_lower_bound) <= 0;
}

bool CumulBoundsPropagator::DisassembleSubtree(int source, int target) {
  tmp_dfs_stack_.clear();
  tmp_dfs_stack_.push_back(source);
  while (!tmp_dfs_stack_.empty()) {
    const int tail = tmp_dfs_stack_.back();
    tmp_dfs_stack_.pop_back();
    for (const ArcInfo& arc : outgoing_arcs_[tail]) {
      const int child_node = arc.head;
      if (tree_parent_node_of_[child_node] != tail) continue;
      if (child_node == target) return false;
      tree_parent_node_of_[child_node] = kParentToBePropagated;
      tmp_dfs_stack_.push_back(child_node);
    }
  }
  return true;
}

bool CumulBoundsPropagator::PropagateCumulBounds(
    const std::function<int64_t(int64_t)>& next_accessor,
    int64_t cumul_offset) {
  tree_parent_node_of_.assign(num_nodes_, kNoParent);
  DCHECK(std::none_of(node_in_queue_.begin(), node_in_queue_.end(),
                      [](bool b) { return b; }));
  DCHECK(bf_queue_.empty());

  if (!InitializeArcsAndBounds(next_accessor, cumul_offset)) {
    return CleanupAndReturnFalse();
  }

  std::vector<int64_t>& current_lb = propagated_bounds_;

  // Bellman-Ford-Tarjan algorithm.
  while (!bf_queue_.empty()) {
    const int node = bf_queue_.front();
    bf_queue_.pop_front();
    node_in_queue_[node] = false;

    if (tree_parent_node_of_[node] == kParentToBePropagated) {
      // The parent of this node is still in the queue, so no need to process
      // node now, since it will be re-enqued when its parent is processed.
      continue;
    }

    const int64_t lower_bound = current_lb[node];
    for (const ArcInfo& arc : outgoing_arcs_[node]) {
      // NOTE: kint64min as a lower bound means no lower bound at all, so we
      // don't use this value to propagate.
      const int64_t induced_lb =
          (lower_bound == std::numeric_limits<int64_t>::min())
              ? std::numeric_limits<int64_t>::min()
              : CapAdd(lower_bound, arc.offset);

      const int head_node = arc.head;
      if (induced_lb <= current_lb[head_node]) {
        // No update necessary for the head_node, continue to next children of
        // node.
        continue;
      }
      if (!UpdateCurrentLowerBoundOfNode(head_node, induced_lb, cumul_offset) ||
          !DisassembleSubtree(head_node, node)) {
        // The new lower bound is infeasible, or a positive cycle was detected
        // in the precedence graph by DisassembleSubtree().
        return CleanupAndReturnFalse();
      }

      tree_parent_node_of_[head_node] = node;
      AddNodeToQueue(head_node);
    }
  }
  return true;
}

DimensionCumulOptimizerCore::DimensionCumulOptimizerCore(
    const RoutingDimension* dimension, bool use_precedence_propagator)
    : dimension_(dimension),
      visited_pickup_delivery_indices_for_pair_(
          dimension->model()->GetPickupAndDeliveryPairs().size(), {-1, -1}) {
  if (use_precedence_propagator) {
    propagator_ = std::make_unique<CumulBoundsPropagator>(dimension);
  }
  const RoutingModel& model = *dimension_->model();
  if (dimension_->HasBreakConstraints()) {
    // Initialize vehicle_to_first_index_ so the variables of the breaks of
    // vehicle v are stored from vehicle_to_first_index_[v] to
    // vehicle_to_first_index_[v+1] - 1.
    const int num_vehicles = model.vehicles();
    vehicle_to_all_break_variables_offset_.reserve(num_vehicles);
    int num_break_vars = 0;
    for (int vehicle = 0; vehicle < num_vehicles; ++vehicle) {
      vehicle_to_all_break_variables_offset_.push_back(num_break_vars);
      const auto& intervals = dimension_->GetBreakIntervalsOfVehicle(vehicle);
      num_break_vars += 2 * intervals.size();  // 2 variables per break.
    }
    all_break_variables_.resize(num_break_vars, -1);
  }
  if (!model.GetDimensionResourceGroupIndices(dimension_).empty()) {
    resource_group_to_resource_to_vehicle_assignment_variables_.resize(
        model.GetResourceGroups().size());
  }
}

DimensionSchedulingStatus DimensionCumulOptimizerCore::OptimizeSingleRoute(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    RoutingLinearSolverWrapper* solver, std::vector<int64_t>* cumul_values,
    std::vector<int64_t>* break_values, int64_t* cost, int64_t* transit_cost,
    bool clear_lp) {
  InitOptimizer(solver);
  // Make sure SetRouteCumulConstraints will properly set the cumul bounds by
  // looking at this route only.
  DCHECK_EQ(propagator_.get(), nullptr);

  RoutingModel* const model = dimension()->model();
  const bool optimize_vehicle_costs =
      (cumul_values != nullptr || cost != nullptr) &&
      (!model->IsEnd(next_accessor(model->Start(vehicle))) ||
       model->IsVehicleUsedWhenEmpty(vehicle));
  const int64_t cumul_offset =
      dimension_->GetLocalOptimizerOffsetForVehicle(vehicle);
  int64_t cost_offset = 0;
  if (!SetRouteCumulConstraints(vehicle, next_accessor,
                                dimension_->transit_evaluator(vehicle),
                                cumul_offset, optimize_vehicle_costs, solver,
                                transit_cost, &cost_offset)) {
    return DimensionSchedulingStatus::INFEASIBLE;
  }
  if (model->CheckLimit()) {
    return DimensionSchedulingStatus::INFEASIBLE;
  }
  const DimensionSchedulingStatus status =
      solver->Solve(model->RemainingTime());
  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    solver->Clear();
    return status;
  }

  SetValuesFromLP(current_route_cumul_variables_, cumul_offset, solver,
                  cumul_values);
  SetValuesFromLP(current_route_break_variables_, cumul_offset, solver,
                  break_values);
  if (cost != nullptr) {
    *cost = CapAdd(cost_offset, solver->GetObjectiveValue());
  }

  if (clear_lp) {
    solver->Clear();
  }
  return status;
}

namespace {

using ResourceGroup = RoutingModel::ResourceGroup;

bool GetDomainOffsetBounds(const Domain& domain, int64_t offset,
                           ClosedInterval* interval) {
  const int64_t lower_bound =
      std::max<int64_t>(CapSub(domain.Min(), offset), 0);
  const int64_t upper_bound =
      domain.Max() == std::numeric_limits<int64_t>::max()
          ? std::numeric_limits<int64_t>::max()
          : CapSub(domain.Max(), offset);
  if (lower_bound > upper_bound) return false;

  *interval = ClosedInterval(lower_bound, upper_bound);
  return true;
}

bool GetIntervalIntersectionWithOffsetDomain(const ClosedInterval& interval,
                                             const Domain& domain,
                                             int64_t offset,
                                             ClosedInterval* intersection) {
  ClosedInterval domain_bounds;
  if (!GetDomainOffsetBounds(domain, offset, &domain_bounds)) {
    return false;
  }
  const int64_t intersection_lb = std::max(interval.start, domain_bounds.start);
  const int64_t intersection_ub = std::min(interval.end, domain_bounds.end);
  if (intersection_lb > intersection_ub) return false;

  *intersection = ClosedInterval(intersection_lb, intersection_ub);
  return true;
}

ClosedInterval GetVariableBounds(int index,
                                 const RoutingLinearSolverWrapper& solver) {
  return ClosedInterval(solver.GetVariableLowerBound(index),
                        solver.GetVariableUpperBound(index));
}

bool TightenStartEndVariableBoundsWithResource(
    const RoutingDimension& dimension, const ResourceGroup::Resource& resource,
    const ClosedInterval& start_bounds, int start_index,
    const ClosedInterval& end_bounds, int end_index, int64_t offset,
    RoutingLinearSolverWrapper* solver) {
  const ResourceGroup::Attributes& attributes =
      resource.GetDimensionAttributes(&dimension);
  ClosedInterval new_start_bounds;
  ClosedInterval new_end_bounds;
  return GetIntervalIntersectionWithOffsetDomain(start_bounds,
                                                 attributes.start_domain(),
                                                 offset, &new_start_bounds) &&
         solver->SetVariableBounds(start_index, new_start_bounds.start,
                                   new_start_bounds.end) &&
         GetIntervalIntersectionWithOffsetDomain(
             end_bounds, attributes.end_domain(), offset, &new_end_bounds) &&
         solver->SetVariableBounds(end_index, new_end_bounds.start,
                                   new_end_bounds.end);
}

}  // namespace

std::vector<DimensionSchedulingStatus>
DimensionCumulOptimizerCore::OptimizeSingleRouteWithResources(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    const std::function<int64_t(int64_t, int64_t)>& transit_accessor,
    const std::vector<RoutingModel::ResourceGroup::Resource>& resources,
    const std::vector<int>& resource_indices, bool optimize_vehicle_costs,
    RoutingLinearSolverWrapper* solver,
    std::vector<int64_t>* costs_without_transits,
    std::vector<std::vector<int64_t>>* cumul_values,
    std::vector<std::vector<int64_t>>* break_values, bool clear_lp) {
  if (resource_indices.empty()) return {};

  InitOptimizer(solver);
  // Make sure SetRouteCumulConstraints will properly set the cumul bounds by
  // looking at this route only.
  DCHECK_EQ(propagator_.get(), nullptr);
  DCHECK_NE(costs_without_transits, nullptr);
  costs_without_transits->clear();

  RoutingModel* const model = dimension()->model();
  if (model->IsEnd(next_accessor(model->Start(vehicle))) &&
      !model->IsVehicleUsedWhenEmpty(vehicle)) {
    // An unused empty vehicle doesn't require resources.
    return {};
  }

  const int64_t cumul_offset =
      dimension_->GetLocalOptimizerOffsetForVehicle(vehicle);
  int64_t cost_offset = 0;
  int64_t transit_cost = 0;
  if (!SetRouteCumulConstraints(vehicle, next_accessor, transit_accessor,
                                cumul_offset, optimize_vehicle_costs, solver,
                                &transit_cost, &cost_offset)) {
    return {DimensionSchedulingStatus::INFEASIBLE};
  }

  costs_without_transits->assign(resource_indices.size(), -1);
  if (cumul_values != nullptr) {
    cumul_values->assign(resource_indices.size(), {});
  }
  if (break_values != nullptr) {
    break_values->assign(resource_indices.size(), {});
  }

  DCHECK_GE(current_route_cumul_variables_.size(), 2);

  const int start_cumul = current_route_cumul_variables_[0];
  const ClosedInterval start_bounds = GetVariableBounds(start_cumul, *solver);
  const int end_cumul = current_route_cumul_variables_.back();
  const ClosedInterval end_bounds = GetVariableBounds(end_cumul, *solver);
  std::vector<DimensionSchedulingStatus> statuses;
  for (int i = 0; i < resource_indices.size(); i++) {
    if (model->CheckLimit()) {
      // The model's deadline has been reached, stop.
      costs_without_transits->clear();
      if (cumul_values != nullptr) {
        cumul_values->clear();
      }
      if (break_values != nullptr) {
        break_values->clear();
      }
      return {};
    }
    if (!TightenStartEndVariableBoundsWithResource(
            *dimension_, resources[resource_indices[i]], start_bounds,
            start_cumul, end_bounds, end_cumul, cumul_offset, solver)) {
      // The resource attributes don't match this vehicle.
      statuses.push_back(DimensionSchedulingStatus::INFEASIBLE);
      continue;
    }

    statuses.push_back(solver->Solve(model->RemainingTime()));
    if (statuses.back() == DimensionSchedulingStatus::INFEASIBLE) {
      continue;
    }
    costs_without_transits->at(i) =
        optimize_vehicle_costs
            ? CapSub(CapAdd(cost_offset, solver->GetObjectiveValue()),
                     transit_cost)
            : 0;

    if (cumul_values != nullptr) {
      SetValuesFromLP(current_route_cumul_variables_, cumul_offset, solver,
                      &cumul_values->at(i));
    }
    if (break_values != nullptr) {
      SetValuesFromLP(current_route_break_variables_, cumul_offset, solver,
                      &break_values->at(i));
    }
  }

  if (clear_lp) {
    solver->Clear();
  }
  return statuses;
}

DimensionSchedulingStatus DimensionCumulOptimizerCore::Optimize(
    const std::function<int64_t(int64_t)>& next_accessor,
    RoutingLinearSolverWrapper* solver, std::vector<int64_t>* cumul_values,
    std::vector<int64_t>* break_values,
    std::vector<std::vector<int>>* resource_indices_per_group, int64_t* cost,
    int64_t* transit_cost, bool clear_lp) {
  InitOptimizer(solver);

  // If both "cumul_values" and "cost" parameters are null, we don't try to
  // optimize the cost and stop at the first feasible solution.
  const bool optimize_costs = (cumul_values != nullptr) || (cost != nullptr);
  bool has_vehicles_being_optimized = false;

  const int64_t cumul_offset = dimension_->GetGlobalOptimizerOffset();

  if (propagator_ != nullptr &&
      !propagator_->PropagateCumulBounds(next_accessor, cumul_offset)) {
    return DimensionSchedulingStatus::INFEASIBLE;
  }

  int64_t total_transit_cost = 0;
  int64_t total_cost_offset = 0;
  const RoutingModel* model = dimension()->model();
  for (int vehicle = 0; vehicle < model->vehicles(); vehicle++) {
    int64_t route_transit_cost = 0;
    int64_t route_cost_offset = 0;
    const bool vehicle_is_used =
        !model->IsEnd(next_accessor(model->Start(vehicle))) ||
        model->IsVehicleUsedWhenEmpty(vehicle);
    const bool optimize_vehicle_costs = optimize_costs && vehicle_is_used;
    if (!SetRouteCumulConstraints(vehicle, next_accessor,
                                  dimension_->transit_evaluator(vehicle),
                                  cumul_offset, optimize_vehicle_costs, solver,
                                  &route_transit_cost, &route_cost_offset)) {
      return DimensionSchedulingStatus::INFEASIBLE;
    }
    total_transit_cost = CapAdd(total_transit_cost, route_transit_cost);
    total_cost_offset = CapAdd(total_cost_offset, route_cost_offset);
    has_vehicles_being_optimized |= optimize_vehicle_costs;
  }
  if (transit_cost != nullptr) {
    *transit_cost = total_transit_cost;
  }

  if (!SetGlobalConstraints(next_accessor, cumul_offset,
                            has_vehicles_being_optimized, solver)) {
    return DimensionSchedulingStatus::INFEASIBLE;
  }

  const DimensionSchedulingStatus status =
      solver->Solve(model->RemainingTime());
  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    solver->Clear();
    return status;
  }

  // TODO(user): In case the status is RELAXED_OPTIMAL_ONLY, check we can
  // safely avoid filling variable and cost values.
  SetValuesFromLP(index_to_cumul_variable_, cumul_offset, solver, cumul_values);
  SetValuesFromLP(all_break_variables_, cumul_offset, solver, break_values);
  SetResourceIndices(solver, resource_indices_per_group);

  if (cost != nullptr) {
    *cost = CapAdd(solver->GetObjectiveValue(), total_cost_offset);
  }

  if (clear_lp) {
    solver->Clear();
  }
  return status;
}

DimensionSchedulingStatus DimensionCumulOptimizerCore::OptimizeAndPack(
    const std::function<int64_t(int64_t)>& next_accessor,
    RoutingLinearSolverWrapper* solver, std::vector<int64_t>* cumul_values,
    std::vector<int64_t>* break_values,
    std::vector<std::vector<int>>* resource_indices_per_group) {
  // Note: We pass a non-nullptr cost to the Optimize() method so the costs
  // are optimized by the solver.
  int64_t cost = 0;
  const glop::GlopParameters original_params = GetGlopParametersForGlobalLP();
  glop::GlopParameters packing_parameters;
  if (!solver->IsCPSATSolver()) {
    packing_parameters = original_params;
    packing_parameters.set_use_dual_simplex(false);
    packing_parameters.set_use_preprocessing(true);
    solver->SetParameters(packing_parameters.SerializeAsString());
  }
  DimensionSchedulingStatus status = DimensionSchedulingStatus::OPTIMAL;
  if (Optimize(next_accessor, solver, /*cumul_values=*/nullptr,
               /*break_values=*/nullptr,
               /*resource_indices_per_group=*/nullptr, &cost,
               /*transit_cost=*/nullptr,
               /*clear_lp=*/false) == DimensionSchedulingStatus::INFEASIBLE) {
    status = DimensionSchedulingStatus::INFEASIBLE;
  }
  if (status != DimensionSchedulingStatus::INFEASIBLE) {
    std::vector<int> vehicles(dimension()->model()->vehicles());
    std::iota(vehicles.begin(), vehicles.end(), 0);
    // Subtle: Even if the status was RELAXED_OPTIMAL_ONLY we try to pack just
    // in case packing manages to make the solution completely feasible.
    status = PackRoutes(vehicles, solver, packing_parameters);
  }
  if (!solver->IsCPSATSolver()) {
    solver->SetParameters(original_params.SerializeAsString());
  }
  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    return status;
  }
  // TODO(user): In case the status is RELAXED_OPTIMAL_ONLY, check we can
  // safely avoid filling variable values.
  const int64_t global_offset = dimension_->GetGlobalOptimizerOffset();
  SetValuesFromLP(index_to_cumul_variable_, global_offset, solver,
                  cumul_values);
  SetValuesFromLP(all_break_variables_, global_offset, solver, break_values);
  SetResourceIndices(solver, resource_indices_per_group);
  solver->Clear();
  return status;
}

DimensionSchedulingStatus
DimensionCumulOptimizerCore::OptimizeAndPackSingleRoute(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    const RoutingModel::ResourceGroup::Resource* resource,
    RoutingLinearSolverWrapper* solver, std::vector<int64_t>* cumul_values,
    std::vector<int64_t>* break_values) {
  const glop::GlopParameters original_params = GetGlopParametersForLocalLP();
  glop::GlopParameters packing_parameters;
  if (!solver->IsCPSATSolver()) {
    packing_parameters = original_params;
    packing_parameters.set_use_dual_simplex(false);
    packing_parameters.set_use_preprocessing(true);
    solver->SetParameters(packing_parameters.SerializeAsString());
  }
  DimensionSchedulingStatus status = DimensionSchedulingStatus::OPTIMAL;
  if (resource == nullptr) {
    // Note: We pass a non-nullptr cost to the OptimizeSingleRoute() method so
    // the costs are optimized by the LP.
    int64_t cost = 0;
    if (OptimizeSingleRoute(vehicle, next_accessor, solver,
                            /*cumul_values=*/nullptr,
                            /*break_values=*/nullptr, &cost,
                            /*transit_cost=*/nullptr, /*clear_lp=*/false) ==
        DimensionSchedulingStatus::INFEASIBLE) {
      status = DimensionSchedulingStatus::INFEASIBLE;
    }
  } else {
    std::vector<int64_t> costs_without_transits;
    const std::vector<DimensionSchedulingStatus> statuses =
        OptimizeSingleRouteWithResources(
            vehicle, next_accessor, dimension_->transit_evaluator(vehicle),
            {*resource}, {0}, /*optimize_vehicle_costs=*/true, solver,
            &costs_without_transits, /*cumul_values=*/nullptr,
            /*break_values=*/nullptr, /*clear_lp=*/false);
    DCHECK_EQ(statuses.size(), 1);
    if (statuses[0] == DimensionSchedulingStatus::INFEASIBLE) {
      status = DimensionSchedulingStatus::INFEASIBLE;
    }
  }

  if (status != DimensionSchedulingStatus::INFEASIBLE) {
    status = PackRoutes({vehicle}, solver, packing_parameters);
  }
  if (!solver->IsCPSATSolver()) {
    solver->SetParameters(original_params.SerializeAsString());
  }

  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    return DimensionSchedulingStatus::INFEASIBLE;
  }
  const int64_t local_offset =
      dimension_->GetLocalOptimizerOffsetForVehicle(vehicle);
  SetValuesFromLP(current_route_cumul_variables_, local_offset, solver,
                  cumul_values);
  SetValuesFromLP(current_route_break_variables_, local_offset, solver,
                  break_values);
  solver->Clear();
  return status;
}

DimensionSchedulingStatus DimensionCumulOptimizerCore::PackRoutes(
    std::vector<int> vehicles, RoutingLinearSolverWrapper* solver,
    const glop::GlopParameters& packing_parameters) {
  const RoutingModel* model = dimension_->model();

  // NOTE(user): Given our constraint matrix, our problem *should* always
  // have an integer optimal solution, in which case we can round to the nearest
  // integer both for the objective constraint bound (returned by
  // GetObjectiveValue()) and the end cumul variable bound after minimizing
  // (see b/154381899 showcasing an example where std::ceil leads to an
  // "imperfect" packing due to rounding precision errors).
  // If this DCHECK ever fails, it can be removed but the code below should be
  // adapted to have a 2-phase approach, solving once with the rounded value as
  // bound and if this fails, solve again using std::ceil.
  DCHECK(solver->SolutionIsInteger());

  // Minimize the route end times without increasing the cost.
  solver->AddObjectiveConstraint();
  solver->ClearObjective();
  for (int vehicle : vehicles) {
    solver->SetObjectiveCoefficient(
        index_to_cumul_variable_[model->End(vehicle)], 1);
  }

  glop::GlopParameters current_params;
  const auto retry_solving = [&current_params, model, solver]() {
    // NOTE: To bypass some cases of false negatives due to imprecisions, we try
    // running Glop with a different use_dual_simplex parameter when running
    // into an infeasible status.
    current_params.set_use_dual_simplex(!current_params.use_dual_simplex());
    solver->SetParameters(current_params.SerializeAsString());
    return solver->Solve(model->RemainingTime());
  };
  if (solver->Solve(model->RemainingTime()) ==
      DimensionSchedulingStatus::INFEASIBLE) {
    if (solver->IsCPSATSolver()) {
      return DimensionSchedulingStatus::INFEASIBLE;
    }

    current_params = packing_parameters;
    if (retry_solving() == DimensionSchedulingStatus::INFEASIBLE) {
      return DimensionSchedulingStatus::INFEASIBLE;
    }
  }

  // Maximize the route start times without increasing the cost or the route end
  // times.
  solver->ClearObjective();
  for (int vehicle : vehicles) {
    const int end_cumul_var = index_to_cumul_variable_[model->End(vehicle)];
    // end_cumul_var <= solver.GetValue(end_cumul_var)
    solver->SetVariableBounds(
        end_cumul_var, solver->GetVariableLowerBound(end_cumul_var),
        MathUtil::FastInt64Round(solver->GetValue(end_cumul_var)));

    // Maximize the starts of the routes.
    solver->SetObjectiveCoefficient(
        index_to_cumul_variable_[model->Start(vehicle)], -1);
  }

  DimensionSchedulingStatus status = solver->Solve(model->RemainingTime());
  if (!solver->IsCPSATSolver() &&
      status == DimensionSchedulingStatus::INFEASIBLE) {
    status = retry_solving();
  }
  return status;
}

#ifndef NDEBUG
#define SET_VARIABLE_NAME(solver, var, name) \
  do {                                       \
    solver->SetVariableName(var, name);      \
  } while (false)
#else
#define SET_VARIABLE_NAME(solver, var, name) \
  do {                                       \
  } while (false)
#endif

void DimensionCumulOptimizerCore::InitOptimizer(
    RoutingLinearSolverWrapper* solver) {
  solver->Clear();
  index_to_cumul_variable_.assign(dimension_->cumuls().size(), -1);
  max_end_cumul_ = solver->CreateNewPositiveVariable();
  SET_VARIABLE_NAME(solver, max_end_cumul_, "max_end_cumul");
  min_start_cumul_ = solver->CreateNewPositiveVariable();
  SET_VARIABLE_NAME(solver, min_start_cumul_, "min_start_cumul");
}

bool DimensionCumulOptimizerCore::ComputeRouteCumulBounds(
    const std::vector<int64_t>& route,
    const std::vector<int64_t>& fixed_transits, int64_t cumul_offset) {
  const int route_size = route.size();
  current_route_min_cumuls_.resize(route_size);
  current_route_max_cumuls_.resize(route_size);
  if (propagator_ != nullptr) {
    for (int pos = 0; pos < route_size; pos++) {
      const int64_t node = route[pos];
      current_route_min_cumuls_[pos] = propagator_->CumulMin(node);
      DCHECK_GE(current_route_min_cumuls_[pos], 0);
      current_route_max_cumuls_[pos] = propagator_->CumulMax(node);
      DCHECK_GE(current_route_max_cumuls_[pos], current_route_min_cumuls_[pos]);
    }
    return true;
  }

  // Extract cumul min/max and fixed transits from CP.
  for (int pos = 0; pos < route_size; ++pos) {
    if (!GetCumulBoundsWithOffset(*dimension_, route[pos], cumul_offset,
                                  &current_route_min_cumuls_[pos],
                                  &current_route_max_cumuls_[pos])) {
      return false;
    }
  }

  // Refine cumul bounds using
  // cumul[i+1] >= cumul[i] + fixed_transit[i] + slack[i].
  for (int pos = 1; pos < route_size; ++pos) {
    const int64_t slack_min = dimension_->SlackVar(route[pos - 1])->Min();
    current_route_min_cumuls_[pos] = std::max(
        current_route_min_cumuls_[pos],
        CapAdd(
            CapAdd(current_route_min_cumuls_[pos - 1], fixed_transits[pos - 1]),
            slack_min));
    current_route_min_cumuls_[pos] = GetFirstPossibleValueForCumulWithOffset(
        *dimension_, route[pos], current_route_min_cumuls_[pos], cumul_offset);
    if (current_route_min_cumuls_[pos] > current_route_max_cumuls_[pos]) {
      return false;
    }
  }

  for (int pos = route_size - 2; pos >= 0; --pos) {
    // If cumul_max[pos+1] is kint64max, it will be translated to
    // double +infinity, so it must not constrain cumul_max[pos].
    if (current_route_max_cumuls_[pos + 1] <
        std::numeric_limits<int64_t>::max()) {
      const int64_t slack_min = dimension_->SlackVar(route[pos])->Min();
      current_route_max_cumuls_[pos] = std::min(
          current_route_max_cumuls_[pos],
          CapSub(
              CapSub(current_route_max_cumuls_[pos + 1], fixed_transits[pos]),
              slack_min));
      current_route_max_cumuls_[pos] = GetLastPossibleValueForCumulWithOffset(
          *dimension_, route[pos], current_route_max_cumuls_[pos],
          cumul_offset);
      if (current_route_max_cumuls_[pos] < current_route_min_cumuls_[pos]) {
        return false;
      }
    }
  }
  return true;
}

bool DimensionCumulOptimizerCore::SetRouteCumulConstraints(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    const std::function<int64_t(int64_t, int64_t)>& transit_accessor,
    int64_t cumul_offset, bool optimize_costs,
    RoutingLinearSolverWrapper* solver, int64_t* route_transit_cost,
    int64_t* route_cost_offset) {
  RoutingModel* const model = dimension_->model();
  // Extract the vehicle's path from next_accessor.
  std::vector<int64_t> path;
  {
    int node = model->Start(vehicle);
    path.push_back(node);
    while (!model->IsEnd(node)) {
      node = next_accessor(node);
      path.push_back(node);
    }
    DCHECK_GE(path.size(), 2);
  }
  const int path_size = path.size();

  std::vector<int64_t> fixed_transit(path_size - 1);
  {
    for (int pos = 1; pos < path_size; ++pos) {
      fixed_transit[pos - 1] = transit_accessor(path[pos - 1], path[pos]);
    }
  }

  if (!ComputeRouteCumulBounds(path, fixed_transit, cumul_offset)) {
    return false;
  }

  // LP Model variables, current_route_cumul_variables_ and lp_slacks.
  // Create LP variables for cumuls.
  std::vector<int>& lp_cumuls = current_route_cumul_variables_;
  lp_cumuls.assign(path_size, -1);
  for (int pos = 0; pos < path_size; ++pos) {
    const int lp_cumul = solver->CreateNewPositiveVariable();
    SET_VARIABLE_NAME(solver, lp_cumul, absl::StrFormat("lp_cumul(%ld)", pos));
    index_to_cumul_variable_[path[pos]] = lp_cumul;
    lp_cumuls[pos] = lp_cumul;
    if (!solver->SetVariableBounds(lp_cumul, current_route_min_cumuls_[pos],
                                   current_route_max_cumuls_[pos])) {
      return false;
    }
    const SortedDisjointIntervalList& forbidden =
        dimension_->forbidden_intervals()[path[pos]];
    if (forbidden.NumIntervals() > 0) {
      std::vector<int64_t> starts;
      std::vector<int64_t> ends;
      for (const ClosedInterval interval :
           dimension_->GetAllowedIntervalsInRange(
               path[pos], CapAdd(current_route_min_cumuls_[pos], cumul_offset),
               CapAdd(current_route_max_cumuls_[pos], cumul_offset))) {
        starts.push_back(CapSub(interval.start, cumul_offset));
        ends.push_back(CapSub(interval.end, cumul_offset));
      }
      solver->SetVariableDisjointBounds(lp_cumul, starts, ends);
    }
  }
  // Create LP variables for slacks.
  std::vector<int> lp_slacks(path_size - 1, -1);
  for (int pos = 0; pos < path_size - 1; ++pos) {
    const IntVar* cp_slack = dimension_->SlackVar(path[pos]);
    lp_slacks[pos] = solver->CreateNewPositiveVariable();
    SET_VARIABLE_NAME(solver, lp_slacks[pos],
                      absl::StrFormat("lp_slacks(%ld)", pos));
    if (!solver->SetVariableBounds(lp_slacks[pos], cp_slack->Min(),
                                   cp_slack->Max())) {
      return false;
    }
  }

  // LP Model constraints and costs.
  // Add all path constraints to LP:
  // cumul[i] + fixed_transit[i] + slack[i] == cumul[i+1]
  // <=> fixed_transit[i] == cumul[i+1] - cumul[i] - slack[i].
  for (int pos = 0; pos < path_size - 1; ++pos) {
    const int ct =
        solver->CreateNewConstraint(fixed_transit[pos], fixed_transit[pos]);
    solver->SetCoefficient(ct, lp_cumuls[pos + 1], 1);
    solver->SetCoefficient(ct, lp_cumuls[pos], -1);
    solver->SetCoefficient(ct, lp_slacks[pos], -1);
  }
  if (route_cost_offset != nullptr) *route_cost_offset = 0;
  if (optimize_costs) {
    // Add soft upper bounds.
    for (int pos = 0; pos < path_size; ++pos) {
      if (!dimension_->HasCumulVarSoftUpperBound(path[pos])) continue;
      const int64_t coef =
          dimension_->GetCumulVarSoftUpperBoundCoefficient(path[pos]);
      if (coef == 0) continue;
      int64_t bound = dimension_->GetCumulVarSoftUpperBound(path[pos]);
      if (bound < cumul_offset && route_cost_offset != nullptr) {
        // Add coef * (cumul_offset - bound) to the cost offset.
        *route_cost_offset = CapAdd(*route_cost_offset,
                                    CapProd(CapSub(cumul_offset, bound), coef));
      }
      bound = std::max<int64_t>(0, CapSub(bound, cumul_offset));
      if (current_route_max_cumuls_[pos] <= bound) {
        // constraint is never violated.
        continue;
      }
      const int soft_ub_diff = solver->CreateNewPositiveVariable();
      SET_VARIABLE_NAME(solver, soft_ub_diff,
                        absl::StrFormat("soft_ub_diff(%ld)", pos));
      solver->SetObjectiveCoefficient(soft_ub_diff, coef);
      // cumul - soft_ub_diff <= bound.
      const int ct = solver->CreateNewConstraint(
          std::numeric_limits<int64_t>::min(), bound);
      solver->SetCoefficient(ct, lp_cumuls[pos], 1);
      solver->SetCoefficient(ct, soft_ub_diff, -1);
    }
    // Add soft lower bounds.
    for (int pos = 0; pos < path_size; ++pos) {
      if (!dimension_->HasCumulVarSoftLowerBound(path[pos])) continue;
      const int64_t coef =
          dimension_->GetCumulVarSoftLowerBoundCoefficient(path[pos]);
      if (coef == 0) continue;
      const int64_t bound = std::max<int64_t>(
          0, CapSub(dimension_->GetCumulVarSoftLowerBound(path[pos]),
                    cumul_offset));
      if (current_route_min_cumuls_[pos] >= bound) {
        // constraint is never violated.
        continue;
      }
      const int soft_lb_diff = solver->CreateNewPositiveVariable();
      SET_VARIABLE_NAME(solver, soft_lb_diff,
                        absl::StrFormat("soft_lb_diff(%ld)", pos));
      solver->SetObjectiveCoefficient(soft_lb_diff, coef);
      // bound - cumul <= soft_lb_diff
      const int ct = solver->CreateNewConstraint(
          bound, std::numeric_limits<int64_t>::max());
      solver->SetCoefficient(ct, lp_cumuls[pos], 1);
      solver->SetCoefficient(ct, soft_lb_diff, 1);
    }
  }
  // Add pickup and delivery limits.
  std::vector<int> visited_pairs;
  StoreVisitedPickupDeliveryPairsOnRoute(
      *dimension_, vehicle, next_accessor, &visited_pairs,
      &visited_pickup_delivery_indices_for_pair_);
  for (int pair_index : visited_pairs) {
    const int64_t pickup_index =
        visited_pickup_delivery_indices_for_pair_[pair_index].first;
    const int64_t delivery_index =
        visited_pickup_delivery_indices_for_pair_[pair_index].second;
    visited_pickup_delivery_indices_for_pair_[pair_index] = {-1, -1};

    DCHECK_GE(pickup_index, 0);
    if (delivery_index < 0) {
      // We didn't encounter a delivery for this pickup.
      continue;
    }

    const int64_t limit = dimension_->GetPickupToDeliveryLimitForPair(
        pair_index, model->GetPickupIndexPairs(pickup_index)[0].second,
        model->GetDeliveryIndexPairs(delivery_index)[0].second);
    if (limit < std::numeric_limits<int64_t>::max()) {
      // delivery_cumul - pickup_cumul <= limit.
      const int ct = solver->CreateNewConstraint(
          std::numeric_limits<int64_t>::min(), limit);
      solver->SetCoefficient(ct, index_to_cumul_variable_[delivery_index], 1);
      solver->SetCoefficient(ct, index_to_cumul_variable_[pickup_index], -1);
    }
  }

  // Add span bound constraint.
  const int64_t span_bound = dimension_->GetSpanUpperBoundForVehicle(vehicle);
  if (span_bound < std::numeric_limits<int64_t>::max()) {
    // end_cumul - start_cumul <= bound
    const int ct = solver->CreateNewConstraint(
        std::numeric_limits<int64_t>::min(), span_bound);
    solver->SetCoefficient(ct, lp_cumuls.back(), 1);
    solver->SetCoefficient(ct, lp_cumuls.front(), -1);
  }
  // Add span cost.
  const int64_t span_cost_coef =
      dimension_->GetSpanCostCoefficientForVehicle(vehicle);
  if (optimize_costs && span_cost_coef > 0) {
    solver->SetObjectiveCoefficient(lp_cumuls.back(), span_cost_coef);
    solver->SetObjectiveCoefficient(lp_cumuls.front(), -span_cost_coef);
  }
  // Add soft span cost.
  if (optimize_costs && dimension_->HasSoftSpanUpperBounds()) {
    SimpleBoundCosts::BoundCost bound_cost =
        dimension_->GetSoftSpanUpperBoundForVehicle(vehicle);
    if (bound_cost.bound < std::numeric_limits<int64_t>::max() &&
        bound_cost.cost > 0) {
      const int span_violation = solver->CreateNewPositiveVariable();
      SET_VARIABLE_NAME(solver, span_violation, "span_violation");
      // end - start <= bound + span_violation
      const int violation = solver->CreateNewConstraint(
          std::numeric_limits<int64_t>::min(), bound_cost.bound);
      solver->SetCoefficient(violation, lp_cumuls.back(), 1.0);
      solver->SetCoefficient(violation, lp_cumuls.front(), -1.0);
      solver->SetCoefficient(violation, span_violation, -1.0);
      // Add span_violation * cost to objective.
      solver->SetObjectiveCoefficient(span_violation, bound_cost.cost);
    }
  }
  // Add global span constraint.
  if (optimize_costs && dimension_->global_span_cost_coefficient() > 0) {
    // min_start_cumul_ <= cumuls[start]
    int ct =
        solver->CreateNewConstraint(std::numeric_limits<int64_t>::min(), 0);
    solver->SetCoefficient(ct, min_start_cumul_, 1);
    solver->SetCoefficient(ct, lp_cumuls.front(), -1);
    // max_end_cumul_ >= cumuls[end]
    ct = solver->CreateNewConstraint(0, std::numeric_limits<int64_t>::max());
    solver->SetCoefficient(ct, max_end_cumul_, 1);
    solver->SetCoefficient(ct, lp_cumuls.back(), -1);
  }
  // Fill transit cost if specified.
  if (route_transit_cost != nullptr) {
    if (optimize_costs && span_cost_coef > 0) {
      const int64_t total_fixed_transit = std::accumulate(
          fixed_transit.begin(), fixed_transit.end(), 0, CapAdd);
      *route_transit_cost = CapProd(total_fixed_transit, span_cost_coef);
    } else {
      *route_transit_cost = 0;
    }
  }
  // For every break that must be inside the route, the duration of that break
  // must be flowed in the slacks of arcs that can intersect the break.
  // This LP modelization is correct but not complete:
  // can miss some cases where the breaks cannot fit.
  // TODO(user): remove the need for returns in the code below.
  current_route_break_variables_.clear();
  if (!dimension_->HasBreakConstraints()) return true;
  const std::vector<IntervalVar*>& breaks =
      dimension_->GetBreakIntervalsOfVehicle(vehicle);
  const int num_breaks = breaks.size();
  // When there are no breaks, only break distance needs to be modeled,
  // and it reduces to a span maximum.
  // TODO(user): Also add the case where no breaks can intersect the route.
  if (num_breaks == 0) {
    int64_t maximum_route_span = std::numeric_limits<int64_t>::max();
    for (const auto& distance_duration :
         dimension_->GetBreakDistanceDurationOfVehicle(vehicle)) {
      maximum_route_span =
          std::min(maximum_route_span, distance_duration.first);
    }
    if (maximum_route_span < std::numeric_limits<int64_t>::max()) {
      const int ct = solver->CreateNewConstraint(
          std::numeric_limits<int64_t>::min(), maximum_route_span);
      solver->SetCoefficient(ct, lp_cumuls.back(), 1);
      solver->SetCoefficient(ct, lp_cumuls.front(), -1);
    }
    return true;
  }
  // Gather visit information: the visit of node i has [start, end) =
  // [cumul[i] - post_travel[i-1], cumul[i] + pre_travel[i]).
  // Breaks cannot overlap those visit intervals.
  std::vector<int64_t> pre_travel(path_size - 1, 0);
  std::vector<int64_t> post_travel(path_size - 1, 0);
  {
    const int pre_travel_index =
        dimension_->GetPreTravelEvaluatorOfVehicle(vehicle);
    if (pre_travel_index != -1) {
      FillPathEvaluation(path, model->TransitCallback(pre_travel_index),
                         &pre_travel);
    }
    const int post_travel_index =
        dimension_->GetPostTravelEvaluatorOfVehicle(vehicle);
    if (post_travel_index != -1) {
      FillPathEvaluation(path, model->TransitCallback(post_travel_index),
                         &post_travel);
    }
  }
  // If the solver is CPSAT, it will need to represent the times at which
  // breaks are scheduled, those variables are used both in the pure breaks
  // part and in the break distance part of the model.
  // Otherwise, it doesn't need the variables and they are not created.
  std::vector<int> lp_break_start;
  std::vector<int> lp_break_duration;
  std::vector<int> lp_break_end;
  if (solver->IsCPSATSolver()) {
    lp_break_start.resize(num_breaks, -1);
    lp_break_duration.resize(num_breaks, -1);
    lp_break_end.resize(num_breaks, -1);
  }

  std::vector<int> slack_exact_lower_bound_ct(path_size - 1, -1);
  std::vector<int> slack_linear_lower_bound_ct(path_size - 1, -1);

  const int64_t vehicle_start_min = current_route_min_cumuls_.front();
  const int64_t vehicle_start_max = current_route_max_cumuls_.front();
  const int64_t vehicle_end_min = current_route_min_cumuls_.back();
  const int64_t vehicle_end_max = current_route_max_cumuls_.back();
  const int all_break_variables_offset =
      vehicle_to_all_break_variables_offset_[vehicle];
  for (int br = 0; br < num_breaks; ++br) {
    const IntervalVar& break_var = *breaks[br];
    if (!break_var.MustBePerformed()) continue;
    const int64_t break_start_min = CapSub(break_var.StartMin(), cumul_offset);
    const int64_t break_start_max = CapSub(break_var.StartMax(), cumul_offset);
    const int64_t break_end_min = CapSub(break_var.EndMin(), cumul_offset);
    const int64_t break_end_max = CapSub(break_var.EndMax(), cumul_offset);
    const int64_t break_duration_min = break_var.DurationMin();
    const int64_t break_duration_max = break_var.DurationMax();
    // The CPSAT solver encodes all breaks that can intersect the route,
    // the LP solver only encodes the breaks that must intersect the route.
    if (solver->IsCPSATSolver()) {
      if (break_end_max <= vehicle_start_min ||
          vehicle_end_max <= break_start_min) {
        all_break_variables_[all_break_variables_offset + 2 * br] = -1;
        all_break_variables_[all_break_variables_offset + 2 * br + 1] = -1;
        current_route_break_variables_.push_back(-1);
        current_route_break_variables_.push_back(-1);
        continue;
      }
      lp_break_start[br] =
          solver->AddVariable(break_start_min, break_start_max);
      SET_VARIABLE_NAME(solver, lp_break_start[br],
                        absl::StrFormat("lp_break_start(%ld)", br));
      lp_break_end[br] = solver->AddVariable(break_end_min, break_end_max);
      SET_VARIABLE_NAME(solver, lp_break_end[br],
                        absl::StrFormat("lp_break_end(%ld)", br));
      lp_break_duration[br] =
          solver->AddVariable(break_duration_min, break_duration_max);
      SET_VARIABLE_NAME(solver, lp_break_duration[br],
                        absl::StrFormat("lp_break_duration(%ld)", br));
      // start + duration = end.
      solver->AddLinearConstraint(0, 0,
                                  {{lp_break_end[br], 1},
                                   {lp_break_start[br], -1},
                                   {lp_break_duration[br], -1}});
      // Record index of variables
      all_break_variables_[all_break_variables_offset + 2 * br] =
          lp_break_start[br];
      all_break_variables_[all_break_variables_offset + 2 * br + 1] =
          lp_break_end[br];
      current_route_break_variables_.push_back(lp_break_start[br]);
      current_route_break_variables_.push_back(lp_break_end[br]);
    } else {
      if (break_end_min <= vehicle_start_max ||
          vehicle_end_min <= break_start_max) {
        all_break_variables_[all_break_variables_offset + 2 * br] = -1;
        all_break_variables_[all_break_variables_offset + 2 * br + 1] = -1;
        current_route_break_variables_.push_back(-1);
        current_route_break_variables_.push_back(-1);
        continue;
      }
    }

    // Create a constraint for every break, that forces it to be scheduled
    // in exactly one place, i.e. one slack or before/after the route.
    // sum_i break_in_slack_i  == 1.
    const int break_in_one_slack_ct = solver->CreateNewConstraint(1, 1);

    if (solver->IsCPSATSolver()) {
      // Break can be before route.
      if (break_end_min <= vehicle_start_max) {
        const int ct = solver->AddLinearConstraint(
            0, std::numeric_limits<int64_t>::max(),
            {{lp_cumuls.front(), 1}, {lp_break_end[br], -1}});
        const int break_is_before_route = solver->AddVariable(0, 1);
        SET_VARIABLE_NAME(solver, break_is_before_route,
                          absl::StrFormat("break_is_before_route(%ld)", br));
        solver->SetEnforcementLiteral(ct, break_is_before_route);
        solver->SetCoefficient(break_in_one_slack_ct, break_is_before_route, 1);
      }
      // Break can be after route.
      if (vehicle_end_min <= break_start_max) {
        const int ct = solver->AddLinearConstraint(
            0, std::numeric_limits<int64_t>::max(),
            {{lp_break_start[br], 1}, {lp_cumuls.back(), -1}});
        const int break_is_after_route = solver->AddVariable(0, 1);
        SET_VARIABLE_NAME(solver, break_is_after_route,
                          absl::StrFormat("break_is_after_route(%ld)", br));
        solver->SetEnforcementLiteral(ct, break_is_after_route);
        solver->SetCoefficient(break_in_one_slack_ct, break_is_after_route, 1);
      }
    }

    // Add the possibility of fitting the break during each slack where it can.
    for (int pos = 0; pos < path_size - 1; ++pos) {
      // Pass on slacks that cannot start before, cannot end after,
      // or are not long enough to contain the break.
      const int64_t slack_start_min =
          CapAdd(current_route_min_cumuls_[pos], pre_travel[pos]);
      if (slack_start_min > break_start_max) break;
      const int64_t slack_end_max =
          CapSub(current_route_max_cumuls_[pos + 1], post_travel[pos]);
      if (break_end_min > slack_end_max) continue;
      const int64_t slack_duration_max =
          std::min(CapSub(CapSub(current_route_max_cumuls_[pos + 1],
                                 current_route_min_cumuls_[pos]),
                          fixed_transit[pos]),
                   dimension_->SlackVar(path[pos])->Max());
      if (slack_duration_max < break_duration_min) continue;

      // Break can fit into slack: make LP variable, add to break and slack
      // constraints.
      // Make a linearized slack lower bound (lazily), that represents
      // sum_br break_duration_min(br) * break_in_slack(br, pos) <=
      // lp_slacks(pos).
      const int break_in_slack = solver->AddVariable(0, 1);
      SET_VARIABLE_NAME(solver, break_in_slack,
                        absl::StrFormat("break_in_slack(%ld, %ld)", br, pos));
      solver->SetCoefficient(break_in_one_slack_ct, break_in_slack, 1);
      if (slack_linear_lower_bound_ct[pos] == -1) {
        slack_linear_lower_bound_ct[pos] = solver->AddLinearConstraint(
            std::numeric_limits<int64_t>::min(), 0, {{lp_slacks[pos], -1}});
      }
      solver->SetCoefficient(slack_linear_lower_bound_ct[pos], break_in_slack,
                             break_duration_min);
      if (solver->IsCPSATSolver()) {
        // Exact relation between breaks, slacks and cumul variables.
        // Make an exact slack lower bound (lazily), that represents
        // sum_br break_duration(br) * break_in_slack(br, pos) <=
        // lp_slacks(pos).
        const int break_duration_in_slack =
            solver->AddVariable(0, slack_duration_max);
        SET_VARIABLE_NAME(
            solver, break_duration_in_slack,
            absl::StrFormat("break_duration_in_slack(%ld, %ld)", br, pos));
        solver->AddProductConstraint(break_duration_in_slack,
                                     {break_in_slack, lp_break_duration[br]});
        if (slack_exact_lower_bound_ct[pos] == -1) {
          slack_exact_lower_bound_ct[pos] = solver->AddLinearConstraint(
              std::numeric_limits<int64_t>::min(), 0, {{lp_slacks[pos], -1}});
        }
        solver->SetCoefficient(slack_exact_lower_bound_ct[pos],
                               break_duration_in_slack, 1);
        // If break_in_slack_i == 1, then
        // 1) break_start >= cumul[pos] + pre_travel[pos]
        const int break_start_after_current_ct = solver->AddLinearConstraint(
            pre_travel[pos], std::numeric_limits<int64_t>::max(),
            {{lp_break_start[br], 1}, {lp_cumuls[pos], -1}});
        solver->SetEnforcementLiteral(break_start_after_current_ct,
                                      break_in_slack);
        // 2) break_end <= cumul[pos+1] - post_travel[pos]
        const int break_ends_before_next_ct = solver->AddLinearConstraint(
            post_travel[pos], std::numeric_limits<int64_t>::max(),
            {{lp_cumuls[pos + 1], 1}, {lp_break_end[br], -1}});
        solver->SetEnforcementLiteral(break_ends_before_next_ct,
                                      break_in_slack);
      }
    }
  }

  if (!solver->IsCPSATSolver()) return true;
  if (!dimension_->GetBreakDistanceDurationOfVehicle(vehicle).empty()) {
    // If there is an optional interval, the following model would be wrong.
    // TODO(user): support optional intervals.
    for (const IntervalVar* interval :
         dimension_->GetBreakIntervalsOfVehicle(vehicle)) {
      if (!interval->MustBePerformed()) return true;
    }
    // When this feature is used, breaks are in sorted order.
    for (int br = 1; br < num_breaks; ++br) {
      if (lp_break_start[br] == -1 || lp_break_start[br - 1] == -1) continue;
      solver->AddLinearConstraint(
          0, std::numeric_limits<int64_t>::max(),
          {{lp_break_end[br - 1], -1}, {lp_break_start[br], 1}});
    }
  }
  for (const auto& distance_duration :
       dimension_->GetBreakDistanceDurationOfVehicle(vehicle)) {
    const int64_t limit = distance_duration.first;
    const int64_t min_break_duration = distance_duration.second;
    // Interbreak limit constraint: breaks are interpreted as being in sorted
    // order, and the maximum duration between two consecutive
    // breaks of duration more than 'min_break_duration' is 'limit'. This
    // considers the time until start of route and after end of route to be
    // infinite breaks.
    // The model for this constraint adds some 'cover_i' variables, such that
    // the breaks up to i and the start of route allows to go without a break.
    // With s_i the start of break i and e_i its end:
    // - the route start covers time from start to start + limit:
    //   cover_0 = route_start + limit
    // - the coverage up to a given break is the largest of the coverage of the
    //   previous break and if the break is long enough, break end + limit:
    //   cover_{i+1} = max(cover_i,
    //                     e_i - s_i >= min_break_duration ? e_i + limit : -inf)
    // - the coverage of the last break must be at least the route end,
    //   to ensure the time point route_end-1 is covered:
    //   cover_{num_breaks} >= route_end
    // - similarly, time point s_i-1 must be covered by breaks up to i-1,
    //   but only if the cover has not reached the route end.
    //   For instance, a vehicle could have a choice between two days,
    //   with a potential break on day 1 and a potential break on day 2,
    //   but the break of day 1 does not have to cover that of day 2!
    //   cover_{i-1} < route_end => s_i <= cover_{i-1}
    // This is sufficient to ensure that the union of the intervals
    // (-infinity, route_start], [route_end, +infinity) and all
    // [s_i, e_i+limit) where e_i - s_i >= min_break_duration is
    // the whole timeline (-infinity, +infinity).
    int previous_cover = solver->AddVariable(CapAdd(vehicle_start_min, limit),
                                             CapAdd(vehicle_start_max, limit));
    SET_VARIABLE_NAME(solver, previous_cover, "previous_cover");
    solver->AddLinearConstraint(limit, limit,
                                {{previous_cover, 1}, {lp_cumuls.front(), -1}});
    for (int br = 0; br < num_breaks; ++br) {
      if (lp_break_start[br] == -1) continue;
      const int64_t break_end_min = CapSub(breaks[br]->EndMin(), cumul_offset);
      const int64_t break_end_max = CapSub(breaks[br]->EndMax(), cumul_offset);
      // break_is_eligible <=>
      // break_end - break_start >= break_minimum_duration.
      const int break_is_eligible = solver->AddVariable(0, 1);
      SET_VARIABLE_NAME(solver, break_is_eligible,
                        absl::StrFormat("break_is_eligible(%ld)", br));
      const int break_is_not_eligible = solver->AddVariable(0, 1);
      SET_VARIABLE_NAME(solver, break_is_not_eligible,
                        absl::StrFormat("break_is_not_eligible(%ld)", br));
      {
        solver->AddLinearConstraint(
            1, 1, {{break_is_eligible, 1}, {break_is_not_eligible, 1}});
        const int positive_ct = solver->AddLinearConstraint(
            min_break_duration, std::numeric_limits<int64_t>::max(),
            {{lp_break_end[br], 1}, {lp_break_start[br], -1}});
        solver->SetEnforcementLiteral(positive_ct, break_is_eligible);
        const int negative_ct = solver->AddLinearConstraint(
            std::numeric_limits<int64_t>::min(), min_break_duration - 1,
            {{lp_break_end[br], 1}, {lp_break_start[br], -1}});
        solver->SetEnforcementLiteral(negative_ct, break_is_not_eligible);
      }
      // break_is_eligible => break_cover == break_end + limit.
      // break_is_not_eligible => break_cover == vehicle_start_min + limit.
      // break_cover's initial domain is the smallest interval that contains the
      // union of sets {vehicle_start_min+limit} and
      // [break_end_min+limit, break_end_max+limit).
      const int break_cover = solver->AddVariable(
          CapAdd(std::min(vehicle_start_min, break_end_min), limit),
          CapAdd(std::max(vehicle_start_min, break_end_max), limit));
      SET_VARIABLE_NAME(solver, break_cover,
                        absl::StrFormat("break_cover(%ld)", br));
      const int limit_cover_ct = solver->AddLinearConstraint(
          limit, limit, {{break_cover, 1}, {lp_break_end[br], -1}});
      solver->SetEnforcementLiteral(limit_cover_ct, break_is_eligible);
      const int empty_cover_ct = solver->AddLinearConstraint(
          CapAdd(vehicle_start_min, limit), CapAdd(vehicle_start_min, limit),
          {{break_cover, 1}});
      solver->SetEnforcementLiteral(empty_cover_ct, break_is_not_eligible);

      const int cover =
          solver->AddVariable(CapAdd(vehicle_start_min, limit),
                              std::numeric_limits<int64_t>::max());
      SET_VARIABLE_NAME(solver, cover, absl::StrFormat("cover(%ld)", br));
      solver->AddMaximumConstraint(cover, {previous_cover, break_cover});
      // Cover chaining. If route end is not covered, break start must be:
      // cover_{i-1} < route_end => s_i <= cover_{i-1}
      const int route_end_is_not_covered = solver->AddReifiedLinearConstraint(
          1, std::numeric_limits<int64_t>::max(),
          {{lp_cumuls.back(), 1}, {previous_cover, -1}});
      const int break_start_cover_ct = solver->AddLinearConstraint(
          0, std::numeric_limits<int64_t>::max(),
          {{previous_cover, 1}, {lp_break_start[br], -1}});
      solver->SetEnforcementLiteral(break_start_cover_ct,
                                    route_end_is_not_covered);

      previous_cover = cover;
    }
    solver->AddLinearConstraint(0, std::numeric_limits<int64_t>::max(),
                                {{previous_cover, 1}, {lp_cumuls.back(), -1}});
  }

  return true;
}

bool DimensionCumulOptimizerCore::SetGlobalConstraints(
    const std::function<int64_t(int64_t)>& next_accessor, int64_t cumul_offset,
    bool optimize_costs, RoutingLinearSolverWrapper* solver) {
  // Global span cost =
  //     global_span_cost_coefficient * (max_end_cumul - min_start_cumul).
  const int64_t global_span_coeff = dimension_->global_span_cost_coefficient();
  if (optimize_costs && global_span_coeff > 0) {
    solver->SetObjectiveCoefficient(max_end_cumul_, global_span_coeff);
    solver->SetObjectiveCoefficient(min_start_cumul_, -global_span_coeff);
  }

  // Node precedence constraints, set when both nodes are visited.
  for (const RoutingDimension::NodePrecedence& precedence :
       dimension_->GetNodePrecedences()) {
    const int first_cumul_var = index_to_cumul_variable_[precedence.first_node];
    const int second_cumul_var =
        index_to_cumul_variable_[precedence.second_node];
    if (first_cumul_var < 0 || second_cumul_var < 0) {
      // At least one of the nodes is not on any route, skip this precedence
      // constraint.
      continue;
    }
    DCHECK_NE(first_cumul_var, second_cumul_var)
        << "Dimension " << dimension_->name()
        << " has a self-precedence on node " << precedence.first_node << ".";

    // cumul[second_node] - cumul[first_node] >= offset.
    const int ct = solver->CreateNewConstraint(
        precedence.offset, std::numeric_limits<int64_t>::max());
    solver->SetCoefficient(ct, second_cumul_var, 1);
    solver->SetCoefficient(ct, first_cumul_var, -1);
  }

  if (!solver->IsCPSATSolver()) {
    // The resource attributes conditional constraints can only be added with
    // the CP-SAT MIP solver.
    return true;
  }

  const RoutingModel& model = *dimension_->model();
  const int num_vehicles = model.vehicles();
  const auto& resource_groups = model.GetResourceGroups();
  for (int rg_index : model.GetDimensionResourceGroupIndices(dimension_)) {
    // Resource domain constraints:
    // Every (used) vehicle requiring a resource from this group must be
    // assigned to exactly one resource in this group, and each resource must be
    // assigned to at most 1 vehicle requiring it.
    // For every resource r with Attributes A = resources[r].attributes(dim)
    // and every vehicle v, assign(r, v) == 1 -->
    // A.start_domain.Min() <= cumul[Start(v)] <= A.start_domain.Max(),
    // and
    // A.end_domain.Min() <= cumul[End(v)] <= A.end_domain.Max().
    const ResourceGroup& resource_group = *resource_groups[rg_index];
    DCHECK(!resource_group.GetVehiclesRequiringAResource().empty());

    const std::vector<ResourceGroup::Resource>& resources =
        resource_group.GetResources();
    int num_required_resources = 0;
    static const int kNoConstraint = -1;
    // Assignment constraints for vehicles: each (used) vehicle must have
    // exactly one resource assigned to it.
    std::vector<int> vehicle_constraints(model.vehicles(), kNoConstraint);
    for (int v : resource_group.GetVehiclesRequiringAResource()) {
      if (model.IsEnd(next_accessor(model.Start(v))) &&
          !model.IsVehicleUsedWhenEmpty(v)) {
        // We don't assign a driver to unused vehicles.
        continue;
      }
      num_required_resources++;
      vehicle_constraints[v] = solver->CreateNewConstraint(1, 1);
    }
    // Assignment constraints for resources: each resource must be assigned to
    // at most one (used) vehicle requiring one.
    const int num_resources = resources.size();
    std::vector<int> resource_constraints(num_resources, kNoConstraint);
    int num_available_resources = 0;
    for (int r = 0; r < num_resources; r++) {
      const ResourceGroup::Attributes& attributes =
          resources[r].GetDimensionAttributes(dimension_);
      if (attributes.start_domain().Max() < cumul_offset ||
          attributes.end_domain().Max() < cumul_offset) {
        // This resource's domain has a cumul max lower than the offset, so it's
        // not possible to restrict any vehicle start/end to this domain; skip
        // it.
        continue;
      }
      num_available_resources++;
      resource_constraints[r] = solver->CreateNewConstraint(0, 1);
    }

    if (num_required_resources > num_available_resources) {
      // There aren't enough resources in this group for vehicles requiring one.
      return false;
    }

    std::vector<int>& resource_to_vehicle_assignment_variables =
        resource_group_to_resource_to_vehicle_assignment_variables_[rg_index];
    resource_to_vehicle_assignment_variables.assign(
        num_resources * num_vehicles, -1);
    // Create assignment variables, add them to the corresponding constraints,
    // and create the reified constraints assign(r, v) == 1 -->
    // A(r).start_domain.Min() <= cumul[Start(v)] <= A(r).start_domain.Max(),
    // and
    // A(r).end_domain.Min() <= cumul[End(v)] <= A(r).end_domain.Max().
    for (int r = 0; r < num_resources; r++) {
      if (resource_constraints[r] == kNoConstraint) continue;
      const ResourceGroup::Attributes& attributes =
          resources[r].GetDimensionAttributes(dimension_);
      for (int v : resource_group.GetVehiclesRequiringAResource()) {
        if (vehicle_constraints[v] == kNoConstraint) continue;

        const int assign_r_to_v = solver->AddVariable(0, 1);
        SET_VARIABLE_NAME(solver, assign_r_to_v,
                          absl::StrFormat("assign_r_to_v(%ld, %ld)", r, v));
        resource_to_vehicle_assignment_variables[r * num_vehicles + v] =
            assign_r_to_v;
        solver->SetCoefficient(vehicle_constraints[v], assign_r_to_v, 1);
        solver->SetCoefficient(resource_constraints[r], assign_r_to_v, 1);

        const auto& add_domain_constraint =
            [&solver, cumul_offset, assign_r_to_v](const Domain& domain,
                                                   int cumul_variable) {
              if (domain == Domain::AllValues()) {
                return;
              }
              ClosedInterval cumul_bounds;
              if (!GetDomainOffsetBounds(domain, cumul_offset, &cumul_bounds)) {
                // This domain cannot be assigned to this vehicle.
                solver->SetVariableBounds(assign_r_to_v, 0, 0);
                return;
              }
              const int cumul_constraint = solver->AddLinearConstraint(
                  cumul_bounds.start, cumul_bounds.end, {{cumul_variable, 1}});
              solver->SetEnforcementLiteral(cumul_constraint, assign_r_to_v);
            };
        add_domain_constraint(attributes.start_domain(),
                              index_to_cumul_variable_[model.Start(v)]);
        add_domain_constraint(attributes.end_domain(),
                              index_to_cumul_variable_[model.End(v)]);
      }
    }
  }
  return true;
}

#undef SET_VARIABLE_NAME

void DimensionCumulOptimizerCore::SetValuesFromLP(
    const std::vector<int>& lp_variables, int64_t offset,
    RoutingLinearSolverWrapper* solver, std::vector<int64_t>* lp_values) const {
  if (lp_values == nullptr) return;
  lp_values->assign(lp_variables.size(), std::numeric_limits<int64_t>::min());
  for (int i = 0; i < lp_variables.size(); i++) {
    const int lp_var = lp_variables[i];
    if (lp_var < 0) continue;  // Keep default value, kint64min.
    const double lp_value_double = solver->GetValue(lp_var);
    const int64_t lp_value_int64 =
        (lp_value_double >= std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : MathUtil::FastInt64Round(lp_value_double);
    (*lp_values)[i] = CapAdd(lp_value_int64, offset);
  }
}

void DimensionCumulOptimizerCore::SetResourceIndices(
    RoutingLinearSolverWrapper* solver,
    std::vector<std::vector<int>>* resource_indices_per_group) const {
  if (resource_indices_per_group == nullptr ||
      resource_group_to_resource_to_vehicle_assignment_variables_.empty()) {
    return;
  }
  const RoutingModel& model = *dimension_->model();
  const int num_vehicles = model.vehicles();
  DCHECK(!model.GetDimensionResourceGroupIndices(dimension_).empty());
  const auto& resource_groups = model.GetResourceGroups();
  resource_indices_per_group->resize(resource_groups.size());
  for (int rg_index : model.GetDimensionResourceGroupIndices(dimension_)) {
    const ResourceGroup& resource_group = *resource_groups[rg_index];
    DCHECK(!resource_group.GetVehiclesRequiringAResource().empty());

    const int num_resources = resource_group.Size();
    std::vector<int>& resource_indices =
        resource_indices_per_group->at(rg_index);
    resource_indices.assign(num_vehicles, -1);
    // Find the resource assigned to each vehicle.
    const std::vector<int>& resource_to_vehicle_assignment_variables =
        resource_group_to_resource_to_vehicle_assignment_variables_[rg_index];
    DCHECK_EQ(resource_to_vehicle_assignment_variables.size(),
              num_resources * num_vehicles);
    for (int v : resource_group.GetVehiclesRequiringAResource()) {
      for (int r = 0; r < num_resources; r++) {
        const int assignment_var =
            resource_to_vehicle_assignment_variables[r * num_vehicles + v];
        if (assignment_var >= 0 && solver->GetValue(assignment_var) == 1) {
          // This resource is assigned to this vehicle.
          resource_indices[v] = r;
          break;
        }
      }
    }
  }
}

// GlobalDimensionCumulOptimizer

GlobalDimensionCumulOptimizer::GlobalDimensionCumulOptimizer(
    const RoutingDimension* dimension,
    RoutingSearchParameters::SchedulingSolver solver_type)
    : optimizer_core_(dimension,
                      /*use_precedence_propagator=*/
                      !dimension->GetNodePrecedences().empty()) {
  switch (solver_type) {
    case RoutingSearchParameters::SCHEDULING_GLOP: {
      solver_ = std::make_unique<RoutingGlopWrapper>(
          /*is_relaxation=*/!dimension->model()
              ->GetDimensionResourceGroupIndices(dimension)
              .empty(),
          GetGlopParametersForGlobalLP());
      break;
    }
    case RoutingSearchParameters::SCHEDULING_CP_SAT: {
      solver_ = std::make_unique<RoutingCPSatWrapper>();
      break;
    }
    default:
      LOG(DFATAL) << "Unrecognized solver type: " << solver_type;
  }
}

DimensionSchedulingStatus
GlobalDimensionCumulOptimizer::ComputeCumulCostWithoutFixedTransits(
    const std::function<int64_t(int64_t)>& next_accessor,
    int64_t* optimal_cost_without_transits) {
  int64_t cost = 0;
  int64_t transit_cost = 0;
  DimensionSchedulingStatus status =
      optimizer_core_.Optimize(next_accessor, {}, solver_.get(), nullptr,
                               nullptr, nullptr, &cost, &transit_cost);
  if (status != DimensionSchedulingStatus::INFEASIBLE &&
      optimal_cost_without_transits != nullptr) {
    *optimal_cost_without_transits = CapSub(cost, transit_cost);
  }
  return status;
}

DimensionSchedulingStatus GlobalDimensionCumulOptimizer::ComputeCumuls(
    const std::function<int64_t(int64_t)>& next_accessor,
    std::vector<int64_t>* optimal_cumuls, std::vector<int64_t>* optimal_breaks,
    std::vector<std::vector<int>>* optimal_resource_indices) {
  return optimizer_core_.Optimize(next_accessor, solver_.get(), optimal_cumuls,
                                  optimal_breaks, optimal_resource_indices,
                                  nullptr, nullptr);
}

DimensionSchedulingStatus GlobalDimensionCumulOptimizer::ComputePackedCumuls(
    const std::function<int64_t(int64_t)>& next_accessor,
    std::vector<int64_t>* packed_cumuls, std::vector<int64_t>* packed_breaks,
    std::vector<std::vector<int>>* resource_indices) {
  return optimizer_core_.OptimizeAndPack(next_accessor, solver_.get(),
                                         packed_cumuls, packed_breaks,
                                         resource_indices);
}

// ResourceAssignmentOptimizer

ResourceAssignmentOptimizer::ResourceAssignmentOptimizer(
    const RoutingModel::ResourceGroup* resource_group,
    LocalDimensionCumulOptimizer* optimizer,
    LocalDimensionCumulOptimizer* mp_optimizer)
    : optimizer_(*optimizer),
      mp_optimizer_(*mp_optimizer),
      model_(*optimizer->dimension()->model()),
      resource_group_(*resource_group) {}

bool ResourceAssignmentOptimizer::ComputeAssignmentCostsForVehicle(
    int v, const std::function<int64_t(int64_t)>& next_accessor,
    const std::function<int64_t(int64_t, int64_t)>& transit_accessor,
    bool optimize_vehicle_costs, std::vector<int64_t>* assignment_costs,
    std::vector<std::vector<int64_t>>* cumul_values,
    std::vector<std::vector<int64_t>>* break_values) {
  DCHECK_NE(assignment_costs, nullptr);
  if (!resource_group_.VehicleRequiresAResource(v) ||
      (next_accessor(model_.Start(v)) == model_.End(v) &&
       !model_.IsVehicleUsedWhenEmpty(v))) {
    assignment_costs->clear();
    return true;
  }
  const RoutingDimension& dimension = *optimizer_.dimension();
  if (dimension.model()->CheckLimit()) {
    // The model's time limit has been reached, stop everything.
    return false;
  }

  const std::vector<ResourceGroup::Resource>& resources =
      resource_group_.GetResources();
  const int num_resources = resources.size();
  std::vector<int> all_resource_indices(num_resources);
  std::iota(all_resource_indices.begin(), all_resource_indices.end(), 0);
  const bool use_mp_optimizer =
      dimension.HasBreakConstraints() &&
      !dimension.GetBreakIntervalsOfVehicle(v).empty();
  LocalDimensionCumulOptimizer& optimizer =
      use_mp_optimizer ? mp_optimizer_ : optimizer_;
  std::vector<DimensionSchedulingStatus> statuses =
      optimizer.ComputeRouteCumulCostsForResourcesWithoutFixedTransits(
          v, next_accessor, transit_accessor, resources, all_resource_indices,
          optimize_vehicle_costs, assignment_costs, cumul_values, break_values);

  if (assignment_costs->empty()) {
    // Couldn't assign any resource to this vehicle.
    return false;
  }
  DCHECK_EQ(assignment_costs->size(), num_resources);
  DCHECK_EQ(statuses.size(), num_resources);
  DCHECK(cumul_values == nullptr || cumul_values->size() == num_resources);
  DCHECK(break_values == nullptr || break_values->size() == num_resources);

  if (use_mp_optimizer) {
    // We already used the mp optimizer, so we don't need to recompute anything.
    // If all assignment costs are negative, it means no resource is feasible
    // for this vehicle.
    return absl::c_any_of(*assignment_costs,
                          [](int64_t cost) { return cost >= 0; });
  }

  std::vector<int> mp_optimizer_resource_indices;
  for (int r = 0; r < num_resources; r++) {
    if (statuses[r] == DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY) {
      mp_optimizer_resource_indices.push_back(r);
    }
  }

  std::vector<int64_t> mp_assignment_costs;
  std::vector<std::vector<int64_t>> mp_cumul_values;
  std::vector<std::vector<int64_t>> mp_break_values;
  mp_optimizer_.ComputeRouteCumulCostsForResourcesWithoutFixedTransits(
      v, next_accessor, transit_accessor, resources,
      mp_optimizer_resource_indices, optimize_vehicle_costs,
      &mp_assignment_costs,
      cumul_values == nullptr ? nullptr : &mp_cumul_values,
      break_values == nullptr ? nullptr : &mp_break_values);
  if (!mp_optimizer_resource_indices.empty() && mp_assignment_costs.empty()) {
    // A timeout was reached during optimization.
    return false;
  }
  DCHECK_EQ(mp_assignment_costs.size(), mp_optimizer_resource_indices.size());
  DCHECK(cumul_values == nullptr ||
         mp_cumul_values.size() == mp_optimizer_resource_indices.size());
  DCHECK(break_values == nullptr ||
         mp_break_values.size() == mp_optimizer_resource_indices.size());
  for (int i = 0; i < mp_optimizer_resource_indices.size(); i++) {
    assignment_costs->at(mp_optimizer_resource_indices[i]) =
        mp_assignment_costs[i];
    if (cumul_values != nullptr) {
      cumul_values->at(mp_optimizer_resource_indices[i])
          .swap(mp_cumul_values[i]);
    }
    if (break_values != nullptr) {
      break_values->at(mp_optimizer_resource_indices[i])
          .swap(mp_break_values[i]);
    }
  }
  return absl::c_any_of(*assignment_costs,
                        [](int64_t cost) { return cost >= 0; });
}

int64_t ResourceAssignmentOptimizer::ComputeBestAssignmentCost(
    const std::vector<std::vector<int64_t>>&
        primary_vehicle_to_resource_assignment_costs,
    const std::vector<std::vector<int64_t>>&
        secondary_vehicle_to_resource_assignment_costs,
    const std::function<bool(int)>& use_primary_for_vehicle,
    std::vector<int>* resource_indices) const {
  const int num_vehicles = model_.vehicles();
  DCHECK_EQ(primary_vehicle_to_resource_assignment_costs.size(), num_vehicles);
  DCHECK_EQ(secondary_vehicle_to_resource_assignment_costs.size(),
            num_vehicles);
  const int num_resources = resource_group_.Size();

  SimpleMinCostFlow flow(
      /*reserve_num_nodes*/ 2 + num_vehicles + num_resources,
      /*reserve_num_arcs*/ num_vehicles + num_vehicles * num_resources +
          num_resources);
  const int source_index = num_vehicles + num_resources;
  const int sink_index = source_index + 1;
  const auto resource_index = [num_vehicles](int r) {
    return num_vehicles + r;
  };

  std::vector<std::vector<ArcIndex>> vehicle_to_resource_arc_index;
  if (resource_indices != nullptr) {
    vehicle_to_resource_arc_index.resize(
        num_vehicles, std::vector<ArcIndex>(num_resources, -1));
  }
  int num_used_vehicles = 0;
  for (int v : resource_group_.GetVehiclesRequiringAResource()) {
    DCHECK(use_primary_for_vehicle(v) ||
           primary_vehicle_to_resource_assignment_costs[v].empty());
    const std::vector<int64_t>& assignment_costs =
        use_primary_for_vehicle(v)
            ? primary_vehicle_to_resource_assignment_costs[v]
            : secondary_vehicle_to_resource_assignment_costs[v];
    if (assignment_costs.empty()) {
      // We don't need a resource for this vehicle.
      continue;
    }
    DCHECK_EQ(assignment_costs.size(), num_resources);
    num_used_vehicles++;
    if (num_used_vehicles > num_resources) {
      // Not enough resources for all vehicles needing one.
      return -1;
    }

    // Add a source->vehicle arc to the flow.
    flow.AddArcWithCapacityAndUnitCost(source_index, v, 1, 0);

    // Add arcs to the min-cost-flow graph.
    bool has_feasible_resource = false;
    for (int r = 0; r < num_resources; r++) {
      const int64_t assignment_cost = assignment_costs[r];
      if (assignment_cost < 0) continue;
      const ArcIndex arc_index = flow.AddArcWithCapacityAndUnitCost(
          v, resource_index(r), 1, assignment_cost);
      if (!vehicle_to_resource_arc_index.empty()) {
        vehicle_to_resource_arc_index[v][r] = arc_index;
      }
      has_feasible_resource = true;
    }
    if (!has_feasible_resource) {
      // Catches cases of infeasibility where ComputeAssignmentCostsForVehicle()
      // hasn't "properly" initialized the primary/secondary assignment costs
      // (this can happen for instance in the ResourceGroupAssignmentFilter
      // when routes are synchronized with an impossible first solution).
      return -1;
    }
  }

  // Add resource->sink arcs to the flow.
  for (int r = 0; r < num_resources; r++) {
    flow.AddArcWithCapacityAndUnitCost(resource_index(r), sink_index, 1, 0);
  }

  // Set the flow supply.
  flow.SetNodeSupply(source_index, num_used_vehicles);
  flow.SetNodeSupply(sink_index, -num_used_vehicles);

  // Solve the min-cost flow and return its cost.
  if (flow.Solve() != SimpleMinCostFlow::OPTIMAL) {
    if (resource_indices != nullptr) resource_indices->clear();
    return -1;
  }

  const int64_t cost = flow.OptimalCost();
  if (resource_indices == nullptr) {
    return cost;
  }

  // Fill the resource indices corresponding to the min-cost assignment.
  resource_indices->assign(num_vehicles, -1);
  for (int v : resource_group_.GetVehiclesRequiringAResource()) {
    for (int r = 0; r < num_resources; r++) {
      if (vehicle_to_resource_arc_index[v][r] >= 0 &&
          flow.Flow(vehicle_to_resource_arc_index[v][r]) > 0) {
        resource_indices->at(v) = r;
        break;
      }
    }
  }
  return cost;
}

}  // namespace operations_research
