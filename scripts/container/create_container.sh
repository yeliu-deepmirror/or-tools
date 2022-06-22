#!/usr/bin/env bash
#
# Create a docker container on given docker image and set up basic user
# development environment.

set -e

# Use argument $1 as $IMG.
# Use argument $2 as $NAME.
IMG=${1:-${IMG}}
NAME=${2:-${NAME}}

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
[[ -d $REPO_DIR ]] || (
  echo >&2 "Failed to find working directory"
  exit 1
)

# Set default $IMG and $NAME.
work_dir_basename=$(basename $REPO_DIR)
if [[ -z $IMG ]]; then
  # Change camel case to lowercase with dashes(-).
  IMG=$(echo $work_dir_basename | sed 's/[A-Z]/-&/g' | sed 's/^-//')
  IMG=${IMG,,}-dev
fi
if [[ -z $NAME ]]; then
  NAME=$work_dir_basename
fi

# Check if the image exists.
if [[ -z $(docker images -f "reference=$IMG" --format='{{.Tag}}') ]]; then
  echo >&2 "Failed to find the image [$IMG]"
  exit 1
fi

# Check if the container exists.
if docker ps -a --format "{{.Names}}" | grep -q "^$NAME$"; then
  echo >&2 "Container [$NAME] is already existing"
  echo >&2 "Run 'docker stop $NAME && docker rm $NAME' before creating new one"
  exit
fi

echo "Creating container [$NAME] on image [$IMG] ..."

# Prepare cache path.
mkdir -p $HOME/.cache
DOCKER_HOME=/home/$USER
if [[ "$USER" == "root" ]]; then
  DOCKER_HOME=/root
fi

# The path where the repo directory is mounted in the container.
WORK_DIR_IN_CONTAINER=${WORK_DIR_IN_CONTAINER:-/$(basename $REPO_DIR)}
echo "Bind mount [$REPO_DIR] as [$WORK_DIR_IN_CONTAINER] in the container"

# Create container.
docker run -it -d --name $NAME \
  --privileged \
  --gpus all \
  --net host \
  --ipc host \
  --hostname in_docker \
  --add-host in_docker:127.0.0.1 \
  --add-host $(hostname):127.0.0.1 \
  --pid host \
  --shm-size 2G \
  -e DISPLAY \
  -e NVIDIA_VISIBLE_DEVICES=all \
  -e NVIDIA_DRIVER_CAPABILITIES=all \
  -v /etc/localtime:/etc/localtime:ro \
  -v /usr/src:/usr/src \
  -v /lib/modules:/lib/modules \
  -v /dev:/dev \
  -v /media:/media:rshared \
  -v /mnt:/mnt \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v $HOME/.cache:$DOCKER_HOME/.cache \
  -v $REPO_DIR:$WORK_DIR_IN_CONTAINER \
  -w $WORK_DIR_IN_CONTAINER \
  $IMG \
  /bin/bash

echo "Container [$NAME] has been created"

# Add current user and group inside the container.
if [[ "$USER" != "root" ]]; then
  echo "Adding user [$USER] inside the container ..."
  docker cp $DIR/docker_adduser.sh $NAME:/tmp
  docker exec \
    -e DOCKER_USER=$USER \
    -e DOCKER_USER_ID=$(id -u) \
    -e DOCKER_GRP=$(id -g -n) \
    -e DOCKER_GRP_ID=$(id -g) \
    $NAME \
    bash -c "/tmp/docker_adduser.sh >/dev/null && rm /tmp/docker_adduser.sh"
  echo "Done"
fi

# Set up basic bash configuration inside the container.
docker exec \
  $NAME \
  bash -c "echo '
if [[ -f ~/.bash_local ]]; then
  . ~/.bash_local
fi
' >> $DOCKER_HOME/.bashrc"

BASH_LOCAL=$DIR/docker_bashrc.sh
if [[ -f $BASH_LOCAL ]]; then
  echo "Adding bash config [$BASH_LOCAL] inside the container ..."
  docker cp $BASH_LOCAL $NAME:$DOCKER_HOME/.bash_local
  echo "Done"
fi
