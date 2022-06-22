# Set the limit of core file size to unlimited.
ulimit -c unlimited

# Bazel command-line completion (aka tab-completion).
if [[ -f /usr/local/lib/bazel/bin/bazel-complete.bash ]]; then
  . /usr/local/lib/bazel/bin/bazel-complete.bash
fi
