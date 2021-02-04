#!/bin/sh

set -xeuo pipefail

git fetch origin nightly
PYTORCH_NIGHTLY_COMMIT=$(git log -1 --pretty=%B origin/nightly | head -1 | \
                             sed 's,.*(\([[:xdigit:]]*\)),\1,' | head -c 7)

# Build PyTorch Nightly Docker
# Full name: ghcr.io/pytorch-nightly/pytorch:${PYTORCH_NIGHTLY_COMMIT}
make -f docker.Makefile \
     DOCKER_REGISTRY=ghcr.io \
     DOCKER_ORG=pytorch-nightly \
     DOCKER_TAG=${PYTORCH_NIGHTLY_COMMIT} \
     INSTALL_CHANNEL=pytorch-nightly BUILD_TYPE=official devel-image

# Push the nightly docker to GitHub Container Registry
make -f docker.Makefile \
     DOCKER_REGISTRY=ghcr.io \
     DOCKER_ORG=pytorch-nightly \
     DOCKER_TAG=${PYTORCH_NIGHTLY_COMMIT} \
     devel-push
