#!/bin/bash
#
# HeliFX Audio Setup Script
# Sets ALSA mixer levels for WM8960 Audio HAT or DigiAMP+
# 
# Usage:
#   sudo ./setup-audio-levels.sh [--card 0|NAME]

set -e

CARD="0"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --card) CARD="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--card 0|NAME]"
            echo "  --card   ALSA card number or name (default: 0)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo "[AUDIO-SETUP] $1"; }

# Detect card type
CARD_NAME=$(aplay -l 2>/dev/null | grep "^card $CARD" | grep -o '\[.*\]' | tr -d '[]' || echo "unknown")

# Set volume based on card type
if echo "$CARD_NAME" | grep -qi "wm8960"; then
    VOLUME="100%"
    log "Detected WM8960 Audio HAT - setting volume to 100%"
else
    VOLUME="75%"
    log "Detected $CARD_NAME - setting volume to 75%"
fi

# Set mixer controls
set_ctrl() {
    amixer -c "$CARD" sset "$1" "$2" unmute >/dev/null 2>&1
}

log "Setting audio levels for card $CARD..."

# Set controls
set_ctrl 'Headphone' "$VOLUME" || log "Headphone control not available"
set_ctrl 'Speaker' "$VOLUME" || log "Speaker control not available"
set_ctrl 'Playback' "$VOLUME" || log "Playback control not available"
set_ctrl 'Digital' "$VOLUME" || true

# Optional: Enable output mixers for WM8960
amixer -c "$CARD" sset 'Left Output Mixer PCM' on >/dev/null 2>&1 || true
amixer -c "$CARD" sset 'Right Output Mixer PCM' on >/dev/null 2>&1 || true

# Optional: Mute capture to reduce noise
amixer -c "$CARD" sset 'Capture' 0 mute >/dev/null 2>&1 || true

log "Audio setup complete!"
