#!/bin/sh
# Renders docs/diagrams/*.puml to SVG and checks the Mermaid blocks of the docs.
# Needs plantuml and graphviz (apt install plantuml graphviz). The Mermaid check also needs
# node and `npm i mermaid jsdom` in MERMAID_DIR (default ~/mm); it is skipped otherwise.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
plantuml -tsvg docs/diagrams/*.puml
dir="${MERMAID_DIR:-$HOME/mm}"
if [ -d "$dir/node_modules" ]; then
    cp tools/check-mermaid.mjs "$dir/"
    (cd "$dir" && node check-mermaid.mjs "$root"/docs/*.md)
fi
