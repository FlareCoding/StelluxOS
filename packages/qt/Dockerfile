# Build environment for the qt recipe. The base image digest and the package
# list come from versions.sh through build arguments, so this file never drifts from the pins.
ARG UBUNTU_IMAGE
FROM ${UBUNTU_IMAGE}

ARG UBUNTU_PKGS
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ${UBUNTU_PKGS} && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /work
ENTRYPOINT ["sh", "/recipe/container.sh"]
