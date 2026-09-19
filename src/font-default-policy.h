#ifndef TEA_FONT_DEFAULT_POLICY_H
#define TEA_FONT_DEFAULT_POLICY_H

/*
 * The OBS Text (FreeType 2) source stores a font as an object with the
 * `face`, `style`, `size`, and `flags` fields.  Keep the family choice in a
 * tiny C-only policy header so captions-source.c and the no-OBS tests agree
 * on the platform defaults.
 *
 * These are platform family candidates that cover Traditional Chinese on the
 * supported desktop platforms.  Names must be visible to OBS's FreeType
 * font/fontconfig lookup (not merely the display name shown by another font
 * picker).  obs_data_set_default_obj() only supplies missing settings, so
 * changing these defaults does not overwrite a font a user already selected
 * in a scene collection.
 */
#define TEA_FONT_FACE_MACOS "Heiti TC"
#define TEA_FONT_FACE_WINDOWS "Microsoft JhengHei"
#define TEA_FONT_FACE_UNIX "Noto Sans CJK TC"

#if defined(__APPLE__)
#define TEA_DEFAULT_FONT_FACE TEA_FONT_FACE_MACOS
#elif defined(_WIN32)
#define TEA_DEFAULT_FONT_FACE TEA_FONT_FACE_WINDOWS
#else
#define TEA_DEFAULT_FONT_FACE TEA_FONT_FACE_UNIX
#endif

#define TEA_DEFAULT_FONT_STYLE ""

#endif /* TEA_FONT_DEFAULT_POLICY_H */
