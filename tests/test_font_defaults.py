"""Static checks for the OBS font-default integration.

This deliberately does not import OBS or configure CMake.  The runtime
default is an obs_data default object, so source-level checks are enough to
guard the two compatibility contracts that can be validated in CI:
platform-specific CJK family selection and non-destructive defaulting.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
policy = (ROOT / "src" / "font-default-policy.h").read_text(encoding="utf-8")
source = (ROOT / "src" / "captions-source.c").read_text(encoding="utf-8")

assert '#define TEA_FONT_FACE_MACOS "Heiti TC"' in policy
assert '#define TEA_FONT_FACE_WINDOWS "Microsoft JhengHei"' in policy
assert '#define TEA_FONT_FACE_UNIX "Noto Sans CJK TC"' in policy
assert "#if defined(__APPLE__)" in policy
assert "#elif defined(_WIN32)" in policy

# OBS applies defaults only when the scene/source has no user value.  Using
# the default-object API here is what preserves an existing custom face.
assert 'obs_data_set_default_obj(settings, "font", font_obj);' in source
assert 'obs_data_set_obj(settings, "font"' not in source
assert 'obs_data_set_default_string(font_obj, "style", TEA_DEFAULT_FONT_STYLE);' in source
assert 'obs_data_set_default_string(font_obj, "face", TEA_DEFAULT_FONT_FACE);' in source

# obs_data_apply() copies user values only.  The child must therefore be
# initialized from effective defaults first, then overlaid with user values;
# otherwise a fresh source silently falls back to text_ft2_source's own font
# (Arial on the affected OBS build) instead of the platform CJK family.
defaults_call = "obs_data_t *child_settings = obs_data_get_defaults(settings);"
apply_call = "obs_data_apply(child_settings, settings);"
assert defaults_call in source
assert apply_call in source
assert source.index(defaults_call) < source.index(apply_call)

print("font default policy checks passed")
