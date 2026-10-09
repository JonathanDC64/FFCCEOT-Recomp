# The Linux build and package image (tools/package.py linux). Ubuntu 22.04 (glibc 2.35) so the AppImage runs on anything
# newer, the pinned LLVM for both the build and the compiler the package ships, and appimagetool.
# Nothing of the game goes in: the ROM is only ever mounted when a smoke test runs.
FROM ubuntu:22.04

ARG LLVM_URL
ARG LLVM_SHA256
ARG APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
ARG RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl xz-utils git python3 python3-pip ninja-build build-essential pkg-config file \
        squashfs-tools desktop-file-utils patchelf \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
        libwayland-dev libxkbcommon-dev libegl-dev libgl-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev \
        libudev-dev libdbus-1-dev \
        xvfb xauth libgl1-mesa-dri libglx-mesa0 libegl1 \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir "cmake==3.31.6"

# The game uses C++20 <format>, which needs libstdc++ 13 (22.04 ships 11). clang takes the newest GCC it finds; the program links
# libstdc++ statically (FFCC_PACKAGE), so the AppImage does not need it on the player's system.
RUN apt-get update && apt-get install -y --no-install-recommends gnupg dirmngr software-properties-common \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update && apt-get install -y --no-install-recommends libstdc++-13-dev \
    && rm -rf /var/lib/apt/lists/*

# LLVM: only what is used (the compilers, lld, clang's headers, the shared libraries the binaries load).
RUN curl -fsSL "$LLVM_URL" -o /tmp/llvm.tar.xz \
    && echo "$LLVM_SHA256  /tmp/llvm.tar.xz" | sha256sum -c - \
    && mkdir /opt/llvm \
    && tar -xJf /tmp/llvm.tar.xz -C /opt/llvm --strip-components=1 --wildcards \
        '*/bin/*' '*/lib/clang/*' '*/lib/libLLVM*' '*/lib/libclang-cpp*' '*/LICENSE.TXT' \
    && rm /tmp/llvm.tar.xz

RUN curl -fsSL "$APPIMAGETOOL_URL" -o /opt/appimagetool \
    && echo "$APPIMAGETOOL_SHA256  /opt/appimagetool" | sha256sum -c - \
    && chmod +x /opt/appimagetool \
    && curl -fsSL "$RUNTIME_URL" -o /opt/runtime-x86_64 \
    && echo "$RUNTIME_SHA256  /opt/runtime-x86_64" | sha256sum -c -

# (the Linux archive carries no licence text: the same file, from the tag, is shipped with the compiler)
RUN curl -fsSL https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-22.1.8/llvm/LICENSE.TXT -o /opt/llvm/LICENSE.TXT \
    && echo "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee  /opt/llvm/LICENSE.TXT" | sha256sum -c -

ENV PATH=/opt/llvm/bin:$PATH CC=clang CXX=clang++ FFCC_IN_DOCKER=1
WORKDIR /src
