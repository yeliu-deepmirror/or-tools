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

//

#include "ortools/linear_solver/linear_solver.h"

#if !defined(_MSC_VER)
#include <unistd.h>
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "ortools/base/accurate_sum.h"
#include "ortools/base/integral_types.h"
#include "ortools/base/logging.h"
#include "ortools/base/map_util.h"
#include "ortools/base/stl_util.h"
#include "ortools/linear_solver/linear_solver.pb.h"
#include "ortools/util/fp_utils.h"
#include "ortools/util/time_limit.h"

namespace operations_research {

constexpr bool kVerfySolution = false;
constexpr bool linear_solver_enable_verbose_output = false;

bool SolverTypeIsMip(MPModelRequest::SolverType solver_type) {
  switch (solver_type) {
    case MPModelRequest::PDLP_LINEAR_PROGRAMMING:
    case MPModelRequest::GLOP_LINEAR_PROGRAMMING:
    case MPModelRequest::CLP_LINEAR_PROGRAMMING:
    case MPModelRequest::GLPK_LINEAR_PROGRAMMING:
    case MPModelRequest::GUROBI_LINEAR_PROGRAMMING:
    case MPModelRequest::XPRESS_LINEAR_PROGRAMMING:
    case MPModelRequest::CPLEX_LINEAR_PROGRAMMING:
      return false;

    case MPModelRequest::SCIP_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::GLPK_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::CBC_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::GUROBI_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::KNAPSACK_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::BOP_INTEGER_PROGRAMMING:
    case MPModelRequest::SAT_INTEGER_PROGRAMMING:
    case MPModelRequest::XPRESS_MIXED_INTEGER_PROGRAMMING:
    case MPModelRequest::CPLEX_MIXED_INTEGER_PROGRAMMING:
      return true;
  }
  LOG(DFATAL) << "Invalid SolverType: " << solver_type;
  return false;
}

double MPConstraint::GetCoefficient(const MPVariable* const var) const {
  DLOG_IF(DFATAL, !interface_->solver_->OwnsVariable(var)) << var;
  if (var == nullptr) return 0.0;
  return gtl::FindWithDefault(coefficients_, var, 0.0);
}

void MPConstraint::SetCoefficient(const MPVariable* const var, double coeff) {
  DLOG_IF(DFATAL, !interface_->solver_->OwnsVariable(var)) << var;
  if (var == nullptr) return;
  if (coeff == 0.0) {
    auto it = coefficients_.find(var);
    // If setting a coefficient to 0 when this coefficient did not
    // exist or was already 0, do nothing: skip
    // interface_->SetCoefficient() and do not store a coefficient in
    // the map.  Note that if the coefficient being set to 0 did exist
    // and was not 0, we do have to keep a 0 in the coefficients_ map,
    // because the extraction of the constraint might rely on it,
    // depending on the underlying solver.
    if (it != coefficients_.end() && it->second != 0.0) {
      const double old_value = it->second;
      it->second = 0.0;
      interface_->SetCoefficient(this, var, 0.0, old_value);
    }
    return;
  }
  auto insertion_result = coefficients_.insert(std::make_pair(var, coeff));
  const double old_value =
      insertion_result.second ? 0.0 : insertion_result.first->second;
  insertion_result.first->second = coeff;
  interface_->SetCoefficient(this, var, coeff, old_value);
}

void MPConstraint::Clear() {
  interface_->ClearConstraint(this);
  coefficients_.clear();
}

void MPConstraint::SetBounds(double lb, double ub) {
  const bool change = lb != lb_ || ub != ub_;
  lb_ = lb;
  ub_ = ub;
  if (change && interface_->constraint_is_extracted(index_)) {
    interface_->SetConstraintBounds(index_, lb_, ub_);
  }
}

double MPConstraint::dual_value() const {
  if (!interface_->IsContinuous()) {
    LOG(DFATAL) << "Dual value only available for continuous problems";
    return 0.0;
  }
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) return 0.0;
  return dual_value_;
}

MPSolver::BasisStatus MPConstraint::basis_status() const {
  if (!interface_->IsContinuous()) {
    LOG(DFATAL) << "Basis status only available for continuous problems";
    return MPSolver::FREE;
  }
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) {
    return MPSolver::FREE;
  }
  // This is done lazily as this method is expected to be rarely used.
  return interface_->row_status(index_);
}

bool MPConstraint::ContainsNewVariables() {
  const int last_variable_index = interface_->last_variable_index();
  for (const auto& entry : coefficients_) {
    const int variable_index = entry.first->index();
    if (variable_index >= last_variable_index ||
        !interface_->variable_is_extracted(variable_index)) {
      return true;
    }
  }
  return false;
}

// ----- MPObjective -----

double MPObjective::GetCoefficient(const MPVariable* const var) const {
  DLOG_IF(DFATAL, !interface_->solver_->OwnsVariable(var)) << var;
  if (var == nullptr) return 0.0;
  return gtl::FindWithDefault(coefficients_, var, 0.0);
}

void MPObjective::SetCoefficient(const MPVariable* const var, double coeff) {
  DLOG_IF(DFATAL, !interface_->solver_->OwnsVariable(var)) << var;
  if (var == nullptr) return;
  if (coeff == 0.0) {
    auto it = coefficients_.find(var);
    // See the discussion on MPConstraint::SetCoefficient() for 0 coefficients,
    // the same reasoning applies here.
    if (it == coefficients_.end() || it->second == 0.0) return;
    it->second = 0.0;
  } else {
    coefficients_[var] = coeff;
  }
  interface_->SetObjectiveCoefficient(var, coeff);
}

void MPObjective::SetOffset(double value) {
  offset_ = value;
  interface_->SetObjectiveOffset(offset_);
}

namespace {
void CheckLinearExpr(const MPSolver& solver, const LinearExpr& linear_expr) {
  for (auto var_value_pair : linear_expr.terms()) {
    CHECK(solver.OwnsVariable(var_value_pair.first))
        << "Bad MPVariable* in LinearExpr, did you try adding an integer to an "
           "MPVariable* directly?";
  }
}
}  // namespace

void MPObjective::OptimizeLinearExpr(const LinearExpr& linear_expr,
                                     bool is_maximization) {
  CheckLinearExpr(*interface_->solver_, linear_expr);
  interface_->ClearObjective();
  coefficients_.clear();
  SetOffset(linear_expr.offset());
  for (const auto& kv : linear_expr.terms()) {
    SetCoefficient(kv.first, kv.second);
  }
  SetOptimizationDirection(is_maximization);
}

void MPObjective::AddLinearExpr(const LinearExpr& linear_expr) {
  CheckLinearExpr(*interface_->solver_, linear_expr);
  SetOffset(offset_ + linear_expr.offset());
  for (const auto& kv : linear_expr.terms()) {
    SetCoefficient(kv.first, GetCoefficient(kv.first) + kv.second);
  }
}

void MPObjective::Clear() {
  interface_->ClearObjective();
  coefficients_.clear();
  offset_ = 0.0;
  SetMinimization();
}

void MPObjective::SetOptimizationDirection(bool maximize) {
  // Note(user): The maximize_ bool would more naturally belong to the
  // MPObjective, but it actually has to be a member of MPSolverInterface,
  // because some implementations (such as GLPK) need that bool for the
  // MPSolverInterface constructor, i.e. at a time when the MPObjective is not
  // constructed yet (MPSolverInterface is always built before MPObjective
  // when a new MPSolver is constructed).
  interface_->maximize_ = maximize;
  interface_->SetOptimizationDirection(maximize);
}

bool MPObjective::maximization() const { return interface_->maximize_; }

bool MPObjective::minimization() const { return !interface_->maximize_; }

double MPObjective::Value() const {
  // Note(user): implementation-wise, the objective value belongs more
  // naturally to the MPSolverInterface, since all of its implementations write
  // to it directly.
  return interface_->objective_value();
}

double MPObjective::BestBound() const {
  // Note(user): the best objective bound belongs to the interface for the
  // same reasons as the objective value does.
  return interface_->best_objective_bound();
}

// ----- MPVariable -----

double MPVariable::solution_value() const {
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) return 0.0;
  // If the underlying solver supports integer variables, and this is an integer
  // variable, we round the solution value (i.e., clients usually expect precise
  // integer values for integer variables).
  return (integer_ && interface_->IsMIP()) ? round(solution_value_)
                                           : solution_value_;
}

double MPVariable::unrounded_solution_value() const {
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) return 0.0;
  return solution_value_;
}

double MPVariable::reduced_cost() const {
  if (!interface_->IsContinuous()) {
    LOG(DFATAL) << "Reduced cost only available for continuous problems";
    return 0.0;
  }
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) return 0.0;
  return reduced_cost_;
}

MPSolver::BasisStatus MPVariable::basis_status() const {
  if (!interface_->IsContinuous()) {
    LOG(DFATAL) << "Basis status only available for continuous problems";
    return MPSolver::FREE;
  }
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) {
    return MPSolver::FREE;
  }
  // This is done lazily as this method is expected to be rarely used.
  return interface_->column_status(index_);
}

void MPVariable::SetBounds(double lb, double ub) {
  const bool change = lb != lb_ || ub != ub_;
  lb_ = lb;
  ub_ = ub;
  if (change && interface_->variable_is_extracted(index_)) {
    interface_->SetVariableBounds(index_, lb_, ub_);
  }
}

void MPVariable::SetInteger(bool integer) {
  if (integer_ != integer) {
    integer_ = integer;
    if (interface_->variable_is_extracted(index_)) {
      interface_->SetVariableInteger(index_, integer);
    }
  }
}

void MPVariable::SetBranchingPriority(int priority) {
  if (priority == branching_priority_) return;
  branching_priority_ = priority;
  interface_->BranchingPriorityChangedForVariable(index_);
}

// ----- Interface shortcuts -----

bool MPSolver::IsMIP() const { return interface_->IsMIP(); }

std::string MPSolver::SolverVersion() const {
  return interface_->SolverVersion();
}

void* MPSolver::underlying_solver() { return interface_->underlying_solver(); }

// ---- Solver-specific parameters ----

OrToolsStatus MPSolver::SetNumThreads(int num_threads) {
  if (num_threads < 1) {
    return OrToolsStatus::Error("num_threads must be a positive number.");
  }
  OrToolsStatus status = interface_->SetNumThreads(num_threads);
  if (status.ok()) {
    num_threads_ = num_threads;
  }
  return status;
}

bool MPSolver::SetSolverSpecificParametersAsString(
    const std::string& parameters) {
  solver_specific_parameter_string_ = parameters;
  return interface_->SetSolverSpecificParametersAsString(parameters);
}

// ----- Solver -----

#if defined(USE_CLP) || defined(USE_CBC)
extern MPSolverInterface* BuildCLPInterface(MPSolver* const solver);
#endif
#if defined(USE_CBC)
extern MPSolverInterface* BuildCBCInterface(MPSolver* const solver);
#endif
#if defined(USE_GLPK)
extern MPSolverInterface* BuildGLPKInterface(bool mip, MPSolver* const solver);
#endif
// extern MPSolverInterface* BuildBopInterface(MPSolver* const solver);
extern MPSolverInterface* BuildGLOPInterface(MPSolver* const solver);
// extern MPSolverInterface* BuildPdlpInterface(MPSolver* const solver);
// extern MPSolverInterface* BuildSatInterface(MPSolver* const solver);
#if defined(USE_SCIP)
extern MPSolverInterface* BuildSCIPInterface(MPSolver* const solver);
#endif
extern MPSolverInterface* BuildGurobiInterface(bool mip,
                                               MPSolver* const solver);
#if defined(USE_CPLEX)
extern MPSolverInterface* BuildCplexInterface(bool mip, MPSolver* const solver);
#endif
#if defined(USE_XPRESS)
extern MPSolverInterface* BuildXpressInterface(bool mip,
                                               MPSolver* const solver);
#endif

namespace {
MPSolverInterface* BuildSolverInterface(MPSolver* const solver) {
  DCHECK(solver != nullptr);
  switch (solver->ProblemType()) {
    case MPSolver::BOP_INTEGER_PROGRAMMING:
      LOG(ERROR) << "BOP_INTEGER_PROGRAMMING removed from repo.";
      return nullptr;
    case MPSolver::GLOP_LINEAR_PROGRAMMING:
      return BuildGLOPInterface(solver);
    case MPSolver::PDLP_LINEAR_PROGRAMMING:
      LOG(ERROR) << "PDLP_LINEAR_PROGRAMMING removed from repo.";
      return nullptr;
    case MPSolver::SAT_INTEGER_PROGRAMMING:
      LOG(ERROR) << "SAT_INTEGER_PROGRAMMING removed from repo.";
      return nullptr;
#if defined(USE_GLPK)
    case MPSolver::GLPK_LINEAR_PROGRAMMING:
      return BuildGLPKInterface(false, solver);
    case MPSolver::GLPK_MIXED_INTEGER_PROGRAMMING:
      return BuildGLPKInterface(true, solver);
#endif
#if defined(USE_CLP) || defined(USE_CBC)
    case MPSolver::CLP_LINEAR_PROGRAMMING:
      return BuildCLPInterface(solver);
#endif
#if defined(USE_CBC)
    case MPSolver::CBC_MIXED_INTEGER_PROGRAMMING:
      return BuildCBCInterface(solver);
#endif
#if defined(USE_SCIP)
    case MPSolver::SCIP_MIXED_INTEGER_PROGRAMMING:
      return BuildSCIPInterface(solver);
#endif
    case MPSolver::GUROBI_LINEAR_PROGRAMMING:
      return BuildGurobiInterface(false, solver);
    case MPSolver::GUROBI_MIXED_INTEGER_PROGRAMMING:
      return BuildGurobiInterface(true, solver);
#if defined(USE_CPLEX)
    case MPSolver::CPLEX_LINEAR_PROGRAMMING:
      return BuildCplexInterface(false, solver);
    case MPSolver::CPLEX_MIXED_INTEGER_PROGRAMMING:
      return BuildCplexInterface(true, solver);
#endif
#if defined(USE_XPRESS)
    case MPSolver::XPRESS_MIXED_INTEGER_PROGRAMMING:
      return BuildXpressInterface(true, solver);
    case MPSolver::XPRESS_LINEAR_PROGRAMMING:
      return BuildXpressInterface(false, solver);
#endif
    default:
      // TODO(user): Revert to the best *available* interface.
      LOG(FATAL) << "Linear solver not recognized.";
  }
  return nullptr;
}
}  // namespace

namespace {
int NumDigits(int n) {
// Number of digits needed to write a non-negative integer in base 10.
// Note(user): max(1, log(0) + 1) == max(1, -inf) == 1.
#if defined(_MSC_VER)
  return static_cast<int>(std::max(1.0L, log(1.0L * n) / log(10.0L) + 1.0));
#else
  return static_cast<int>(std::max(1.0, log10(static_cast<double>(n)) + 1.0));
#endif
}
}  // namespace

MPSolver::MPSolver(const std::string& name,
                   OptimizationProblemType problem_type)
    : name_(name),
      problem_type_(problem_type),
      construction_time_(ortools::Time::Now().NanoSeconds()) {
  interface_.reset(BuildSolverInterface(this));
  if (linear_solver_enable_verbose_output) {
    EnableOutput();
  }
  objective_.reset(new MPObjective(interface_.get()));
}

MPSolver::~MPSolver() { Clear(); }

extern bool GurobiIsCorrectlyInstalled();

// static
bool MPSolver::SupportsProblemType(OptimizationProblemType problem_type) {
#ifdef USE_CLP
  if (problem_type == CLP_LINEAR_PROGRAMMING) return true;
#endif
#ifdef USE_GLPK
  if (problem_type == GLPK_LINEAR_PROGRAMMING ||
      problem_type == GLPK_MIXED_INTEGER_PROGRAMMING) {
    return true;
  }
#endif
  if (problem_type == BOP_INTEGER_PROGRAMMING) return true;
  if (problem_type == SAT_INTEGER_PROGRAMMING) return true;
  if (problem_type == GLOP_LINEAR_PROGRAMMING) return true;
  if (problem_type == PDLP_LINEAR_PROGRAMMING) return true;
  if (problem_type == GUROBI_LINEAR_PROGRAMMING ||
      problem_type == GUROBI_MIXED_INTEGER_PROGRAMMING) {
    return GurobiIsCorrectlyInstalled();
  }
#ifdef USE_SCIP
  if (problem_type == SCIP_MIXED_INTEGER_PROGRAMMING) return true;
#endif
#ifdef USE_CBC
  if (problem_type == CBC_MIXED_INTEGER_PROGRAMMING) return true;
#endif
#ifdef USE_XPRESS
  if (problem_type == XPRESS_MIXED_INTEGER_PROGRAMMING ||
      problem_type == XPRESS_LINEAR_PROGRAMMING) {
    return true;
  }
#endif
#ifdef USE_CPLEX
  if (problem_type == CPLEX_LINEAR_PROGRAMMING ||
      problem_type == CPLEX_MIXED_INTEGER_PROGRAMMING) {
    return true;
  }
#endif
  return false;
}

// TODO(user): post c++ 14, instead use
//   std::pair<MPSolver::OptimizationProblemType, const std::string_view>
// once pair gets a constexpr constructor.
namespace {
struct NamedOptimizationProblemType {
  MPSolver::OptimizationProblemType problem_type;
  std::string_view name;
};
}  // namespace

#if defined(_MSC_VER)
const
#else
constexpr
#endif
    NamedOptimizationProblemType kOptimizationProblemTypeNames[] = {
        {MPSolver::GLOP_LINEAR_PROGRAMMING, "glop"},
        {MPSolver::CLP_LINEAR_PROGRAMMING, "clp"},
        {MPSolver::GUROBI_LINEAR_PROGRAMMING, "gurobi_lp"},
        {MPSolver::GLPK_LINEAR_PROGRAMMING, "glpk_lp"},
        {MPSolver::CPLEX_LINEAR_PROGRAMMING, "cplex_lp"},
        {MPSolver::XPRESS_LINEAR_PROGRAMMING, "xpress_lp"},
        {MPSolver::SCIP_MIXED_INTEGER_PROGRAMMING, "scip"},
        {MPSolver::CBC_MIXED_INTEGER_PROGRAMMING, "cbc"},
        {MPSolver::SAT_INTEGER_PROGRAMMING, "sat"},
        {MPSolver::BOP_INTEGER_PROGRAMMING, "bop"},
        {MPSolver::GUROBI_MIXED_INTEGER_PROGRAMMING, "gurobi"},
        {MPSolver::GLPK_MIXED_INTEGER_PROGRAMMING, "glpk"},
        {MPSolver::PDLP_LINEAR_PROGRAMMING, "pdlp"},
        {MPSolver::KNAPSACK_MIXED_INTEGER_PROGRAMMING, "knapsack"},
        {MPSolver::CPLEX_MIXED_INTEGER_PROGRAMMING, "cplex"},
        {MPSolver::XPRESS_MIXED_INTEGER_PROGRAMMING, "xpress"},

};
// static
bool MPSolver::ParseSolverType(std::string_view solver_id,
                               MPSolver::OptimizationProblemType* type) {
  // Normalize the solver id.
  const std::string id(solver_id);

  // Support the full enum name
  MPModelRequest::SolverType solver_type;
  if (MPModelRequest::SolverType_Parse(id, &solver_type)) {
    *type = static_cast<MPSolver::OptimizationProblemType>(solver_type);
    return true;
  }

  // Reverse lookup in the kOptimizationProblemTypeNames[] array.
  for (auto& named_solver : kOptimizationProblemTypeNames) {
    if (named_solver.name == id) {
      *type = named_solver.problem_type;
      return true;
    }
  }
  return false;
}

const std::string_view ToString(
    MPSolver::OptimizationProblemType optimization_problem_type) {
  for (const auto& named_solver : kOptimizationProblemTypeNames) {
    if (named_solver.problem_type == optimization_problem_type) {
      return named_solver.name;
    }
  }
  LOG(FATAL) << "Unrecognized solver type: "
             << static_cast<int>(optimization_problem_type);
  return "";
}

bool AbslParseFlag(const std::string_view text,
                   MPSolver::OptimizationProblemType* solver_type,
                   std::string* error) {
  DCHECK(solver_type != nullptr);
  DCHECK(error != nullptr);
  const bool result = MPSolver::ParseSolverType(text, solver_type);
  if (!result) {
    *error = std::string("Solver type: ") + std::string(text) + " does not exist.";
  }
  return result;
}

/* static */
MPSolver::OptimizationProblemType MPSolver::ParseSolverTypeOrDie(
    const std::string& solver_id) {
  MPSolver::OptimizationProblemType problem_type;
  CHECK(MPSolver::ParseSolverType(solver_id, &problem_type)) << solver_id;
  return problem_type;
}

/* static */
MPSolver* MPSolver::CreateSolver(const std::string& solver_id) {
  MPSolver::OptimizationProblemType problem_type;
  if (!MPSolver::ParseSolverType(solver_id, &problem_type)) {
    LOG(WARNING) << "Unrecognized solver type: " << solver_id;
    return nullptr;
  }
  if (!MPSolver::SupportsProblemType(problem_type)) {
    LOG(WARNING) << "Support for " << solver_id
                 << " not linked in, or the license was not found.";
    return nullptr;
  }
  MPSolver* solver = new MPSolver("", problem_type);
  return solver;
}

MPVariable* MPSolver::LookupVariableOrNull(const std::string& var_name) const {
  if (!variable_name_to_index_) GenerateVariableNameIndex();

  std::map<std::string, int>::const_iterator it =
      variable_name_to_index_->find(var_name);
  if (it == variable_name_to_index_->end()) return nullptr;
  return variables_[it->second];
}

MPConstraint* MPSolver::LookupConstraintOrNull(
    const std::string& constraint_name) const {
  if (!constraint_name_to_index_) GenerateConstraintNameIndex();

  const auto it = constraint_name_to_index_->find(constraint_name);
  if (it == constraint_name_to_index_->end()) return nullptr;
  return constraints_[it->second];
}

void MPSolver::Clear() {
  {
    std::lock_guard<std::mutex> guard(global_count_mutex_);
    global_num_variables_ += variables_.size();
    global_num_constraints_ += constraints_.size();
  }
  MutableObjective()->Clear();
  gtl::STLDeleteElements(&variables_);
  gtl::STLDeleteElements(&constraints_);
  if (variable_name_to_index_) {
    variable_name_to_index_->clear();
  }
  variable_is_extracted_.clear();
  if (constraint_name_to_index_) {
    constraint_name_to_index_->clear();
  }
  constraint_is_extracted_.clear();
  interface_->Reset();
  solution_hint_.clear();
}

void MPSolver::Reset() { interface_->Reset(); }

bool MPSolver::InterruptSolve() { return interface_->InterruptSolve(); }

void MPSolver::SetStartingLpBasis(
    const std::vector<BasisStatus>& variable_statuses,
    const std::vector<BasisStatus>& constraint_statuses) {
  interface_->SetStartingLpBasis(variable_statuses, constraint_statuses);
}

MPVariable* MPSolver::MakeVar(double lb, double ub, bool integer,
                              const std::string& name) {
  const int var_index = NumVariables();
  MPVariable* v =
      new MPVariable(var_index, lb, ub, integer, name, interface_.get());
  if (variable_name_to_index_) {
    gtl::InsertOrDie(&*variable_name_to_index_, v->name(), var_index);
  }
  variables_.push_back(v);
  variable_is_extracted_.push_back(false);
  interface_->AddVariable(v);
  return v;
}

MPVariable* MPSolver::MakeNumVar(double lb, double ub,
                                 const std::string& name) {
  return MakeVar(lb, ub, false, name);
}

MPVariable* MPSolver::MakeIntVar(double lb, double ub,
                                 const std::string& name) {
  return MakeVar(lb, ub, true, name);
}

MPVariable* MPSolver::MakeBoolVar(const std::string& name) {
  return MakeVar(0.0, 1.0, true, name);
}

void MPSolver::MakeVarArray(int nb, double lb, double ub, bool integer,
                            const std::string& name,
                            std::vector<MPVariable*>* vars) {
  DCHECK_GE(nb, 0);
  if (nb <= 0) return;
  const int num_digits = NumDigits(nb);
  for (int i = 0; i < nb; ++i) {
    if (name.empty()) {
      vars->push_back(MakeVar(lb, ub, integer, name));
    } else {
      std::string vname =
          fmt::format("%s%0*d", name.c_str(), num_digits, i);
      vars->push_back(MakeVar(lb, ub, integer, vname));
    }
  }
}

void MPSolver::MakeNumVarArray(int nb, double lb, double ub,
                               const std::string& name,
                               std::vector<MPVariable*>* vars) {
  MakeVarArray(nb, lb, ub, false, name, vars);
}

void MPSolver::MakeIntVarArray(int nb, double lb, double ub,
                               const std::string& name,
                               std::vector<MPVariable*>* vars) {
  MakeVarArray(nb, lb, ub, true, name, vars);
}

void MPSolver::MakeBoolVarArray(int nb, const std::string& name,
                                std::vector<MPVariable*>* vars) {
  MakeVarArray(nb, 0.0, 1.0, true, name, vars);
}

MPConstraint* MPSolver::MakeRowConstraint(double lb, double ub) {
  return MakeRowConstraint(lb, ub, "");
}

MPConstraint* MPSolver::MakeRowConstraint() {
  return MakeRowConstraint(-infinity(), infinity(), "");
}

MPConstraint* MPSolver::MakeRowConstraint(double lb, double ub,
                                          const std::string& name) {
  const int constraint_index = NumConstraints();
  MPConstraint* const constraint =
      new MPConstraint(constraint_index, lb, ub, name, interface_.get());
  if (constraint_name_to_index_) {
    gtl::InsertOrDie(&*constraint_name_to_index_, constraint->name(),
                     constraint_index);
  }
  constraints_.push_back(constraint);
  constraint_is_extracted_.push_back(false);
  interface_->AddRowConstraint(constraint);
  return constraint;
}

MPConstraint* MPSolver::MakeRowConstraint(const std::string& name) {
  return MakeRowConstraint(-infinity(), infinity(), name);
}

MPConstraint* MPSolver::MakeRowConstraint(const LinearRange& range) {
  return MakeRowConstraint(range, "");
}

MPConstraint* MPSolver::MakeRowConstraint(const LinearRange& range,
                                          const std::string& name) {
  CheckLinearExpr(*this, range.linear_expr());
  MPConstraint* constraint =
      MakeRowConstraint(range.lower_bound(), range.upper_bound(), name);
  for (const auto& kv : range.linear_expr().terms()) {
    constraint->SetCoefficient(kv.first, kv.second);
  }
  return constraint;
}

int MPSolver::ComputeMaxConstraintSize(int min_constraint_index,
                                       int max_constraint_index) const {
  int max_constraint_size = 0;
  DCHECK_GE(min_constraint_index, 0);
  DCHECK_LE(max_constraint_index, constraints_.size());
  for (int i = min_constraint_index; i < max_constraint_index; ++i) {
    MPConstraint* const ct = constraints_[i];
    if (ct->coefficients_.size() > max_constraint_size) {
      max_constraint_size = ct->coefficients_.size();
    }
  }
  return max_constraint_size;
}

bool MPSolver::HasInfeasibleConstraints() const {
  bool hasInfeasibleConstraints = false;
  for (int i = 0; i < constraints_.size(); ++i) {
    if (constraints_[i]->lb() > constraints_[i]->ub()) {
      LOG(WARNING) << "Constraint " << constraints_[i]->name() << " (" << i
                   << ") has contradictory bounds:"
                   << " lower bound = " << constraints_[i]->lb()
                   << " upper bound = " << constraints_[i]->ub();
      hasInfeasibleConstraints = true;
    }
  }
  return hasInfeasibleConstraints;
}

bool MPSolver::HasIntegerVariables() const {
  for (const MPVariable* const variable : variables_) {
    if (variable->integer()) return true;
  }
  return false;
}

MPSolver::ResultStatus MPSolver::Solve() {
  MPSolverParameters default_param;
  return Solve(default_param);
}

MPSolver::ResultStatus MPSolver::Solve(const MPSolverParameters& param) {
  // Special case for infeasible constraints so that all solvers have
  // the same behavior.
  // TODO(user): replace this by model extraction to proto + proto validation
  // (the proto has very low overhead compared to the wrapper, both in
  // performance and memory, so it's ok).
  if (HasInfeasibleConstraints()) {
    interface_->result_status_ = MPSolver::INFEASIBLE;
    return interface_->result_status_;
  }

  MPSolver::ResultStatus status = interface_->Solve(param);
  if (kVerfySolution) {
    if (status != MPSolver::OPTIMAL && status != MPSolver::FEASIBLE) {
      VLOG(1) << "--verify_solution enabled, but the solver did not find a"
              << " solution: skipping the verification.";
    } else if (!VerifySolution(
                   param.GetDoubleParam(MPSolverParameters::PRIMAL_TOLERANCE), true)) {
      status = MPSolver::ABNORMAL;
      interface_->result_status_ = status;
    }
  }
  DCHECK_EQ(interface_->result_status_, status);
  return status;
}

void MPSolver::Write(const std::string& file_name) {
  interface_->Write(file_name);
}

namespace {
std::string PrettyPrintVar(const MPVariable& var) {
  const std::string prefix = "Variable '" + var.name() + "': domain = ";
  if (var.lb() >= MPSolver::infinity() || var.ub() <= -MPSolver::infinity() ||
      var.lb() > var.ub()) {
    return prefix + "∅";  // Empty set.
  }
  // Special case: integer variable with at most two possible values
  // (and potentially none).
  if (var.integer() && var.ub() - var.lb() <= 1) {
    const int64_t lb = static_cast<int64_t>(ceil(var.lb()));
    const int64_t ub = static_cast<int64_t>(floor(var.ub()));
    if (lb > ub) {
      return prefix + "∅";
    } else if (lb == ub) {
      return fmt::format("%s{ %d }", prefix.c_str(), lb);
    } else {
      return fmt::format("%s{ %d, %d }", prefix.c_str(), lb, ub);
    }
  }
  // Special case: single (non-infinite) real value.
  if (var.lb() == var.ub()) {
    return fmt::format("%s{ %f }", prefix.c_str(), var.lb());
  }
  return prefix + (var.integer() ? "Integer" : "Real") + " in " +
         (var.lb() <= -MPSolver::infinity()
              ? std::string("]-∞")
              : fmt::format("[%f", var.lb())) +
         ", " +
         (var.ub() >= MPSolver::infinity() ? std::string("+∞[")
                                           : fmt::format("%f]", var.ub()));
}

std::string PrettyPrintConstraint(const MPConstraint& constraint) {
  std::string prefix = "Constraint '" + constraint.name() + "': ";
  if (constraint.lb() >= MPSolver::infinity() ||
      constraint.ub() <= -MPSolver::infinity() ||
      constraint.lb() > constraint.ub()) {
    return prefix + "ALWAYS FALSE";
  }
  if (constraint.lb() <= -MPSolver::infinity() &&
      constraint.ub() >= MPSolver::infinity()) {
    return prefix + "ALWAYS TRUE";
  }
  prefix += "<linear expr>";
  // Equality.
  if (constraint.lb() == constraint.ub()) {
    return fmt::format("%s = %f", prefix.c_str(), constraint.lb());
  }
  // Inequalities.
  if (constraint.lb() <= -MPSolver::infinity()) {
    return fmt::format("%s ≤ %f", prefix.c_str(), constraint.ub());
  }
  if (constraint.ub() >= MPSolver::infinity()) {
    return fmt::format("%s ≥ %f", prefix.c_str(), constraint.lb());
  }
  return fmt::format("%s ∈ [%f, %f]", prefix.c_str(), constraint.lb(),
                         constraint.ub());
}
}  // namespace

OrToolsStatus MPSolver::ClampSolutionWithinBounds() {
  interface_->ExtractModel();
  for (MPVariable* const variable : variables_) {
    const double value = variable->solution_value();
    if (std::isnan(value)) {
      return OrToolsStatus::Error(std::string("NaN value for ") + PrettyPrintVar(*variable));
    }
    if (value < variable->lb()) {
      variable->set_solution_value(variable->lb());
    } else if (value > variable->ub()) {
      variable->set_solution_value(variable->ub());
    }
  }
  interface_->sync_status_ = MPSolverInterface::SOLUTION_SYNCHRONIZED;
  return OrToolsStatus::OK();
}

std::vector<double> MPSolver::ComputeConstraintActivities() const {
  // TODO(user): test this failure case.
  if (!interface_->CheckSolutionIsSynchronizedAndExists()) return {};
  std::vector<double> activities(constraints_.size(), 0.0);
  for (int i = 0; i < constraints_.size(); ++i) {
    const MPConstraint& constraint = *constraints_[i];
    AccurateSum<double> sum;
    for (const auto& entry : constraint.coefficients_) {
      sum.Add(entry.first->solution_value() * entry.second);
    }
    activities[i] = sum.Value();
  }
  return activities;
}

// TODO(user): split.
bool MPSolver::VerifySolution(double tolerance, bool log_errors) const {
  double max_observed_error = 0;
  if (tolerance < 0) tolerance = infinity();
  int num_errors = 0;

  // Verify variables.
  for (int i = 0; i < variables_.size(); ++i) {
    const MPVariable& var = *variables_[i];
    const double value = var.solution_value();
    // Check for NaN.
    if (std::isnan(value)) {
      ++num_errors;
      max_observed_error = infinity();
      LOG_IF(ERROR, log_errors) << "NaN value for " << PrettyPrintVar(var);
      continue;
    }
    // Check lower bound.
    if (var.lb() != -infinity()) {
      if (value < var.lb() - tolerance) {
        ++num_errors;
        max_observed_error = std::max(max_observed_error, var.lb() - value);
        LOG_IF(ERROR, log_errors)
            << "Value " << value << " too low for " << PrettyPrintVar(var);
      }
    }
    // Check upper bound.
    if (var.ub() != infinity()) {
      if (value > var.ub() + tolerance) {
        ++num_errors;
        max_observed_error = std::max(max_observed_error, value - var.ub());
        LOG_IF(ERROR, log_errors)
            << "Value " << value << " too high for " << PrettyPrintVar(var);
      }
    }
    // Check integrality.
    if (IsMIP() && var.integer()) {
      if (fabs(value - round(value)) > tolerance) {
        ++num_errors;
        max_observed_error =
            std::max(max_observed_error, fabs(value - round(value)));
        LOG_IF(ERROR, log_errors)
            << "Non-integer value " << value << " for " << PrettyPrintVar(var);
      }
    }
  }
  if (!IsMIP() && HasIntegerVariables()) {
    LOG_IF(INFO, log_errors) << "Skipped variable integrality check, because "
                             << "a continuous relaxation of the model was "
                             << "solved (i.e., the selected solver does not "
                             << "support integer variables).";
  }

  // Verify constraints.
  const std::vector<double> activities = ComputeConstraintActivities();
  for (int i = 0; i < constraints_.size(); ++i) {
    const MPConstraint& constraint = *constraints_[i];
    const double activity = activities[i];
    // Re-compute the activity with a inaccurate summing algorithm.
    double inaccurate_activity = 0.0;
    for (const auto& entry : constraint.coefficients_) {
      inaccurate_activity += entry.first->solution_value() * entry.second;
    }
    // Catch NaNs.
    if (std::isnan(activity) || std::isnan(inaccurate_activity)) {
      ++num_errors;
      max_observed_error = infinity();
      LOG_IF(ERROR, log_errors)
          << "NaN value for " << PrettyPrintConstraint(constraint);
      continue;
    }
    // Check bounds.
    if (constraint.indicator_variable() == nullptr ||
        std::round(constraint.indicator_variable()->solution_value()) ==
            constraint.indicator_value()) {
      if (constraint.lb() != -infinity()) {
        if (activity < constraint.lb() - tolerance) {
          ++num_errors;
          max_observed_error =
              std::max(max_observed_error, constraint.lb() - activity);
          LOG_IF(ERROR, log_errors)
              << "Activity " << activity << " too low for "
              << PrettyPrintConstraint(constraint);
        } else if (inaccurate_activity < constraint.lb() - tolerance) {
          LOG_IF(WARNING, log_errors)
              << "Activity " << activity << ", computed with the (inaccurate)"
              << " standard sum of its terms, is too low for "
              << PrettyPrintConstraint(constraint);
        }
      }
      if (constraint.ub() != infinity()) {
        if (activity > constraint.ub() + tolerance) {
          ++num_errors;
          max_observed_error =
              std::max(max_observed_error, activity - constraint.ub());
          LOG_IF(ERROR, log_errors)
              << "Activity " << activity << " too high for "
              << PrettyPrintConstraint(constraint);
        } else if (inaccurate_activity > constraint.ub() + tolerance) {
          LOG_IF(WARNING, log_errors)
              << "Activity " << activity << ", computed with the (inaccurate)"
              << " standard sum of its terms, is too high for "
              << PrettyPrintConstraint(constraint);
        }
      }
    }
  }

  // Verify that the objective value wasn't reported incorrectly.
  const MPObjective& objective = Objective();
  AccurateSum<double> objective_sum;
  objective_sum.Add(objective.offset());
  double inaccurate_objective_value = objective.offset();
  for (const auto& entry : objective.coefficients_) {
    const double term = entry.first->solution_value() * entry.second;
    objective_sum.Add(term);
    inaccurate_objective_value += term;
  }
  const double actual_objective_value = objective_sum.Value();
  if (!AreWithinAbsoluteOrRelativeTolerances(
          objective.Value(), actual_objective_value, tolerance, tolerance)) {
    ++num_errors;
    max_observed_error = std::max(
        max_observed_error, fabs(actual_objective_value - objective.Value()));
    LOG_IF(ERROR, log_errors)
        << "Objective value " << objective.Value() << " isn't accurate"
        << ", it should be " << actual_objective_value
        << " (delta=" << actual_objective_value - objective.Value() << ").";
  } else if (!AreWithinAbsoluteOrRelativeTolerances(objective.Value(),
                                                    inaccurate_objective_value,
                                                    tolerance, tolerance)) {
    LOG_IF(WARNING, log_errors)
        << "Objective value " << objective.Value() << " doesn't correspond"
        << " to the value computed with the standard (and therefore inaccurate)"
        << " sum of its terms.";
  }
  if (num_errors > 0) {
    LOG_IF(ERROR, log_errors)
        << "There were " << num_errors << " errors above the tolerance ("
        << tolerance << "), the largest was " << max_observed_error;
    return false;
  }
  return true;
}

bool MPSolver::OutputIsEnabled() const { return !interface_->quiet(); }

void MPSolver::EnableOutput() { interface_->set_quiet(false); }

void MPSolver::SuppressOutput() { interface_->set_quiet(true); }

int64_t MPSolver::iterations() const { return interface_->iterations(); }

int64_t MPSolver::nodes() const { return interface_->nodes(); }

double MPSolver::ComputeExactConditionNumber() const {
  return interface_->ComputeExactConditionNumber();
}

bool MPSolver::OwnsVariable(const MPVariable* var) const {
  if (var == nullptr) return false;
  if (var->index() >= 0 && var->index() < variables_.size()) {
    // Then, verify that the variable with this index has the same address.
    return variables_[var->index()] == var;
  }
  return false;
}

void MPSolver::SetHint(std::vector<std::pair<const MPVariable*, double>> hint) {
  for (const auto& var_value_pair : hint) {
    CHECK(OwnsVariable(var_value_pair.first))
        << "hint variable does not belong to this solver";
  }
  solution_hint_ = std::move(hint);
}

void MPSolver::GenerateVariableNameIndex() const {
  if (variable_name_to_index_) return;
  variable_name_to_index_ = std::map<std::string, int>();
  for (const MPVariable* const var : variables_) {
    gtl::InsertOrDie(&*variable_name_to_index_, var->name(), var->index());
  }
}

void MPSolver::GenerateConstraintNameIndex() const {
  if (constraint_name_to_index_) return;
  constraint_name_to_index_ = std::map<std::string, int>();
  for (const MPConstraint* const cst : constraints_) {
    gtl::InsertOrDie(&*constraint_name_to_index_, cst->name(), cst->index());
  }
}

bool MPSolver::NextSolution() { return interface_->NextSolution(); }

void MPSolver::SetCallback(MPCallback* mp_callback) {
  interface_->SetCallback(mp_callback);
}

bool MPSolver::SupportsCallbacks() const {
  return interface_->SupportsCallbacks();
}

// Global counters.
std::mutex MPSolver::global_count_mutex_;
int64_t MPSolver::global_num_variables_ = 0;
int64_t MPSolver::global_num_constraints_ = 0;

// static
int64_t MPSolver::global_num_variables() {
  // Why not ReaderMutexLock? See go/totw/197#when-are-shared-locks-useful.
  std::lock_guard<std::mutex> guard(global_count_mutex_);
  return global_num_variables_;
}

// static
int64_t MPSolver::global_num_constraints() {
  // Why not ReaderMutexLock? See go/totw/197#when-are-shared-locks-useful.
  std::lock_guard<std::mutex> guard(global_count_mutex_);
  return global_num_constraints_;
}

bool MPSolverResponseStatusIsRpcError(MPSolverResponseStatus status) {
  switch (status) {
    // Cases that don't yield an RPC error when they happen on the server.
    case MPSOLVER_OPTIMAL:
    case MPSOLVER_FEASIBLE:
    case MPSOLVER_INFEASIBLE:
    case MPSOLVER_NOT_SOLVED:
    case MPSOLVER_UNBOUNDED:
    case MPSOLVER_ABNORMAL:
    case MPSOLVER_UNKNOWN_STATUS:
      return false;
    // Cases that should never happen with the linear solver server. We prefer
    // to consider those as "not RPC errors".
    case MPSOLVER_MODEL_IS_VALID:
    case MPSOLVER_CANCELLED_BY_USER:
      return false;
    // Cases that yield an RPC error when they happen on the server.
    case MPSOLVER_MODEL_INVALID:
    case MPSOLVER_MODEL_INVALID_SOLUTION_HINT:
    case MPSOLVER_MODEL_INVALID_SOLVER_PARAMETERS:
    case MPSOLVER_SOLVER_TYPE_UNAVAILABLE:
    case MPSOLVER_INCOMPATIBLE_OPTIONS:
      return true;
  }
  LOG(DFATAL)
      << "MPSolverResponseStatusIsRpcError() called with invalid status "
      << "(value: " << status << ")";
  return false;
}

// ---------- MPSolverInterface ----------

const int MPSolverInterface::kDummyVariableIndex = 0;

// TODO(user): Initialize objective value and bound to +/- inf (depending on
// optimization direction).
MPSolverInterface::MPSolverInterface(MPSolver* const solver)
    : solver_(solver),
      sync_status_(MODEL_SYNCHRONIZED),
      result_status_(MPSolver::NOT_SOLVED),
      maximize_(false),
      last_constraint_index_(0),
      last_variable_index_(0),
      objective_value_(0.0),
      best_objective_bound_(0.0),
      quiet_(true) {}

MPSolverInterface::~MPSolverInterface() {}

void MPSolverInterface::Write(const std::string& filename) {
  LOG(WARNING) << "Writing model not implemented in this solver interface.";
}

void MPSolverInterface::ExtractModel() {
  switch (sync_status_) {
    case MUST_RELOAD: {
      ExtractNewVariables();
      ExtractNewConstraints();
      ExtractObjective();

      last_constraint_index_ = solver_->constraints_.size();
      last_variable_index_ = solver_->variables_.size();
      sync_status_ = MODEL_SYNCHRONIZED;
      break;
    }
    case MODEL_SYNCHRONIZED: {
      // Everything has already been extracted.
      DCHECK_EQ(last_constraint_index_, solver_->constraints_.size());
      DCHECK_EQ(last_variable_index_, solver_->variables_.size());
      break;
    }
    case SOLUTION_SYNCHRONIZED: {
      // Nothing has changed since last solve.
      DCHECK_EQ(last_constraint_index_, solver_->constraints_.size());
      DCHECK_EQ(last_variable_index_, solver_->variables_.size());
      break;
    }
  }
}

// TODO(user): remove this method.
void MPSolverInterface::ResetExtractionInformation() {
  sync_status_ = MUST_RELOAD;
  last_constraint_index_ = 0;
  last_variable_index_ = 0;
  solver_->variable_is_extracted_.assign(solver_->variables_.size(), false);
  solver_->constraint_is_extracted_.assign(solver_->constraints_.size(), false);
}

bool MPSolverInterface::CheckSolutionIsSynchronized() const {
  if (sync_status_ != SOLUTION_SYNCHRONIZED) {
    LOG(DFATAL)
        << "The model has been changed since the solution was last computed."
        << " MPSolverInterface::sync_status_ = " << sync_status_;
    return false;
  }
  return true;
}

// Default version that can be overwritten by a solver-specific
// version to accommodate for the quirks of each solver.
bool MPSolverInterface::CheckSolutionExists() const {
  if (result_status_ != MPSolver::OPTIMAL &&
      result_status_ != MPSolver::FEASIBLE) {
    LOG(DFATAL) << "No solution exists. MPSolverInterface::result_status_ = "
                << result_status_;
    return false;
  }
  return true;
}

double MPSolverInterface::objective_value() const {
  if (!CheckSolutionIsSynchronizedAndExists()) return 0;
  return objective_value_;
}

double MPSolverInterface::best_objective_bound() const {
  const double trivial_worst_bound =
      maximize_ ? -std::numeric_limits<double>::infinity()
                : std::numeric_limits<double>::infinity();
  if (!IsMIP()) {
    VLOG(1) << "Best objective bound only available for discrete problems.";
    return trivial_worst_bound;
  }
  if (!CheckSolutionIsSynchronized()) {
    return trivial_worst_bound;
  }
  // Special case for empty model.
  if (solver_->variables_.empty() && solver_->constraints_.empty()) {
    return solver_->Objective().offset();
  }
  return best_objective_bound_;
}

void MPSolverInterface::InvalidateSolutionSynchronization() {
  if (sync_status_ == SOLUTION_SYNCHRONIZED) {
    sync_status_ = MODEL_SYNCHRONIZED;
  }
}

double MPSolverInterface::ComputeExactConditionNumber() const {
  // Override this method in interfaces that actually support it.
  LOG(DFATAL) << "ComputeExactConditionNumber not implemented for "
              << ProtoEnumToString<MPModelRequest::SolverType>(
                     static_cast<MPModelRequest::SolverType>(
                         solver_->ProblemType()));
  return 0.0;
}

void MPSolverInterface::SetCommonParameters(const MPSolverParameters& param) {
  // TODO(user): Overhaul the code that sets parameters to enable changing
  // GLOP parameters without issuing warnings.
  // By default, we let GLOP keep its own default tolerance, much more accurate
  // than for the rest of the solvers.
  //
  if (solver_->ProblemType() != MPSolver::GLOP_LINEAR_PROGRAMMING) {
    SetPrimalTolerance(
        param.GetDoubleParam(MPSolverParameters::PRIMAL_TOLERANCE));
    SetDualTolerance(param.GetDoubleParam(MPSolverParameters::DUAL_TOLERANCE));
  }
  SetPresolveMode(param.GetIntegerParam(MPSolverParameters::PRESOLVE));
  // TODO(user): In the future, we could distinguish between the
  // algorithm to solve the root LP and the algorithm to solve node
  // LPs. Not sure if underlying solvers support it.
  int value = param.GetIntegerParam(MPSolverParameters::LP_ALGORITHM);
  if (value != MPSolverParameters::kDefaultIntegerParamValue) {
    SetLpAlgorithm(value);
  }
}

void MPSolverInterface::SetMIPParameters(const MPSolverParameters& param) {
  if (solver_->ProblemType() != MPSolver::GLOP_LINEAR_PROGRAMMING) {
    SetRelativeMipGap(
        param.GetDoubleParam(MPSolverParameters::RELATIVE_MIP_GAP));
  }
}

void MPSolverInterface::SetUnsupportedDoubleParam(
    MPSolverParameters::DoubleParam param) {
  LOG(WARNING) << "Trying to set an unsupported parameter: " << param << ".";
}
void MPSolverInterface::SetUnsupportedIntegerParam(
    MPSolverParameters::IntegerParam param) {
  LOG(WARNING) << "Trying to set an unsupported parameter: " << param << ".";
}
void MPSolverInterface::SetDoubleParamToUnsupportedValue(
    MPSolverParameters::DoubleParam param, double value) {
  LOG(WARNING) << "Trying to set a supported parameter: " << param
               << " to an unsupported value: " << value;
}
void MPSolverInterface::SetIntegerParamToUnsupportedValue(
    MPSolverParameters::IntegerParam param, int value) {
  LOG(WARNING) << "Trying to set a supported parameter: " << param
               << " to an unsupported value: " << value;
}

OrToolsStatus MPSolverInterface::SetNumThreads(int num_threads) {
  return OrToolsStatus::FormatError("SetNumThreads() not supported by %s.", SolverVersion());
}

bool MPSolverInterface::SetSolverSpecificParametersAsString(
    const std::string& parameters) {
  if (parameters.empty()) {
    return true;
  }

  LOG(WARNING) << "SetSolverSpecificParametersAsString() not supported by "
               << SolverVersion();
  return false;
}

// ---------- MPSolverParameters ----------

const double MPSolverParameters::kDefaultRelativeMipGap = 1e-4;
// For the primal and dual tolerances, choose the same default as CLP and GLPK.
const double MPSolverParameters::kDefaultPrimalTolerance =
    operations_research::kDefaultPrimalTolerance;
const double MPSolverParameters::kDefaultDualTolerance = 1e-7;
const MPSolverParameters::PresolveValues MPSolverParameters::kDefaultPresolve =
    MPSolverParameters::PRESOLVE_ON;
const MPSolverParameters::IncrementalityValues
    MPSolverParameters::kDefaultIncrementality =
        MPSolverParameters::INCREMENTALITY_ON;

const double MPSolverParameters::kDefaultDoubleParamValue = -1.0;
const int MPSolverParameters::kDefaultIntegerParamValue = -1;
const double MPSolverParameters::kUnknownDoubleParamValue = -2.0;
const int MPSolverParameters::kUnknownIntegerParamValue = -2;

// The constructor sets all parameters to their default value.
MPSolverParameters::MPSolverParameters()
    : relative_mip_gap_value_(kDefaultRelativeMipGap),
      primal_tolerance_value_(kDefaultPrimalTolerance),
      dual_tolerance_value_(kDefaultDualTolerance),
      presolve_value_(kDefaultPresolve),
      scaling_value_(kDefaultIntegerParamValue),
      lp_algorithm_value_(kDefaultIntegerParamValue),
      incrementality_value_(kDefaultIncrementality),
      lp_algorithm_is_default_(true) {}

void MPSolverParameters::SetDoubleParam(MPSolverParameters::DoubleParam param,
                                        double value) {
  switch (param) {
    case RELATIVE_MIP_GAP: {
      relative_mip_gap_value_ = value;
      break;
    }
    case PRIMAL_TOLERANCE: {
      primal_tolerance_value_ = value;
      break;
    }
    case DUAL_TOLERANCE: {
      dual_tolerance_value_ = value;
      break;
    }
    default: {
      LOG(ERROR) << "Trying to set an unknown parameter: " << param << ".";
    }
  }
}

void MPSolverParameters::SetIntegerParam(MPSolverParameters::IntegerParam param,
                                         int value) {
  switch (param) {
    case PRESOLVE: {
      if (value != PRESOLVE_OFF && value != PRESOLVE_ON) {
        LOG(ERROR) << "Trying to set a supported parameter: " << param
                   << " to an unknown value: " << value;
      }
      presolve_value_ = value;
      break;
    }
    case SCALING: {
      if (value != SCALING_OFF && value != SCALING_ON) {
        LOG(ERROR) << "Trying to set a supported parameter: " << param
                   << " to an unknown value: " << value;
      }
      scaling_value_ = value;
      break;
    }
    case LP_ALGORITHM: {
      if (value != DUAL && value != PRIMAL && value != BARRIER) {
        LOG(ERROR) << "Trying to set a supported parameter: " << param
                   << " to an unknown value: " << value;
      }
      lp_algorithm_value_ = value;
      lp_algorithm_is_default_ = false;
      break;
    }
    case INCREMENTALITY: {
      if (value != INCREMENTALITY_OFF && value != INCREMENTALITY_ON) {
        LOG(ERROR) << "Trying to set a supported parameter: " << param
                   << " to an unknown value: " << value;
      }
      incrementality_value_ = value;
      break;
    }
    default: {
      LOG(ERROR) << "Trying to set an unknown parameter: " << param << ".";
    }
  }
}

void MPSolverParameters::ResetDoubleParam(
    MPSolverParameters::DoubleParam param) {
  switch (param) {
    case RELATIVE_MIP_GAP: {
      relative_mip_gap_value_ = kDefaultRelativeMipGap;
      break;
    }
    case PRIMAL_TOLERANCE: {
      primal_tolerance_value_ = kDefaultPrimalTolerance;
      break;
    }
    case DUAL_TOLERANCE: {
      dual_tolerance_value_ = kDefaultDualTolerance;
      break;
    }
    default: {
      LOG(ERROR) << "Trying to reset an unknown parameter: " << param << ".";
    }
  }
}

void MPSolverParameters::ResetIntegerParam(
    MPSolverParameters::IntegerParam param) {
  switch (param) {
    case PRESOLVE: {
      presolve_value_ = kDefaultPresolve;
      break;
    }
    case SCALING: {
      scaling_value_ = kDefaultIntegerParamValue;
      break;
    }
    case LP_ALGORITHM: {
      lp_algorithm_is_default_ = true;
      break;
    }
    case INCREMENTALITY: {
      incrementality_value_ = kDefaultIncrementality;
      break;
    }
    default: {
      LOG(ERROR) << "Trying to reset an unknown parameter: " << param << ".";
    }
  }
}

void MPSolverParameters::Reset() {
  ResetDoubleParam(RELATIVE_MIP_GAP);
  ResetDoubleParam(PRIMAL_TOLERANCE);
  ResetDoubleParam(DUAL_TOLERANCE);
  ResetIntegerParam(PRESOLVE);
  ResetIntegerParam(SCALING);
  ResetIntegerParam(LP_ALGORITHM);
  ResetIntegerParam(INCREMENTALITY);
}

double MPSolverParameters::GetDoubleParam(
    MPSolverParameters::DoubleParam param) const {
  switch (param) {
    case RELATIVE_MIP_GAP: {
      return relative_mip_gap_value_;
    }
    case PRIMAL_TOLERANCE: {
      return primal_tolerance_value_;
    }
    case DUAL_TOLERANCE: {
      return dual_tolerance_value_;
    }
    default: {
      LOG(ERROR) << "Trying to get an unknown parameter: " << param << ".";
      return kUnknownDoubleParamValue;
    }
  }
}

int MPSolverParameters::GetIntegerParam(
    MPSolverParameters::IntegerParam param) const {
  switch (param) {
    case PRESOLVE: {
      return presolve_value_;
    }
    case LP_ALGORITHM: {
      if (lp_algorithm_is_default_) return kDefaultIntegerParamValue;
      return lp_algorithm_value_;
    }
    case INCREMENTALITY: {
      return incrementality_value_;
    }
    case SCALING: {
      return scaling_value_;
    }
    default: {
      LOG(ERROR) << "Trying to get an unknown parameter: " << param << ".";
      return kUnknownIntegerParamValue;
    }
  }
}

}  // namespace operations_research
