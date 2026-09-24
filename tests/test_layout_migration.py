"""Static checks for OBS settings migration wiring.

The mode decision itself is exercised by caption-layout-test.cpp without OBS;
these checks make sure captions-source.c actually uses the schema marker and
the two-stage defaults/user-value distinction that production migration needs.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src" / "captions-source.c").read_text(encoding="utf-8")
layout = (ROOT / "src" / "caption-layout.h").read_text(encoding="utf-8")

assert "TEA_LAYOUT_SCHEMA_VERSION 1" in layout
assert 'TEA_LAYOUT_SCHEMA_KEY "layout_schema_version"' in layout
assert "tea_caption_layout_mode_from_settings" in layout

# New defaults are Fixed; update() deliberately treats an old settings object
# without a user marker/value as legacy Auto.
assert 'obs_data_set_default_int(settings, "layout_mode", TEA_LAYOUT_MODE_FIXED);' in source
assert 'obs_data_set_default_int(settings, TEA_LAYOUT_SCHEMA_KEY, TEA_LAYOUT_SCHEMA_VERSION);' in source
assert "obs_data_has_default_value(settings, \"max_lines\")" in source
assert "obs_data_has_user_value(settings, TEA_LAYOUT_SCHEMA_KEY)" in source
assert "obs_data_has_user_value(settings, \"layout_mode\")" in source
assert "tea_caption_layout_mode_from_settings" in source
assert "obs_data_set_int(settings, TEA_LAYOUT_SCHEMA_KEY, TEA_LAYOUT_SCHEMA_VERSION);" in source


# Per-line renderer: existing scenes keep the neutral defaults (no fades, no
# pause breaks, no screen row limit, no background, no tail); only a brand-new
# source gets the recommended values, behind its own schema marker.
render_defaults = [
    'obs_data_set_default_bool(settings, TEA_KEY_FADE_IN, false);',
    'obs_data_set_default_bool(settings, TEA_KEY_FADE_OUT, false);',
    'obs_data_set_default_int(settings, TEA_KEY_PAUSE_MS, TEA_DEFAULT_PAUSE_MS);',
    'obs_data_set_default_int(settings, TEA_KEY_MAX_ROWS, 0);',
    'obs_data_set_default_bool(settings, TEA_KEY_BG, false);',
    'obs_data_set_default_bool(settings, TEA_KEY_TAIL, false);',
]
for line in render_defaults:
    assert line in source, line
assert "#define TEA_DEFAULT_PAUSE_MS 0" in source
new_block = "if (from_new_source_defaults && !obs_data_has_user_value(settings, TEA_RENDER_SCHEMA_KEY)) {"
assert new_block in source
block = source[source.index(new_block):]
block = block[: block.index("}")]
assert "obs_data_set_int(settings, TEA_RENDER_SCHEMA_KEY, TEA_RENDER_SCHEMA_VERSION);" in block
assert "obs_data_set_bool(settings, TEA_KEY_FADE_OUT, true);" in block

# Appearance never reconnects: the only calls that (re)start the ASR session
# are the connection-policy path and the Tools dialog's "Reconnect All", and
# the connection-relevant keys are read nowhere else.
assert source.count("tea_asr_client_start(ctx->client)") == 2
apply_fn = source[source.index("static void tea_apply_connection("):]
apply_fn = apply_fn[: apply_fn.index("\n}\n")]
assert "tea_connection_decide(" in apply_fn and "tea_asr_client_start(ctx->client)" in apply_fn
update_fn = source[source.index("static void tea_captions_source_update("):]
update_fn = update_fn[: update_fn.index("\n}\n")]
assert "tea_asr_client_start" not in update_fn and "tea_asr_client_set_stable_captions" not in update_fn
assert "tea_apply_connection(ctx, settings, false);" in update_fn
for read in ('obs_data_get_string(settings, "server_host")', 'obs_data_get_int(settings, "server_port")',
             'obs_data_get_string(settings, "token_path")', 'obs_data_get_bool(settings, "stable_captions")'):
    assert source.count(read) == apply_fn.count(read) == 1, read

print("layout migration checks passed")
