// Copyright 2022 DeepMirror Inc. All rights reserved.

#ifndef GRAPH_SIMPLIFY_VISION_MAP_ILP_H_
#define GRAPH_SIMPLIFY_VISION_MAP_ILP_H_

#include <set>
#include <string>
#include <vector>

namespace dm::graph {

// solve the vision map summarization problem
// https://deepmirror.atlassian.net/wiki/spaces/MTD/pages/1595375655/Visual+map+summarization

// bazel run @com_google_ortools//ortools/linear_solver:linear_solver_test
struct VisionSummarizationProblem {
  // for the problem
  size_t num_images;
  size_t num_points;

  // weight for each point
  std::vector<double> points_weight;
  std::vector<std::vector<bool>> image_visibility;

  // for the objective
  size_t min_points_per_image;
  size_t desired_num_points;

  // solver options
  double lambda = 1.0;
  bool verbose = false;
};

struct VisionSummarizationResult {
  std::set<size_t> points_left;
  std::vector<std::vector<bool>> image_visibility;
};

bool VisionSummarization(const VisionSummarizationProblem& problem,
                         VisionSummarizationResult* result);

}  // namespace dm::graph

#endif  // GRAPH_SIMPLIFY_VISION_MAP_ILP_H_
