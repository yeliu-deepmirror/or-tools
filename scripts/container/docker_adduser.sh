#!/usr/bin/env bash

set -e

# Add a group to the system.
addgroup --gid "$DOCKER_GRP_ID" "$DOCKER_GRP"
# Add a user to the system.
adduser "$DOCKER_USER" \
  --uid "$DOCKER_USER_ID" --gid "$DOCKER_GRP_ID" \
  --disabled-password --force-badname --gecos '' \
  2>/dev/null
# Copy the default configuration files for the user.
# This step is skipped in `adduser` since we already have the home directory
# for the cache path mounted when we created the container.
cp -r /etc/skel/. /home/${DOCKER_USER}

# Allow members of group sudo to execute any command.
echo '%sudo ALL=(ALL) NOPASSWD:ALL' >>/etc/sudoers
# Add the user to group sudo.
usermod -aG sudo "$DOCKER_USER"

# Set user files ownership to current user, such as .bashrc, .profile, etc.
chown ${DOCKER_USER}:${DOCKER_GRP} /home/${DOCKER_USER}
ls -ad /home/${DOCKER_USER}/.??* | xargs chown -R ${DOCKER_USER}:${DOCKER_GRP}
