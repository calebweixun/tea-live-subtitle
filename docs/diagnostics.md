# 診斷：字幕沒有出來的時候

字幕停住的原因有四種，外掛與 server 各自記錄一半：

1. 音訊沒有離開 OBS（音訊來源被靜音、停用、移除，或擷取停了）；
2. 音訊送出去了，但幾乎是靜音（來源音量被關小，或擷取到的是別的音軌）；
3. 音訊有聲音，但 server 的語音偵測（VAD）不認為是語音（音樂、噪音、語言不符）；
4. 連線或 server 卡住了（有送音訊，server 卻沒有任何回應）。

外掛在 OBS 記錄檔裡寫出前兩種與第四種的證據，server 的心跳則寫出第三種。兩邊對照，就能知道問題出在哪一段。

## OBS 記錄檔裡的外掛訊息

每一行都以 `[tea-live-subtitle]` 開頭（macOS：`~/Library/Application Support/obs-studio/logs/`，
也可以從 OBS「說明 → 記錄檔」開啟）。

**連線狀態**：只在狀態改變時寫一行，附上原因。

```
asr-client: connection connecting: http://127.0.0.1:8327
asr-client: capabilities: partial_transcripts=1 stable_transcripts=1 segmentation=300-3000 ms
asr-client: connection connected: WebSocket open to 127.0.0.1:8327
asr-client: connection session active: 127.0.0.1:8327 sent {"audio":{...},"segmentation":{"end_silence_ms":870},"stable":{"agreement":2},...}; server: transcript_mode=revisable endpoint_silence_ms=870 min_interval_ms=300
asr-client: connection reconnecting: connection closed; retry in 1.0 s (last close code 1011)
asr-client: connection waiting for an audio source: no audio source selected for this caption source
```

`session active` 那一行是實際送出的 `session.start`，以及 server 回報實際生效的切段靜音與預覽間隔。

**音訊來源**：

```
audio-tap: capturing audio source 'macOS 螢幕擷取'
audio-tap: receiving audio from 'macOS 螢幕擷取': 48000 Hz, 2 channel(s) -> downmixed to mono, resampled to 16000 Hz
```

第二行在第一次收到音訊時才出現；只有第一行、沒有第二行，表示這個來源根本沒有送出音訊。

**心跳**：連線中每 10 秒一行。

```
asr-client: heartbeat 10.0s: sent 10026 ms audio in 286 frames, max gap 53 ms, level rms -15.0 dBFS peak -10.5 dBFS; events partial=26 stable=25 final=2 speech.started=3 segment.queued=2 audio.ack=199 other=3; last text 0.2 s ago; speech state speech detected
```

| 欄位 | 意思 |
|---|---|
| `sent N ms audio in F frames` | 這 10 秒送出多少音訊（正常時約 10000 ms） |
| `max gap` | 兩次送出之間最長的空檔（正常約 50 ms） |
| `level rms / peak` | 實際送出的音訊音量，已經過外掛的降為單聲道與重新取樣到 16 kHz；數位靜音是 -120 dBFS |
| `events ...` | 這 10 秒收到的各種 server 事件數；`audio.ack` 每 100 ms 左右一則，是 server 還活著的證據 |
| `last text` | 距離最後一則 partial／stable／final 多久 |
| `speech state` | `listening`（沒有語音）、`speech detected`（server 的 VAD 在語音中）、`segment processing`（一句已切出、辨識中） |

**警告**：每一種最多每 30 秒一行。

| 訊息 | 條件 | 通常代表 |
|---|---|---|
| `WARN no audio sent for N ms while connected` | 連線中超過 3 秒沒有送出任何音訊 | 音訊來源被停用、移除、場景切掉，或擷取停了 |
| `WARN audio is being sent but has been below -60 dBFS for N ms` | 有送音訊，但超過 10 秒都低於 -60 dBFS | 來源被靜音（靜音時外掛送的是靜音）、音量推桿關小、擷取到空的音軌 |
| `WARN N ms of audible audio sent without speech.started or any transcript -- the server hears no speech` | 送出累計 15 秒以上有聲音的音訊，server 卻沒有回報任何語音或文字 | server 的 VAD 不認為那是語音：音樂、背景音、語言不符 |
| `WARN sending audio but no event from the server for N ms` | 在送音訊，但超過 5 秒沒有收到任何 server 事件（連 `audio.ack` 都沒有） | 連線或 server 卡住 |

`speech.started` 與 `segment.queued` 用來追蹤上面的語音狀態，不再寫成 `unhandled event type`。

## 畫面上的診斷列

來源屬性「在畫面上顯示診斷列（除錯用）」（`diag_overlay`，外觀類設定，預設關閉）會在字幕下方畫一行小字：

```
診斷: 辨識中 | 聆聽中 | 輸入 -23 dBFS | 上次出字 4 秒前
```

依序是連線狀態、語音狀態、目前輸入音量（最近約 0.5 秒從音訊來源取到的音訊；沒有音訊時顯示「沒有音訊輸入」）、
距離上次出字的秒數。這一行不淡出、不在文字底色裡，也不算在行數上限裡。建議在預覽用的場景打開，直播場景關閉。

## 辨識事件記錄檔

來源屬性「記錄辨識事件（除錯用）」（`event_trace`，連線類設定，按「套用連線設定」後生效，預設關閉）會把每一次連線的
所有 server 事件寫成一個檔案：

* 位置：OBS 的外掛設定資料夾下的 `traces/`（macOS：
  `~/Library/Application Support/obs-studio/plugin_config/tea-live-subtitle/traces/`），檔名
  `tea-trace-<日期時間>-<session 前 8 碼>.jsonl`；開始時會在 OBS 記錄檔寫出完整路徑：
  `asr-client: event trace for session ...: <path> (up to 32 MB)`。
* 格式：每行一個 `{"t_ms": <距 hello 的毫秒>, "event": {...server 事件...}}`，第一行是
  `{"t_ms": 0, "meta": {...送出的 session.start...}}`。和 `tests/replay` 讀的格式相同。
* 每個檔案最多 32 MB，到上限就停止寫入並在記錄檔說明。檔案裡有辨識出的文字，不含音訊、不含 token。

字幕不見時，把那段時間的記錄檔和 OBS 記錄檔一起傳給我們，就能用重播工具（`tests/replay/README.md`）還原當時畫面上每一刻顯示了什麼。

## 怎麼判斷「沒有字幕」是哪一段出問題

把外掛的心跳（每 10 秒）和 server 的心跳（`stream.heartbeat`，每 5 秒，含收到音訊的 RMS／peak dBFS、
最長接收空檔、VAD 機率的最大值／平均／語音比例；server 的 W11 診斷日誌，docs/04「W11｜串流診斷日誌」）對照：

| 外掛（OBS 記錄檔／診斷列） | server 記錄 | 結論 |
|---|---|---|
| `WARN no audio sent`，心跳 `sent 0 ms audio`，診斷列「沒有音訊輸入」 | 接收空檔很長（`stream.audio_stalled`） | 音訊沒有離開 OBS：檢查音訊來源是否還在、是否被停用、場景是否切掉 |
| `WARN ... below -60 dBFS`，心跳 `rms -120.0 dBFS` 或很低 | 收到的 RMS 同樣很低（`stream.audio_quiet`） | 送出去的是靜音：來源被靜音、音量關小，或擷取到的不是有聲音的那一路 |
| 心跳音量正常（例如 -30 ～ -10 dBFS）、`speech.started=0`，`WARN ... server hears no speech` | 收到的音量正常，VAD 機率很低（`stream.vad_no_speech`） | 音訊到了 server，但 VAD 不認為是語音：音樂、噪音或語言不符；字幕停住是預期的 |
| 心跳音量正常、`speech.started` 有增加、`last text` 很久 | 有切出語音段，但 final／preview 很慢或失敗 | server 端辨識慢或失敗：看 server 的 `stream.segment_done`、`stream.preview`、`worker_busy` |
| `WARN sending audio but no event from the server`，`audio.ack=0` | server 沒有這段時間的心跳 | 連線或 server 卡住：看 server 是否還在執行、網路是否中斷 |
| 外掛心跳與 server 都正常、有 `speech.started` 也有文字，但畫面上沒有字 | — | 顯示端問題：打開診斷列確認「上次出字」，再用事件記錄檔與重播工具檢查淡出、行數上限等設定 |
