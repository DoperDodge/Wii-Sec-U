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
# pkg.devkitpro.org intermittently 403s CI runners, so retry with backoff;
# the CI workflow additionally caches this layer so a successful sync is
# reused instead of re-hitting the server every run.
RUN set -e; \
    ok=0; \
    for i in 1 2 3 4 5; do \
        if dkp-pacman -Sy --noconfirm ppc-libjpeg-turbo; then ok=1; break; fi; \
        echo "dkp-pacman attempt $i failed; retrying in 20s"; \
        sleep 20; \
    done; \
    test "$ok" = 1

WORKDIR /project
