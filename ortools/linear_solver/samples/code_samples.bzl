load("//bazel:variables.bzl", "COPTS")

def code_sample_cc(sample):
  native.cc_binary(
      name = sample,
      srcs = [sample + ".cc"],
      copts = ["-DUSE_SCIP"] + COPTS,
      deps = [
        "//ortools/base",
        "//ortools/linear_solver",
        "//ortools/linear_solver:linear_solver_cc_proto",
      ],
  )

  native.cc_test(
      name = sample+"_test",
      size = "small",
      srcs = [sample + ".cc"],
      copts = ["-DUSE_SCIP"] + COPTS,
      deps = [
        ":"+sample,
        "//ortools/base",
        "//ortools/linear_solver",
        "//ortools/linear_solver:linear_solver_cc_proto",
      ],
  )

