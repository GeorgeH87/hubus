#!/bin/sh
# Renders and checks the documentation (docs/development.md, section "Documentation"):
#   - every PlantUML timing diagram (docs/diagrams/*.puml) renders, and its SVG next to it is up to date,
#   - every Mermaid diagram in the Markdown files renders,
#   - every relative link and #anchor in the Markdown files resolves.
# Usage: sh tools/build-docs.sh            render the SVGs and check everything
#        sh tools/build-docs.sh --check    do not write the SVGs; fail if one is out of date (CI)
# Needs the tools of `sh tools/setup-docs.sh` (in $HUBUS_DOCS_TOOLS, default ~/.local/share/hubus-docs).
set -eu
cd "$(dirname "$0")/.."

tools="${HUBUS_DOCS_TOOLS:-$HOME/.local/share/hubus-docs}"
check=0
[ "${1:-}" = "--check" ] && check=1
fail=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== PlantUML timing diagrams"
mkdir -p "$tmp/puml"
java -jar "$tools/plantuml.jar" -tsvg -nometadata -failfast2 -o "$tmp/puml" docs/diagrams/*.puml
for puml in docs/diagrams/*.puml; do
    name="$(basename "$puml" .puml)"
    if [ "$check" = 1 ]; then
        if ! cmp -s "$tmp/puml/$name.svg" "docs/diagrams/$name.svg"; then
            echo "out of date: docs/diagrams/$name.svg (run sh tools/build-docs.sh)"
            fail=1
        fi
    else
        cp "$tmp/puml/$name.svg" "docs/diagrams/$name.svg"
    fi
    echo "ok: $puml"
done
for svg in docs/diagrams/*.svg; do
    [ -f "${svg%.svg}.puml" ] || { echo "no source for $svg"; fail=1; }
done

echo "== Mermaid diagrams"
mkdir -p "$tmp/mmd"
for md in $(git ls-files --cached --others --exclude-standard '*.md'); do
    [ -f "$md" ] && grep -q '^```mermaid' "$md" || continue
    n="$(grep -c '^```mermaid' "$md")"
    out="$tmp/mmd/$(echo "$md" | tr '/' '_')"
    if "$tools/node_modules/.bin/mmdc" -q -p tools/puppeteer.json -i "$md" -o "$out" >"$tmp/mmdc.log" 2>&1; then
        echo "ok: $md ($n)"
    else
        echo "FAILED: $md"
        cat "$tmp/mmdc.log"
        fail=1
    fi
done

echo "== Links"
python3 tools/check-docs-links.py || fail=1

[ "$fail" = 0 ] && echo "documentation ok"
exit "$fail"
