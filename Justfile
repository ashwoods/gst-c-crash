# Common tasks. `just` lists them.

set positional-arguments

[private]
default:
    @just --list --unsorted

# Create .venv and a meson build/ folder (CC=cc, no sccache); safe to repeat
setup:
    sh -e .ultimate/worktree-setup.sh

# Build the plugin into build/gst
build:
    ninja -C build

# Build, then run pytest; extra arguments go to pytest (default: tests)
test *args: build
    [ $# -gt 0 ] || set -- tests; GST_PLUGIN_PATH="$PWD/build/gst" .venv/bin/pytest -q -p no:cacheprovider "$@"

# Show an element (leaky, faulty, mangler) with gst-inspect-1.0
inspect element: build
    GST_PLUGIN_PATH="$PWD/build/gst" gst-inspect-1.0 "$1"

# Build the plugin in Docker (Debian bookworm), into build/
docker-build:
    docker compose run --rm build

# Run the tests in Docker
docker-test:
    docker compose run --rm test

# Open a shell in Docker, with GST_PLUGIN_PATH set
docker-shell:
    docker compose run --rm shell

# Check C files (default: gst/*.c) against GStreamer's style with gst-indent, as the pre-commit hook does
indent *files:
    #!/bin/sh
    [ -x .venv/bin/gst-indent-1.0 ] || { echo "gst-indent not found: run just setup" >&2; exit 1; }
    [ $# -gt 0 ] || set -- gst/*.c
    status=0
    for f in "$@"; do
      # gst-indent applies GStreamer's style by default; run it twice, as it
      # can break lines differently on a second run
      .venv/bin/gst-indent-1.0 -st <"$f" | .venv/bin/gst-indent-1.0 -st | diff -u -p -L "$f" -L "$f (gst-indent)" "$f" - || status=1
    done
    [ $status = 0 ] && echo "Style OK" || echo "Style errors: fix them with just fix-indent" >&2
    exit $status

# Rewrite C files (default: gst/*.c) in GStreamer's style with gst-indent
fix-indent *files:
    #!/bin/sh -e
    [ -x .venv/bin/gst-indent-1.0 ] || { echo "gst-indent not found: run just setup" >&2; exit 1; }
    [ $# -gt 0 ] || set -- gst/*.c
    tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT
    for f in "$@"; do
      # stdin to stdout: given a file, gst-indent leaves a file~ backup beside it
      .venv/bin/gst-indent-1.0 -st <"$f" >"$tmp/1"
      .venv/bin/gst-indent-1.0 -st <"$tmp/1" >"$tmp/2"
      cmp -s "$f" "$tmp/2" || { cat "$tmp/2" >"$f"; echo "Reformatted $f"; }
    done

# Remove build/
clean:
    rm -rf build

# Install the pre-commit style check (uses .venv's gst-indent, from just setup)
install-hook:
    install -m 755 hooks/pre-commit.hook "$(git rev-parse --git-path hooks)/pre-commit"
