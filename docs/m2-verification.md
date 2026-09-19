# M2 驗收報告：tea-live-subtitle

- 驗收對象：commit `7430462`（M2 實作）＋ `baec29a`（CI 修復），對照基準 `a0dfd1c`（M1）
- 規格來源：`tea-asr-service` repo 的 `docs/08-obs-plugin.md`、`docs/04-api.md`、`docs/03-architecture.md`
- 驗收方式：純程式碼閱讀 + GitHub Actions 結果比對。**未實際跑 OBS、未跑本機建置**，見文末「無法驗證」清單。

## 結論先講：不建議直接放行 M3

CI 綠燈、核心狀態機正確，但有一項**違反 server 容量限制**的架構偏離（多來源會壞掉且無法自我診斷）與一項**掉幀計數永遠回報 0（形同假造安全訊號）**，都直接對應規格「驗收條件」第4、5條，必須先修。另有一個生命週期上的潛在死鎖風險與一個沒有更新的舊版全域設定視窗。細節如下。

---

## 逐項主張查證

### 主張1：音訊擷取用 `obs_source_add_audio_capture_callback`，callback 內只做降混＋寫 ring buffer，resample 延後到 worker thread
**屬實。**
- `src/audio-tap.c:140-153`：`tea_audio_tap_set_source` 掛/卸 `obs_source_add_audio_capture_callback`。
- `src/audio-tap.c:55-116`（`tea_on_audio_capture`）：只做 `obs_get_audio_info`（僅第一次呼叫，取樣率設定；非阻塞、非I/O）、堆疊上的 `mono_chunk[512]` 降混（純算術，`sum/=channels`），以及呼叫 `tea_ring_buffer_write`。無 `malloc`/`bmalloc`、無 log I/O、無網路呼叫。
- resample（`audio_resampler_create`/`audio_resampler_resample`）只出現在 `tea_audio_tap_pull_pcm16`（`audio-tap.c:156-199`），該函式文件明確標注「call only from a single worker thread」，且實際由 `asr-client.cpp:641-662`（`pumpAudio`，跑在 `TeaAsrClient` 自己的 `QThread`）呼叫。callback 本身完全不碰 resample。
- `muted` 分支（`audio-tap.c:76-81`）改送等量靜音樣本以維持 sample clock 連續，符合規格第34-49行要求。

小瑕疵（非阻塞性問題）：`tea_on_audio_capture` 每次呼叫會依 512-sample chunk 迴圈呼叫 `tea_ring_buffer_write`（每個 chunk 各自 `trylock`），commit message 說「搶不到鎖就丟棄整批樣本」實際上是丟棄該次呼叫中「未能上鎖的那些 512-sample chunk」，不是一次性丟棄整個 callback 的所有樣本；差異對正確性無影響，只是敘述略誇大。

### 主張2：ring buffer 從 C11 atomics 改 `pthread_mutex_trylock`，音訊執行緒搶不到鎖就丟棄，絕不等待
**基本屬實，且逐行確認無阻塞路徑。**
- `src/ring-buffer.h:85-107`（`tea_ring_buffer_write`，producer/audio thread 專用）：`pthread_mutex_trylock` 失敗立刻 `return`（`ring-buffer.h:92-98`），不重試、不 spin、不等待。
- consumer 端 `tea_ring_buffer_read`（`ring-buffer.h:126-138`）才用一般 `pthread_mutex_lock`（可能短暫阻塞），但這是 worker thread，不是即時音訊執行緒，符合規格「音訊 callback 只寫 ring buffer」的分工。
- 丟棄計數：producer 端搶不到鎖時計入 `pending_contention_drops`（thread-local、無競爭，`ring-buffer.h:96`），下次成功拿到鎖時才併入 `dropped_samples`（`ring-buffer.h:100-103`）。**潛在問題**：若鎖長期無法取得（理論上不該發生，因為 consumer 只在 memcpy 等級時間持鎖），`pending_contention_drops` 會持續累積但不會反映到對外可見的 `dropped_samples`，直到下一次成功上鎖為止——不是「絕不可見」，但有短暫延遲視窗，屬低風險、可接受。
- 緩衝滿了丟最舊樣本並計數：`ring-buffer.h:66-80`（`tea_ring_buffer_write_locked`）正確地把 `read_pos` 往前推、扣 `fill`、加 `dropped_samples`。

### 主張3：手寫 RFC 6455 是否正確
**框架層級（framing）本身寫得對；handshake 驗證確實有做；但有幾個非嚴重的協定/穩健性缺口。**

逐項對照 `src/asr-client.cpp`：
- **Masking**：outgoing frame 一律加 4-byte random mask 並 XOR payload（`asr-client.cpp:358-369`），header 的長度 byte 一律 `0x80 | len`（`asr-client.cpp:347-355`），符合 RFC「client→server 必須 masked」。Incoming frame 對 masked/unmasked 都能解（`asr-client.cpp:414, 432-448`）——沒有主動拒絕「伺服器不該 mask 卻 mask 了」的情況，屬於寬容而非嚴格驗證，不算錯誤。
- **Payload length 7/16/64-bit 三種編碼**：outgoing（`asr-client.cpp:346-356`）與 incoming（`asr-client.cpp:415-430`）都完整處理 125/126(16-bit BE)/127(64-bit BE) 三種情形，位元組序（big-endian，符合 RFC）正確。
- **Fragmentation（分片重組）**：`processIncomingWsBytes`（`asr-client.cpp:404-472`）對 opcode 0x0（continuation）會 append 進 `fragPayload_`，`fin` 才觸發 `handleWsFrame`；首個分片幀（opcode 0x1/0x2 且 `fin=false`）正確記錄 `fragOpcode_`。邏輯正確。**缺口**：沒有處理「連續兩個非 continuation 起始幀（尚未收到前一個的 fin）」這種協定違規（理論上伺服器不會這樣做，屬於防禦性缺口而非會被觸發的 bug）。
- **Control frame（ping/pong/close）**：收到 `0x9`(ping) 立即回 `0xA`(pong)（`asr-client.cpp:494-496`），符合規格「15秒 server WS ping，30秒無 pong 切斷」需要 client 及時回應。**缺口**：收到伺服器主動送出的 `0x8`(close) 時，只呼叫 `disconnectFromHost()`（`asr-client.cpp:490-493`），沒有依 RFC 6455 §5.5.1 回送 close frame 確認——嚴格說不合規，但因為緊接著就斷 TCP，實務上不太可能造成可觀察的問題（頂多讓伺服器端記錄一次「未收到 close ack」的日誌）。
- **Handshake／`Sec-WebSocket-Accept` 驗證**：`sendHandshakeRequest`（`asr-client.cpp:274-297`）確實產生隨機 16-byte key、算出預期 accept 值（`asr-client.cpp:281`），`tryConsumeHandshakeResponse`（`asr-client.cpp:299-334`）**真的比對** `acceptValue != wsAcceptExpected_`，不符就標記失敗並斷線（`asr-client.cpp:322-327`）。**這一項主張屬實，不是擺著好看的檢查。**
- **邊界風險（非本次規格要求但值得留意）**：`processIncomingWsBytes` 對 incoming payload 長度沒有任何上限檢查（`len` 可到 2^64），只要伺服器回一個聲稱超大 payload 的 frame header，`recvBuffer_` 就會無限累積等待湊滿——因為伺服器是受信任的本機服務，實務風險低，但仍是「沒做輸入驗證」的缺口。

### 主張4：16-byte header／`send_until_sample` 流控是否符合 docs/04
**符合。**
- Header 組裝：`appendLeU64(frame, nextSeq_)` 接著 `appendLeU64(frame, nextSample_)`（`asr-client.cpp:676-677`），`appendLeU64`（`asr-client.cpp:42-46`）逐 byte `(v >> 8*i) & 0xFF` 由低位元組先塞入 `QByteArray`——即 little-endian，欄位順序（seq 在前、start_sample 在後）與位元組序都符合 docs/04 第88行。
- `send_until_sample` 初值取自 `session.started`（`asr-client.cpp:525`），之後只在 `flow.control` 時 `if (v > sendUntilSample_) sendUntilSample_ = v;`（`asr-client.cpp:530-535`）——**確實不會回退**，符合規格「窗口不回退」。
- 超送防護：`pumpAudio`（`asr-client.cpp:667-685`）逐 chunk 檢查 `endSample > sendUntilSample_` 才 `break`，也就是任何一個 frame 的 `end_sample` 絕不會超過目前窗口——符合規格「任何 frame 的 end_sample 不得超過此值」。
- `seq`/`start_sample` 連續性：`nextSeq_++`、`nextSample_ = endSample`（`asr-client.cpp:682-683`）逐 frame 累加，且只有單一 `pumpAudio` 呼叫路徑會送出 binary frame，不會有 seq 跳號或重複。

### 主張5：字幕狀態機（`(session_id, segment_id)` 去重、遞增 revision、terminal 後拒絕 partial）
**核心邏輯屬實，但有一個因固定容量表格造成的邊界缺陷。**
- Terminal 後拒絕 partial：`caption-state.c:181-186`（`if (seg->terminal) return;`）。
- 只套用遞增 revision：`caption-state.c:187-192`（`revision <= seg->revision && seg->revision != 0` 則丟棄）。
- `segment.skipped`/`segment.error` 清掉暫定狀態、不留半句話：`tea_caption_state_on_segment_dropped`（`caption-state.c:234-253`）只標記 terminal、清 preview，不會塞入任何 finalized 文字。
- `session.cancelled` 清空未定稿預覽：`caption-state.c:255-265`。
- **缺陷**：segment 追蹤表 `TEA_MAX_TRACKED_SEGMENTS=16`（`caption-state.c:29`），用 round-robin 覆蓋最舊 slot（`caption-state.c:150-156`）。一個 continuous session 若持續超過約 16 個 segment 的生命週期（依 docs/04 的 `max_segment_ms=14000`，約每14秒一段，16 段約 3-4 分鐘），最舊的 segment 追蹤資訊就會被覆蓋。若此時因為任何原因（伺服器 bug、未來版本行為變化）收到針對某個已被淘汰、且早已定稿的舊 `segment_id` 的重複/遲到訊息，程式會把它當「全新 segment」（`revision=0`, `terminal=false`）重新處理，可能導致重複把同一段文字再 push 一次到畫面（`tea_push_finalized_locked`，`caption-state.c:158-168`）——即規格禁止的「回改/重複」情形在超過表格容量後失去保護。v0.1 wire 協定本身保證同一 session 內 final 只送一次、TCP 有序不重送，所以**目前條件下不會被觸發**，但這是一個容量寫死、沒有真正做「全 session 週期去重」的設計缺口，建議在長時間直播（規格驗收條件4：連續1小時）前擴大或改用真正的去重結構（例如 hashmap 或按 `segment_index` 嚴格遞增校驗）。

### 主張6：per-source 連線 vs. 規格建議單一全域連線——代價評估
**屬實，且代價比 commit message 描述的更嚴重：這是一個會直接違反 server 容量限制、且無法自我診斷的架構缺陷。**
- 確認架構：`captions-source.c:189-192` 在 `tea_captions_source_create` 裡，**每一個** `tea_live_subtitle_source` 實例各自呼叫 `tea_caption_state_create()`、`tea_audio_tap_create()`、`tea_asr_client_create()`——即每個 source 各開一條獨立 TCP + WebSocket 連線、各自握手、各自送 `session.start(profile="continuous")`。這與 `src/asr-client.h:16-21` 檔案開頭的註解**互相矛盾**：該註解寫著「There is exactly one of these per plugin instance (owned by tea-service.c)... matching docs/08's 'global connection settings, per-source appearance only' split」——但 repo 裡根本没有 `tea-service.c` 這個檔案（`ls src/` 確認），這段註解是失真/過時文件，會誤導之後接手的人。
- **對照 docs/04 capabilities 範例**：`limits.max_continuous_sessions=1`、`max_total_connections=5`（docs/04 第66行）。若使用者在同一個 scene collection 放兩個以上的字幕來源（例如雙語字幕、或給不同場景各放一個），第二個以後的 `session.start` 會被 server 依 docs/04 錯誤表（第162行 `queue_full／session_limit → 429 → start拒絕或flow pause；超配close1013`）拒絕或直接關閉連線。
- **程式碼對此完全沒有處理**：`handleJsonMessage` 收到 `type=="error"` 時只是 log + `setStatus("server error: <code>")`（`asr-client.cpp:581-592`），不會區分「session_limit 這種不會自己好的錯誤」跟「暫時性錯誤」；`onSocketDisconnected` 一律呼叫 `scheduleReconnect()`（`asr-client.cpp:702`），退避帽在 16 秒（`asr-client.cpp:192`）就會**無限期地每 16 秒重試一次**，永遠連不上、永遠占用一個 TCP 連線名額（同時逼近 `max_total_connections=5`），字幕來源會一直卡在「connecting/server error: session_limit」，使用者難以判斷是設定錯誤還是 server 掛了。
- 結論：per-source 架構在**多字幕來源**這個明顯會被使用者嘗試的場景下，是會直接壞掉且訊息不明確的設計，不只是「代價」，是規格驗收條件3（「server 不可用時 source 顯示明確狀態」）事實上也沒被滿足——這裡不是 server 不可用，是被 server 依協定正確拒絕，但 UI 呈現不出「這是容量問題，請只用一個字幕來源」這種可操作的訊息。

### 安全性查證
- **token 是否可能被記錄進 log**：`asr-client.cpp:39-40` 有明確自我提醒註解「NEVER pass token/PCM/prompt/transcript text to obs_log」。實際檢查所有 `obs_log` 呼叫（`asr-client.cpp:323, 491, 501, 584-585, 594`）：分別記錄「handshake 失敗」「server closed」「unknown opcode」「server error code+retryable」「unhandled event type」——**沒有一處把 token、PCM、prompt 或逐字稿文字帶入 log**。`readToken()`（`asr-client.cpp:247-265`）明確註解「Never log file contents」且程式碼確實沒有 log。**驗證通過，符合 docs/03 第122行的禁令。**
- **token 是否可能被存進 `obs_data` 而洩漏到 scene collection**：檢查 `captions-source.c` 全檔案，`token_path`（檔案路徑字串）確實被存進 `obs_data_t *settings`（`captions-source.c:131, 237, 278`）並因此會序列化進 scene collection——但這是**路徑**，不是 token 內容本身，符合規格原文「設定視窗要能覆寫路徑，但不要把 token 寫進 scene collection JSON」的字面要求（規格區分的是「token 內容」不可存，「路徑」可以存）。token 實際內容只在 `TeaAsrClient::readToken()` 讀出後留在記憶體，用於組 HTTP header（`asr-client.cpp:291-293`），從未經過任何 `obs_data_set_*` 呼叫。**未發現違規**，但這只是程式碼審查層級的驗證，並未實際存一份 scene collection 檔案出來 grep 確認（見文末「無法驗證」）。
- **PCM/prompt/transcript 是否被記錄**：同上，沒有任何 `obs_log` 呼叫帶入 PCM buffer、`text` 欄位或 transcript 內容。`caption-state.c` 完全不呼叫 `obs_log`。**符合規定。**

### 記憶體與生命週期
- **切換音訊來源**：`captions-source.c:138-147` 呼叫 `tea_audio_tap_set_source`，內部先 `obs_source_remove_audio_capture_callback` + `obs_source_release` 舊來源，再對新來源 `obs_source_get_ref` + `obs_source_add_audio_capture_callback`（`audio-tap.c:140-154`），全程在 `attach_lock` 保護下——**沒有發現 use-after-free 或參照計數錯誤**。
- **來源刪除／關閉場景**：`tea_captions_source_destroy`（`captions-source.c:198-226`）依序 `tea_asr_client_stop` → `tea_asr_client_destroy` → `tea_audio_tap_destroy` → `tea_caption_state_destroy` → 釋放 text child source → 釋放快取字串。順序正確：先停止會回呼 `captions`/`tap` 的 client，再釋放它們，避免 use-after-free。
- **潛在死鎖/卡死風險（未在此次程式碼中被排除，屬中高風險）**：`TeaAsrClient` 解構子（`asr-client.cpp:56-61`）：
  ```cpp
  QMetaObject::invokeMethod(this, "doStop", Qt::BlockingQueuedConnection);
  thread_.quit();
  thread_.wait(2000);
  ```
  第一行 `BlockingQueuedConnection` **沒有 timeout**，會讓呼叫端執行緒（也就是 OBS 呼叫 `tea_captions_source_destroy` 的那個執行緒——刪除來源/切場景時通常是 OBS 主執行緒）無限期等待 `thread_` 的事件迴圈把 `doStop()` 執行完。只要 `thread_` 的事件迴圈當下沒有在跑（理論上不該發生，但 Qt 物件在解構期間、或 OBS 關閉應用程式時的解構順序若恰好先停掉了事件迴圈，就可能發生），這裡就會永久卡住——對應使用者要求驗證的「OBS 不當機、不卡住」直接相關。第二行 `thread_.wait(2000)` 就算真的等超過 2 秒也不會強制終止執行緒或回報錯誤，只是靜默放行，讓 `TeaAsrClient`（連同其 `QThread` 成員）繼續被解構——若該執行緒此時仍在跑，等同解構一個還在執行中的 `QThread`，屬於 Qt 官方文件明確警告會造成程式崩潰的用法。**這是本次審查中最需要在 M3 前釐清/補強的生命週期風險**，雖然目前程式碼路徑下不容易主動觸發，但沒有任何機制阻止它在邊界情況（例如 OBS 關閉時的解構順序、或網路堆疊卡住的極端情況）發生。

### CI／建置查證
- `7430462` 首次推送後 CI **失敗**（run `35419972166`，2026-09-19T03:57:02Z，`conclusion: failure`），與其 commit message 中「Windows MSVC 對 C11 atomic 報錯」「巢狀 `server_*/` 讓註解提前結束導致三平台全滅」的描述一致（時間點吻合：`7430462` commit 時間 11:56，CI 執行於稍後）。
- `baec29a` 推送後 CI **全綠**（run `35420311911`，2026-09-19T04:04:35Z，`conclusion: success`），且確認涵蓋三平台：`Build for Ubuntu 🐧`、`Build for macOS 🍏`、`Build for Windows 🪟` 皆 success，另外 `clang-format`、`gersemi` 格式檢查也過。**主張屬實：CI 修復確實讓三平台建置通過。**

---

## 缺陷清單（依嚴重度排序）

1. **【高】多字幕來源會違反 server `max_continuous_sessions=1` 限制，且失敗時使用者無法判斷原因**（`captions-source.c:189-192`，`asr-client.cpp:581-592, 702`）。
   - 觸發條件：使用者在同一份 scene collection 裡加入第2個（或更多）`tea_live_subtitle_source`。
   - 後果：第2個以後的來源永遠連不上 server（被 429/session_limit 拒絕或被關閉），並以16秒週期無限重試，UI 只顯示模糊的「server error: xxx」或「reconnecting」，不會提示「請只用一個字幕來源」。這直接沖淡規格驗收條件3（server 狀態要明確）。

2. **【高】掉幀計數 API 永遠回傳 0，形同假造安全訊號**（`asr-client.cpp:768-772`：`tea_asr_client_dropped_audio_frames` 直接 `return 0;` 並附註「TODO: expose droppedPcmChunks_」）。
   - 觸發條件：ring buffer 溢位（`ring-buffer.h` 的 `dropped_samples`）或 asr-client 自己的 PCM 佇列溢位（`asr-client.cpp:657-660` 的 `droppedPcmChunks_`）實際發生時。
   - 後果：規格驗收條件4明確要求「掉幀計數為0或有明確原因」，但目前不管實際掉了多少幀，對外一律回報0——這比「沒做」更糟，因為使用者/測試者會誤信沒有掉幀。且 `tea_audio_tap_dropped_samples()`（ring buffer 層的掉幀數）根本沒有被任何呼叫路徑串到 UI 或這個 API 上。

3. **【中】`TeaAsrClient` 解構子的 `BlockingQueuedConnection` 呼叫沒有 timeout，理論上可造成 OBS 主執行緒永久卡死**（`asr-client.cpp:56-61`）。
   - 觸發條件：邊界情況下 `thread_` 事件迴圈未及時處理排入的 `doStop()`（例如 OBS 應用程式關閉時的物件解構順序、或系統資源枯竭）。
   - 後果：對應使用者要求驗證的「OBS 不當機、不卡住」要求，目前程式碼沒有防止最壞情況的機制。

4. **【中】全域設定視窗（Tools 選單）仍是 M1 的假 UI，未反映任何 M2 的連線狀態**（`src/settings-dialog.cpp:35-39` 的註解仍寫「Phase 1 does not connect to anything yet... phase 2」，`statusLabel` 永遠顯示「not connected」）。
   - 後果：規格第5節要求的「連線狀態、掉幀統計、重連按鈕、協定版本與 capabilities 顯示」全部未實作，且遺留的 Tools 選單項目會誤導使用者以為那裡是設定連線的地方。commit `7430462` 的 diff 完全沒有碰 `settings-dialog.cpp`，可見這不是刻意延後，而是被遺漏。

5. **【低】`asr-client.h` 檔頭註解描述與實際架構矛盾**（`asr-client.h:16-21` 提到「owned by tea-service.c」「exactly one of these per plugin instance」），但該檔案不存在、實際是 per-source 多實例。屬於文件債，容易誤導之後接手者，建議與 `captions-source.c:26-36` 的（正確、誠實揭露取捨的）註解對齊。

6. **【低】字幕去重表容量固定為16個 segment，超過後失去對舊 segment_id 的追蹤**（`caption-state.c:29, 150-156`），長時間直播下若出現任何遲到/重複的伺服器事件（目前 wire 協定理論上不會發生），可能導致重複顯示已定稿文字。建議在 M3 前評估是否需要擴大或改為真正跨 session 週期的去重。

7. **【低】WS 收到伺服器 close frame 未回送 close frame 確認**（`asr-client.cpp:490-493`），不完全符合 RFC 6455 §5.5.1，但因為緊接著斷 TCP，實務影響極小。

8. **【低】WS 接收端對 incoming frame payload 長度沒有上限檢查**（`asr-client.cpp:404-472`），信任本機 server 不會送出病態長度；如果之後這個 client 曾被考慮連到非本機/非受信任的 server，這裡需要補強。

---

## 放行建議

**不建議直接放行 M3。** 建議先處理：
- 缺陷1（多來源違反容量限制）：至少要做到「偵測到 session_limit 錯誤時，清楚地在 source 狀態顯示『已有其他字幕來源在使用中』，並停止無意義的無限重試」；長期應該重新考慮是否真的要放棄規格建議的全域單一連線架構。
- 缺陷2（掉幀計數造假）：至少把 `droppedPcmChunks_` 與 `tea_audio_tap_dropped_samples()` 接上這個 API，不要回傳寫死的0。
- 缺陷3（解構死鎖風險）：`BlockingQueuedConnection` 至少要有 fallback（例如改用有 timeout 的等待，或確認 thread 生命週期在所有解構路徑下都安全）。
- 缺陷4（設定視窗未更新）：要嘛把 Tools 選單的舊 dialog 補上規格要求的欄位，要嘽明確拿掉這個選單項目並在文件/commit message 說明改走 per-source 屬性面板的理由（目前狀態是兩邊都不完整）。

缺陷5-8 可以在 M3 過程中順帶處理，不必卡在 M2 驗收。

其餘查證的部分（音訊 callback 不阻塞、WS framing 正確性、flow control、字幕狀態機核心邏輯、token/PCM/prompt 不進 log、記憶體釋放順序、CI 三平台通過）**皆屬實**，是這次 M2 紮實的部分。

---

## 我無法驗證的項目（老實列出）

以下項目受限於「不可本機建置」「不可連真實 server」「需要真人在 OBS UI 操作」等限制，本次**完全沒有實測**，只能從程式碼推論：

1. **實機在 OBS 內的行為**：字幕是否真的顯示、文修訂觀感、`text_ft2_source`/`_v2` 是否真能在 32.2.1 建立、`obs_properties` 面板實際互動是否順暢——全部需要真人在 OBS UI 操作，我只讀了程式碼路徑。
2. **與真實 server 的協定互動**：flow control 在真實網路延遲/伺服器負載下的行為、`session.started`/`flow.control`/`transcript.*` 事件的真實時序、斷線重連後 server 端是否真的建立新 session——需要實機跑 `tea-asr-service` 並用 OBS 連線觀察封包，我只驗證了 client 端程式碼邏輯符合 docs/04 文字敘述。
3. **連續直播一小時的記憶體/延遲穩定性**（驗收條件4）：`pendingPcm_`、`fragPayload_`、`recvBuffer_`、caption-state 的 16-slot 表格等在長時間運行下的實際行為，只能從程式碼靜態推論不會無界增長，無法用工具（如 Valgrind/Instruments）實測，因為不能本機建置。
4. **scene collection 匯出檔實際內容**：我確認了程式碼路徑上 token 內容不會進 `obs_data`，但沒有實際跑一次 OBS、存一份 scene collection JSON 出來 grep 驗證（受限於不能本機建置/執行外掛）。
5. **Windows/Linux 平台上是否有本文未發現的平台特定行為差異**（例如 `pthread_mutex_trylock` 在 OBS 的 Windows `util/threading.h` 實作下的實際語意、`QStandardPaths::AppConfigLocation` 在各平台展開的實際路徑是否符合規格要求的 `~/Library/Application Support/TEA ASR/token`）——只在 macOS 分支（`Q_OS_MAC`）讀了程式碼，其餘平台路徑只能假設 `QStandardPaths` 行為符合預期。
6. **多字幕來源同時對真實 server 的行為**（缺陷1的實測）：我是從 docs/04 的 `max_continuous_sessions=1` 條款與程式碼裡完全沒有對應處理邏輯推論出這個問題會發生，但沒有真的開兩個來源連上真實 server 觀察 429/session_limit 的實際回應與程式反應。
7. **`settings-dialog.cpp` 的 Tools 選單項目在當前 build 是否會造成任何崩潰**（只是功能上是舊的，沒有走查是否有其他隱藏 bug），因為沒有本機建置無法實際點開它。
