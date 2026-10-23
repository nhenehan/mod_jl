#!/usr/bin/env bash
# Runs a command in the mod_jl dev container (podman, rootless).
#
#   ct/dev.sh build            build or refresh the image
#   ct/dev.sh                  interactive shell
#   ct/dev.sh <cmd> [args...]  run a command in /src/mod_jl
#
# Mounts mod_jl read-write at /src/mod_jl. rexi and jexi are fetched from
# GitHub by CMake and Julia's Pkg; the Julia depot and cargo's registry live
# in the volume mod_jl-depot.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
image=localhost/mod_jl-dev:latest

if [[ ${1:-} == build ]]; then
    exec podman build -t "$image" -f "$here/Containerfile" "$here"
fi

podman image exists "$image" || podman build -t "$image" -f "$here/Containerfile" "$here"
podman volume exists mod_jl-depot || podman volume create mod_jl-depot >/dev/null

tty=()
[[ -t 0 && -t 1 ]] && tty=(-it)
[[ $# -eq 0 ]] && set -- bash

exec podman run --rm "${tty[@]}" \
    --userns=keep-id --user "$(id -u):$(id -g)" \
    --network=host \
    -v "$root:/src/mod_jl" \
    -v mod_jl-depot:/depot:U \
    -e HOME=/depot/home \
    -e CARGO_HOME=/depot/cargo \
    "$image" bash -c 'mkdir -p /depot/home /depot/julia /depot/cargo; exec "$@"' _ "$@"
