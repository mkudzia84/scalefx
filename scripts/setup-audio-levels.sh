#!/bin/bash
#
# HeliFX Audio Setup Script
# Sets ALSA mixer levels for WM8960 Audio HAT or DigiAMP+
# Automatically detects and configures all compatible audio cards
# 
# Usage:
#   sudo ./setup-audio-levels.sh [--verbose]

set -e

VERBOSE=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose|-v) VERBOSE=1; shift ;;
        --help|-h)
            echo "Usage: $0 [--verbose]"
            echo "  --verbose  Show detailed output"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo "[AUDIO-SETUP] $1"; }
log_verbose() { [ $VERBOSE -eq 1 ] && echo "[AUDIO-SETUP] $1"; }

log "HeliFX Audio Level Setup"

# Function to configure a specific card
configure_card() {
    local CARD="$1"
    local CARD_NAME="$2"
    
    log_verbose "Configuring card $CARD: $CARD_NAME"
    
    # Set volume based on card type
    local VOLUME
    if echo "$CARD_NAME" | grep -qi "wm8960"; then
        VOLUME="100%"
        log "  Card $CARD ($CARD_NAME): WM8960 detected - setting to 100%"
    elif echo "$CARD_NAME" | grep -qi "digiamp"; then
        VOLUME="75%"
        log "  Card $CARD ($CARD_NAME): DigiAMP+ detected - setting to 75%"
    else
        log_verbose "  Card $CARD ($CARD_NAME): Unknown type - skipping"
        return 1
    fi
    
    # Set controls using amixer
    amixer -c "$CARD" sset 'Headphone' "$VOLUME" unmute >/dev/null 2>&1 || log_verbose "    Headphone control not available"
    amixer -c "$CARD" sset 'Speaker' "$VOLUME" unmute >/dev/null 2>&1 || log_verbose "    Speaker control not available"
    amixer -c "$CARD" sset 'Playback' "$VOLUME" unmute >/dev/null 2>&1 || log_verbose "    Playback control not available"
    amixer -c "$CARD" sset 'Digital' "$VOLUME" unmute >/dev/null 2>&1 || log_verbose "    Digital control not available"
    
    # Optional: Enable output mixers for WM8960
    if echo "$CARD_NAME" | grep -qi "wm8960"; then
        amixer -c "$CARD" sset 'Left Output Mixer PCM' on >/dev/null 2>&1 || true
        amixer -c "$CARD" sset 'Right Output Mixer PCM' on >/dev/null 2>&1 || true
        log_verbose "    Enabled output mixers"
    fi
    
    # Optional: Mute capture to reduce noise
    amixer -c "$CARD" sset 'Capture' 0 mute >/dev/null 2>&1 || true
    
    log "  Card $CARD configured successfully"
    return 0
}

# Auto-detect and configure all compatible cards
log "Auto-detecting audio cards..."

CONFIGURED=0

# Iterate over all sound cards
while IFS= read -r line; do
    if [[ $line =~ ^card\ ([0-9]+):.*\[(.*)\] ]]; then
        CARD_NUM="${BASH_REMATCH[1]}"
        CARD_NAME="${BASH_REMATCH[2]}"
        
        log_verbose "Found card $CARD_NUM: $CARD_NAME"
        
        # Try to configure this card
        if configure_card "$CARD_NUM" "$CARD_NAME"; then
            CONFIGURED=$((CONFIGURED + 1))
        fi
    fi
done < <(aplay -l 2>/dev/null)

if [ $CONFIGURED -eq 0 ]; then
    log "Warning: No compatible audio cards found (WM8960, DigiAMP+)"
    exit 1
fi

log "Configured $CONFIGURED audio card(s)"

log "Audio setup complete!"
