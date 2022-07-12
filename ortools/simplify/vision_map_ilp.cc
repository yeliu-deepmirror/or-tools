// Copyright 2022 DeepMirror Inc. All rights reserved.

#include "ortools/simplify/vision_map_ilp.h"
#include "ortools/linear_solver/linear_solver.h"

namespace dm::graph {

bool VisionSummarization(const VisionSummarizationProblem& problem,
                         VisionSummarizationResult* result) {
  if (problem.desired_num_points > problem.num_points) return true;

  CHECK_EQ(problem.points_weight.size(), problem.num_points);
  CHECK_EQ(problem.image_visibility.size(), problem.num_images);
  CHECK(result != nullptr);

  // make ortools problem
  std::unique_ptr<operations_research::MPSolver> solver(
      operations_research::MPSolver::CreateSolver("scip"));
  if (!solver) {
    LOG_IF(WARNING, problem.verbose) << "scip solver unavailable.";
    return false;
  }
  // solver->SetTimeLimit(60 * 60 * 1e9);

  // add variables
  std::vector<const operations_research::MPVariable*> x(problem.num_points);
  std::vector<const operations_research::MPVariable*> tau(problem.num_images);
  for (size_t i = 0; i < problem.num_points; i++) {
    x[i] = solver->MakeIntVar(0, 1, "");
  }
  const double infinity = solver->infinity();
  for (size_t i = 0; i < problem.num_images; i++) {
    tau[i] = solver->MakeIntVar(0.0, infinity, "");
  }

  // add constraints
  // number of points left matches desired_num_points
  operations_research::LinearExpr valid_point_sum;
  for (size_t i = 0; i < problem.num_points; i++) {
    valid_point_sum += x[i];
  }
  solver->MakeRowConstraint(valid_point_sum == problem.desired_num_points);

  // every image should have enough observation
  for (size_t i = 0; i < problem.num_images; i++) {
    operations_research::LinearExpr image_viz_sum;
    for (size_t j = 0; j < problem.num_points; j++) {
      if (problem.image_visibility[i][j]) {
        image_viz_sum += x[j];
      }
    }
    image_viz_sum += tau[i];
    solver->MakeRowConstraint(image_viz_sum >= problem.min_points_per_image);
  }

  // Objective.
  operations_research::MPObjective* const objective = solver->MutableObjective();
  for (size_t i = 0; i < problem.num_points; i++) {
    objective->SetCoefficient(x[i], problem.points_weight[i]);
  }
  for (size_t i = 0; i < problem.num_images; i++) {
    objective->SetCoefficient(tau[i], problem.lambda);
  }

  LOG_IF(INFO, problem.verbose) << "Solving...";
  const operations_research::MPSolver::ResultStatus result_status = solver->Solve();
  if (result_status != operations_research::MPSolver::OPTIMAL) {
    LOG_IF(ERROR, problem.verbose) << "The problem does not have an optimal solution!";
    return false;
  }

  // make the output
  result->points_left.clear();
  for (size_t i = 0; i < problem.num_points; i++) {
    if (x[i]->solution_value() > 0.5) {
      result->points_left.insert(i);
    }
  }
  // make the visibility map
  result->image_visibility.clear();
  for (size_t i = 0; i < problem.num_images; i++) {
    std::vector<bool> image_viz(problem.num_points, false);
    for (size_t j = 0; j < problem.num_points; j++) {
      if (problem.image_visibility[i][j] &&
          result->points_left.find(j) != result->points_left.end()) {
        image_viz[j] = true;
      }
    }
    result->image_visibility.emplace_back(std::move(image_viz));
  }

  LOG_IF(INFO, problem.verbose) << "Solution:";
  LOG_IF(INFO, problem.verbose) << "Objective value = " << objective->Value();
  LOG_IF(INFO, problem.verbose) << "Problem solved in " << solver->DurationSinceConstruction() << " milliseconds";
  LOG_IF(INFO, problem.verbose) << "Problem solved in " << solver->iterations() << " iterations";

  return true;
}

}  // namespace dm::graph
