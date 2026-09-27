# syntax=docker/dockerfile:1
#
# One image for every AquaSPH service: aquasph-api, aquasph-worker,
# aquasph-admin and aquactl, plus the solver the worker runs. One image
# means the API, the worker and the solver in a deployment always come
# from the same commit; the worker's solver_id (the solver binary's hash)
# then changes exactly when the image does, which is what the result
# cache relies on (docs/platform/adr/0002-content-addressed-cache.md).
#
#   docker build -t aquasph --build-arg GIT_REV=$(git rev-parse --short HEAD) .
#
# Ubuntu 24.04 for the C++ stages because it is the toolchain the solver
# is tested with (GCC 13, as on the CI runners). The result cache relies
# on bit-reproducible output, and floating-point results are only
# guaranteed identical for the same compiler: another GCC may contract
# a*b+c into an FMA differently.

ARG BASE=ubuntu:24.04

# ---- 1. solver (C++17, OpenMP) ------------------------------------------
FROM ${BASE} AS solver
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
# CMake's configure step copies configs/ into the build tree.
COPY configs ./configs
ARG GIT_REV=unknown
# Tests are built and run in CI, not in the image: the image is the
# deliverable, and GoogleTest would only add minutes and megabytes.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_TESTS=OFF \
          -DAQUASPH_GIT_REV_OVERRIDE="${GIT_REV}" \
 && cmake --build build --target aquasph -j"$(nproc)" \
 && strip build/aquasph

# ---- 2. control plane (Go, static) --------------------------------------
FROM golang:1.25-bookworm AS control
WORKDIR /src/backend
COPY backend/go.mod backend/go.sum ./
RUN go mod download
COPY backend/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/ ./cmd/...

# ---- 3. runtime -----------------------------------------------------------
FROM ${BASE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends libgomp1 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /var/lib/aquasph aquasph \
 # Created here and owned by the service user, so a named volume mounted
 # on it inherits the ownership instead of arriving root-owned.
 && mkdir -p /var/lib/aquasph/artifacts /var/lib/aquasph/work \
 && chown -R aquasph:aquasph /var/lib/aquasph
COPY --from=solver /src/build/aquasph /usr/local/bin/aquasph
COPY --from=control /out/ /usr/local/bin/
COPY configs/scenarios /opt/aquasph/scenarios
ENV AQUASPH_SOLVER_PATH=/usr/local/bin/aquasph \
    AQUASPH_SCENARIO_DIR=/opt/aquasph/scenarios \
    AQUASPH_ARTIFACT_DIR=/var/lib/aquasph/artifacts \
    AQUASPH_WORK_DIR=/var/lib/aquasph/work
USER aquasph
WORKDIR /var/lib/aquasph
EXPOSE 8080 9090
CMD ["aquasph-api"]
