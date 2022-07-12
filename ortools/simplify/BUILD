load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_library(
    name = "vision_map_ilp",
    srcs = ["vision_map_ilp.cc"],
    hdrs = ["vision_map_ilp.h"],
    deps = [
        "//ortools/base",
        "//ortools/linear_solver",
        "//ortools/linear_solver:linear_solver_cc_proto",
        "@com_github_glog//:glog",
    ],
)

cc_test(
    name = "vision_map_ilp_test",
    srcs = ["vision_map_ilp_test.cc"],
    tags = [
        "no_msan",
        "no_asan",
        "no_tsan",
        "no_ubsan",
    ],
    deps = [
        ":vision_map_ilp",
        "@com_google_googletest//:gtest_main",
    ],
)
