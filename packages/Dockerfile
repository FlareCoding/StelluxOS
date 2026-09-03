# Build environment for the Stellux developer packages. The base image
# digest and the package list come from versions.sh through build
# arguments so this file never drifts from the pins.
ARG ALPINE_IMAGE
FROM ${ALPINE_IMAGE}

ARG ALPINE_PKGS
RUN apk add --no-cache ${ALPINE_PKGS}

WORKDIR /work
ENTRYPOINT ["sh", "/work/build.sh"]
