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

#ifndef OR_TOOLS_MATH_OPT_SOLVERS_GUROBI_SOLVER_H_
#define OR_TOOLS_MATH_OPT_SOLVERS_GUROBI_SOLVER_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "ortools/base/linked_hash_map.h"
#include "ortools/gurobi/environment.h"
#include "ortools/math_opt/callback.pb.h"
#include "ortools/math_opt/core/inverted_bounds.h"
#include "ortools/math_opt/core/solve_interrupter.h"
#include "ortools/math_opt/core/solver_interface.h"
#include "ortools/math_opt/model.pb.h"
#include "ortools/math_opt/model_parameters.pb.h"
#include "ortools/math_opt/model_update.pb.h"
#include "ortools/math_opt/parameters.pb.h"
#include "ortools/math_opt/result.pb.h"
#include "ortools/math_opt/solution.pb.h"
#include "ortools/math_opt/solvers/gurobi/g_gurobi.h"
#include "ortools/math_opt/solvers/gurobi_callback.h"
#include "ortools/math_opt/solvers/message_callback_data.h"
#include "ortools/math_opt/sparse_containers.pb.h"

namespace operations_research {
namespace math_opt {

class GurobiSolver : public SolverInterface {
 public:
  static absl::StatusOr<std::unique_ptr<GurobiSolver>> New(
      const ModelProto& input_model,
      const SolverInterface::InitArgs& init_args);

  absl::StatusOr<SolveResultProto> Solve(
      const SolveParametersProto& parameters,
      const ModelSolveParametersProto& model_parameters,
      MessageCallback message_cb,
      const CallbackRegistrationProto& callback_registration, Callback cb,
      SolveInterrupter* interrupter) override;
  absl::Status Update(const ModelUpdateProto& model_update) override;
  bool CanUpdate(const ModelUpdateProto& model_update) override;

 private:
  struct GurobiCallbackData {
    explicit GurobiCallbackData(GurobiCallbackInput callback_input,
                                SolveInterrupter* const local_interrupter)
        : callback_input(std::move(callback_input)),
          local_interrupter(local_interrupter) {}
    const GurobiCallbackInput callback_input;

    // Interrupter triggered when either the user interrupter passed to Solve()
    // is triggered or after one user callback returned a true `terminate`.
    //
    // This is not the user interrupter though so it safe for callbacks to
    // trigger it.
    //
    // It is optional; it is not null when either we have a LP/MIP callback or a
    // user interrupter. But it can be null if we only have a message callback.
    SolveInterrupter* const local_interrupter;

    MessageCallbackData message_callback_data;

    absl::Status status = absl::OkStatus();
  };

  explicit GurobiSolver(std::unique_ptr<Gurobi> g_gurobi);

  // For easing reading the code, we declare these types:
  using VariableId = int64_t;
  using LinearConstraintId = int64_t;
  using QuadraticConstraintId = int64_t;
  using GurobiVariableIndex = int;
  using GurobiLinearConstraintIndex = int;
  using GurobiQuadraticConstraintIndex = int;
  using GurobiAnyConstraintIndex = int;

  static constexpr GurobiVariableIndex kUnspecifiedIndex = -1;
  static constexpr GurobiAnyConstraintIndex kUnspecifiedConstraint = -2;
  static constexpr double kInf = std::numeric_limits<double>::infinity();

  // Data associated with each linear constraint. With it we know if the
  // underlying representation is either:
  //   linear_terms <= upper_bound (if lower bound <= -GRB_INFINITY)
  //   linear_terms >= lower_bound (if upper bound >= GRB_INFINTY)
  //   linear_terms == xxxxx_bound (if upper_bound == lower_bound)
  //   linear_term - slack == 0 (with slack bounds equal to xxxxx_bound)
  struct LinearConstraintData {
    GurobiLinearConstraintIndex constraint_index = kUnspecifiedConstraint;
    // only valid for true ranged constraints.
    GurobiVariableIndex slack_index = kUnspecifiedIndex;
    double lower_bound = -kInf;
    double upper_bound = kInf;
  };

  struct SlackInfo {
    LinearConstraintId id;
    LinearConstraintData& constraint_data;
    SlackInfo(const LinearConstraintId input_id,
              LinearConstraintData& input_constraint)
        : id(input_id), constraint_data(input_constraint) {}
  };

  struct SolutionClaims {
    bool primal_feasible_solution_exists;
    bool dual_feasible_solution_exists;
  };

  struct SolutionsAndClaims {
    std::vector<SolutionProto> solutions;
    SolutionClaims solution_claims;
  };

  template <typename SolutionType>
  struct SolutionAndClaim {
    std::optional<SolutionType> solution;
    bool feasible_solution_exists = false;
  };

  using IdHashMap = gtl::linked_hash_map<int64_t, int>;

  absl::StatusOr<ProblemStatusProto> GetProblemStatus(
      const int grb_termination, const SolutionClaims solution_claims);
  absl::StatusOr<SolveResultProto> ExtractSolveResultProto(
      absl::Time start, const ModelSolveParametersProto& model_parameters);
  absl::Status FillRays(const ModelSolveParametersProto& model_parameters,
                        const SolutionClaims solution_claims,
                        SolveResultProto& result);
  absl::StatusOr<GurobiSolver::SolutionsAndClaims> GetSolutions(
      const ModelSolveParametersProto& model_parameters);
  absl::StatusOr<SolveStatsProto> GetSolveStats(absl::Time start,
                                                SolutionClaims solution_claims);

  absl::StatusOr<double> GetBestDualBound();
  absl::StatusOr<double> GetBestPrimalBound(bool has_primal_feasible_solution);
  bool PrimalSolutionQualityAvailable() const;
  absl::StatusOr<double> GetPrimalSolutionQuality() const;

  // Warning: is read from gurobi, take care with gurobi update.
  absl::StatusOr<bool> IsMaximize() const;

  static absl::StatusOr<TerminationProto> ConvertTerminationReason(
      int gurobi_status, SolutionClaims solution_claims);

  // Returns solution information appropriate and available for an LP (linear
  // constraints + linear objective, only).
  absl::StatusOr<SolutionsAndClaims> GetLpSolution(
      const ModelSolveParametersProto& model_parameters);
  // Returns solution information appropriate and available for a QP (linear
  // constraints + quadratic objective, only).
  absl::StatusOr<SolutionsAndClaims> GetQpSolution(
      const ModelSolveParametersProto& model_parameters);
  // Returns solution information appropriate and available for a QCP
  // (linear/quadratic constraints + linear/quadratic objective, only).
  absl::StatusOr<SolutionsAndClaims> GetQcpSolution(
      const ModelSolveParametersProto& model_parameters);
  // Returns solution information appropriate and available for a MIP
  // (integrality on some/all decision variables).
  absl::StatusOr<SolutionsAndClaims> GetMipSolutions(
      const ModelSolveParametersProto& model_parameters);

  // return bool field should be true if a primal solution exists.
  absl::StatusOr<SolutionAndClaim<PrimalSolutionProto>>
  GetConvexPrimalSolutionIfAvailable(
      const ModelSolveParametersProto& model_parameters);
  absl::StatusOr<SolutionAndClaim<DualSolutionProto>>
  GetLpDualSolutionIfAvailable(
      const ModelSolveParametersProto& model_parameters);
  absl::StatusOr<std::optional<BasisProto>> GetBasisIfAvailable();

  absl::Status SetParameters(const SolveParametersProto& parameters);
  absl::Status AddNewLinearConstraints(
      const LinearConstraintsProto& constraints);
  absl::Status AddNewQuadraticConstraints(
      const google::protobuf::Map<QuadraticConstraintId,
                                  QuadraticConstraintProto>& constraints);
  absl::Status AddNewVariables(const VariablesProto& new_variables);
  absl::Status AddNewSlacks(const std::vector<SlackInfo>& new_slacks);
  absl::Status ChangeCoefficients(const SparseDoubleMatrixProto& matrix);
  // NOTE: Clears any existing quadratic objective terms.
  absl::Status ResetQuadraticObjectiveTerms(
      const SparseDoubleMatrixProto& terms);
  // Updates objective so that it is the sum of everything in terms, plus all
  // other terms prexisting in the objective that are not overwritten by terms.
  absl::Status UpdateQuadraticObjectiveTerms(
      const SparseDoubleMatrixProto& terms);
  absl::Status LoadModel(const ModelProto& input_model);

  absl::Status UpdateDoubleListAttribute(const SparseDoubleVectorProto& update,
                                         const char* attribute_name,
                                         const IdHashMap& id_hash_map);
  absl::Status UpdateInt32ListAttribute(const SparseInt32VectorProto& update,
                                        const char* attribute_name,
                                        const IdHashMap& id_hash_map);
  absl::Status UpdateGurobiIndices();
  absl::Status UpdateLinearConstraints(
      const LinearConstraintUpdatesProto& update,
      std::vector<GurobiVariableIndex>& deleted_variables_index);

  int num_linear_constraints() const;
  int num_quadratic_constraints() const;
  int get_model_index(GurobiVariableIndex index) const { return index; }
  int get_model_index(const LinearConstraintData& index) const {
    return index.constraint_index;
  }

  // Fills in result with the values in gurobi_values aided by the index
  // conversion from map which should be either variables_map_ or
  // linear_constraints_map_ as appropriate. Only key/value pairs that passes
  // the filter predicate are added.
  template <typename T>
  void GurobiVectorToSparseDoubleVector(
      absl::Span<const double> gurobi_values, const T& map,
      SparseDoubleVectorProto& result,
      const SparseVectorFilterProto& filter) const;
  absl::StatusOr<BasisProto> GetGurobiBasis();
  absl::Status SetGurobiBasis(const BasisProto& basis);
  absl::StatusOr<DualRayProto> GetGurobiDualRay(
      const SparseVectorFilterProto& linear_constraints_filter,
      const SparseVectorFilterProto& variables_filter, bool is_maximize);
  // Returns true if the problem has any integrality constraints.
  absl::StatusOr<bool> IsMIP() const;
  // Returns true if the problem has a quadratic objective.
  absl::StatusOr<bool> IsQP() const;
  // Returns true if the problem has any quadratic constraints.
  absl::StatusOr<bool> IsQCP() const;

  absl::StatusOr<std::unique_ptr<GurobiCallbackData>> RegisterCallback(
      const CallbackRegistrationProto& registration, Callback cb,
      const MessageCallback message_cb, absl::Time start,
      SolveInterrupter* interrupter);

  // Returns the ids of variables and linear constraints with inverted bounds.
  absl::StatusOr<InvertedBounds> ListInvertedBounds() const;

  const std::unique_ptr<Gurobi> gurobi_;

  // Note that we use linked_hash_map because the index of the gurobi_model_
  // variables/constraints is exactly the order in which they are added to the
  // model.
  // Internal correspondence from variable proto IDs to Gurobi-numbered
  // variables.
  gtl::linked_hash_map<VariableId, GurobiVariableIndex> variables_map_;
  // Internal correspondence from linear constraint proto IDs to
  // Gurobi-numbered linear constraint and extra information.
  gtl::linked_hash_map<LinearConstraintId, LinearConstraintData>
      linear_constraints_map_;
  // Internal correspondence from quadratic constraint proto IDs to
  // Gurobi-numbered quadratic constraint and extra information.
  gtl::linked_hash_map<QuadraticConstraintId, GurobiQuadraticConstraintIndex>
      quadratic_constraints_map_;
  // For those constraints that need a slack in the gurobi_model_
  // representation (i.e. those with
  // LinearConstraintData.slack_index != kUnspecifiedIndex), we keep a hash_map
  // of those LinearConstraintId's to a reference of the LinearConstraintData
  // stored in the linear_constraints_map_. This means that we only have one
  // location were we store information about constraints, but two places from
  // where we can access it. Furthermore, the internal slack_index associated
  // with each actual slack variable is increasing in the order of them in this
  // linear_slack_map_, and is the main reason why we choose to use
  // gtl::linked_hash_map, as the order of insertion (of the remaining
  // elements) is preserved after removals.
  // Note that after deletions, the renumbering of variables must be done
  // simultaneously for variables and slacks following the merged-order of the
  // variable_index and linear_slack_map_.slack_index.
  gtl::linked_hash_map<LinearConstraintId, LinearConstraintData&> slack_map_;
  // Number of Gurobi variables, this internal quantity is updated when we
  // actually add or remove variables or constraints in the underlying Gurobi
  // model. Furthermore, since 'ModelUpdate' can trigger both creation and
  // deletion of variables and constraints, we batch those changes in two
  // steps: first add all new variables and constraints, and then delete all
  // variables and constraints that need deletion. Finally flush changes at
  // the gurobi model level (if any deletion was performed).
  int num_gurobi_variables_ = 0;
  // Gurobi does not expose a way to query quadratic objective terms from the
  // model, so we track them. Notes:
  //   * Keys are in upper triangular order (.first <= .second)
  //   * Terms not in the map have zero coefficients
  // Note also that the map may also have entries with zero coefficient value.
  absl::flat_hash_map<std::pair<VariableId, VariableId>, double>
      quadratic_objective_coefficients_;

  static constexpr int kGrbBasicConstraint = 0;
  static constexpr int kGrbNonBasicConstraint = -1;
};

}  // namespace math_opt
}  // namespace operations_research

#endif  // OR_TOOLS_MATH_OPT_SOLVERS_GUROBI_SOLVER_H_
