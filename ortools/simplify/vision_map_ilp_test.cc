// Copyright 2022 DeepMirror Inc. All rights reserved.

#include "ortools/simplify/vision_map_ilp.h"
#include <random>
#include "glog/logging.h"
#include "gtest/gtest.h"

namespace dm::graph {

TEST(VisionMapILP, SimpleTest) {
  // make a random problem to solve

  const size_t num_images = 100;
  const size_t num_points = 10000;
  const size_t init_min_points_per_image = 200;
  const size_t min_points_per_image = 100;
  const size_t desired_num_points = num_points * 0.5;

  VisionSummarizationProblem problem;
  problem.num_images = num_images;
  problem.num_points = num_points;
  problem.min_points_per_image = min_points_per_image;
  problem.desired_num_points = desired_num_points;
  problem.points_weight = std::vector<double>(num_points, 1.0);
  problem.verbose = true;

  // make random observation (each image has 200 observations)
  static std::default_random_engine engine(2022);
  std::uniform_real_distribution<double> distribution(0, 1.0);
  double prob_threshold = static_cast<double>(init_min_points_per_image) / num_points;
  int obs_cnt = 0;
  for (size_t i = 0; i < num_images; i++) {
    std::vector<bool> image_viz(num_points, false);
    for (size_t j = 0; j < num_points; j++) {
      if (distribution(engine) < prob_threshold) {
        image_viz[j] = true;
        obs_cnt++;
      }
    }
    problem.image_visibility.emplace_back(std::move(image_viz));
  }
  LOG(INFO) << "mean observation per image before optimization: " << obs_cnt / num_images;

  // solve the problem
  VisionSummarizationResult result;
  EXPECT_TRUE(VisionSummarization(problem, &result));
  EXPECT_EQ(result.points_left.size(), desired_num_points);

  // count observation of image
  int total_cnt = 0;
  for (size_t i = 0; i < num_images; i++) {
    int cnt = 0;
    for (bool ret : result.image_visibility[i]) {
      cnt += ret;
    }
    total_cnt += cnt;
    // EXPECT_GE(cnt, min_points_per_image);
  }
  LOG(INFO) << "mean observation per image after optimization: " << total_cnt / num_images;
}

}  // namespace dm::graph
