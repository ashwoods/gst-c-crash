"""MkDocs hook: say where each example snippet lives and how it is tested.

A fenced block that includes `--8<-- "examples/<path>:<section>"` gets one
muted line after it per example file:

    Source: examples/python/run_pipeline.py · tested by test_run_pipeline_bad_description

The path links to the section's lines on GitHub, each test name to its `def`.
Tests say which sections they cover with a marker (see tests/conftest.py):

    @pytest.mark.covers("run_pipeline:build", "run_pipeline:retry")

A key is the example's name with "-" turned into "_" (run_pipeline.py,
run-pipeline.c and run-pipeline.rs are all `run_pipeline`), a colon and the
section name, so one marker covers a section in every language.

The build warns, which `mkdocs build --strict` turns into a failure, when a
section the docs include has no covering test, when a marker names a section
no example has, or when an included section does not exist.
"""
import ast
import logging
import re
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.source_lines")

OPEN = re.compile(r"^(\s*)(`{3,}|~{3,})")
INCLUDE = re.compile(r'^\s*--8<--\s+"(examples/[^":]+):([^"]+)"\s*$')
START = re.compile(r"--8<--\s*\[start:([^\]]+)\]")
END = re.compile(r"--8<--\s*\[end:([^\]]+)\]")

# set by on_config
root = Path()
blob = ""
tests = {}     # "run_pipeline:build" -> [(test name, tests/test_x.py, line)]
sections = {}  # "run_pipeline:build" -> True, for every section any example has


def key(path, section):
    return f"{Path(path).stem.replace('-', '_')}:{section}"


def section_lines(path):
    """Return {section: (first, last)}: the lines between its markers."""
    starts, found = {}, {}
    for number, line in enumerate(
            (root / path).read_text().splitlines(), start=1):
        if m := START.search(line):
            starts[m[1]] = number + 1
        elif (m := END.search(line)) and m[1] in starts:
            found[m[1]] = (starts[m[1]], number - 1)
    return found


def markers(path):
    """Yield (key, test name, line of its def) for each covers marker."""
    for node in ast.walk(ast.parse(path.read_text())):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for deco in node.decorator_list:
            if (isinstance(deco, ast.Call)
                    and isinstance(deco.func, ast.Attribute)
                    and deco.func.attr == "covers"):
                for arg in deco.args:
                    yield ast.literal_eval(arg), node.name, node.lineno


def on_config(config):
    global root, blob, tests, sections
    root = Path(config.config_file_path).parent
    blob = f"{config.repo_url.rstrip('/')}/blob/master"

    sections = {}
    for path in sorted((root / "examples").rglob("*")):
        if "target" in path.relative_to(root).parts or not path.is_file():
            continue
        if path.suffix in (".py", ".c", ".rs"):
            rel = path.relative_to(root).as_posix()
            for name in section_lines(rel):
                sections[key(rel, name)] = True

    tests = {}
    for path in sorted((root / "tests").glob("test_*.py")):
        rel = path.relative_to(root).as_posix()
        for covered, name, line in markers(path):
            if covered not in sections:
                log.warning(
                    "%s:%d: %s covers %r, but no example has that section "
                    "(expected example_name:section, as in run_pipeline:build)",
                    rel, line, name, covered)
            tests.setdefault(covered, []).append((name, rel, line))
    return config


def source_line(path, used, indent):
    """Return the markdown paragraph for one example file and the sections used."""
    spans = section_lines(path)
    first = last = None
    names = []
    for section in used:
        if section not in spans:
            log.warning("%s has no section %r (snippet in the docs)",
                        path, section)
            continue
        first = min(spans[section][0], first or spans[section][0])
        last = max(spans[section][1], last or spans[section][1])
        covering = tests.get(key(path, section), [])
        if not covering:
            log.warning(
                "%s:%s is included in the docs but no test covers it: add "
                '@pytest.mark.covers("%s") to the tests that exercise it',
                path, section, key(path, section))
        for test in covering:
            if test not in names:
                names.append(test)

    # two sections of one file (crashes.md's handler and child): one link,
    # spanning from the first section's start to the last one's end
    url = f"{blob}/{path}" + (f"#L{first}-L{last}" if first else "")
    text = f"Source: [{path}]({url})"
    if names:
        links = ", ".join(f"[{name}]({blob}/{file}#L{line})"
                          for name, file, line in names)
        text += f" · tested by {links}"
    # attr_list gives the paragraph its class; raw HTML would not survive
    # the indentation of a tab
    return text + "\n" + indent + "{: .source-line }"


def on_page_markdown(markdown, page, config, files):
    out, fence, used = [], None, {}
    for line in markdown.split("\n"):
        out.append(line)
        if fence is None:
            if m := OPEN.match(line):
                fence, used = (m[1], m[2][0], len(m[2])), {}
            continue
        indent, char, size = fence
        if m := INCLUDE.match(line):
            used.setdefault(m[1], []).append(m[2])
        elif re.match(rf"^\s*{re.escape(char)}{{{size},}}\s*$", line):
            fence = None
            for path, names in used.items():
                out += ["", indent + source_line(path, names, indent)]
            if used:
                out.append("")
    return "\n".join(out)
