#!/bin/bash
# Generate comprehensive test video files with audio tracks
# Uses FFmpeg to create videos in various formats with embedded audio
# Based on cuems-videocomposer video generation formats

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-$SCRIPT_DIR/test_video_files}"
AUDIO_SOURCE_DIR="$SCRIPT_DIR/audio"
DURATION="${2:-30}"  # Duration in seconds for test clips

cd "$SCRIPT_DIR"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "🎬 Generating comprehensive test video files with audio..."
echo "Source audio: $AUDIO_SOURCE_DIR"
echo "Output: $OUTPUT_DIR"
echo "Duration: ${DURATION}s"
echo ""

# Check for source audio files
if [ ! -d "$AUDIO_SOURCE_DIR" ] || [ -z "$(ls -A "$AUDIO_SOURCE_DIR"/*.mp3 2>/dev/null)" ]; then
    echo "❌ No source MP3 files found in $AUDIO_SOURCE_DIR"
    echo "   Please add MP3 files to the audio/ directory first"
    exit 1
fi

# Get first source audio file
SOURCE_AUDIO=$(ls "$AUDIO_SOURCE_DIR"/*.mp3 2>/dev/null | head -1)
basename_audio=$(basename "$SOURCE_AUDIO" .mp3)

echo "Using source audio: $(basename "$SOURCE_AUDIO")"
echo ""

# Function to create test video with audio
create_test_video() {
    local output="$1"
    local codec="$2"
    local extra_video_args="$3"
    local format="$4"
    local audio_file="$5"
    local sample_rate="$6"
    
    if [ -f "$output" ]; then
        echo -e "${YELLOW}⏭  Skipping (exists): $(basename "$output")${NC}"
        return 0
    fi
    
    echo -e "${GREEN}🔄 Creating: $(basename "$output") (codec: $codec, audio: ${sample_rate}Hz)${NC}"
    
    # Create video with test pattern and add audio
    # Using color bars pattern as video source
    ffmpeg_output=$(ffmpeg -y \
        -f lavfi -i "testsrc2=duration=${DURATION}:size=1920x1080:rate=25" \
        -i "$audio_file" \
        -t "$DURATION" \
        -c:v "$codec" \
        $extra_video_args \
        -c:a aac -b:a 192k -ar "$sample_rate" -ac 2 \
        -shortest \
        -f "$format" \
        "$output" -loglevel error -stats 2>&1)
    
    ffmpeg_exit_code=$?
    
    # Check for actual errors (not just progress messages)
    if [ $ffmpeg_exit_code -ne 0 ] || echo "$ffmpeg_output" | grep -qi "error\|failed\|cannot"; then
        echo -e "${YELLOW}⚠  Warning: $(basename "$output") may have issues${NC}"
        echo "$ffmpeg_output" | grep -i "error\|failed" | head -3
        if [ ! -f "$output" ]; then
            return 1
        fi
    fi
    
    if [ -f "$output" ]; then
        return 0
    else
        echo -e "${YELLOW}⚠  Failed: $(basename "$output")${NC}"
        return 1
    fi
}

# Get audio files for different sample rates
# We'll use the same source but resample to different rates
AUDIO_44100="${OUTPUT_DIR}/.temp_audio_44100.wav"
AUDIO_48000="${OUTPUT_DIR}/.temp_audio_48000.wav"

echo "📦 Preparing audio tracks..."
if [ ! -f "$AUDIO_44100" ]; then
    ffmpeg -y -i "$SOURCE_AUDIO" -t "$DURATION" -ar 44100 -ac 2 -acodec pcm_s16le "$AUDIO_44100" -loglevel error -stats
fi
if [ ! -f "$AUDIO_48000" ]; then
    ffmpeg -y -i "$SOURCE_AUDIO" -t "$DURATION" -ar 48000 -ac 2 -acodec pcm_s16le "$AUDIO_48000" -loglevel error -stats
fi

# ============================================================================
# H.264 TEST FILES (MP4, MOV, AVI) - Hardware decodable
# ============================================================================
echo ""
echo "📦 Generating H.264 test files..."

# H.264 MP4 files
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_h264.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_48000" "48000"

# H.264 MOV files
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264.mov" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "mov" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_h264.mov" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "mov" \
    "$AUDIO_48000" "48000"

# H.264 AVI files
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264.avi" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "avi" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_h264.avi" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p" "avi" \
    "$AUDIO_48000" "48000"

# ============================================================================
# HEVC TEST FILES (MP4, MKV) - Hardware decodable
# ============================================================================
echo ""
echo "📦 Generating HEVC test files..."

# HEVC MP4 files
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_hevc.mp4" \
    "libx265" "-preset fast -crf 23 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_44100" "44100" || echo "  ⚠ HEVC encoder may not be available"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_hevc.mp4" \
    "libx265" "-preset fast -crf 23 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_48000" "48000" || echo "  ⚠ HEVC encoder may not be available"

# HEVC MKV files
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_hevc.mkv" \
    "libx265" "-preset fast -crf 23 -pix_fmt yuv420p" "matroska" \
    "$AUDIO_44100" "44100" || echo "  ⚠ HEVC encoder may not be available"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_hevc.mkv" \
    "libx265" "-preset fast -crf 23 -pix_fmt yuv420p" "matroska" \
    "$AUDIO_48000" "48000" || echo "  ⚠ HEVC encoder may not be available"

# ============================================================================
# AV1 TEST FILES (MP4) - Hardware decodable on newer hardware
# ============================================================================
echo ""
echo "📦 Generating AV1 test files..."

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_av1.mp4" \
    "libaom-av1" "-cpu-used 4 -crf 30 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_44100" "44100" || \
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_av1.mp4" \
    "libsvtav1" "-preset 4 -crf 30 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_44100" "44100" || \
    echo "  ⚠ AV1 encoder not available, skipping"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_av1.mp4" \
    "libaom-av1" "-cpu-used 4 -crf 30 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_48000" "48000" || \
create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_av1.mp4" \
    "libsvtav1" "-preset 4 -crf 30 -pix_fmt yuv420p" "mp4" \
    "$AUDIO_48000" "48000" || \
    echo "  ⚠ AV1 encoder not available, skipping"

# ============================================================================
# HAP TEST FILES (MOV) - GPU-optimized codec
# ============================================================================
echo ""
echo "📦 Generating HAP test files..."

# HAP (8-bit)
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_hap.mov" \
    "hap" "-format hap -pix_fmt yuv420p" "mov" \
    "$AUDIO_44100" "44100" || \
    echo "  ⚠ HAP encoder not available, skipping"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_hap.mov" \
    "hap" "-format hap -pix_fmt yuv420p" "mov" \
    "$AUDIO_48000" "48000" || \
    echo "  ⚠ HAP encoder not available, skipping"

# HAP Q (10-bit)
create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_hapq.mov" \
    "hap" "-format hapq -pix_fmt yuv422p10le" "mov" \
    "$AUDIO_44100" "44100" || \
    echo "  ⚠ HAP Q encoder not available, skipping"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_hapq.mov" \
    "hap" "-format hapq -pix_fmt yuv422p10le" "mov" \
    "$AUDIO_48000" "48000" || \
    echo "  ⚠ HAP Q encoder not available, skipping"

# ============================================================================
# MPEG-4 TEST FILES (AVI) - Software codec
# ============================================================================
echo ""
echo "📦 Generating MPEG-4 test files (software codec)..."

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_mpeg4.avi" \
    "mpeg4" "-qscale:v 3 -pix_fmt yuv420p" "avi" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_48000_mpeg4.avi" \
    "mpeg4" "-qscale:v 3 -pix_fmt yuv420p" "avi" \
    "$AUDIO_48000" "48000"

# ============================================================================
# Different frame rates (H.264)
# ============================================================================
echo ""
echo "📦 Generating different frame rate test files..."

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264_24fps.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p -r 24" "mp4" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264_30fps.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p -r 30" "mp4" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264_50fps.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p -r 50" "mp4" \
    "$AUDIO_44100" "44100"

# ============================================================================
# Different resolutions (H.264)
# ============================================================================
echo ""
echo "📦 Generating different resolution test files..."

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264_720p.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p -vf scale=1280:720" "mp4" \
    "$AUDIO_44100" "44100"

create_test_video "${OUTPUT_DIR}/${basename_audio}_44100_h264_480p.mp4" \
    "libx264" "-preset fast -crf 23 -pix_fmt yuv420p -vf scale=854:480" "mp4" \
    "$AUDIO_44100" "44100"

# Clean up temporary audio files
echo ""
echo "🧹 Cleaning up temporary files..."
rm -f "$AUDIO_44100" "$AUDIO_48000"

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "✅ Test video file generation complete!"
echo ""
echo "Generated files:"
ls -lh "$OUTPUT_DIR"/*.{mp4,mov,avi,mkv} 2>/dev/null | awk '{print "  " $9, "(" $5 ")"}' || echo "  (no video files found)"
echo ""
echo "Formats generated:"
echo "  • H.264: MP4, MOV, AVI (44.1kHz and 48kHz)"
echo "  • HEVC: MP4, MKV (44.1kHz and 48kHz)"
echo "  • AV1: MP4 (44.1kHz and 48kHz) [if encoder available]"
echo "  • HAP: MOV (44.1kHz and 48kHz) [if encoder available]"
echo "  • HAP Q: MOV (44.1kHz and 48kHz) [if encoder available]"
echo "  • MPEG-4: AVI (44.1kHz and 48kHz)"
echo "  • H.264 variants: 24fps, 30fps, 50fps, 720p, 480p"
echo ""
echo "All videos include audio tracks at the specified sample rates."
echo ""

