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

# The wsu-stream encoder needs the ppc-libjpeg-turbo portlib, which this
# devkitppc image already ships. Deliberately no `dkp-pacman -Sy` here:
# pkg.devkitpro.org blocks CI runners, so the image must stay buildable
# without reaching devkitPro's package server.

WORKDIR /project
