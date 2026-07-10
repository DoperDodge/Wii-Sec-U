# Build environment for both Wii-Sec-U Aroma plugins.
#
#   docker build -t wsu-wiiu-builder -f wiiu/Dockerfile .
#   docker run --rm -v "$PWD":/project wsu-wiiu-builder \
#       make -C wiiu/input-plugin -j$(nproc)
#   docker run --rm -v "$PWD":/project wsu-wiiu-builder \
#       make -C wiiu/stream-plugin -j$(nproc)
#
# Image versions follow the wiiu-env plugin ecosystem (same pins as
# actively maintained plugins like ftpiiu_plugin).
FROM ghcr.io/wiiu-env/devkitppc:20260225

COPY --from=ghcr.io/wiiu-env/wiiupluginsystem:20260418 /artifacts $DEVKITPRO

# libjpeg-turbo portlib for the wsu-stream software encoder.
RUN dkp-pacman -Sy --noconfirm ppc-libjpeg-turbo || \
    pacman -Sy --noconfirm ppc-libjpeg-turbo

WORKDIR /project
