FROM ubuntu:24.04@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates clang cmake curl g++ git ninja-build python3 unzip \
    && rm -rf /var/lib/apt/lists/*

RUN curl --fail --location --retry 3 \
      https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-linux-amd64.zip \
      -o /tmp/rexglue.zip \
    && echo '0388edb3f8fb1444f1b0ad5d440cb02d9690ac20a76b04fd6b7dcc19f59328a6  /tmp/rexglue.zip' | sha256sum -c - \
    && unzip -q /tmp/rexglue.zip -d /opt/rexglue \
    && chmod +x /opt/rexglue/linux-amd64/bin/rexglue \
    && rm /tmp/rexglue.zip

RUN apt-get update \
    && apt-get install -y --no-install-recommends libx11-xcb1 libwayland-client0 libvulkan1 \
    && rm -rf /var/lib/apt/lists/* \
    && ldd /opt/rexglue/linux-amd64/bin/rexglue \
    && /opt/rexglue/linux-amd64/bin/rexglue --version
