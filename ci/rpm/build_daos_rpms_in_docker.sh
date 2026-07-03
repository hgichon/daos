#!/bin/bash
set -euo pipefail

# Build DAOS RPMs inside a disposable container and copy resulting RPMs back
# to the host before container teardown.
#
# Usage:
#   ./ci/rpm/build_daos_rpms_in_docker.sh <el9|el8|leap15>
#
# Arguments:
#   <target>   Image flavor tag suffix (el9, el8, leap15)
#
# Requirements:
#   - docker command must be available on host
#   - host must contain a built image for the specified target, e.g. daos/el9:build-ci
#   - host must contain /home/daos directory with write permissions for the current user
#
# Environment variables:
#   JOBS             SCons parallelism for main build (default: 88)
#   KEEP_CONTAINER   If true, don't remove container on exit (default: false)
#   HOST_RPM_DIR     Host directory for collected RPMs
#                   (default: <repo>/artifacts/<target>)

usage() {
  cat <<'EOF'
Usage: build_daos_rpms_in_docker.sh <el9|leap15>

Example:
  ./ci/rpm/build_daos_rpms_in_docker.sh el9
  ./ci/rpm/build_daos_rpms_in_docker.sh leap15
EOF
}

if [[ $# -lt 1 || $# -gt 1 ]]; then
  usage >&2
  exit 1
fi

TARGET="$1"
JOBS="${JOBS:-88}"
KEEP_CONTAINER="${KEEP_CONTAINER:-false}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." >/dev/null 2>&1 && pwd)"
WORKDIR_HOST="${REPO_ROOT}"
WORKDIR_CONT="/workdir"
HOST_RPM_DIR_DEFAULT="${REPO_ROOT}/artifacts/${TARGET}"
HOST_RPM_DIR="${HOST_RPM_DIR:-$HOST_RPM_DIR_DEFAULT}"

command -v docker >/dev/null 2>&1 || {
  echo "docker command not found" >&2
  exit 1
}

case "$TARGET" in
  el9|leap15) ;;
  *)
    echo "Invalid target: $TARGET (allowed: el9, leap15)" >&2
    exit 1
    ;;
esac

CONTAINER_NAME="daos/${TARGET}:build-ci"
STAGE_NAME_VALUE="$TARGET"

docker image inspect "${CONTAINER_NAME}" >/dev/null 2>&1 || {
  echo "Image not found: ${CONTAINER_NAME}" >&2
  echo "Build it first, e.g. docker build --target build-ci -t ${CONTAINER_NAME} ..." >&2
  exit 1
}

for required in \
  "${REPO_ROOT}/ci/rpm/build_deps.sh" \
  "${REPO_ROOT}/ci/rpm/gen_rpms.sh"\
  "${REPO_ROOT}/ci/parse_ci_envs.sh"; do
  [[ -f "$required" ]] || {
    echo "Missing required file: $required" >&2
    exit 1
  }
done

echo "Starting container from image: ${CONTAINER_NAME}"
CONTAINER="$(
  docker run \
    --userns=keep-id \
    -t \
    -d \
    -u "1101:1101" \
    -w "${WORKDIR_CONT}" \
    -v "${WORKDIR_HOST}:${WORKDIR_CONT}:rw,z" \
    "${CONTAINER_NAME}" \
    cat
)"

echo "Container ID: ${CONTAINER}"

cleanup() {
  if [[ "${KEEP_CONTAINER}" == "true" ]]; then
    echo "Keeping container (KEEP_CONTAINER=true): ${CONTAINER}"
    return
  fi
  echo "Stopping/removing container: ${CONTAINER}"
  docker rm -f "${CONTAINER}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "Container top:"
docker top "${CONTAINER}" -eo pid,comm

echo "Build deps:"
docker exec -i --user daos_server "${CONTAINER}" bash -lc './ci/rpm/build_deps.sh'

echo "Clean scons:"
docker exec -i --user daos_server "${CONTAINER}" bash -lc '/home/daos/venv/bin/scons -c'

echo "Remove old build artifacts:"
docker exec -i --user daos_server "${CONTAINER}" bash -lc \
  'rm -rf _build.external install build daos_m.conf daos.conf iof.conf cart-Linux.conf .sconsign.dblite .sconsign-Linux.dblite .sconf-temp .sconf-temp-Linux'

echo "Build DAOS:"
docker exec -i --user daos_server "${CONTAINER}" bash -lc \
       "/home/daos/venv/bin/scons --config=force -j ${JOBS} \
       --build-deps=no install USE_INSTALLED=all COMPILER=gcc \
       BUILD_TYPE=dev PREFIX=/opt/daos TARGET_TYPE=release"

echo "Generate RPMs with STAGE_NAME=${STAGE_NAME_VALUE}:"
docker exec -i --user daos_server "${CONTAINER}" bash -lc \
       "STAGE_NAME=${STAGE_NAME_VALUE} ./ci/rpm/gen_rpms.sh ${TARGET} false"

echo "Collect RPM artifacts to host: ${HOST_RPM_DIR}"
mkdir -p "${HOST_RPM_DIR}"
docker cp "${CONTAINER}:/home/daos/rpms/." "${HOST_RPM_DIR}" >/dev/null

echo "RPM artifacts copied to: ${HOST_RPM_DIR}"

echo "Done."