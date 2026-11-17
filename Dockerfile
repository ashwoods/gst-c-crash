FROM debian:bookworm-slim 

# --- RUNTIME --- #

RUN set -ex \
    && apt-get -yq update \
    && apt-get install -yq --no-upgrade --no-install-recommends \
        gir1.2-gst-* \
        gstreamer1.0-nice \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-tools \
        libgstreamer-plugins-base1.0 \
        libgstreamer1.0-0 \
        python3-gst-1.0 \
        python3-pip \
        python3 \
    && apt-get -yq clean \
    && rm -rf /var/lib/apt/lists/*

# --- BUILD AND DEBUG --- #

RUN set -ex \
    && apt-get -yq update \
    && apt-get install -yq --no-install-recommends --no-upgrade \
        build-essential \
        ca-certificates \
        curl \
        debhelper \
        gdb \
        git \
        libgstreamer1.0-dev \
        meson \
        ninja-build \
        pkg-config \
        python3-dev \
        valgrind \
    && apt-get -yq clean \
    && rm -rf /var/lib/apt/lists/*

# --- RUST --- #

# The Rust examples need a newer rustc than bookworm's. docker-compose.yml sets
# CARGO_HOME to the repository, for the crates; the toolchain stays here.
ENV RUSTUP_HOME=/usr/local/rustup \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | CARGO_HOME=/usr/local/cargo sh -s -- -y --profile minimal \
        --default-toolchain stable --no-modify-path

# --- TEST --- #

# bookworm has no gst-validate package, so the tests that use it skip here

# mixtape[test] pins pytest 5, which breaks on current Python
RUN pip3 install --break-system-packages mixtape==0.5.1 pytest pytest-asyncio

WORKDIR /src


