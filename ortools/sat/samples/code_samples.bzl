load("//bazel:variables.bzl", "COPTS")

def code_sample_cc(sample):
  native.cc_binary(
      name = sample,
      srcs = [sample + ".cc"],
      copts = COPTS,
      deps = [
        "//ortools/sat:cp_model",
        "//ortools/sat:cp_model_solver",
      ],
  )

  native.cc_test(
      name = sample+"_test",
      size = "small",
      srcs = [sample + ".cc"],
      copts = COPTS,
      deps = [
        ":"+sample,
        "//ortools/sat:cp_model",
        "//ortools/sat:cp_model_solver",
      ],
  )

