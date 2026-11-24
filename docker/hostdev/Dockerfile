FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends clang lld cmake ninja-build gdb make python3 ca-certificates file binutils glslang-tools spirv-tools pkg-config git g++ libvulkan-dev mesa-vulkan-drivers vulkan-tools xvfb libx11-dev x11-utils imagemagick && rm -rf /var/lib/apt/lists/*
