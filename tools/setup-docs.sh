#!/bin/sh
# Installs the documentation tools in fixed versions: PlantUML (timing diagrams) and mermaid-cli (renders the
# Mermaid diagrams to check them). Run as the normal user after `sudo sh tools/setup-ubuntu.sh`, which installs Java,
# Node.js, and the libraries of the headless browser that mermaid-cli uses.
# Usage: sh tools/setup-docs.sh        The tools go to $HUBUS_DOCS_TOOLS (default: ~/.local/share/hubus-docs).
set -eu

PLANTUML_VERSION=1.2026.9
MERMAID_CLI_VERSION=12.0.0

dir="${HUBUS_DOCS_TOOLS:-$HOME/.local/share/hubus-docs}"
mkdir -p "$dir"

jar="$dir/plantuml-$PLANTUML_VERSION.jar"
if [ ! -f "$jar" ]; then
    curl -fsSL -o "$jar.tmp" \
        "https://github.com/plantuml/plantuml/releases/download/v$PLANTUML_VERSION/plantuml-$PLANTUML_VERSION.jar"
    mv "$jar.tmp" "$jar"
fi
ln -sf "$jar" "$dir/plantuml.jar"

if [ ! -x "$dir/node_modules/.bin/mmdc" ] ||
    [ "$(cd "$dir" && npm ls --depth=0 @mermaid-js/mermaid-cli 2>/dev/null | grep -c "@$MERMAID_CLI_VERSION")" = 0 ]; then
    (cd "$dir" && npm install --no-audit --no-fund --silent "@mermaid-js/mermaid-cli@$MERMAID_CLI_VERSION")
fi

java -jar "$dir/plantuml.jar" -version | head -1
"$dir/node_modules/.bin/mmdc" --version
