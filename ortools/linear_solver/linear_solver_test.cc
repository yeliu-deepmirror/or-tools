// Copyright 2010-2021 Google LLC
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

#include "ortools/linear_solver/linear_solver.h"

namespace operations_research {

void LinearSolverTest() {
  MPSolver* solver = MPSolver::CreateSolver("glop");

  const double infinity = solver->infinity();
  // Create the variables x and y.
  MPVariable* const x = solver->MakeNumVar(0.0, infinity, "x");
  MPVariable* const y = solver->MakeNumVar(0.0, infinity, "y");

  LOG(INFO) << "Number of variables = " << solver->NumVariables();

  // [START constraints]
  // x + 7 * y <= 17.5.
  MPConstraint* const c0 = solver->MakeRowConstraint(-infinity, 17.5, "c0");
  c0->SetCoefficient(x, 1);
  c0->SetCoefficient(y, 7);

  // x <= 3.5.
  MPConstraint* const c1 = solver->MakeRowConstraint(-infinity, 3.5, "c1");
  c1->SetCoefficient(x, 1);
  c1->SetCoefficient(y, 0);

  LOG(INFO) << "Number of constraints = " << solver->NumConstraints();
  // [END constraints]

  // [START objective]
  // Maximize x + 10 * y.
  MPObjective* const objective = solver->MutableObjective();
  objective->SetCoefficient(x, 1);
  objective->SetCoefficient(y, 10);
  objective->SetMaximization();
  // [END objective]

  // [START solve]
  const MPSolver::ResultStatus result_status = solver->Solve();
  // Check that the problem has an optimal solution.
  if (result_status != MPSolver::OPTIMAL) {
    LOG(FATAL) << "The problem does not have an optimal solution!";
  }
  // [END solve]

  // [START print_solution]
  LOG(INFO) << "Solution:" << std::endl;
  LOG(INFO) << "Objective value = " << objective->Value();
  LOG(INFO) << "x = " << x->solution_value();
  LOG(INFO) << "y = " << y->solution_value();
  // [END print_solution]

  // [START advanced]
  LOG(INFO) << "Advanced usage:";
  LOG(INFO) << "Problem solved in " << solver->DurationSinceConstruction() << " milliseconds";
  LOG(INFO) << "Problem solved in " << solver->iterations() << " iterations";
  // [END advanced]
}


// Mixed Integer Programs (MIPs) example
// https://developers.google.com/optimization/assignment/assignment_example
void AssignmentProgramTest() {
  // create the problem
  const std::vector<std::vector<double>> costs{
      {90, 80, 75, 70}, {35, 85, 55, 65}, {125, 95, 90, 95}, {45, 110, 95, 115}, {50, 100, 90, 100},
  };
  const int num_workers = costs.size();
  const int num_tasks = costs[0].size();

  // Solver
  // Create the mip solver with the SCIP backend.
  std::unique_ptr<MPSolver> solver(MPSolver::CreateSolver("scip"));
  if (!solver) {
    LOG(WARNING) << "SCIP solver unavailable.";
    return;
  }

  // Variables
  // x[i][j] is an array of 0-1 variables, which will be 1
  // if worker i is assigned to task j.
  std::vector<std::vector<const MPVariable*>> x(num_workers,
                                                std::vector<const MPVariable*>(num_tasks));
  for (int i = 0; i < num_workers; ++i) {
    for (int j = 0; j < num_tasks; ++j) {
      x[i][j] = solver->MakeIntVar(0, 1, "");
    }
  }

  // Constraints
  // Each worker is assigned to at most one task.
  for (int i = 0; i < num_workers; ++i) {
    LinearExpr worker_sum;
    for (int j = 0; j < num_tasks; ++j) {
      worker_sum += x[i][j];
    }
    solver->MakeRowConstraint(worker_sum <= 1.0);
  }
  // Each task is assigned to exactly one worker.
  for (int j = 0; j < num_tasks; ++j) {
    LinearExpr task_sum;
    for (int i = 0; i < num_workers; ++i) {
      task_sum += x[i][j];
    }
    solver->MakeRowConstraint(task_sum == 1.0);
  }

  // Objective.
  MPObjective* const objective = solver->MutableObjective();
  for (int i = 0; i < num_workers; ++i) {
    for (int j = 0; j < num_tasks; ++j) {
      objective->SetCoefficient(x[i][j], costs[i][j]);
    }
  }
  objective->SetMinimization();

  // Solve
  const MPSolver::ResultStatus result_status = solver->Solve();

  // Print solution.
  // Check that the problem has a feasible solution.
  if (result_status != MPSolver::OPTIMAL && result_status != MPSolver::FEASIBLE) {
    LOG(FATAL) << "No solution found " << result_status;
  }

  LOG(INFO) << "Total cost = " << objective->Value() << "\n\n";

  for (int i = 0; i < num_workers; ++i) {
    for (int j = 0; j < num_tasks; ++j) {
      // Test if x[i][j] is 0 or 1 (with tolerance for floating point
      // arithmetic).
      if (x[i][j]->solution_value() > 0.5) {
        LOG(INFO) << "Worker " << i << " assigned to task " << j << ".  Cost = " << costs[i][j];
      }
    }
  }
  LOG(INFO) << "Advanced usage:";
  LOG(INFO) << "Problem solved in " << solver->DurationSinceConstruction() << " milliseconds";
  LOG(INFO) << "Problem solved in " << solver->iterations() << " iterations";
}

}  // namespace operations_research

int main(int argc, char** argv) {
  LOG(INFO) << "\nLinearSolverTest";
  operations_research::LinearSolverTest();
  LOG(INFO) << "\nAssignmentProgramTest";
  operations_research::AssignmentProgramTest();
  LOG(INFO) << "done.";
  return 1;
}
