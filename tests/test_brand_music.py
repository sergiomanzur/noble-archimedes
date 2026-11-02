"""Fast checks for the original, reproducible launcher soundtrack."""
import importlib.util
import json
import math
from array import array
from pathlib import Path
import wave


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "generate_music", ROOT / "tools/branding/generate_music.py"
)
music = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(music)


def test_score_is_reproducible_and_committed():
    assert music.build_score() == music.build_score()
    assert music.build_score() == json.loads(
        (ROOT / "assets/noble/music-score.json").read_text(encoding="utf-8")
    )


def test_small_render_is_deterministic_and_loops():
    score = music.build_score()
    pcm = music.render(score, sample_rate=2000)
    assert pcm == music.render(score, sample_rate=2000)
    assert len(pcm) == 32 * 2000 * 2
    assert 1000 < max(abs(x) for x in pcm) <= round(32767 * 0.20)
    for channel in (0, 1):
        # A join must be as smooth as nearby ordinary sample steps.
        seam = abs(pcm[channel] - pcm[-2 + channel])
        local_steps = [abs(pcm[i] - pcm[i - 2]) for i in range(2 + channel, 202, 2)]
        assert seam <= max(local_steps) * 2 + 2


def test_distributed_wave_is_quiet_stereo_pcm_and_continuous():
    with wave.open(str(ROOT / "assets/noble/launcher-theme.wav"), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 44100)
        assert wav.getnframes() == 32 * 44100
        assert wav.getcomptype() == "NONE"
        pcm = array("h", wav.readframes(wav.getnframes()))
    assert max(abs(x) for x in pcm) <= round(32767 * 0.20)
    rms = math.sqrt(sum(x * x for x in pcm) / len(pcm)) / 32767
    assert 0.015 < rms < 0.10
    assert any(pcm[i] != pcm[i + 1] for i in range(0, 10000, 2))
    for channel in (0, 1):
        assert abs(pcm[channel] - pcm[-2 + channel]) < 200


if __name__ == '__main__':
    test_score_is_reproducible_and_committed()
    test_small_render_is_deterministic_and_loops()
    test_distributed_wave_is_quiet_stereo_pcm_and_continuous()
    print('3 original music checks passed')
