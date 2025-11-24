#!/bin/bash
# Generate comprehensive test audio files from source MP3s
# Converts to all FFmpeg-supported formats with various bitrates/frequencies

set -e

SOURCE_DIR="audio"
OUTPUT_DIR="test_audio_files"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "🎵 Generating comprehensive test audio files..."
echo "Source: $SOURCE_DIR"
echo "Output: $OUTPUT_DIR"
echo ""

# Function to convert with progress
convert_file() {
    local input="$1"
    local output="$2"
    local extra_args="$3"
    
    if [ -f "$output" ]; then
        echo -e "${YELLOW}⏭  Skipping (exists): $(basename "$output")${NC}"
        return 0
    fi
    
    echo -e "${GREEN}🔄 Converting: $(basename "$output")${NC}"
    ffmpeg -i "$input" $extra_args -y "$output" -loglevel error -stats || {
        echo -e "${YELLOW}⚠  Failed: $(basename "$output")${NC}"
        return 1
    }
}

# Get all source files into an array
mapfile -t SOURCE_FILES < <(ls "$SOURCE_DIR"/*.mp3 2>/dev/null | sort)

if [ ${#SOURCE_FILES[@]} -eq 0 ]; then
    echo "❌ No source MP3 files found in $SOURCE_DIR"
    exit 1
fi

echo "Found ${#SOURCE_FILES[@]} source file(s)"
echo ""

# File index counter (rotates through source files - advances only when changing format)
FILE_INDEX=0

# ============================================================================
# WAV FILES - All bit depth and sample rate combinations
# ============================================================================
echo "📦 Generating WAV files..."

# Get source file for this format (all WAV files use the same source)
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

for sample_rate in 44100 48000 88200 96000; do
    for bit_depth in 16 24 32; do
        output="${OUTPUT_DIR}/${basename_src}_${sample_rate}_${bit_depth}bit.wav"
        convert_file "$source_file" "$output" \
            "-ar $sample_rate -acodec pcm_s${bit_depth}le -ac 2"
    done
done

# ============================================================================
# AIFF FILES - All bit depth and sample rate combinations
# ============================================================================
echo ""
echo "📦 Generating AIFF files..."

# Get source file for this format (all AIFF files use the same source)
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

for sample_rate in 44100 48000 88200 96000; do
    for bit_depth in 16 24 32; do
        output="${OUTPUT_DIR}/${basename_src}_${sample_rate}_${bit_depth}bit.aiff"
        convert_file "$source_file" "$output" \
            "-ar $sample_rate -acodec pcm_s${bit_depth}be -ac 2 -f aiff"
    done
done

# ============================================================================
# MP3 FILES - All bitrate and sample rate combinations
# ============================================================================
echo ""
echo "📦 Generating MP3 files..."

# Get source file for this format (all MP3 files use the same source)
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

for sample_rate in 44100 48000; do
    # CBR (Constant Bitrate)
    for bitrate in 128k 192k 256k 320k; do
        output="${OUTPUT_DIR}/${basename_src}_${sample_rate}_${bitrate}.mp3"
        convert_file "$source_file" "$output" \
            "-ar $sample_rate -acodec libmp3lame -b:a $bitrate -ac 2"
    done
    
    # VBR (Variable Bitrate) - quality levels
    for vbr_quality in 0 2 4; do
        output="${OUTPUT_DIR}/${basename_src}_${sample_rate}_vbr${vbr_quality}.mp3"
        convert_file "$source_file" "$output" \
            "-ar $sample_rate -acodec libmp3lame -q:a $vbr_quality -ac 2"
    done
done

# ============================================================================
# FLAC FILES - Lossless compression (one representative file)
# ============================================================================
echo ""
echo "📦 Generating FLAC files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one FLAC file (44.1kHz, compression 5)
output="${OUTPUT_DIR}/${basename_src}_44100_flac5.flac"
convert_file "$source_file" "$output" \
    "-ar 44100 -acodec flac -compression_level 5 -ac 2"

# ============================================================================
# OGG VORBIS FILES - One representative file
# ============================================================================
echo ""
echo "📦 Generating OGG Vorbis files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one OGG file (44.1kHz, quality 5)
output="${OUTPUT_DIR}/${basename_src}_44100_ogg_q5.ogg"
convert_file "$source_file" "$output" \
    "-ar 44100 -acodec libvorbis -q:a 5 -ac 2"

# ============================================================================
# OPUS FILES - One representative file
# ============================================================================
echo ""
echo "📦 Generating OPUS files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one OPUS file (48kHz, 128k) - OPUS only supports 8k, 12k, 16k, 24k, 48k
output="${OUTPUT_DIR}/${basename_src}_48000_opus_128k.opus"
convert_file "$source_file" "$output" \
    "-ar 48000 -acodec libopus -b:a 128k -ac 2"

# ============================================================================
# AAC/M4A FILES - One representative file
# ============================================================================
echo ""
echo "📦 Generating AAC/M4A files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one AAC/M4A file (44.1kHz, 192k)
output="${OUTPUT_DIR}/${basename_src}_44100_aac_192k.m4a"
convert_file "$source_file" "$output" \
    "-ar 44100 -acodec aac -b:a 192k -ac 2"

# ============================================================================
# AC3 FILES - One representative file
# ============================================================================
echo ""
echo "📦 Generating AC3 files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one AC3 file (44.1kHz, 256k)
output="${OUTPUT_DIR}/${basename_src}_44100_ac3_256k.ac3"
convert_file "$source_file" "$output" \
    "-ar 44100 -acodec ac3 -b:a 256k -ac 2"

# ============================================================================
# ALAC FILES - One representative file
# ============================================================================
echo ""
echo "📦 Generating ALAC files..."

# Get source file for this format
source_file="${SOURCE_FILES[$FILE_INDEX]}"
FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
basename_src=$(basename "$source_file" .mp3)

# Generate one ALAC file (44.1kHz)
output="${OUTPUT_DIR}/${basename_src}_44100_alac.m4a"
convert_file "$source_file" "$output" \
    "-ar 44100 -acodec alac -ac 2"

# ============================================================================
# WMA FILES - Windows Media Audio (if available)
# ============================================================================
echo ""
echo "📦 Generating WMA files..."

if ffmpeg -encoders 2>/dev/null | grep -q "wmav2\|wmapro"; then
    # Get source file for this format
    source_file="${SOURCE_FILES[$FILE_INDEX]}"
    FILE_INDEX=$(( (FILE_INDEX + 1) % ${#SOURCE_FILES[@]} ))
    basename_src=$(basename "$source_file" .mp3)
    
    # Generate one WMA file (44.1kHz, 192k)
    output="${OUTPUT_DIR}/${basename_src}_44100_wma_192k.wma"
    convert_file "$source_file" "$output" \
        "-ar 44100 -acodec wmav2 -b:a 192k -ac 2" || true
else
    echo "⚠  WMA encoder not available, skipping..."
fi

# ============================================================================
# Generate files from ALL source files (simpler versions)
# ============================================================================
echo ""
echo "📦 Generating standard format files from all source MP3s..."

for source_file in "$SOURCE_DIR"/*.mp3; do
    if [ ! -f "$source_file" ]; then
        continue
    fi
    
    basename_src=$(basename "$source_file" .mp3)
    
    # One representative file per format per source
    echo "  Processing: $(basename "$source_file")"
    
    # Standard WAV (44.1kHz, 16-bit)
    convert_file "$source_file" "${OUTPUT_DIR}/${basename_src}_44100_16bit.wav" \
        "-ar 44100 -acodec pcm_s16le -ac 2" || true
    
    # Standard MP3 (44.1kHz, 192k)
    convert_file "$source_file" "${OUTPUT_DIR}/${basename_src}_44100_192k.mp3" \
        "-ar 44100 -acodec libmp3lame -b:a 192k -ac 2" || true
    
    # Standard FLAC (44.1kHz, compression 5)
    convert_file "$source_file" "${OUTPUT_DIR}/${basename_src}_44100_flac5.flac" \
        "-ar 44100 -acodec flac -compression_level 5 -ac 2" || true
    
    # Standard OGG (44.1kHz, quality 5)
    convert_file "$source_file" "${OUTPUT_DIR}/${basename_src}_44100_ogg_q5.ogg" \
        "-ar 44100 -acodec libvorbis -q:a 5 -ac 2" || true
done

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "✅ Test file generation complete!"
echo ""
echo "Generated files:"
ls -lh "$OUTPUT_DIR" | tail -n +2 | wc -l | xargs echo "  Total files:"
echo ""
echo "Formats generated:"
echo "  • WAV: 16 combinations (4 sample rates × 4 bit depths)"
echo "  • AIFF: 16 combinations (4 sample rates × 4 bit depths)"
echo "  • MP3: 14 combinations (2 sample rates × 7 bitrate/quality options)"
echo "  • FLAC: 1 file (44.1kHz, compression 5)"
echo "  • OGG: 1 file (44.1kHz, quality 5)"
echo "  • OPUS: 1 file (48kHz, 128k) - OPUS only supports 8k, 12k, 16k, 24k, 48k"
echo "  • AAC/M4A: 1 file (44.1kHz, 192k)"
echo "  • AC3: 1 file (44.1kHz, 256k)"
echo "  • ALAC: 1 file (44.1kHz)"
echo "  • WMA: 1 file (44.1kHz, 192k) [if available]"
echo "  • Plus standard versions from all source files"
echo ""
echo "Output directory: $OUTPUT_DIR"

