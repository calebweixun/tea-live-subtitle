# Caption replay

Offline replay of a recorded server event trace through the plugin's real caption state machine
(`src/caption-state.c`) and per-line display policy (`src/caption-display.h`). It needs no OBS, no GPU and no server.
It prints the screen over time and flags every moment an open sentence's visible text shrinks or vanishes. The
causes it reports are fade-out, row limit, a retracted tail, or a corrected tail. It also audits every
sentence end (did the final extend what was shown?) and reports the largest amount of text that appears in one frame.

A trace is JSONL, one `{"t_ms": <client receive time, ms>, "event": {...server event...}}` per line. The
server repo's `benchmarks/capture_event_trace.py` writes this format, and so does the plugin's "record recognition
events" setting (`docs/diagnostics.md`).

```sh
cc  -std=c11   -c -Itests/stubs -Isrc src/caption-state.c -o /tmp/caption-state.o
c++ -std=c++17 -Itests/stubs -Isrc tests/replay/caption-replay.cpp /tmp/caption-state.o -o /tmp/caption-replay
/tmp/caption-replay real-900.jsonl --fade-delay-ms 1500 --fade-ms 200 --max-rows 2 --max-lines 3 --width 1800
```

The defaults are the settings of the live report: fade-out after 1500 ms, 200 ms fades, 2 rows, 3 sentences,
1800 px, a 48 px font, black outline and shadow, and unconfirmed text shown. `--quiet` prints only the flags and the
summary. `--punct off|sentence|comma` and `--comma-min N` select punctuation line breaks.

The `# rows` line reports visible characters per row, rows per segment, and punctuation breaks: how many happened, whether
the mark was committed or still in the tail, and how long after the mark first showed. It also counts layout moves:
a row of a line whose text only grew that lost or changed characters, which must be 0.
The `# duplication` line counts lines that at some moment showed the same 4+ character run twice (after
normalising away punctuation and spaces) when that segment's final does not repeat it; each is listed with a
`#   dup:` line. The `# close outcomes` line splits sentence ends into finals that continued the shown text, shown
tails kept over a different final, and finals whose remainder replaced the tail, with examples. The largest burst
counts only characters that are new in a frame (outside the longest common subsequence with the previous frame), so
scrolling a row away does not count.
`audio.ack` events feed the server-progress signal that keeps a still-open line up (the plugin's 10 s open-line
timeout); `--ignore-acks` replays as if the server sent none, so only the timeout applies.
`--selftest-width-change W MS` switches the box width at MS as a real reflow, to check that the detector notices moves.

To compare with an older plugin, extract its `src/` and `tests/stubs/` (for example with
`git archive <rev> src tests/stubs`). Build the same tool against that tree with `-DTEA_REPLAY_LEGACY`. That
selects the API from before the live fixes: gap-based line breaks (`--pause-ms`, default 870) and no activity
tracking.

Glyph widths come from a simple font model: CJK characters are one font size wide, ASCII half of that, a space a
quarter. That is enough to reproduce wrapping and the row limit, but it is not pixel-exact.
