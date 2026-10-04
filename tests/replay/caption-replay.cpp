/*
 * Offline caption replay: feeds a recorded server event trace (JSONL, one
 * {"t_ms": <client receive time>, "event": {...server event...}} per line)
 * through the plugin's real caption state machine (src/caption-state.c) and
 * the real per-line display policy (src/caption-display.h), exactly the way
 * captions-source.c drives them, and prints what the screen shows over time.
 *
 * It flags every moment the visible text of a line shrinks or vanishes while
 * its server segment is still open (the "text disappears mid-speech" and
 * "text goes backwards" reports), and the largest amount of text that
 * appears at once. No OBS, no GPU: glyph widths come from a simple font
 * model (CJK = font size, ASCII = half, space = quarter), which is enough to
 * reproduce wrapping and the row limit.
 *
 * Build (see tests/replay/README.md):
 *   cc  -std=c11   -c -Itests/stubs -Isrc src/caption-state.c -o caption-state.o
 *   c++ -std=c++17 -Itests/stubs -Isrc tests/replay/caption-replay.cpp caption-state.o -o caption-replay
 * Build against an older src/ with -DTEA_REPLAY_LEGACY (the API before the
 * live fixes: pause-gap line breaks, no activity tracking) to compare.
 *
 * Usage: caption-replay TRACE.jsonl [--fade-delay-ms 1500] [--fade-ms 200]
 *          [--max-rows 2] [--max-lines 3] [--width 1800] [--padding 10]
 *          [--font-px 48] [--outline-extra 6] [--tail on|off]
 *          [--punct off|sentence|comma] [--comma-min 8]
 *          [--pause-ms 870 (legacy build only)] [--quiet]
 */

#include "caption-state.h"
#include "caption-display.h"
#include "caption-align.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

/* ---------------- minimal JSON ---------------- */

struct Json {
	enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::vector<std::pair<std::string, Json>> o;

	const Json *get(const char *key) const
	{
		for (const auto &kv : o)
			if (kv.first == key)
				return &kv.second;
		return nullptr;
	}
	std::string str(const char *key) const
	{
		const Json *v = get(key);
		return v && v->kind == Str ? v->s : std::string();
	}
	double num(const char *key, double fallback) const
	{
		const Json *v = get(key);
		return v && v->kind == Num ? v->n : fallback;
	}
};

struct Parser {
	const char *p;
	const char *end;
	bool ok = true;

	void ws()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}
	static void put_utf8(std::string &out, uint32_t cp)
	{
		char buf[5];
		size_t n = tea_utf8_encode(cp, buf);
		out.append(buf, n);
	}
	uint32_t hex4()
	{
		uint32_t v = 0;
		for (int i = 0; i < 4 && p < end; i++, p++) {
			char c = *p;
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= (uint32_t)(c - 'A' + 10);
			else
				ok = false;
		}
		return v;
	}
	std::string string()
	{
		std::string out;
		p++; /* opening quote */
		while (p < end && *p != '"') {
			if (*p != '\\') {
				out.push_back(*p++);
				continue;
			}
			p++;
			if (p >= end)
				break;
			char c = *p++;
			switch (c) {
			case 'n':
				out.push_back('\n');
				break;
			case 't':
				out.push_back('\t');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'u': {
				uint32_t cp = hex4();
				if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
					p += 2;
					uint32_t lo = hex4();
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				put_utf8(out, cp);
				break;
			}
			default:
				out.push_back(c);
			}
		}
		if (p < end)
			p++; /* closing quote */
		return out;
	}
	Json value()
	{
		ws();
		Json v;
		if (p >= end) {
			ok = false;
			return v;
		}
		if (*p == '{') {
			v.kind = Json::Obj;
			p++;
			ws();
			while (p < end && *p != '}') {
				ws();
				std::string key = string();
				ws();
				if (p < end && *p == ':')
					p++;
				Json item = value();
				v.o.emplace_back(key, item);
				ws();
				if (p < end && *p == ',')
					p++;
				ws();
			}
			if (p < end)
				p++;
		} else if (*p == '[') {
			v.kind = Json::Arr;
			p++;
			ws();
			while (p < end && *p != ']') {
				v.a.push_back(value());
				ws();
				if (p < end && *p == ',')
					p++;
				ws();
			}
			if (p < end)
				p++;
		} else if (*p == '"') {
			v.kind = Json::Str;
			v.s = string();
		} else if (!strncmp(p, "true", 4)) {
			v.kind = Json::Bool;
			v.b = true;
			p += 4;
		} else if (!strncmp(p, "false", 5)) {
			v.kind = Json::Bool;
			p += 5;
		} else if (!strncmp(p, "null", 4)) {
			p += 4;
		} else {
			char *after = nullptr;
			v.kind = Json::Num;
			v.n = strtod(p, &after);
			if (after == p)
				ok = false;
			p = after;
		}
		return v;
	}
};

/* ---------------- settings ---------------- */

struct Settings {
	uint32_t fade_delay_ms = 1500;
	uint32_t fade_ms = 200;
	int max_rows = 2;
	int max_lines = 3;
	int width = 1800;
	int padding = 10;
	int font_px = 48;
	uint32_t outline_extra = 6; /* outline + drop shadow */
	bool tail = true;
	uint32_t pause_ms = 870; /* legacy build only: the old client-side gap line break */
	int punct_mode = 0;      /* TEA_PUNCT_BREAK_* (not in the legacy build) */
	int comma_min = 8;
	bool soft = false; /* --soft on: soft breaks for runs without punctuation */
	int soft_min = 16; /* --soft-min N */
	/* detector self-test: switch the box width mid-run (a real reflow) */
	int width2 = 0;
	uint64_t width_change_ns = 0;
	bool quiet = false;
	bool ignore_acks = false;  /* as if the server sent no audio.ack: only the open-line timeout */
	bool singing_show = false; /* --singing show: "Always show" instead of automatic */
};

uint32_t fake_advance(uint32_t cp, int font_px)
{
	if (tea_cp_is_space(cp))
		return (uint32_t)font_px / 4u;
	if (cp < 0x80)
		return (uint32_t)font_px / 2u;
	return (uint32_t)font_px;
}

/* ---------------- the simulated source ---------------- */

struct Line {
	tea_display_line_t meta;
	std::string text; /* committed + tail */
	bool open = false;
};

#ifdef TEA_SOFT_MIN_DEFAULT
#define TEA_SOFT_GAP_MS_OR_0 TEA_SOFT_GAP_MS
#define TEA_SOFT_HARD_EXTRA_OR_0 TEA_SOFT_HARD_EXTRA
#else
#define TEA_SOFT_GAP_MS_OR_0 0
#define TEA_SOFT_HARD_EXTRA_OR_0 0
#endif

struct RowView {
	uint64_t key;
	std::string text;
	std::string tail; /* the part of `text` that is unconfirmed */
	float alpha;
	size_t start = 0, end = 0;
	bool punct_break = false;
	bool soft_break = false;
};

struct Sim {
	Settings cfg;
	tea_caption_state_t *state = nullptr;
	std::unique_ptr<tea_glyph_cache_t> cache{new tea_glyph_cache_t()};
	std::map<uint64_t, Line> lines;
	std::vector<uint64_t> order;
	uint64_t rev_seen = UINT64_MAX;
	uint64_t session_activity = 0;
	uint64_t session_activity_ns = 0;
	uint64_t server_progress = 0;
	uint64_t server_progress_ns = 0;
	/* hiding (singing / pause) */
	int hidden_started = 0; /* lines hidden while in the display */
	int hidden_faded = 0;   /* ... of which had text on screen and faded out */
	int shown_again = 0;    /* hidden lines shown again (revision back to speech) */

	void learn(const std::string &text)
	{
		size_t pos = 0;
		while (pos < text.size()) {
			uint32_t cp = tea_utf8_decode(text.data(), text.size(), &pos);
			if (!tea_glyph_cache_lookup(cache.get(), cp, nullptr))
				tea_glyph_cache_store(cache.get(), cp, fake_advance(cp, cfg.font_px));
		}
	}

	uint64_t fade_ref(const Line &line) const
	{
#ifdef TEA_REPLAY_LEGACY
		return line.meta.changed_ns;
#elif defined(TEA_CAPTION_STATE_HAS_SERVER_PROGRESS)
		return tea_display_line_fade_ref(&line.meta, session_activity_ns, server_progress_ns, cfg.fade_delay_ms,
						 TEA_OPEN_LINE_TIMEOUT_MS);
#else
		return tea_display_line_fade_ref(&line.meta, session_activity_ns);
#endif
	}

	/* tea_sync_server_progress() */
	void sync_progress(uint64_t now)
	{
#ifdef TEA_CAPTION_STATE_HAS_SERVER_PROGRESS
		const uint64_t progress = tea_caption_state_server_progress(state);
		if (progress != server_progress) {
			server_progress = progress;
			server_progress_ns = now;
		}
#else
		(void)now;
#endif
	}

	/* tea_sync_lines() */
	void sync(uint64_t now)
	{
		uint64_t rev = tea_caption_state_revision(state);
		if (rev == rev_seen)
			return;
		rev_seen = rev;
		tea_caption_snapshot_t snap;
		tea_caption_state_snapshot(state, cfg.tail, &snap);
#ifndef TEA_REPLAY_LEGACY
		if (snap.activity != session_activity) {
			session_activity = snap.activity;
			session_activity_ns = now;
		}
#endif
		std::map<uint64_t, Line> next;
		order.clear();
		for (int i = 0; i < snap.count; i++) {
			const tea_caption_snapshot_line_t &s = snap.lines[i];
			std::string display = std::string(s.text) + (s.tail ? s.tail : "");
			for (char &c : display)
				if (c == '\n' || c == '\r')
					c = ' ';
			const size_t locked = strlen(s.text);
			learn(display);
			auto it = lines.find(s.key);
			Line line;
#ifdef TEA_CAPTION_STATE_HAS_HIDING
			/* tea_sync_line(): a hidden line not on screen never appears;
			 * one on screen keeps its text and fades out */
			if (s.hidden) {
				if (it == lines.end())
					continue;
				line = it->second;
				if (tea_display_line_note_hidden(&line.meta, true, now) == TEA_HIDE_STARTED) {
					hidden_started++;
					if (!tea_display_line_has_visible_text(&line.meta))
						tea_display_line_retire(&line.meta);
					else
						hidden_faded++;
				}
				next[s.key] = line;
				order.push_back(s.key);
				continue;
			}
			if (it != lines.end() && it->second.meta.hidden) {
				lines.erase(it); /* shown again: a new line from scratch */
				it = lines.end();
				shown_again++;
			}
#endif
			if (it == lines.end()) {
				tea_display_line_init(&line.meta, s.key, display.size(), locked, now);
			} else {
				line = it->second;
#ifdef TEA_REPLAY_LEGACY
				tea_display_line_observe(&line.meta, line.text.c_str(), line.text.size(),
							 display.c_str(), display.size(), locked, now, cfg.pause_ms);
#else
				tea_display_line_observe(&line.meta, line.text.c_str(), line.text.size(),
							 display.c_str(), display.size(), locked, now);
#endif
			}
#ifndef TEA_REPLAY_LEGACY
			tea_display_line_note_activity(&line.meta, s.activity, s.open, now);
#endif
			line.text = display;
			line.open = s.open;
			next[s.key] = line;
			order.push_back(s.key);
		}
		tea_caption_snapshot_free(&snap);
		lines.swap(next);
	}

	/* tea_expire_lines() */
	void expire(uint64_t now)
	{
		for (auto &kv : lines) {
			Line &line = kv.second;
			if (!tea_display_line_has_visible_text(&line.meta))
				continue;
			if (tea_fade_out_done(true, now, fade_ref(line), cfg.fade_delay_ms, cfg.fade_ms))
				tea_display_line_retire(&line.meta);
		}
	}

	tea_caption_frame_t geometry() const
	{
		return tea_caption_frame_resolve(TEA_LAYOUT_MODE_FIXED, cfg.width, cfg.padding, 0, true,
						 cfg.outline_extra, (uint32_t)cfg.font_px, true, 0);
	}

	/* tea_layout()'s wrap; like the plugin, keeps the soft breaks it decided */
	int wrap(tea_display_line_t &meta, const std::string &text, tea_wrap_row_t *ring) const
	{
		tea_caption_frame_t geo = geometry();
#ifdef TEA_REPLAY_LEGACY
		return tea_wrap_line(&meta, text.c_str(), geo.wrap_width, cfg.outline_extra, geo.word_wrap, cache.get(),
				     ring, 32);
#else
		tea_punct_break_t punct;
		punct.mode = cfg.punct_mode;
		punct.comma_min_chars = cfg.comma_min;
#ifdef TEA_SOFT_MIN_DEFAULT
		tea_soft_break_t soft_cfg;
		tea_soft_break_defaults(&soft_cfg, cfg.soft, cfg.soft_min);
		tea_soft_wrap_t soft;
		soft.cfg = &soft_cfg;
#ifdef TEA_DISPLAY_MAX_WRAP
		tea_display_line_wrap_begin(&meta, tea_wrap_sig(geo.wrap_width, cfg.outline_extra, geo.word_wrap,
								&punct, &soft_cfg, (uint64_t)cfg.font_px));
#endif
		int total = tea_wrap_line_soft(&meta, text.c_str(), geo.wrap_width, cfg.outline_extra, geo.word_wrap,
					       &punct, &soft, cache.get(), ring, 32);
		if (total >= 0) {
			tea_display_line_soft_commit(&meta, &soft);
#ifdef TEA_DISPLAY_MAX_WRAP
			tea_wrap_row_t ordered[32];
			int kept = tea_wrap_rows_in_order(ring, total, 32, ordered);
			tea_display_line_wrap_commit(&meta, ordered, kept);
#endif
		}
		return total;
#else
		return tea_wrap_line_ex(&meta, text.c_str(), geo.wrap_width, cfg.outline_extra, geo.word_wrap, &punct,
					cache.get(), ring, 32);
#endif
#endif
	}

	/* tea_layout() + the alpha tea_draw_pieces() would use */
	std::vector<RowView> layout(uint64_t now)
	{
		struct Pending {
			uint64_t key;
			tea_wrap_row_t row;
		};
		std::vector<Pending> rows;
		for (uint64_t key : order) {
			Line &line = lines[key];
			tea_wrap_row_t ring[32], ordered[32];
			int total = wrap(line.meta, line.text, ring);
			if (total < 0)
				continue;
			int kept = tea_wrap_rows_in_order(ring, total, 32, ordered);
			if (total > kept && kept > 0)
				tea_display_line_evict_through(&line.meta, ordered[0].start);
			for (int r = 0; r < kept; r++)
				rows.push_back({key, ordered[r]});
		}
		const int limit = cfg.max_rows > 0 ? cfg.max_rows : 20;
		const int evict = tea_rows_to_evict((int)rows.size(), limit);
		for (int i = 0; i < evict; i++)
			tea_display_line_evict_through(&lines[rows[(size_t)i].key].meta, rows[(size_t)i].row.next);
		std::vector<RowView> out;
		for (size_t i = (size_t)evict; i < rows.size(); i++) {
			const Line &line = lines[rows[i].key];
			const tea_wrap_row_t &r = rows[i].row;
			RowView v;
			v.key = rows[i].key;
			v.text = line.text.substr(r.start, r.end - r.start);
			size_t locked = line.meta.locked_len;
			if (locked < r.end)
				v.tail = line.text.substr(locked > r.start ? locked : r.start,
							  r.end - (locked > r.start ? locked : r.start));
			v.alpha = tea_fade_out_alpha(true, now, fade_ref(line), cfg.fade_delay_ms, cfg.fade_ms);
			v.start = r.start;
			v.end = r.end;
#ifndef TEA_REPLAY_LEGACY
			v.punct_break = r.punct_break;
#endif
#ifdef TEA_SOFT_MIN_DEFAULT
			v.soft_break = r.soft_break;
#endif
			out.push_back(v);
		}
		return out;
	}
};

/* key -> (committed + tail, activity) of every line in the caption state */
struct Shown {
	std::string text;
	uint64_t activity = 0;
};
std::map<uint64_t, Shown> shown_lines(tea_caption_state_t *state, bool tail)
{
	std::map<uint64_t, Shown> out;
	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(state, tail, &snap);
	for (int i = 0; i < snap.count; i++) {
		Shown v;
		v.text = std::string(snap.lines[i].text) + (snap.lines[i].tail ? snap.lines[i].tail : "");
#ifndef TEA_REPLAY_LEGACY
		v.activity = snap.lines[i].activity;
#endif
		out[snap.lines[i].key] = v;
	}
	tea_caption_snapshot_free(&snap);
	return out;
}

size_t utf8_chars(const std::string &s)
{
	size_t n = 0;
	for (unsigned char c : s)
		if ((c & 0xC0) != 0x80)
			n++;
	return n;
}

/* Characters of `after` not matched by the longest common subsequence with
 * `before` (code points, raw). */
size_t new_chars(const std::string &before, const std::string &after)
{
	std::vector<uint32_t> a, b;
	size_t pos = 0;
	while (pos < before.size())
		a.push_back(tea_utf8_decode(before.data(), before.size(), &pos));
	pos = 0;
	while (pos < after.size())
		b.push_back(tea_utf8_decode(after.data(), after.size(), &pos));
	std::vector<size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
	for (size_t i = 1; i <= a.size(); i++) {
		for (size_t j = 1; j <= b.size(); j++)
			cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : std::max(prev[j], cur[j - 1]);
		std::swap(prev, cur);
	}
	return b.size() - prev[b.size()];
}

bool starts_with(const std::string &s, const std::string &prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

/* What a line shows: its rows that are not faded out (alpha > 5%). */
std::map<uint64_t, std::string> visible_by_line(const std::vector<RowView> &rows)
{
	std::map<uint64_t, std::string> out;
	for (const RowView &r : rows)
		if (r.alpha > 0.05f)
			out[r.key] += r.text;
	return out;
}

std::string render_screen(const std::vector<RowView> &rows)
{
	std::string out;
	for (const RowView &r : rows) {
		if (r.alpha <= 0.05f)
			continue;
		if (!out.empty())
			out += " / ";
		if (r.alpha < 0.99f)
			out += "~";
		std::string committed = r.text.substr(0, r.text.size() - r.tail.size());
		out += committed;
		if (!r.tail.empty())
			out += "\xe3\x80\x94" + r.tail + "\xe3\x80\x95"; /* 〔tail〕 */
	}
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s TRACE.jsonl [options]\n", argv[0]);
		return 2;
	}
	Settings cfg;
	for (int i = 2; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char * {
			return i + 1 < argc ? argv[++i] : "0";
		};
		if (a == "--fade-delay-ms")
			cfg.fade_delay_ms = (uint32_t)std::atoi(next());
		else if (a == "--fade-ms")
			cfg.fade_ms = (uint32_t)std::atoi(next());
		else if (a == "--max-rows")
			cfg.max_rows = std::atoi(next());
		else if (a == "--max-lines")
			cfg.max_lines = std::atoi(next());
		else if (a == "--width")
			cfg.width = std::atoi(next());
		else if (a == "--padding")
			cfg.padding = std::atoi(next());
		else if (a == "--font-px")
			cfg.font_px = std::atoi(next());
		else if (a == "--outline-extra")
			cfg.outline_extra = (uint32_t)std::atoi(next());
		else if (a == "--tail")
			cfg.tail = std::strcmp(next(), "off") != 0;
		else if (a == "--pause-ms")
			cfg.pause_ms = (uint32_t)std::atoi(next());
		else if (a == "--punct") {
			std::string m = next();
			cfg.punct_mode = m == "comma" ? 2 : m == "sentence" ? 1 : 0;
		} else if (a == "--soft") {
			cfg.soft = std::strcmp(next(), "on") == 0;
		} else if (a == "--soft-min") {
			cfg.soft_min = std::atoi(next());
		} else if (a == "--comma-min")
			cfg.comma_min = std::atoi(next());
		else if (a == "--selftest-width-change") {
			cfg.width2 = std::atoi(next());
			cfg.width_change_ns = (uint64_t)std::atoll(next()) * TEA_NS_PER_MS;
		} else if (a == "--singing") {
			cfg.singing_show = std::strcmp(next(), "show") == 0;
		} else if (a == "--ignore-acks")
			cfg.ignore_acks = true;
		else if (a == "--quiet")
			cfg.quiet = true;
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}

	struct Event {
		uint64_t t_ns;
		Json ev;
	};
	std::vector<Event> events;
	std::ifstream in(argv[1]);
	if (!in) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	std::string raw;
	while (std::getline(in, raw)) {
		if (raw.empty())
			continue;
		Parser parser{raw.data(), raw.data() + raw.size()};
		Json line = parser.value();
		const Json *ev = line.get("event");
		if (!parser.ok || !ev || ev->kind != Json::Obj)
			continue;
		double t = line.num("t_ms", -1);
		if (t < 0)
			continue;
		events.push_back({(uint64_t)std::llround(t * 1e6), *ev});
	}

	Sim sim;
	sim.cfg = cfg;
	sim.state = tea_caption_state_create();
	tea_caption_state_set_max_lines(sim.state, cfg.max_lines);
	tea_caption_state_set_stable_tail_lines(sim.state, cfg.tail);
#ifdef TEA_CAPTION_STATE_HAS_HIDING
	/* the plugin's default: automatic (hides only what the server labels singing) */
	tea_caption_state_set_hide_singing(sim.state, !cfg.singing_show);
#endif

	std::string session;
	bool stable = false;
	size_t next_event = 0;
	const uint64_t tick = 16 * TEA_NS_PER_MS;
	const uint64_t last = events.empty() ? 0 : events.back().t_ns + 4000 * TEA_NS_PER_MS;

	std::string prev_screen;
	std::map<uint64_t, std::string> prev_visible;
	std::map<uint64_t, std::string> prev_display;
	std::map<uint64_t, bool> prev_open;
	std::map<uint64_t, size_t> prev_visible_from;
	int flags_fade = 0, flags_evict = 0, flags_hidden = 0, flags_rewrite = 0, flags_other = 0, final_shrinks = 0;
	int flags_hidden_by_class = 0, singing_events = 0;
	/* layout stability and row statistics */
	struct PrevRow {
		size_t start;
		std::string text;
	};
	std::map<uint64_t, std::vector<PrevRow>> prev_rows;
	int layout_moves = 0;
	size_t span_max = 0; /* the longest run of characters between punctuation / soft breaks */
	std::map<std::string, bool> soft_seen;
	double row_chars_sum = 0;
	uint64_t row_samples = 0;
	size_t row_chars_max = 0;
	std::map<uint64_t, Line> last_seen; /* for rows per segment */
	struct Snap {
		uint64_t t;
		std::string text;
		size_t shown_end;
	};
	std::map<uint64_t, std::vector<Snap>> shown_history;
	struct Break {
		uint64_t key;
		size_t end;
		std::string prefix;
		uint64_t t;
		bool committed; /* the mark was committed text when the break first showed */
	};
	std::map<std::string, Break> breaks_seen;
	size_t max_burst = 0;
	uint64_t max_burst_t = 0;
	int bursts_over_8 = 0;

	std::printf("# replay %s  fade_delay=%ums fade=%ums max_rows=%d max_lines=%d width=%d font=%dpx tail=%s "
		    "punct=%s comma_min=%d%s\n",
		    argv[1], cfg.fade_delay_ms, cfg.fade_ms, cfg.max_rows, cfg.max_lines, cfg.width, cfg.font_px,
		    cfg.tail ? "on" : "off",
		    cfg.punct_mode == 2   ? "comma"
		    : cfg.punct_mode == 1 ? "sentence"
					  : "off",
		    cfg.comma_min,
#ifdef TEA_REPLAY_LEGACY
		    "  [LEGACY build: pre-fix src/]"
#else
		    ""
#endif
	);

	/* segment close audit: what was shown right before the close vs. after */
	std::map<std::string, uint64_t> seg_key;
	int close_extends = 0, close_shorter = 0, close_differs = 0, close_changed_shown = 0;
	/* close outcomes: the final continued the shown text / the shown text was
	 * kept over a different final / the final replaced (part of) it */
	int out_extends = 0, out_kept = 0, out_took = 0;
	std::vector<std::string> kept_examples, took_examples;
	std::map<uint64_t, std::string> key_final;

	for (uint64_t now = 0; now <= last; now += tick) {
		while (next_event < events.size() && events[next_event].t_ns <= now) {
			const Json &e = events[next_event].ev;
			const std::string type = e.str("type");
			const std::string seg = e.str("segment_id");
			const bool is_close = type == "transcript.final" ||
					      (type == "transcript.stable" && e.str("state") != "open");
			const bool is_text = type == "transcript.partial" || type == "transcript.stable" ||
					     type == "transcript.final";
			std::map<uint64_t, Shown> before;
			if (is_text)
				before = shown_lines(sim.state, cfg.tail);
			const double idx = e.num("segment_index", -1);
			const uint64_t seg_index = idx >= 0 ? (uint64_t)idx : TEA_CAPTION_SEGMENT_INDEX_UNKNOWN;
			if (type == "session.started") {
				session = e.str("session_id");
				/* the trace session asks for stable captions exactly like the plugin */
				stable = true;
				tea_caption_state_set_stable_mode(sim.state, stable);
			} else if (type == "transcript.partial") {
				tea_caption_state_on_partial(sim.state, session.c_str(), seg.c_str(),
							     (uint64_t)e.num("revision", 0), e.str("text").c_str());
			} else if (type == "transcript.stable") {
				tea_caption_state_on_stable(sim.state, session.c_str(), seg.c_str(), seg_index,
							    (uint64_t)e.num("stable_revision", 0),
							    e.str("text").c_str(), e.str("state").c_str());
			} else if (type == "transcript.final") {
#ifdef TEA_CAPTION_STATE_HAS_HIDING
				if (!e.str("audio_class").empty())
					tea_caption_state_on_audio_class(sim.state, session.c_str(), seg.c_str(),
									 seg_index, e.str("audio_class").c_str(),
									 TEA_AUDIO_CLASS_REVISION_FINAL);
#endif
				tea_caption_state_on_final_indexed(sim.state, session.c_str(), seg.c_str(), seg_index,
								   (uint64_t)e.num("revision", 1),
								   e.str("text").c_str());
			} else if (type == "segment.skipped" || type == "segment.error") {
				tea_caption_state_on_segment_dropped(sim.state, session.c_str(), seg.c_str());
			}
#ifdef TEA_CAPTION_STATE_HAS_SERVER_PROGRESS
			else if (type == "audio.ack" && !cfg.ignore_acks) {
				tea_caption_state_on_audio_ack(sim.state, (uint64_t)e.num("received_sample", 0));
			}
#endif
#ifdef TEA_CAPTION_STATE_HAS_HIDING
			else if (type == "segment.audio_class") {
				tea_caption_state_on_audio_class(sim.state, session.c_str(), seg.c_str(), seg_index,
								 e.str("class").c_str(),
								 (uint64_t)e.num("revision", 0));
				if (e.str("class") == "singing")
					singing_events++;
			}
#endif
			if (is_text) {
				std::map<uint64_t, Shown> after = shown_lines(sim.state, cfg.tail);
#ifndef TEA_REPLAY_LEGACY
				for (const auto &kv : after) {
					auto b = before.find(kv.first);
					if (b == before.end() || b->second.activity != kv.second.activity)
						seg_key[seg] = kv.first;
				}
#endif
				auto k = seg_key.find(seg);
				if (is_close && k != seg_key.end() && before.count(k->second)) {
					const std::string &d = before[k->second].text;
					const std::string f = e.str("text");
					const std::string res = after.count(k->second) ? after[k->second].text
										       : std::string();
					const char *rel;
					if (starts_with(f, d)) {
						rel = "extends shown";
						if (type == "transcript.final")
							close_extends++;
					} else if (starts_with(d, f)) {
						rel = "SHORTER than shown";
						if (type == "transcript.final")
							close_shorter++;
					} else {
						rel = "DIFFERS from shown";
						if (type == "transcript.final")
							close_differs++;
					}
					if (!starts_with(res, d))
						close_changed_shown++;
					if (type == "transcript.final") {
						key_final[k->second] = f;
						tea_norm_t nd, nf;
						tea_norm_build(d.data(), d.size(), &nd);
						tea_norm_build(f.data(), f.size(), &nf);
						char buf[1024];
						std::snprintf(buf, sizeof(buf),
							      "seg %lld: shown [%s] final [%s] -> [%s]",
							      (long long)e.num("segment_index", -1), d.c_str(),
							      f.c_str(), res.c_str());
#ifdef TEA_CAPTION_STATE_HAS_LAST_CLOSE
						const int outcome = tea_caption_state_last_close(sim.state);
#else
						/* older state machine: infer from what the line shows */
						const int outcome = tea_norm_starts_with(&nf, &nd) ? 1
												   : (res == d ? 2 : 3);
#endif
						if (outcome == 1) {
							out_extends++;
						} else if (outcome == 2) {
							out_kept++;
							kept_examples.push_back(buf);
						} else {
							out_took++;
							took_examples.push_back(buf);
						}
					}
					std::printf(
						"   %8.3fs  CLOSE %-17s seg %lld %s: shown [%s] server [%s] -> now [%s]%s\n",
						(double)now / 1e9, type.c_str(), (long long)e.num("segment_index", -1),
						rel, d.c_str(), f.c_str(), res.c_str(),
						starts_with(res, d) ? "" : "  <-- shown text changed");
				}
			}
			next_event++;
		}

		if (cfg.width2 > 0 && now >= cfg.width_change_ns)
			sim.cfg.width = cfg.width2;
		sim.sync(now);
		sim.sync_progress(now);
		sim.expire(now);
		std::vector<RowView> rows = sim.layout(now);
		std::map<uint64_t, std::string> visible = visible_by_line(rows);

		/* flags: a line whose segment is open shows less than before */
		for (const auto &kv : prev_visible) {
			const uint64_t key = kv.first;
			const std::string &before = kv.second;
			auto line_it = sim.lines.find(key);
			const bool open_now = line_it != sim.lines.end() && line_it->second.open;
			const bool was_open = prev_open[key];
			const std::string after = visible.count(key) ? visible[key] : std::string();
			if (starts_with(after, before))
				continue;
			if (!was_open && !open_now)
				continue; /* a closed line leaving the screen is the normal end of a sentence */
			std::string why;
#ifdef TEA_CAPTION_STATE_HAS_HIDING
			if (line_it != sim.lines.end() && line_it->second.meta.hidden) {
				/* intended: singing / paused, not a mid-speech failure */
				flags_hidden_by_class++;
				std::printf("   %8.3fs  HIDDEN (singing)  [%s] -> [%s]\n", (double)now / 1e9,
					    before.c_str(), after.c_str());
				continue;
			}
#endif
			if (line_it == sim.lines.end()) {
				flags_other++;
				why = "line left the caption state while open";
			} else {
				const std::string &d0 = prev_display[key];
				const std::string &d1 = line_it->second.text;
				if (was_open && !open_now && !starts_with(d1, d0)) {
					final_shrinks++;
					why = "final-correction (segment closed with different text)";
				} else if (!starts_with(d1, d0) && starts_with(d0, d1)) {
					flags_hidden++;
					why = "RETRACT: tail hidden/shortened";
				} else if (!starts_with(d1, d0)) {
					flags_rewrite++;
					why = "tail-rewrite: unconfirmed words corrected";
				}
				if (line_it->second.meta.visible_from > prev_visible_from[key] &&
				    tea_display_line_has_visible_text(&line_it->second.meta)) {
					flags_evict++;
					why += why.empty() ? "ROW-LIMIT: an open line lost a row" : " + ROW-LIMIT";
				}
				if (!tea_display_line_has_visible_text(&line_it->second.meta) || after.empty()) {
					flags_fade++;
					why += why.empty() ? "FADE-OUT while the segment is open" : " + FADE-OUT";
				}
				if (why.empty()) {
					flags_other++;
					why = "other";
				}
			}
			std::printf("!! %8.3fs  %s  [%s] -> [%s]\n", (double)now / 1e9, why.c_str(), before.c_str(),
				    after.c_str());
		}

		/* bursts: how many characters appear in one frame that were not on
		 * screen the frame before, per line: the characters of the visible
		 * text that an alignment with the previous frame's visible text does
		 * not account for (longest common subsequence). Text that only
		 * scrolled to another row, or that stayed while a word before it was
		 * corrected, is not new. */
		size_t appeared = 0;
		for (const auto &kv : visible) {
			const std::string &before = prev_visible.count(kv.first) ? prev_visible[kv.first]
										 : std::string();
			appeared += new_chars(before, kv.second);
		}
		if (appeared > max_burst) {
			max_burst = appeared;
			max_burst_t = now;
		}
		if (appeared >= 8)
			bursts_over_8++;

		/* shown text never moves: while a line's text only grows, every row
		 * it showed keeps its start and its text (the last one may grow) */
		{
			std::map<uint64_t, std::vector<PrevRow>> cur_rows;
			for (const RowView &r : rows)
				cur_rows[r.key].push_back({r.start, r.text});
			for (const auto &kv : prev_rows) {
				auto line_it = sim.lines.find(kv.first);
				if (line_it == sim.lines.end() ||
				    !starts_with(line_it->second.text, prev_display[kv.first]))
					continue; /* gone, or text rewritten (counted above) */
				const std::vector<PrevRow> &old_rows = kv.second;
				const std::vector<PrevRow> &new_rows = cur_rows[kv.first];
				for (size_t i = 0; i < old_rows.size(); i++) {
					if (old_rows[i].start < line_it->second.meta.visible_from)
						continue; /* scrolled out whole (row limit) or faded */
					bool ok = false;
					for (const PrevRow &n : new_rows) {
						if (n.start != old_rows[i].start)
							continue;
						ok = i + 1 < old_rows.size() ? n.text == old_rows[i].text
									     : starts_with(n.text, old_rows[i].text);
					}
					if (!ok) {
						layout_moves++;
						std::printf(
							"!! %8.3fs  LAYOUT MOVE: row [%s] of an unchanged prefix moved\n",
							(double)now / 1e9, old_rows[i].text.c_str());
					}
				}
			}
			prev_rows.swap(cur_rows);
		}
		/* row statistics, punctuation first shown, breaks first shown */
		size_t span = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			const RowView &r = rows[i];
			if (r.alpha <= 0.05f)
				continue;
			size_t chars = utf8_chars(r.text);
			/* a run that continues from the previous row of the same line
			 * across a width wrap */
			const bool continues = i > 0 && rows[i - 1].key == r.key && !rows[i - 1].punct_break &&
					       !rows[i - 1].soft_break && rows[i - 1].alpha > 0.05f;
			span = continues ? span + chars : chars;
			if (span > span_max)
				span_max = span;
			if (r.soft_break && i + 1 < rows.size() && rows[i + 1].key == r.key)
				soft_seen[std::to_string(r.key) + ":" + std::to_string(r.end)] = true;
			row_chars_sum += (double)chars;
			row_samples++;
			if (chars > row_chars_max)
				row_chars_max = chars;
			if (r.punct_break && i + 1 < rows.size() && rows[i + 1].key == r.key) {
				const std::string &text = sim.lines[r.key].text;
				std::string id = std::to_string(r.key) + ":" + text.substr(0, r.end);
				if (!breaks_seen.count(id))
					breaks_seen[id] = {r.key, r.end, text.substr(0, r.end), now,
							   r.end <= sim.lines[r.key].meta.locked_len};
			}
		}
		for (const auto &kv : sim.lines) {
			size_t shown_end = 0;
			for (const RowView &r : rows)
				if (r.key == kv.first && r.alpha > 0.05f && r.end > shown_end)
					shown_end = r.end;
			std::vector<Snap> &h = shown_history[kv.first];
			if (h.empty() || h.back().text != kv.second.text || h.back().shown_end != shown_end)
				h.push_back({now, kv.second.text, shown_end});
			last_seen[kv.first] = kv.second;
		}

		std::string screen = render_screen(rows);
		if (screen != prev_screen && !cfg.quiet)
			std::printf("   %8.3fs  %s\n", (double)now / 1e9, screen.empty() ? "(empty)" : screen.c_str());
		prev_screen = screen;
		prev_visible = visible;
		prev_display.clear();
		prev_open.clear();
		prev_visible_from.clear();
		for (const auto &kv : sim.lines) {
			prev_display[kv.first] = kv.second.text;
			prev_open[kv.first] = kv.second.open;
			prev_visible_from[kv.first] = kv.second.meta.visible_from;
		}
	}

	/* rows per segment: the whole text of each line, wrapped as if no row had left */
	int segs = 0, rows_total = 0, rows_max = 0;
	for (auto &kv : last_seen) {
		Line line = kv.second;
		line.meta.visible_from = 0;
		tea_wrap_row_t ring[32];
		int n = sim.wrap(line.meta, line.text, ring);
		if (n <= 0)
			continue;
		segs++;
		rows_total += n;
		if (n > rows_max)
			rows_max = n;
	}
	/* how long after its punctuation was first on screen did each break show */
	int breaks = 0, within_1s = 0, at_committed = 0;
	uint64_t latency_max = 0;
	std::vector<uint64_t> latencies;
	for (const auto &kv : breaks_seen) {
		const Break &b = kv.second;
		uint64_t first = b.t;
		for (const Snap &h : shown_history[b.key]) {
			if (h.shown_end >= b.end && starts_with(h.text, b.prefix)) {
				first = h.t;
				break;
			}
		}
		uint64_t latency = b.t - first;
		latencies.push_back(latency);
		breaks++;
		at_committed += b.committed ? 1 : 0;
		if (latency <= 1000 * TEA_NS_PER_MS)
			within_1s++;
		if (latency > latency_max)
			latency_max = latency;
	}
	std::sort(latencies.begin(), latencies.end());
	std::printf(
		"# soft breaks: %s (after %d characters, a pause > %d ms or at %d) | soft breaks shown=%zu | longest "
		"run between punctuation/soft breaks=%zu chars\n",
		cfg.soft ? "on" : "off", cfg.soft_min, TEA_SOFT_GAP_MS_OR_0, cfg.soft_min + TEA_SOFT_HARD_EXTRA_OR_0,
		soft_seen.size(), span_max);
	std::printf("# rows: avg visible chars/row=%.1f max=%zu | rows per segment avg=%.2f max=%d (%d segments) | "
		    "punctuation breaks=%d (mark already committed=%d, still in the tail=%d), shown within 1 s of the "
		    "mark=%d, median %.0f ms, max %.0f ms | layout moves=%d\n",
		    row_samples ? row_chars_sum / (double)row_samples : 0.0, row_chars_max,
		    segs ? (double)rows_total / segs : 0.0, rows_max, segs, breaks, at_committed, breaks - at_committed,
		    within_1s, latencies.empty() ? 0.0 : (double)latencies[latencies.size() / 2] / 1e6,
		    (double)latency_max / 1e6, layout_moves);
	/* duplication: a line that showed a normalised run of 4+ characters more
	 * often than its segment's final has it */
	int dup_lines = 0;
	std::vector<std::string> dup_examples;
	for (const auto &kv : shown_history) {
		auto f = key_final.find(kv.first);
		if (f == key_final.end())
			continue;
		tea_norm_t nf;
		tea_norm_build(f->second.data(), f->second.size(), &nf);
		for (const Snap &h : kv.second) {
			tea_norm_t nt;
			tea_norm_build(h.text.data(), h.text.size(), &nt);
			if (tea_norm_has_new_repeat(&nt, &nf, 4)) {
				dup_lines++;
				dup_examples.push_back(h.text + "  (final: " + f->second + ")");
				break;
			}
		}
	}
	std::printf("# duplication: lines that showed a repeated 4+ character run their final does not have=%d\n",
		    dup_lines);
	for (const std::string &x : dup_examples)
		std::printf("#   dup: %s\n", x.c_str());
	std::printf("# close outcomes: final continued the shown text=%d, shown tail kept over a different final=%d, "
		    "final replaced the tail=%d\n",
		    out_extends, out_kept, out_took);
	for (const std::string &x : kept_examples)
		std::printf("#   kept: %s\n", x.c_str());
	for (const std::string &x : took_examples)
		std::printf("#   took: %s\n", x.c_str());
	std::printf("# close audit: finals that extend what was shown=%d, shorter=%d, different=%d; "
		    "closes that changed shown text=%d\n",
		    close_extends, close_shorter, close_differs, close_changed_shown);
#ifdef TEA_CAPTION_STATE_HAS_HIDING
	std::printf(
		"# singing: segment.audio_class singing events=%d, singing segments=%llu, hide=%s, lines hidden while "
		"on screen=%d (faded out=%d, never shown text=%d), shown again after a revision=%d, open-line changes "
		"from hiding=%d\n",
		singing_events, (unsigned long long)tea_caption_state_singing_segments(sim.state),
		cfg.singing_show ? "off (--singing show)" : "auto", sim.hidden_started, sim.hidden_faded,
		sim.hidden_started - sim.hidden_faded, sim.shown_again, flags_hidden_by_class);
#endif
	std::printf("# summary: open-line shrink events: fade-out=%d row-limit=%d retract(tail hidden/shortened)=%d "
		    "tail-rewrite=%d other=%d | final corrections=%d | largest burst=%zu chars at %.3fs, "
		    "frames with >=8 new chars=%d\n",
		    flags_fade, flags_evict, flags_hidden, flags_rewrite, flags_other, final_shrinks, max_burst,
		    (double)max_burst_t / 1e9, bursts_over_8);
	tea_caption_state_destroy(sim.state);
	return 0;
}
