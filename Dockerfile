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
        debhelper \
        gdb \
        git \
        libgstreamer1.0-dev \
        meson \
        ninja-build \
        python3-dev \
        valgrind \
    && apt-get -yq clean \
    && rm -rf /var/lib/apt/lists/*

# --- TEST --- #

# bookworm has no gst-validate package, so the tests that use it skip here

# mixtape[test] pins pytest 5, which breaks on current Python
RUN pip3 install --break-system-packages mixtape==0.5.1 pytest pytest-asyncio

WORKDIR /src


