# Build environment for the mesa recipe. The base image digest, the package list and the tool
# versions come from versions.sh through build arguments, so this file never drifts from the pins.
ARG UBUNTU_IMAGE
FROM ${UBUNTU_IMAGE}

ARG UBUNTU_PKGS
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ${UBUNTU_PKGS} && \
    rm -rf /var/lib/apt/lists/*

# Mesa's Rust support needs a newer Meson than the distribution's
ARG MESON_VER
ARG MESON_SHA256
RUN echo "meson==${MESON_VER} --hash=sha256:${MESON_SHA256}" > /tmp/meson.txt && \
    pip3 install -q --no-cache-dir --break-system-packages --require-hashes -r /tmp/meson.txt && \
    rm /tmp/meson.txt

# Rust with the standard library of both targets, and the binding generators
ARG RUSTUP_VER
ARG RUSTUP_SHA256
ARG RUST_VER
ARG BINDGEN_VER
ARG CBINDGEN_VER
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -fsSL --retry 3 -o /tmp/rustup-init \
        "https://static.rust-lang.org/rustup/archive/${RUSTUP_VER}/x86_64-unknown-linux-gnu/rustup-init" && \
    echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c - > /dev/null && \
    chmod +x /tmp/rustup-init && \
    /tmp/rustup-init -y -q --no-modify-path --profile minimal --default-toolchain "${RUST_VER}" \
        --target x86_64-unknown-linux-musl --target aarch64-unknown-linux-musl && \
    rm /tmp/rustup-init && \
    cargo install -q --locked bindgen-cli --version "${BINDGEN_VER}" && \
    cargo install -q --locked cbindgen --version "${CBINDGEN_VER}" && \
    rm -rf "$CARGO_HOME/registry" "$CARGO_HOME/git"

WORKDIR /work
ENTRYPOINT ["sh", "/recipe/container.sh"]
