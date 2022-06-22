#!/usr/bin/env bash
#
# Run commands in a running container.
# Without any arguments to keep interactive.
#
# Example:
#   Get inside the default container:
#     $ ./exec_container.sh
#   Get inside [test] container:
#     $ ./exec_container.sh test
#   Run `bazel test //common/time:time_test` without get inside the container:
#     $ ./exec_container.sh -- bazel test //common/time:time_test

set -e

# Use argument $1 as $NAME.
NAME=${1:-${NAME}}

# The directory contains current script.
DIR=$(dirname $(realpath "$BASH_SOURCE"))
# The directory contains .git directory.
REPO_DIR=${REPO_DIR:-$(
  d=$DIR
  while [[ $d =~ ^$HOME ]]; do
    [[ -d $d/.git ]] && echo $d && break
    d=$(dirname "$d")
  done
)}
[[ -n $REPO_DIR ]] || (
  echo >&2 "Failed to find working directory"
  exit 1
)

# Set default $NAME.
if [[ -z $NAME || $NAME == "--" ]]; then
  NAME=$(basename $REPO_DIR)
fi

WORK_DIR_IN_CONTAINER=${WORK_DIR_IN_CONTAINER:-/$(basename $REPO_DIR)}

# Check if the container exists.
if ! docker ps -a --format "{{.Names}}" | grep -q "^$NAME$"; then
  echo "Container [$NAME] does not exist"
  exit 1
fi
# Check if the container is running.
if ! docker ps --format "{{.Names}}" | grep -q "^$NAME$"; then
  echo "Starting container [$NAME] ..."
  docker start $NAME >/dev/null
fi

# Allow docker to connect to the X server.
xhost +local:docker >/dev/null

if [[ $# -gt 1 ]]; then
  # Execute command ${*:2}.
  # NOTE(ziye): Kill `docker exec` command will not terminate the spawned process.
  # Check https://github.com/moby/moby/issues/9098.
  docker exec \
    -u $USER \
    -e USER \
    -e HISTFILE=$WORK_DIR_IN_CONTAINER/.${NAME}_bash_history \
    $NAME \
    /bin/bash -c ". ~/.bash_local; ${*:2}"
else
  # Get inside the container.
  docker exec -it \
    -u $USER \
    -e USER \
    -e HISTFILE=$WORK_DIR_IN_CONTAINER/.${NAME}_bash_history \
    $NAME \
    /bin/bash
fi

# Disallow docker to connect to the X server.
xhost -local:docker >/dev/null
