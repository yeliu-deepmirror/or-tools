#!/usr/bin/env bash
#
# Create the docker container of current project and run commands inside it.
# Without any arguments to keep interactive.
#
# Example:
#   Get inside the container:
#     $ ./run.sh
#   Run `bazel test //common/time:time_test` without get inside the container:
#     $ ./run.sh bazel test //common/time:time_test

set -e

# The directory contains current script.
DIR=$(dirname $(realpath "$BASH_SOURCE"))

# Create the container if non-existent.
$(find $DIR -name create_container.sh)

# Run commands or keep interactive.
$(find $DIR -name exec_container.sh) -- ${*}
