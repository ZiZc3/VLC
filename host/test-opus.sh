#!/usr/bin/env bash
# Regression test for issue #7 ("vlc could not decode the format opus").
#
#   bash host/test-opus.sh [media.mkv]
#
# Plays an Opus-audio file through the host build's static libvlc (the same
# plugin set the PS5 title links) and fails unless audio was actually decoded.
# Without the fix the log shows "could not decode the format opus" and the
# output WAV is empty.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
work=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
media=${1:-$work/testmedia/opus_test.mkv}
out=$work/out/opus-test
probe=$work/out/vlc_probe

[[ -f $media ]] || { echo "test media missing: $media (make an Opus .mkv first)" >&2; exit 2; }
[[ -x $probe ]] || { echo "vlc_probe not built: run bash host/build-probe.sh first" >&2; exit 2; }

rm -rf "$out"
mkdir -p "$out"
log=$out/probe.log
"$probe" "$media" "$out" 50 >"$log" 2>&1 || true

fail=0
if grep -qi 'could not decode the format.*opus' "$log"; then
    echo "FAIL: still cannot decode opus:"; grep -i 'could not decode' "$log"
    fail=1
fi
[[ -s $out/audio.wav ]] || { echo "FAIL: no audio.wav produced"; fail=1; }
# 44-byte header + at least a second of S16 stereo 48 kHz
if [[ -f $out/audio.wav && $(stat -c%s "$out/audio.wav") -lt $((44 + 48000*2*2)) ]]; then
    echo "FAIL: audio.wav has < 1 s of audio ($(stat -c%s "$out/audio.wav") bytes)"
    fail=1
fi
if [[ $fail == 0 ]]; then
    secs=$(python3 -c "import os;print((os.path.getsize('$out/audio.wav')-44)/192000)")
    echo "PASS: opus decoded, ${secs}s of audio in $out/audio.wav"
fi
exit $fail
