# Build environment for the APK (tools/build_all.sh). Contains no game data.
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-pip git ca-certificates curl unzip xz-utils make perl file \
      openjdk-17-jdk-headless glslang-tools spirv-tools \
 && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir --break-system-packages capstone==5.0.9 pefile==2024.8.26
ENV ANDROID_SDK=/sdk ANDROID_HOME=/sdk
RUN mkdir -p /sdk/cmdline-tools && cd /tmp \
 && curl -fsSLo tools.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
 && unzip -q tools.zip -d /sdk/cmdline-tools && mv /sdk/cmdline-tools/cmdline-tools /sdk/cmdline-tools/latest && rm tools.zip \
 && yes | /sdk/cmdline-tools/latest/bin/sdkmanager --licenses > /dev/null \
 && /sdk/cmdline-tools/latest/bin/sdkmanager "platform-tools" "platforms;android-35" "build-tools;34.0.0" "ndk;27.3.13750724" "cmake;3.31.6" > /dev/null \
 && chmod -R a+rwX /sdk
WORKDIR /w
