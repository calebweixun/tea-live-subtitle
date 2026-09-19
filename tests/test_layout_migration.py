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

print("layout migration checks passed")
