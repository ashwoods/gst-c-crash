FROM debian:bullseye-slim 

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
        python3.8 \
    && apt-get -yq clean \
    && rm -rf /var/lib/apt/lists/*

# --- BUILD AND DEBUG --- #

RUN set -ex \
    && apt-get -yq update \
    && apt-get install -yq --no-install-recommends --no-upgrade \
        build-essential \
        debhelper \
        git \
        libgstreamer1.0-dev \
        meson \
        ninja-build \
        python3-dev \
    && apt-get -yq clean \
    && rm -rf /var/lib/apt/lists/*

# --- TEST --- #

RUN pip3 install mixtape[test]

WORKDIR /src


