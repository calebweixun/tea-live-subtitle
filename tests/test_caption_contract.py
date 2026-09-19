"""Static, OBS-free contract checks for transcript/status separation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
caption_state = (ROOT / "src" / "caption-state.c").read_text(encoding="utf-8")
caption_header = (ROOT / "src" / "caption-state.h").read_text(encoding="utf-8")
asr_client = (ROOT / "src" / "asr-client.cpp").read_text(encoding="utf-8")
source = (ROOT / "src" / "captions-source.c").read_text(encoding="utf-8")

# A server/client status must never be routed into the transcript state.
assert "tea_caption_state_set_status" not in caption_state
assert "tea_caption_state_set_status" not in caption_header
assert "tea_caption_state_set_status" not in asr_client
assert "status_text" not in caption_state

# An empty transcript is represented by an empty string. The source owns the
# optional neutral waiting prompt and chooses it only after this render call.
assert 'char *out = bstrdup("");' in caption_state
assert 'obs_module_text("TeaLiveSubtitle.PlaceholderText")' in source
assert "show_placeholder" in source

# The status path consumed by the Tools dialog remains intact.
assert "tea_asr_client_status_text(ctx->client)" in source
assert ".status_text = status_text" in source

print("caption status contract checks passed")
