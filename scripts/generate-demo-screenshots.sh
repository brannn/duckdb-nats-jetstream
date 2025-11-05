#!/bin/bash
# Generate screenshots of demo pages for README
# Requires: Chrome/Chromium browser

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"

# Create images directory if it doesn't exist
mkdir -p "$IMAGES_DIR"

echo "Generating demo screenshots..."

# Check if Chrome is available
if command -v google-chrome &> /dev/null; then
    CHROME="google-chrome"
elif command -v chromium &> /dev/null; then
    CHROME="chromium"
elif command -v chromium-browser &> /dev/null; then
    CHROME="chromium-browser"
elif [ -f "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ]; then
    CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
else
    echo "Error: Chrome/Chromium not found. Please install Chrome or Chromium."
    exit 1
fi

# Generate JSON demo screenshot
echo "Capturing JSON demo..."
"$CHROME" --headless --screenshot="$IMAGES_DIR/demo-json.png" \
    --window-size=1400,1000 \
    --default-background-color=0 \
    "file://$PROJECT_ROOT/demo.html"

# Generate Protobuf demo screenshot
echo "Capturing Protobuf demo..."
"$CHROME" --headless --screenshot="$IMAGES_DIR/demo-protobuf.png" \
    --window-size=1400,1000 \
    --default-background-color=0 \
    "file://$PROJECT_ROOT/demo-protobuf.html"

echo "Screenshots generated successfully!"
echo "  - $IMAGES_DIR/demo-json.png"
echo "  - $IMAGES_DIR/demo-protobuf.png"

