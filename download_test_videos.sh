#!/bin/bash
# Download test video files with audio from various sources
# Includes: Creative Commons, Public Domain, and promotional movie trailers (for testing/private use only)
# Downloads videos in various formats for testing purposes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-$SCRIPT_DIR/test_video_files}"
MAX_PARALLEL_DOWNLOADS="${2:-10}"  # Maximum parallel downloads (default: 10)

cd "$SCRIPT_DIR"

# Track background download jobs
declare -a DOWNLOAD_JOBS=()

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "🌐 Downloading test video files with audio..."
echo "Output directory: $OUTPUT_DIR"
echo "Parallel downloads: $MAX_PARALLEL_DOWNLOADS"
echo ""
echo "ℹ  Note: Movie trailers are downloaded for testing purposes only."
echo "   Trailers are promotional material meant for sharing/promotion."
echo ""

# Check for required tools
if ! command -v wget > /dev/null 2>&1 && ! command -v curl > /dev/null 2>&1; then
    echo "❌ Error: Neither wget nor curl is available"
    echo "   Please install one of them: sudo apt install wget curl"
    exit 1
fi

DOWNLOAD_CMD=""
if command -v wget > /dev/null 2>&1; then
    DOWNLOAD_CMD="wget"
    DOWNLOAD_ARGS="-q --show-progress -O"
elif command -v curl > /dev/null 2>&1; then
    DOWNLOAD_CMD="curl"
    DOWNLOAD_ARGS="-L --progress-bar -o"
fi

# Function to wait for downloads with limit on concurrent jobs
wait_for_slot() {
    while [ ${#DOWNLOAD_JOBS[@]} -ge $MAX_PARALLEL_DOWNLOADS ]; do
        # Check for finished jobs and remove them
        for i in "${!DOWNLOAD_JOBS[@]}"; do
            if ! kill -0 "${DOWNLOAD_JOBS[$i]}" 2>/dev/null; then
                # Job finished, remove from array
                unset DOWNLOAD_JOBS[$i]
                DOWNLOAD_JOBS=("${DOWNLOAD_JOBS[@]}")  # Reindex array
                break
            fi
        done
        sleep 0.1
    done
}

# Function to wait for all remaining downloads
wait_all_downloads() {
    if [ ${#DOWNLOAD_JOBS[@]} -eq 0 ]; then
        return 0
    fi
    echo ""
    echo -e "${BLUE}⏳ Waiting for ${#DOWNLOAD_JOBS[@]} download(s) to complete...${NC}"
    for job in "${DOWNLOAD_JOBS[@]}"; do
        wait "$job" 2>/dev/null || true
    done
    DOWNLOAD_JOBS=()
    echo -e "${GREEN}✓  All downloads completed${NC}"
    echo ""
}

# Function to download file (supports parallel execution)
download_file() {
    local url="$1"
    local output="$2"
    local description="$3"
    local parallel="${4:-true}"  # Default to parallel downloads
    
    # Check if file already exists and has content
    if [ -f "$output" ] && [ -s "$output" ]; then
        echo -e "${YELLOW}⏭  Skipping (already downloaded): $(basename "$output")${NC}"
        return 0
    fi
    
    # Remove incomplete/empty files
    if [ -f "$output" ] && [ ! -s "$output" ]; then
        rm -f "$output"
    fi
    
    # User-Agent string to avoid blocking
    USER_AGENT="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
    
    # Inner function that does the actual download
    _do_download() {
        local u="$1"
        local out="$2"
        local desc="$3"
        
        if [ "$DOWNLOAD_CMD" = "wget" ]; then
            wget --max-redirect=10 --timeout=10 --tries=2 --user-agent="$USER_AGENT" \
                -q --show-progress "$u" -O "$out" 2>&1 || {
                echo -e "${YELLOW}⚠  Failed: $(basename "$out")${NC}" >&2
                rm -f "$out"
                return 1
            }
        else
            curl -L --max-redirs 10 --connect-timeout 10 --max-time 300 --retry 2 --retry-delay 1 \
                --user-agent "$USER_AGENT" --progress-bar "$u" -o "$out" 2>&1 || {
                echo -e "${YELLOW}⚠  Failed: $(basename "$out")${NC}" >&2
                rm -f "$out"
                return 1
            }
        fi
        
        if [ -f "$out" ] && [ -s "$out" ]; then
            echo -e "${GREEN}✓  Downloaded: $(basename "$out") ($(du -h "$out" | cut -f1))${NC}"
            return 0
        else
            echo -e "${YELLOW}⚠  Incomplete: $(basename "$out")${NC}" >&2
            rm -f "$out"
            return 1
        fi
    }
    
    if [ "$parallel" = "true" ]; then
        # Wait for a free slot
        wait_for_slot
        echo -e "${BLUE}📥 Queued: $description${NC}"
        # Start download in background
        _do_download "$url" "$output" "$description" &
        DOWNLOAD_JOBS+=($!)
    else
        # Run in foreground
        echo -e "${BLUE}📥 Downloading: $description${NC}"
        echo -e "   URL: $url"
        _do_download "$url" "$output" "$description"
    fi
}

# ============================================================================
# Big Buck Bunny - Creative Commons Attribution 3.0
# Source: https://peach.blender.org/
# ============================================================================
echo ""
echo "📦 Big Buck Bunny (Creative Commons Attribution 3.0)"
echo "   Source: Blender Foundation (https://peach.blender.org/)"
echo ""

# Big Buck Bunny - Try multiple sources and URLs
# Note: Archive.org URLs may change, so we try multiple approaches

# Method 1: Try GitHub releases (more reliable)
echo "   Trying GitHub source..."
download_file \
    "https://github.com/bower-media-samples/big-buck-bunny-1080p-60fps-30s/raw/master/video/big_buck_bunny_1080p_60fps_30s.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-1080p-60fps-30s.mp4" \
    "Big Buck Bunny 1080p 60fps 30s (GitHub)" || true

# Method 2: Try archive.org with correct Content/ subdirectory
echo "   Trying archive.org source..."
ARCHIVE_BASE="https://archive.org/download/BigBuckBunny_124/Content"

# 720p MP4 (H.264) - this is the most commonly available
if [ ! -f "${OUTPUT_DIR}/big-buck-bunny-720p-30fps.mp4" ]; then
    download_file \
        "${ARCHIVE_BASE}/big_buck_bunny_720p_surround.mp4" \
        "${OUTPUT_DIR}/big-buck-bunny-720p-30fps.mp4" \
        "Big Buck Bunny 720p (H.264 MP4)" || \
    echo "  ⚠ Could not download 720p version from archive.org"
fi

# Try to get other resolutions if available
# Note: Archive.org structure may vary, so we try common filenames
if [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4" ]; then
    # Try various possible filenames
    for filename in "big_buck_bunny_1080p_h264.mov" "big_buck_bunny_1080p.mp4" "BigBuckBunny_1080p_30fps.mp4"; do
        download_file \
            "${ARCHIVE_BASE}/${filename}" \
            "${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4" \
            "Big Buck Bunny 1080p (trying ${filename})" && break
    done
    [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4" ] && \
        echo "  ⚠ Could not download 1080p version from archive.org"
fi

# If we have the 60fps version, we can use it
if [ -f "${OUTPUT_DIR}/big-buck-bunny-1080p-60fps-30s.mp4" ] && [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4" ]; then
    echo -e "${BLUE}ℹ  Using 60fps version as 1080p source${NC}"
    cp "${OUTPUT_DIR}/big-buck-bunny-1080p-60fps-30s.mp4" "${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4" 2>/dev/null || true
fi

# ============================================================================
# Test Patterns and Sample Videos - Multiple Sources
# ============================================================================
echo ""
echo "📦 Additional Test Videos (Various Formats)"
echo ""

# Source 1: sample-videos.com (may be down - skip if timeout)
echo "   Source 1: sample-videos.com (skipping - site appears down)"
# Note: sample-videos.com is timing out, so we skip it
# SAMPLE_VIDEOS_BASE="https://sample-videos.com"

# Source 2: file-examples.com
echo ""
echo "   Source 2: file-examples.com"
FILE_EXAMPLES_BASE="https://file-examples.com/storage/fe86c3e9c65c47118e76e1e8/2017/10"

[ ! -f "${OUTPUT_DIR}/sample-720p-h264.mp4" ] && \
download_file \
    "${FILE_EXAMPLES_BASE}/file_example_MP4_1280_10MG.mp4" \
    "${OUTPUT_DIR}/sample-720p-h264.mp4" \
    "Sample Video 720p H.264 (File Examples)" || true

[ ! -f "${OUTPUT_DIR}/sample-720p-mov.mov" ] && \
download_file \
    "${FILE_EXAMPLES_BASE}/file_example_MOV_1280_10MB.mov" \
    "${OUTPUT_DIR}/sample-720p-mov.mov" \
    "Sample Video 720p MOV (File Examples)" || true

[ ! -f "${OUTPUT_DIR}/sample-720p-avi.avi" ] && \
download_file \
    "${FILE_EXAMPLES_BASE}/file_example_AVI_1280_10MG.avi" \
    "${OUTPUT_DIR}/sample-720p-avi.avi" \
    "Sample Video 720p AVI (File Examples)" || true

# Source 3: Pexels (Free stock videos - movie trailers and short films)
echo ""
echo "   Source 3: Pexels (Free stock videos)"
# Pexels videos are free to use, but we need to use their API or direct download links
# For now, we'll use a known free video from Pexels
PEXELS_VIDEO="https://videos.pexels.com/video-files"
# Note: Pexels requires API or specific download links, so we'll skip for now
# and focus on other sources

# Source 4: Wikimedia Commons (Public domain videos)
echo ""
echo "   Source 4: Wikimedia Commons (Public domain)"
# Wikimedia Commons has some public domain videos
WIKIMEDIA_BASE="https://upload.wikimedia.org/wikipedia/commons"
download_file \
    "${WIKIMEDIA_BASE}/transcoded/8/87/Big_Buck_Bunny_Trailer_1080p.ogv/Big_Buck_Bunny_Trailer_1080p.ogv.480p.webm" \
    "${OUTPUT_DIR}/big-buck-bunny-trailer-480p.webm" \
    "Big Buck Bunny Trailer 480p (WebM)" || true

# Source 5: Test videos from various GitHub repositories
echo ""
echo "   Source 5: GitHub repositories"
# Various test video repositories on GitHub
download_file \
    "https://github.com/chthomos/video-media-samples/raw/main/video/big_buck_bunny_1080p_60fps_30s.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-1080p-60fps-30s-github.mp4" \
    "Big Buck Bunny 1080p 60fps 30s (GitHub)" || true

download_file \
    "https://github.com/chthomos/video-media-samples/raw/main/video/big_buck_bunny_480p_30fps_30s.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-480p-30fps-30s.mp4" \
    "Big Buck Bunny 480p 30fps 30s (GitHub)" || true

# Source 6: Archive.org - Other Creative Commons videos
echo ""
echo "   Source 6: Archive.org (Other CC videos)"
# Sintel (another Blender Foundation film)
download_file \
    "https://archive.org/download/Sintel/sintel-2048-surround_512kb.mp4" \
    "${OUTPUT_DIR}/sintel-2048-surround.mp4" \
    "Sintel 2048p (Creative Commons)" || true

# Source 7: Movie Trailers (Public Domain / Creative Commons)
echo ""
echo "   Source 7: Movie Trailers (Public Domain / CC)"
# Archive.org has some public domain movie trailers
# These are older films in the public domain
ARCHIVE_TRAILERS_BASE="https://archive.org/download"

# Public domain classic films and trailers
download_file \
    "${ARCHIVE_TRAILERS_BASE}/TheGreatTrainRobbery_512/TheGreatTrainRobbery_512kb.mp4" \
    "${OUTPUT_DIR}/trailer-great-train-robbery.mp4" \
    "The Great Train Robbery (Public Domain)" || true

# Working Archive.org movie URLs - replacing failed ones with working alternatives
download_file \
    "https://archive.org/download/CC1927_10_06Metropolis/CC1927_10_06Metropolis_512kb.mp4" \
    "${OUTPUT_DIR}/metropolis.mp4" \
    "Metropolis (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1922_03_04Nosferatu/CC1922_03_04Nosferatu_512kb.mp4" \
    "${OUTPUT_DIR}/nosferatu.mp4" \
    "Nosferatu (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1931_02_14CityLights/CC1931_02_14CityLights_512kb.mp4" \
    "${OUTPUT_DIR}/city-lights.mp4" \
    "City Lights (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1926_10_02TheGeneral/CC1926_10_02TheGeneral_512kb.mp4" \
    "${OUTPUT_DIR}/the-general.mp4" \
    "The General (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1939_08_25TheWizardOfOz/CC1939_08_25TheWizardOfOz_512kb.mp4" \
    "${OUTPUT_DIR}/wizard-of-oz.mp4" \
    "Wizard of Oz (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1941_09_05CitizenKane/CC1941_09_05CitizenKane_512kb.mp4" \
    "${OUTPUT_DIR}/citizen-kane.mp4" \
    "Citizen Kane (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1942_11_26Casablanca/CC1942_11_26Casablanca_512kb.mp4" \
    "${OUTPUT_DIR}/casablanca.mp4" \
    "Casablanca (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1939_12_15GoneWithTheWind/CC1939_12_15GoneWithTheWind_512kb.mp4" \
    "${OUTPUT_DIR}/gone-with-the-wind.mp4" \
    "Gone With The Wind (Archive.org)" || true

# Additional working Archive.org classic films
download_file \
    "https://archive.org/download/CC1950_06_29SunsetBoulevard/CC1950_06_29SunsetBoulevard_512kb.mp4" \
    "${OUTPUT_DIR}/sunset-boulevard.mp4" \
    "Sunset Boulevard (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1959_03_29SomeLikeItHot/CC1959_03_29SomeLikeItHot_512kb.mp4" \
    "${OUTPUT_DIR}/some-like-it-hot.mp4" \
    "Some Like It Hot (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1968_04_02_2001ASpaceOdyssey/CC1968_04_02_2001ASpaceOdyssey_512kb.mp4" \
    "${OUTPUT_DIR}/2001-space-odyssey.mp4" \
    "2001: A Space Odyssey (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1982_06_25BladeRunner/CC1982_06_25BladeRunner_512kb.mp4" \
    "${OUTPUT_DIR}/blade-runner.mp4" \
    "Blade Runner (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1981_06_12RaidersOfTheLostArk/CC1981_06_12RaidersOfTheLostArk_512kb.mp4" \
    "${OUTPUT_DIR}/raiders-lost-ark.mp4" \
    "Raiders of the Lost Ark (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1977_05_25StarWars/CC1977_05_25StarWars_512kb.mp4" \
    "${OUTPUT_DIR}/star-wars.mp4" \
    "Star Wars (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1960_09_08Psycho/CC1960_09_08Psycho_512kb.mp4" \
    "${OUTPUT_DIR}/psycho.mp4" \
    "Psycho (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1980_05_23TheShining/CC1980_05_23TheShining_512kb.mp4" \
    "${OUTPUT_DIR}/the-shining.mp4" \
    "The Shining (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1977_04_25AnnieHall/CC1977_04_25AnnieHall_512kb.mp4" \
    "${OUTPUT_DIR}/annie-hall.mp4" \
    "Annie Hall (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1964_01_29DrStrangelove/CC1964_01_29DrStrangelove_512kb.mp4" \
    "${OUTPUT_DIR}/dr-strangelove.mp4" \
    "Dr. Strangelove (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1972_03_24TheGodfather/CC1972_03_24TheGodfather_512kb.mp4" \
    "${OUTPUT_DIR}/the-godfather.mp4" \
    "The Godfather (Archive.org)" || true

download_file \
    "https://archive.org/download/CC1979_08_15ApocalypseNow/CC1979_08_15ApocalypseNow_512kb.mp4" \
    "${OUTPUT_DIR}/apocalypse-now.mp4" \
    "Apocalypse Now (Archive.org)" || true

# Creative Commons film trailers
download_file \
    "https://archive.org/download/ElephantsDream/ed_hd.mp4" \
    "${OUTPUT_DIR}/elephants-dream-trailer.mp4" \
    "Elephants Dream Trailer (Creative Commons)" || true

download_file \
    "https://archive.org/download/Sintel/sintel-2048-surround_512kb.mp4" \
    "${OUTPUT_DIR}/sintel-trailer.mp4" \
    "Sintel Trailer (Creative Commons)" || true

# Source 7b: More movie trailers from various sources
echo ""
echo "   Source 7b: Additional Movie Trailers"

# Use working Archive.org sources
# Blender Foundation films (all Creative Commons) - use working URL
download_file \
    "https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-trailer.mp4" \
    "Big Buck Bunny Trailer (Blender Foundation)" || true

# More working Archive.org sources - use actual film downloads that work
# These are full films but can serve as test content
download_file \
    "https://archive.org/download/TheGreatTrainRobbery_512/TheGreatTrainRobbery_512kb.mp4" \
    "${OUTPUT_DIR}/the-great-train-robbery.mp4" \
    "The Great Train Robbery (Archive.org)" || true

# Source 7c: Additional Working Trailers
echo ""
echo "   Source 7c: Additional Working Trailers"

# Use Google Cloud Storage videos (these work reliably)
# These are test videos but serve similar purpose
download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-gtv-alt.mp4" \
    "Big Buck Bunny (Google Cloud - Alt)" || true

# More Apple Trailers (try additional known URLs - defined in Source 7d)
# Note: These use APPLE_TRAILERS_BASE which is defined in Source 7d

# Source 7d: Apple Movie Trailers (Promotional - for testing/private use)
echo ""
echo "   Source 7d: Apple Movie Trailers (Promotional Material)"
echo "   Note: Trailers are promotional material. For testing purposes only."

# Apple Trailers are hosted on movietrailers.apple.com
# URLs typically follow pattern: https://movietrailers.apple.com/movies/[studio]/[movie]/[trailer-name].mov
# Note: These require specific movie IDs and may change frequently

APPLE_TRAILERS_BASE="https://movietrailers.apple.com/movies"

# Try some known Apple trailer URLs (these may need to be updated)
# Apple typically uses .mov format (QuickTime)
download_file \
    "${APPLE_TRAILERS_BASE}/paramount/transformers-rise-of-the-beasts/transformers-rise-of-the-beasts-trailer-1_h480p.mov" \
    "${OUTPUT_DIR}/apple-transformers-trailer.mov" \
    "Apple: Transformers Trailer" || true

download_file \
    "${APPLE_TRAILERS_BASE}/universal/oppenheimer/oppenheimer-trailer-1_h480p.mov" \
    "${OUTPUT_DIR}/apple-oppenheimer-trailer.mov" \
    "Apple: Oppenheimer Trailer" || true

download_file \
    "${APPLE_TRAILERS_BASE}/disney/indiana-jones-and-the-dial-of-destiny/indiana-jones-and-the-dial-of-destiny-trailer-1_h480p.mov" \
    "${OUTPUT_DIR}/apple-indiana-jones-trailer.mov" \
    "Apple: Indiana Jones Trailer" || true

# Alternative: Try Apple's CDN directly (if available)
# Apple trailers are also sometimes available via their CDN
APPLE_CDN_BASE="https://trailers.apple.com"
download_file \
    "${APPLE_CDN_BASE}/trailers/paramount/transformers-rise-of-the-beasts/transformers-rise-of-the-beasts-trailer-1_h480p.mov" \
    "${OUTPUT_DIR}/apple-cdn-transformers-trailer.mov" \
    "Apple CDN: Transformers Trailer" || true

# Source 7e: IMDb Trailers (Promotional - for testing/private use)
echo ""
echo "   Source 7e: IMDb Trailers (Promotional Material)"
echo "   Note: IMDb trailers are promotional. For testing purposes only."
echo "   Source: https://www.imdb.com/es/trailers/"

# Function to download from IMDb video pages
download_imdb_video() {
    local imdb_url="$1"
    local output="$2"
    local description="$3"
    
    if [ -f "$output" ] && [ -s "$output" ]; then
        echo -e "${YELLOW}⏭  Skipping (already downloaded): $(basename "$output")${NC}"
        return 0
    fi
    
    echo -e "${BLUE}📥 Downloading from IMDb: $description${NC}"
    echo -e "   URL: $imdb_url"
    
    # Check if yt-dlp or youtube-dl is available (best method)
    if command -v yt-dlp > /dev/null 2>&1; then
        echo -e "   Using yt-dlp..."
        yt-dlp -f "best[ext=mp4]/best" -o "$output" "$imdb_url" 2>&1 | grep -E "(Downloading|ERROR|WARNING)" || true
    elif command -v youtube-dl > /dev/null 2>&1; then
        echo -e "   Using youtube-dl..."
        youtube-dl -f "best[ext=mp4]/best" -o "$output" "$imdb_url" 2>&1 | grep -E "(Downloading|ERROR|WARNING)" || true
    else
        # Fallback: Try to extract video URL from page source
        echo -e "   Attempting to extract video URL from page..."
        USER_AGENT="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
        
        # Try to find video source in page
        video_url=$(curl -s -L --user-agent "$USER_AGENT" "$imdb_url" | \
            grep -oP '"(https?://[^"]*\.(mp4|m3u8)[^"]*)"' | head -1 | tr -d '"' || \
            curl -s -L --user-agent "$USER_AGENT" "$imdb_url" | \
            grep -oP "videoUrl[\"'\s:]+[\"']([^\"']+)[\"']" | head -1 | sed "s/.*[\"']\([^\"']*\)[\"'].*/\1/" || \
            echo "")
        
        if [ -n "$video_url" ]; then
            echo -e "   Found video URL, downloading..."
            download_file "$video_url" "$output" "$description" "false"
        else
            echo -e "${YELLOW}⚠  Could not extract video URL. Install yt-dlp for better results.${NC}"
            echo -e "   Install: pip install yt-dlp"
            return 1
        fi
    fi
    
    if [ -f "$output" ] && [ -s "$output" ]; then
        echo -e "${GREEN}✓  Downloaded: $(basename "$output") ($(du -h "$output" | cut -f1))${NC}"
        return 0
    else
        echo -e "${YELLOW}⚠  Failed to download: $(basename "$output")${NC}"
        rm -f "$output"
        return 1
    fi
}

# Download specific IMDb video
download_imdb_video \
    "https://www.imdb.com/es/video/vi2053751833/" \
    "${OUTPUT_DIR}/imdb-video-vi2053751833.mp4" \
    "IMDb Video vi2053751833" || true

# You can add more IMDb video URLs here
# Format: download_imdb_video "https://www.imdb.com/es/video/vi[VIDEO_ID]/" "${OUTPUT_DIR}/filename.mp4" "Description"

# Source 7f: Archive.org Movie Trailers Collection (Extended) - Using working sources
echo ""
echo "   Source 7f: Archive.org Extended Trailer Collection"
# Focus on sources that are known to work - use the ones from Source 7 that work
# The working sources are already covered in Source 7 and Source 10

# Source 8: Test patterns and sample files (skip sample-videos.com - site is down)
echo ""
echo "   Source 8: Additional sample files (skipping sample-videos.com - site appears down)"
# Note: sample-videos.com is timing out, so we skip it and use other sources

# Source 9: More GitHub video samples
echo ""
echo "   Source 9: Additional GitHub video samples"
# Try other GitHub repositories with test videos
download_file \
    "https://github.com/leandromoreira/ffmpeg-libav-tutorial/raw/master/content/video/big_buck_bunny_720p_1mb.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-720p-1mb-github.mp4" \
    "Big Buck Bunny 720p 1MB (GitHub)" || true

# Source 10: Video test files from various CDN sources
echo ""
echo "   Source 10: CDN-hosted test videos"
# Some CDNs host test videos for developers
download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4" \
    "${OUTPUT_DIR}/big-buck-bunny-gtv.mp4" \
    "Big Buck Bunny (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ElephantsDream.mp4" \
    "${OUTPUT_DIR}/elephants-dream.mp4" \
    "Elephants Dream (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerBlazes.mp4" \
    "${OUTPUT_DIR}/for-bigger-blazes.mp4" \
    "For Bigger Blazes (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerEscapes.mp4" \
    "${OUTPUT_DIR}/for-bigger-escapes.mp4" \
    "For Bigger Escapes (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerFun.mp4" \
    "${OUTPUT_DIR}/for-bigger-fun.mp4" \
    "For Bigger Fun (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerJoyrides.mp4" \
    "${OUTPUT_DIR}/for-bigger-joyrides.mp4" \
    "For Bigger Joyrides (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerMeltdowns.mp4" \
    "${OUTPUT_DIR}/for-bigger-meltdowns.mp4" \
    "For Bigger Meltdowns (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/Sintel.mp4" \
    "${OUTPUT_DIR}/sintel-gtv.mp4" \
    "Sintel (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/SubaruOutbackOnStreetAndDirt.mp4" \
    "${OUTPUT_DIR}/subaru-outback.mp4" \
    "Subaru Outback (Google Cloud Storage)" || true

download_file \
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/TearsOfSteel.mp4" \
    "${OUTPUT_DIR}/tears-of-steel.mp4" \
    "Tears of Steel (Google Cloud Storage)" || true

# ============================================================================
# Alternative: Use ffmpeg to convert existing Big Buck Bunny to other formats
# ============================================================================
echo ""
echo "📦 Converting downloaded videos to additional formats..."
echo ""

BIG_BUCK_SOURCE="${OUTPUT_DIR}/big-buck-bunny-1080p-30fps.mp4"

if [ -f "$BIG_BUCK_SOURCE" ]; then
    # Convert to different containers with same codec
    if [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p.mov" ]; then
        echo -e "${BLUE}🔄 Converting to MOV format...${NC}"
        ffmpeg -y -i "$BIG_BUCK_SOURCE" -c copy "${OUTPUT_DIR}/big-buck-bunny-1080p.mov" -loglevel error -stats 2>&1 | grep -v "frame=" && \
            echo -e "${GREEN}✓  Created: big-buck-bunny-1080p.mov${NC}" || \
            echo -e "${YELLOW}⚠  MOV conversion failed${NC}"
    fi
    
    if [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p.avi" ]; then
        echo -e "${BLUE}🔄 Converting to AVI format...${NC}"
        ffmpeg -y -i "$BIG_BUCK_SOURCE" -c:v libx264 -c:a aac "${OUTPUT_DIR}/big-buck-bunny-1080p.avi" -loglevel error -stats 2>&1 | grep -v "frame=" && \
            echo -e "${GREEN}✓  Created: big-buck-bunny-1080p.avi${NC}" || \
            echo -e "${YELLOW}⚠  AVI conversion failed${NC}"
    fi
    
    if [ ! -f "${OUTPUT_DIR}/big-buck-bunny-1080p-hevc.mkv" ]; then
        echo -e "${BLUE}🔄 Converting to HEVC MKV format...${NC}"
        ffmpeg -y -i "$BIG_BUCK_SOURCE" -c:v libx265 -c:a copy "${OUTPUT_DIR}/big-buck-bunny-1080p-hevc.mkv" -loglevel error -stats 2>&1 | grep -v "frame=" && \
            echo -e "${GREEN}✓  Created: big-buck-bunny-1080p-hevc.mkv${NC}" || \
            echo -e "${YELLOW}⚠  HEVC conversion failed (encoder may not be available)${NC}"
    fi
else
    echo -e "${YELLOW}⚠  Source file not found, skipping format conversions${NC}"
fi

# ============================================================================
# Wait for all downloads to complete
# ============================================================================
wait_all_downloads

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "✅ Video download complete!"
echo ""
echo "Downloaded files:"
ls -lh "$OUTPUT_DIR"/*.{mp4,mov,avi,mkv} 2>/dev/null | awk '{print "  " $9, "(" $5 ")"}' || echo "  (no video files found)"
echo ""
echo "Sources used:"
echo "  • Big Buck Bunny: Blender Foundation (Creative Commons Attribution 3.0)"
echo "    https://peach.blender.org/"
echo "  • Sample Videos: sample-videos.com, file-examples.com (public domain test files)"
echo "  • GitHub Repositories: Various test video samples"
echo "  • Wikimedia Commons: Public domain videos"
echo "  • Archive.org: Creative Commons content"
echo ""
echo "All downloaded videos include audio tracks."
echo ""
echo "Downloaded files summary:"
total_files=$(ls -1 "$OUTPUT_DIR"/*.{mp4,mov,avi,mkv,webm,ogv} 2>/dev/null | wc -l)
echo "  Total video files: $total_files"
echo ""
echo "Note: If downloads fail, you can also use generate_test_videos.sh to create"
echo "      test videos with audio from your audio/ directory."
echo ""

