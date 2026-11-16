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

# Check gst/*.c against GStreamer's style with GNU indent, as the pre-commit hook does
indent:
    #!/bin/sh
    for i in gnuindent gindent indent; do
      $i --version 2>/dev/null | grep -q GNU && INDENT=$i && break
    done
    [ -n "$INDENT" ] || { echo "GNU indent not found: brew install gnu-indent, or apt install indent" >&2; exit 1; }
    opts="--braces-on-if-line --case-brace-indentation0 --case-indentation2
      --braces-after-struct-decl-line --line-length80 --no-tabs --cuddle-else
      --dont-line-up-parentheses --continuation-indentation4 --honour-newlines
      --tab-size8 --indent-level2 --leave-preprocessor-space"
    tmp=$(mktemp) && trap 'rm -f "$tmp"' EXIT
    status=0
    for f in gst/*.c; do
      # indent twice: it breaks lines differently on every second run
      $INDENT $opts "$f" -o "$tmp" 2>/dev/null && $INDENT $opts "$tmp" 2>/dev/null
      diff -u -p "$f" "$tmp" || status=1
    done
    [ $status = 0 ] && echo "Style OK" || echo "Style errors: fix them with gst-indent <file>" >&2
    exit $status

# Remove build/
clean:
    rm -rf build

# Install the pre-commit style check (needs GNU indent)
install-hook:
    install -m 755 hooks/pre-commit.hook "$(git rev-parse --git-path hooks)/pre-commit"
