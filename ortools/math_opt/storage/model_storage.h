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

#ifndef OR_TOOLS_MATH_OPT_STORAGE_MODEL_STORAGE_H_
#define OR_TOOLS_MATH_OPT_STORAGE_MODEL_STORAGE_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/meta/type_traits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "ortools/base/map_util.h"
#include "ortools/base/strong_int.h"
#include "ortools/math_opt/model.pb.h"
#include "ortools/math_opt/model_update.pb.h"
#include "ortools/math_opt/sparse_containers.pb.h"
#include "ortools/math_opt/storage/model_storage_types.h"
#include "ortools/math_opt/storage/sparse_matrix.h"
#include "ortools/math_opt/storage/update_trackers.h"

namespace operations_research {
namespace math_opt {

// An index based C++ API for building & storing optimization problems.
//
// Note that this API should usually not be used by C++ users that should prefer
// the math_opt/cpp/model.h API.
//
// It supports the efficient creation and modification of an optimization model,
// and the export of ModelProto and ModelUpdateProto protos.
//
// All methods run in amortized O(1) (as amortized over calls to that exact
// function) unless otherwise specified.
//
// Models problems of the form:
//   min sum_{j in J} c_j * x_j + d
//   s.t. lb^c_i <= sum_{j in J} A_ij * x_j <= ub^c_i        for all i in I,
//        lb^v_j <= x_j <= ub^v_j                            for all j in J,
//        x_j integer                                        for all j in Z,
// where above:
//  * I: the set of linear constraints,
//  * J: the set of variables,
//  * Z: a subset of J, the integer variables,
//  * x: the decision variables (indexed by J),
//  * c: the linear objective, one double per variable,
//  * d: the objective offset, a double scalar,
//  * lb^c: the constraint lower bounds, one double per linear constraint,
//  * ub^c: the constraint upper bounds, one double per linear constraint,
//  * lb^v: the variable lower bounds, one double per variable,
//  * ub^v: the variable upper bounds, one double per variable,
//  * A: the linear constraint matrix, a double per variable/constraint pair.
//
// The min in the objective can also be changed to a max.
//
// A simple example:
//
// Model the problem:
//   max 2.0 * x + y
//   s.t. x + y <= 1.5
//            x in {0.0, 1.0}
//       0 <= y <= 2.5
//
//   using ::operations_research::math_opt::ModelStorage;
//   using ::operations_research::math_opt::VariableId;
//   using ::operations_research::math_opt::LinearConstraintId;
//   using ::operations_research::math_opt::ModelProto;
//   using ::operations_research::math_opt::ModelProtoUpdate;
//
//   ModelStorage model("my_model");
//   const VariableId x = model.AddVariable(0.0, 1.0, true, "x");
//   const VariableId y = model.AddVariable(0.0, 2.5, false, "y");
//   const LinearConstraintId c = model.AddLinearConstraint(
//       -std::numeric_limits<double>::infinity, 1.5, "c");
//   model.set_linear_constraint_coefficient(x, c, 1.0);
//   model.set_linear_constraint_coefficient(y, c, 1.0);
//   model.set_linear_objective_coefficient(x, 2.0);
//   model.set_linear_objective_coefficient(y, 1.0);
//   model.set_maximize();
//
// Now, export to a proto describing the model:
//
//   const ModelProto model_proto = model.ExportModel();
//
// Modify the problem and get a model update proto:
//
//   const UpdateTrackerId update_tracker = model.NewUpdateTracker();
//   c.set_upper_bound(2.0);
//   const std::optional<ModelUpdateProto> update_proto =
//     model.ExportModelUpdate(update_tracker);
//   model.Checkpoint(update_tracker);
//
// Reading and writing model properties:
//
// Properties of the model (e.g. variable/constraint bounds) can be written
// and read in amortized O(1) time. Deleting a variable will take time
// O(#constraints containing the variable), and likewise deleting a constraint
// will take time O(#variables in the constraint). The constraint matrix is
// stored as hash map where the key is a {LinearConstraintId, VariableId}
// pair and the value is the coefficient. The nonzeros of the matrix are
// additionally stored by row and by column.
//
// Exporting the Model proto:
//
// The Model proto is an equivalent representation to ModelStorage. It has a
// smaller memory footprint and optimized for storage/transport, rather than
// efficient modification. It is also the format consumed by solvers in this
// library. The Model proto can be generated by calling
// ModelStorage::ExportModel().
//
// Incrementalism, the ModelUpdate proto, and Checkpoints:
//
// To update an existing model as specified by a Model proto, solvers consume a
// ModelUpdate proto, which describes the changes to a model (e.g. new variables
// or a change in a variable bound). ModelStorage::NewUpdateTracker() tracks the
// changes made and produces a ModelUpdate proto describing these changes with
// the method ModelStorage::ExportModelUpdate(). The changes returned will be
// the modifications since the previous call to ModelStorage::Checkpoint(). Note
// that, for newly initialized models, before the first checkpoint, there is no
// additional memory overhead from tracking changes. See
// docs/ortools/math_opt/docs/model_building_complexity.md
// for details.
//
// On bad input:
//
// Using a bad variable id or constraint id (an id not in the current model,
// which includes ids that have been deleted) on any method will result in an
// immediate failure (either a CHECK failure or an exception, which is an
// implementation detail you should not rely on). We make no attempt to say if a
// model is invalid (e.g. a variable lower bound is infinite, exceeds an upper
// bound, or is NaN). The exported models are validated instead, see
// model_validator.h.
class ModelStorage {
 public:
  // Returns a storage from the input proto. Returns a failure status if the
  // input proto is invalid.
  //
  // Variable/constraint names can be repeated in the input proto but will be
  // considered invalid when solving.
  //
  // See ApplyUpdateProto() for dealing with subsequent updates.
  static absl::StatusOr<std::unique_ptr<ModelStorage>> FromModelProto(
      const ModelProto& model_proto);

  // Creates an empty minimization problem.
  explicit ModelStorage(absl::string_view name = "") : name_(name) {}

  ModelStorage(const ModelStorage&) = delete;
  ModelStorage& operator=(const ModelStorage&) = delete;

  // Returns a clone of the model, optionally changing model's name.
  //
  // The variables and constraints have the same ids. The clone will also not
  // reused any id of variable/constraint that was deleted in the original.
  //
  // Note that the returned model does not have any update tracker.
  std::unique_ptr<ModelStorage> Clone(
      std::optional<absl::string_view> new_name = std::nullopt) const;

  inline const std::string& name() const { return name_; }

  //////////////////////////////////////////////////////////////////////////////
  // Variables
  //////////////////////////////////////////////////////////////////////////////

  // Adds a continuous unbounded variable to the model and returns its id.
  //
  // See AddVariable(double, double, bool, absl::string_view) for details.
  inline VariableId AddVariable(absl::string_view name = "");

  // Adds a variable to the model and returns its id.
  //
  // The returned ids begin at zero and increase by one with each call to
  // AddVariable. Deleted ids are NOT reused. If no variables are deleted,
  // the ids in the model will be consecutive.
  VariableId AddVariable(double lower_bound, double upper_bound,
                         bool is_integer, absl::string_view name = "");

  inline double variable_lower_bound(VariableId id) const;
  inline double variable_upper_bound(VariableId id) const;
  inline bool is_variable_integer(VariableId id) const;
  inline const std::string& variable_name(VariableId id) const;

  inline void set_variable_lower_bound(VariableId id, double lower_bound);
  inline void set_variable_upper_bound(VariableId id, double upper_bound);
  inline void set_variable_is_integer(VariableId id, bool is_integer);
  inline void set_variable_as_integer(VariableId id);
  inline void set_variable_as_continuous(VariableId id);

  // Removes a variable from the model.
  //
  // It is an error to use a deleted variable id as input to any subsequent
  // function calls on the model. Runs in O(#constraints containing the
  // variable).
  void DeleteVariable(VariableId id);

  // The number of variables in the model.
  //
  // Equal to the number of variables created minus the number of variables
  // deleted.
  inline int num_variables() const;

  // The returned id of the next call to AddVariable.
  //
  // Equal to the number of variables created.
  inline VariableId next_variable_id() const;

  // Returns true if this id has been created and not yet deleted.
  inline bool has_variable(VariableId id) const;

  // The VariableIds in use (not deleted), order not defined.
  std::vector<VariableId> variables() const;

  // Returns a sorted vector of all existing (not deleted) variables in the
  // model.
  //
  // Runs in O(n log(n)), where n is the number of variables returned.
  std::vector<VariableId> SortedVariables() const;

  //////////////////////////////////////////////////////////////////////////////
  // Linear Constraints
  //////////////////////////////////////////////////////////////////////////////

  // Adds a linear constraint to the model with a lower bound of -inf and an
  // upper bound of +inf and returns its id.
  //
  // See AddLinearConstraint(double, double, absl::string_view) for details.
  inline LinearConstraintId AddLinearConstraint(absl::string_view name = "");

  // Adds a linear constraint to the model returns its id.
  //
  // The returned ids begin at zero and increase by one with each call to
  // AddLinearConstraint. Deleted ids are NOT reused. If no linear
  // constraints are deleted, the ids in the model will be consecutive.
  LinearConstraintId AddLinearConstraint(double lower_bound, double upper_bound,
                                         absl::string_view name = "");

  inline double linear_constraint_lower_bound(LinearConstraintId id) const;
  inline double linear_constraint_upper_bound(LinearConstraintId id) const;
  inline const std::string& linear_constraint_name(LinearConstraintId id) const;

  inline void set_linear_constraint_lower_bound(LinearConstraintId id,
                                                double lower_bound);
  inline void set_linear_constraint_upper_bound(LinearConstraintId id,
                                                double upper_bound);

  // Removes a linear constraint from the model.
  //
  // It is an error to use a deleted linear constraint id as input to any
  // subsequent function calls on the model. Runs in O(#variables in the linear
  // constraint).
  void DeleteLinearConstraint(LinearConstraintId id);

  // The number of linear constraints in the model.
  //
  // Equal to the number of linear constraints created minus the number of
  // linear constraints deleted.
  inline int num_linear_constraints() const;

  // The returned id of the next call to AddLinearConstraint.
  //
  // Equal to the number of linear constraints created.
  inline LinearConstraintId next_linear_constraint_id() const;

  // Returns true if this id has been created and not yet deleted.
  inline bool has_linear_constraint(LinearConstraintId id) const;

  // The LinearConstraintsIds in use (not deleted), order not defined.
  std::vector<LinearConstraintId> linear_constraints() const;

  // Returns a sorted vector of all existing (not deleted) linear constraints in
  // the model.
  //
  // Runs in O(n log(n)), where n is the number of linear constraints returned.
  std::vector<LinearConstraintId> SortedLinearConstraints() const;

  //////////////////////////////////////////////////////////////////////////////
  // Linear constraint matrix
  //////////////////////////////////////////////////////////////////////////////

  // Returns 0.0 if the entry is not in matrix.
  inline double linear_constraint_coefficient(LinearConstraintId constraint,
                                              VariableId variable) const;
  inline bool is_linear_constraint_coefficient_nonzero(
      LinearConstraintId constraint, VariableId variable) const;

  // Setting a value to 0.0 will delete the {constraint, variable} pair from the
  // underlying sparse matrix representation (and has no effect if the pair is
  // not present).
  inline void set_linear_constraint_coefficient(LinearConstraintId constraint,
                                                VariableId variable,
                                                double value);

  // The {linear constraint, variable} pairs with nonzero linear constraint
  // matrix coefficients.
  inline const absl::flat_hash_map<std::pair<LinearConstraintId, VariableId>,
                                   double>&
  linear_constraint_matrix() const;

  // Returns the variables with nonzero coefficients in a linear constraint.
  inline const absl::flat_hash_set<VariableId>& variables_in_linear_constraint(
      LinearConstraintId constraint) const;

  // Returns the linear constraints with nonzero coefficients on a variable.
  inline const absl::flat_hash_set<LinearConstraintId>&
  linear_constraints_with_variable(VariableId variable) const;

  //////////////////////////////////////////////////////////////////////////////
  // Objective
  //////////////////////////////////////////////////////////////////////////////

  inline bool is_maximize() const;
  inline double objective_offset() const;
  // Returns 0.0 if this variable has no linear objective coefficient.
  inline double linear_objective_coefficient(VariableId variable) const;
  // The ordering of the input variables does not matter.
  inline double quadratic_objective_coefficient(
      VariableId first_variable, VariableId second_variable) const;
  inline bool is_linear_objective_coefficient_nonzero(
      VariableId variable) const;
  // The ordering of the input variables does not matter.
  inline bool is_quadratic_objective_coefficient_nonzero(
      VariableId first_variable, VariableId second_variable) const;

  inline void set_is_maximize(bool is_maximize);
  inline void set_maximize();
  inline void set_minimize();
  inline void set_objective_offset(double value);

  // Setting a value to 0.0 will delete the variable from the underlying sparse
  // representation (and has no effect if the variable is not present).
  inline void set_linear_objective_coefficient(VariableId variable,
                                               double value);
  // Setting a value to 0.0 will delete the variable pair from the underlying
  // sparse representation (and has no effect if the pair is not present).
  // The ordering of the input variables does not matter.
  inline void set_quadratic_objective_coefficient(VariableId first_variable,
                                                  VariableId second_variable,
                                                  double value);

  // Equivalent to calling set_linear_objective_coefficient(v, 0.0) for every
  // variable with nonzero objective coefficient.
  //
  // Runs in O(# nonzero linear/quadratic objective terms).
  inline void clear_objective();

  // The variables with nonzero linear objective coefficients.
  inline const absl::flat_hash_map<VariableId, double>& linear_objective()
      const;

  inline int64_t num_quadratic_objective_terms() const;

  // The variable pairs with nonzero quadratic objective coefficients. The keys
  // are ordered such that .first <= .second. All values are nonempty.
  //
  // TODO(b/233630053) do no allocate the result, expose an iterator API.
  inline std::vector<std::tuple<VariableId, VariableId, double>>
  quadratic_objective_terms() const;

  // Returns a sorted vector of all variables in the model with nonzero linear
  // objective coefficients.
  //
  // Runs in O(n log(n)), where n is the number of variables returned.
  std::vector<VariableId> SortedLinearObjectiveNonzeroVariables() const;

  //////////////////////////////////////////////////////////////////////////////
  // Export
  //////////////////////////////////////////////////////////////////////////////

  // Returns a proto representation of the optimization model.
  //
  // See FromModelProto() to build a ModelStorage from a proto.
  ModelProto ExportModel() const;

  // Creates a tracker that can be used to generate a ModelUpdateProto with the
  // updates that happened since the last checkpoint. The tracker initial
  // checkpoint corresponds to the current state of the model.
  //
  // Thread-safety: this method must not be used while modifying the
  // ModelStorage. The user is expected to use proper synchronization primitive
  // to serialize changes to the model and trackers creations. That said
  // multiple trackers can be created concurrently.
  //
  // For each update tracker we define a checkpoint that is the starting point
  // used to compute the ModelUpdateProto.
  //
  // Example:
  //   ModelStorage model;
  //   ...
  //   ASSIGN_OR_RETURN(const auto solver,
  //                    Solver::New(solver_type, model.ExportModel(),
  //                                /*initializer=*/{}));
  //   const UpdateTrackerId update_tracker = model.NewUpdatesTracker();
  //
  //   ASSIGN_OR_RETURN(const auto result_1,
  //                    solver->Solve(/*parameters=*/{});
  //
  //   model.AddVariable(0.0, 1.0, true, "y");
  //   model.set_maximize(true);
  //
  //   const std::optional<ModelUpdateProto> update_proto =
  //     model.ExportModelUpdate(update_tracker);
  //   model.Checkpoint(update_tracker);
  //
  //   if (update_proto) {
  //     ASSIGN_OR_RETURN(const bool updated, solver->Update(*update_proto));
  //     if (!updated) {
  //       // The update is not supported by the solver, we create a new one.
  //       ASSIGN_OR_RETURN(const auto new_model_proto, model.ExportModel());
  //       ASSIGN_OR_RETURN(solver,
  //                        Solver::New(solver_type, new_model_proto,
  //                                    /*initializer=*/{}));
  //     }
  //   }
  //   ASSIGN_OR_RETURN(const auto result_2,
  //                    solver->Solve(/*parameters=*/{});
  //
  UpdateTrackerId NewUpdateTracker();

  // Deletes the input tracker.
  //
  // It must not be used anymore after its destruction. It can be deleted once,
  // trying to delete it a second time or use it will raise an assertion
  // (CHECK).
  //
  // The update trackers are automatically deleted when the ModelStorage is
  // destroyed. Calling this function is thus only useful for performance
  // reasons, to ensure the ModelStorage does not keep data for update trackers
  // that are not needed anymore.
  //
  // Thread-safety: this method can be called at any time, even during the
  // creation of other trackers or during model modification. It must not be
  // called concurrently with ExportModelUpdate() or Checkpoint() though.
  void DeleteUpdateTracker(UpdateTrackerId update_tracker);

  // Returns a proto representation of the changes to the model since the most
  // recent checkpoint (i.e. last time Checkpoint() was called); nullopt if the
  // update would have been empty.
  //
  // Thread-safety: this method must not be used while modifying the
  // ModelStorage or after calling DeleteUpdateTracker(). The user is expected
  // to use proper synchronization primitive to serialize changes to the model
  // and the use of this method.
  //
  // It can be called concurrently for different update trackers though.
  std::optional<ModelUpdateProto> ExportModelUpdate(
      UpdateTrackerId update_tracker) const;

  // Uses the current model state as the starting point to calculate the
  // ModelUpdateProto next time ExportModelUpdate() is called.
  //
  // Thread-safety: this method must not be used while modifying the
  // ModelStorage or after calling DeleteUpdateTracker(). The user is expected
  // to use proper synchronization primitive to serialize changes to the model
  // and the use of this method.
  //
  // It can be called concurrently for different update trackers though.
  void Checkpoint(UpdateTrackerId update_tracker);

  // Apply the provided update to this model. Returns a failure if the update is
  // not valid.
  //
  // As with FromModelProto(), duplicated names are ignored.
  //
  // It takes O(num_variables + num_constraints) extra memory and execution to
  // apply the update (due to the need to build a ModelSummary). So even a small
  // update will have some cost.
  absl::Status ApplyUpdateProto(const ModelUpdateProto& update_proto);

 private:
  struct VariableData {
    double lower_bound = -std::numeric_limits<double>::infinity();
    double upper_bound = std::numeric_limits<double>::infinity();
    bool is_integer = false;
    std::string name = "";
  };

  struct LinearConstraintData {
    double lower_bound = -std::numeric_limits<double>::infinity();
    double upper_bound = std::numeric_limits<double>::infinity();
    std::string name = "";
  };

  struct UpdateTrackerData {
    UpdateTrackerData(VariableId variables_checkpoint,
                      LinearConstraintId linear_constraints_checkpoint)
        : variables_checkpoint(variables_checkpoint),
          linear_constraints_checkpoint(linear_constraints_checkpoint) {}

    // Returns a proto representation of the changes to the model since the most
    // recent call to SharedCheckpoint() or nullopt if no changes happened.
    //
    // Thread-safety: this method is threadsafe.
    std::optional<ModelUpdateProto> ExportModelUpdate(
        const ModelStorage& storage) const;

    // Use the current model state as the starting point to calculate the
    // ModelUpdateProto next time ExportSharedModelUpdate() is called.
    void Checkpoint(const ModelStorage& storage);

    // Update information
    //
    // Implicitly, all data for variables and constraints added after the last
    // checkpoint are considered "new" and will NOT be stored in the "dirty"
    // data structures below.
    VariableId variables_checkpoint;
    LinearConstraintId linear_constraints_checkpoint;
    bool dirty_objective_direction = false;
    bool dirty_objective_offset = false;

    absl::flat_hash_set<VariableId> dirty_variable_deletes;
    absl::flat_hash_set<VariableId> dirty_variable_lower_bounds;
    absl::flat_hash_set<VariableId> dirty_variable_upper_bounds;
    absl::flat_hash_set<VariableId> dirty_variable_is_integer;

    absl::flat_hash_set<VariableId> dirty_linear_objective_coefficients;
    // NOTE: quadratic objective coefficients are considered dirty, and
    // therefore tracked in this set, if and only if both variables in the term
    // are "old", i.e. not added since the last checkpoint.
    absl::flat_hash_set<std::pair<VariableId, VariableId>>
        dirty_quadratic_objective_coefficients;

    absl::flat_hash_set<LinearConstraintId> dirty_linear_constraint_deletes;
    absl::flat_hash_set<LinearConstraintId>
        dirty_linear_constraint_lower_bounds;
    absl::flat_hash_set<LinearConstraintId>
        dirty_linear_constraint_upper_bounds;

    // Only for pairs where both the variable and constraint are before the
    // checkpoint, i.e.
    //   var_id < variables_checkpoint  &&
    //   lin_con_id < linear_constraints_checkpoint
    absl::flat_hash_set<std::pair<LinearConstraintId, VariableId>>
        dirty_linear_constraint_matrix_keys;
  };

  template <typename T>
  void set_variable_property(VariableId id, T value, T VariableData::*field,
                             absl::flat_hash_set<VariableId>
                                 UpdateTrackerData::*const dirty_set_field);

  inline void set_linear_constraint_property(
      const LinearConstraintId id, double value,
      double LinearConstraintData::*field,
      absl::flat_hash_set<LinearConstraintId> UpdateTrackerData::*const
          dirty_set_field);

  // Adds a variable at the given id, updating next_variable_id_ and the
  // row/column collections as side effect. It CHECKs that the provided id is
  // not less than next_variable_id_.
  //
  // This is used internally by AddVariable() and AddVariables().
  void AddVariableInternal(VariableId id, double lower_bound,
                           double upper_bound, bool is_integer,
                           absl::string_view name);

  // Adds all variables from the given proto using AddVariableInternal(). Thus
  // Ids must be greater or equal to next_variable_id_.
  void AddVariables(const VariablesProto& variables);

  // Adds a linear constraint at the given id, updating
  // next_linear_constraint_id_ and the row/column collections as side effect.
  // It CHECKs that the provided id is not less than next_linear_constraint_id_.
  //
  // This is used internally by AddLinearConstraint() and
  // AddLinearConstraints().
  void AddLinearConstraintInternal(LinearConstraintId id, double lower_bound,
                                   double upper_bound, absl::string_view name);

  // Adds all constraints from the given proto using
  // AddLinearConstraintInternal(). Thus Ids must be greater or equal to
  // next_linear_constraint_id_.
  void AddLinearConstraints(const LinearConstraintsProto& linear_constraints);

  // Updates the objective linear coefficients. The coefficients of variables
  // not in the input are kept as-is.
  void UpdateLinearObjectiveCoefficients(
      const SparseDoubleVectorProto& coefficients);

  // Updates the objective quadratic coefficients. The coefficients of the pairs
  // of variables not in the input are kept as-is.
  void UpdateQuadraticObjectiveCoefficients(
      const SparseDoubleMatrixProto& coefficients);

  // Updates the linear constraints' coefficients. The coefficients of
  // (constraint, variable) pairs not in the input are kept as-is.
  void UpdateLinearConstraintCoefficients(
      const SparseDoubleMatrixProto& coefficients);

  // Export a single variable to proto.
  void AppendVariable(VariableId id, VariablesProto& variables_proto) const;

  // Export a single linear constraint to proto.
  void AppendLinearConstraint(
      LinearConstraintId id,
      LinearConstraintsProto& linear_constraints_proto) const;

  std::string name_;
  VariableId next_variable_id_ = VariableId(0);
  LinearConstraintId next_linear_constraint_id_ = LinearConstraintId(0);

  bool is_maximize_ = false;
  double objective_offset_ = 0.0;

  absl::flat_hash_map<VariableId, VariableData> variables_;
  absl::flat_hash_map<LinearConstraintId, LinearConstraintData>
      linear_constraints_;

  // The values of the map must never include zero.
  absl::flat_hash_map<VariableId, double> linear_objective_;

  SparseSymmetricMatrix quadratic_objective_;

  // The values of the map must never include zero.
  absl::flat_hash_map<std::pair<LinearConstraintId, VariableId>, double>
      linear_constraint_matrix_;

  absl::flat_hash_map<VariableId, absl::flat_hash_set<LinearConstraintId>>
      matrix_columns_;
  absl::flat_hash_map<LinearConstraintId, absl::flat_hash_set<VariableId>>
      matrix_rows_;

  UpdateTrackers<UpdateTrackerData> update_trackers_;
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// Inlined function implementations
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////////////////////////

VariableId ModelStorage::AddVariable(absl::string_view name) {
  return AddVariable(-std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(), false, name);
}

double ModelStorage::variable_lower_bound(const VariableId id) const {
  return variables_.at(id).lower_bound;
}

double ModelStorage::variable_upper_bound(const VariableId id) const {
  return variables_.at(id).upper_bound;
}

bool ModelStorage::is_variable_integer(VariableId id) const {
  return variables_.at(id).is_integer;
}

const std::string& ModelStorage::variable_name(const VariableId id) const {
  return variables_.at(id).name;
}

template <typename T>
void ModelStorage::set_variable_property(
    const VariableId id, const T value, T VariableData::*const field,
    absl::flat_hash_set<VariableId> UpdateTrackerData::*const dirty_set_field) {
  VariableData& var_data = variables_.at(id);
  if (var_data.*field != value) {
    var_data.*field = value;
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      if (id < update_tracker->variables_checkpoint) {
        (update_tracker.get()->*dirty_set_field).insert(id);
      }
    }
  }
}

void ModelStorage::set_variable_lower_bound(const VariableId id,
                                            const double lower_bound) {
  set_variable_property(id, lower_bound, &VariableData::lower_bound,
                        &UpdateTrackerData::dirty_variable_lower_bounds);
}

void ModelStorage::set_variable_upper_bound(const VariableId id,
                                            const double upper_bound) {
  set_variable_property(id, upper_bound, &VariableData::upper_bound,
                        &UpdateTrackerData::dirty_variable_upper_bounds);
}

void ModelStorage::set_variable_is_integer(const VariableId id,
                                           const bool is_integer) {
  set_variable_property(id, is_integer, &VariableData::is_integer,
                        &UpdateTrackerData::dirty_variable_is_integer);
}

void ModelStorage::set_variable_as_integer(VariableId id) {
  set_variable_is_integer(id, true);
}

void ModelStorage::set_variable_as_continuous(VariableId id) {
  set_variable_is_integer(id, false);
}

int ModelStorage::num_variables() const { return variables_.size(); }

VariableId ModelStorage::next_variable_id() const { return next_variable_id_; }

bool ModelStorage::has_variable(const VariableId id) const {
  return variables_.contains(id);
}

////////////////////////////////////////////////////////////////////////////////
// Linear Constraints
////////////////////////////////////////////////////////////////////////////////

LinearConstraintId ModelStorage::AddLinearConstraint(absl::string_view name) {
  return AddLinearConstraint(-std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::infinity(), name);
}

double ModelStorage::linear_constraint_lower_bound(
    const LinearConstraintId id) const {
  return linear_constraints_.at(id).lower_bound;
}

double ModelStorage::linear_constraint_upper_bound(
    const LinearConstraintId id) const {
  return linear_constraints_.at(id).upper_bound;
}

const std::string& ModelStorage::linear_constraint_name(
    const LinearConstraintId id) const {
  return linear_constraints_.at(id).name;
}

void ModelStorage::set_linear_constraint_property(
    const LinearConstraintId id, const double value,
    double LinearConstraintData::*const field,
    absl::flat_hash_set<LinearConstraintId> UpdateTrackerData::*const
        dirty_set_field) {
  LinearConstraintData& lin_con_data = linear_constraints_.at(id);
  if (lin_con_data.*field != value) {
    lin_con_data.*field = value;
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      if (id < update_tracker->linear_constraints_checkpoint) {
        (update_tracker.get()->*dirty_set_field).insert(id);
      }
    }
  }
}

void ModelStorage::set_linear_constraint_lower_bound(
    const LinearConstraintId id, const double lower_bound) {
  set_linear_constraint_property(
      id, lower_bound, &LinearConstraintData::lower_bound,
      &UpdateTrackerData::dirty_linear_constraint_lower_bounds);
}

void ModelStorage::set_linear_constraint_upper_bound(
    const LinearConstraintId id, const double upper_bound) {
  set_linear_constraint_property(
      id, upper_bound, &LinearConstraintData::upper_bound,
      &UpdateTrackerData::dirty_linear_constraint_upper_bounds);
}

int ModelStorage::num_linear_constraints() const {
  return linear_constraints_.size();
}

LinearConstraintId ModelStorage::next_linear_constraint_id() const {
  return next_linear_constraint_id_;
}

bool ModelStorage::has_linear_constraint(const LinearConstraintId id) const {
  return linear_constraints_.contains(id);
}

////////////////////////////////////////////////////////////////////////////////
// Linear Constraint Matrix
////////////////////////////////////////////////////////////////////////////////

double ModelStorage::linear_constraint_coefficient(
    LinearConstraintId constraint, VariableId variable) const {
  return gtl::FindWithDefault(linear_constraint_matrix_,
                              {constraint, variable});
}

bool ModelStorage::is_linear_constraint_coefficient_nonzero(
    LinearConstraintId constraint, VariableId variable) const {
  return linear_constraint_matrix_.contains({constraint, variable});
}

void ModelStorage::set_linear_constraint_coefficient(
    LinearConstraintId constraint, VariableId variable, double value) {
  bool was_updated = false;
  if (value == 0.0) {
    if (linear_constraint_matrix_.erase({constraint, variable}) > 0) {
      was_updated = true;
      matrix_columns_.at(variable).erase(constraint);
      matrix_rows_.at(constraint).erase(variable);
    }
  } else {
    const auto [iterator, inserted] =
        linear_constraint_matrix_.try_emplace({constraint, variable}, value);
    if (inserted) {
      was_updated = true;
    } else if (iterator->second != value) {
      iterator->second = value;
      was_updated = true;
    }

    matrix_columns_.at(variable).insert(constraint);

    matrix_rows_.at(constraint).insert(variable);
  }
  if (was_updated) {
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      if (constraint < update_tracker->linear_constraints_checkpoint &&
          variable < update_tracker->variables_checkpoint) {
        update_tracker->dirty_linear_constraint_matrix_keys.emplace(constraint,
                                                                    variable);
      }
    }
  }
}

const absl::flat_hash_map<std::pair<LinearConstraintId, VariableId>, double>&
ModelStorage::linear_constraint_matrix() const {
  return linear_constraint_matrix_;
}

const absl::flat_hash_set<VariableId>&
ModelStorage::variables_in_linear_constraint(
    LinearConstraintId constraint) const {
  return matrix_rows_.at(constraint);
}

const absl::flat_hash_set<LinearConstraintId>&
ModelStorage::linear_constraints_with_variable(VariableId variable) const {
  return matrix_columns_.at(variable);
}

////////////////////////////////////////////////////////////////////////////////
// Objective
////////////////////////////////////////////////////////////////////////////////

namespace internal {

inline std::pair<VariableId, VariableId> MakeOrderedPair(const VariableId a,
                                                         const VariableId b) {
  return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

}  // namespace internal

bool ModelStorage::is_maximize() const { return is_maximize_; }

double ModelStorage::objective_offset() const { return objective_offset_; }

double ModelStorage::linear_objective_coefficient(VariableId variable) const {
  return gtl::FindWithDefault(linear_objective_, variable);
}

double ModelStorage::quadratic_objective_coefficient(
    const VariableId first_variable, const VariableId second_variable) const {
  return quadratic_objective_.get(first_variable, second_variable);
}

bool ModelStorage::is_linear_objective_coefficient_nonzero(
    VariableId variable) const {
  return linear_objective_.contains(variable);
}

bool ModelStorage::is_quadratic_objective_coefficient_nonzero(
    const VariableId first_variable, const VariableId second_variable) const {
  return quadratic_objective_.get(first_variable, second_variable) != 0.0;
}

void ModelStorage::set_is_maximize(bool is_maximize) {
  if (is_maximize_ != is_maximize) {
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      update_tracker->dirty_objective_direction = true;
    }
    is_maximize_ = is_maximize;
  }
}

void ModelStorage::set_maximize() { set_is_maximize(true); }

void ModelStorage::set_minimize() { set_is_maximize(false); }

void ModelStorage::set_objective_offset(double value) {
  if (value != objective_offset_) {
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      update_tracker->dirty_objective_offset = true;
    }
    objective_offset_ = value;
  }
}

void ModelStorage::set_linear_objective_coefficient(VariableId variable,
                                                    double value) {
  bool was_updated = false;
  if (value == 0.0) {
    if (linear_objective_.erase(variable) > 0) {
      was_updated = true;
    }
  } else {
    const auto [iterator, inserted] =
        linear_objective_.try_emplace(variable, value);
    if (inserted) {
      was_updated = true;
    } else if (iterator->second != value) {
      iterator->second = value;
      was_updated = true;
    }
  }
  if (was_updated) {
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      if (variable < update_tracker->variables_checkpoint) {
        update_tracker->dirty_linear_objective_coefficients.insert(variable);
      }
    }
  }
}

void ModelStorage::set_quadratic_objective_coefficient(
    const VariableId first_variable, const VariableId second_variable,
    double value) {
  const bool updated =
      quadratic_objective_.set(first_variable, second_variable, value);
  const std::pair<VariableId, VariableId> key =
      internal::MakeOrderedPair(first_variable, second_variable);
  if (updated) {
    for (const auto& [_, update_tracker] :
         update_trackers_.GetUpdatedTrackers()) {
      if (key.second < update_tracker->variables_checkpoint) {
        update_tracker->dirty_quadratic_objective_coefficients.insert(key);
      }
    }
  }
}

void ModelStorage::clear_objective() {
  set_objective_offset(0.0);
  while (!linear_objective_.empty()) {
    set_linear_objective_coefficient(linear_objective_.begin()->first, 0.0);
  }
  for (const auto& [_, update_tracker] :
       update_trackers_.GetUpdatedTrackers()) {
    for (const auto [var_pair, value] : quadratic_objective_.values()) {
      if (value == 0.0) continue;
      if (var_pair.second < update_tracker->variables_checkpoint) {
        update_tracker->dirty_quadratic_objective_coefficients.insert(var_pair);
      }
    }
  }
  quadratic_objective_.Clear();
}

const absl::flat_hash_map<VariableId, double>& ModelStorage::linear_objective()
    const {
  return linear_objective_;
}

int64_t ModelStorage::num_quadratic_objective_terms() const {
  return quadratic_objective_.nonzeros();
}

std::vector<std::tuple<VariableId, VariableId, double>>
ModelStorage::quadratic_objective_terms() const {
  return quadratic_objective_.Terms();
}

}  // namespace math_opt
}  // namespace operations_research

#endif  // OR_TOOLS_MATH_OPT_STORAGE_MODEL_STORAGE_H_
