# M2 修復覆驗報告：tea-live-subtitle

- 覆驗對象：`4d689b0`、`ce516e0`、`a850c12`（修復三連發），對照基準 `baec29a`（M2 原始實作）與前一輪 `docs/m2-verification.md`
- 規格來源：`tea-asr-service` repo 的 `docs/08-obs-plugin.md`、`docs/04-api.md`、`docs/03-architecture.md`（唯讀，未修改）
- 覆驗方式：純程式碼閱讀（含實際交叉查證 `tea-asr-service` 伺服器端程式碼）+ GitHub Actions 結果比對。**未實際跑 OBS、未跑本機建置、未連真實 server**。
- `tea-asr-service` 未被修改；本報告未寫入 `docs/m2-verification.md`；未建立 tag/release；未本機 cmake 建置。

## 結論先講：不建議放行 M3

修復者自述的 5 大主張中，**最關鍵的主張1（`session_limit`/close 1013 偵測）查證結果是「不符」，而且不是無害的無效修復，是會產生錯誤診斷、覆蓋伺服器自身信號、讓原本可恢復的情況變成使用者必須手動介入的永久失敗**。這是本次覆驗中最需要在 M3 前修正的問題。其餘 4 項主張（掉幀計數、解構子 timeout、設定視窗表格、去重表擴容）大致屬實，但解構子的 `terminate()` 兜底引入了一個新的、機率低但嚴重度更高的風險，值得記錄但不必然是阻斷項。

---

## 逐項查證

### 主張1：偵測 `error.code=="session_limit"` 與 WS close 1013，設 `fatalNonRetryable_` 停止重試 —— **不符（核心前提錯誤）**

**程式碼確實照著自述實作了**（`src/asr-client.cpp:601, 605-611` 的 close-1013 分支；`src/asr-client.cpp:710-719` 的 JSON error 分支），這部分本身沒有寫錯。**但這個修復偵測的信號，跟它宣稱要解決的問題（多來源違反 `max_continuous_sessions=1`）完全對不上**，交叉查證 `tea-asr-service` 原始碼如下：

1. **伺服器根本沒有對 `max_continuous_sessions` 做任何連線數量限制**。`tea-asr-service/src/tea_asr/api/app.py:285-292`（`/v1/stream` websocket handler）：
   ```python
   activity.sessions += 1
   activity.touch()
   try:
       await run_stream(websocket, scheduler, ...)
   ```
   全檔案（`app.py`、`stream.py`）找不到任何地方比較 `activity.sessions`（或任何連線計數）against `max_continuous_sessions`/`max_total_connections` 並拒絕連線。`capabilities` 端點裡的 `max_continuous_sessions=1`（`app.py:261`）**純粹是給 client 看的宣告值，伺服器沒有據此做任何 enforcement**。也就是說，兩個字幕來源同時連上同一台 server，兩邊都會被接受，各自開一條 session——不會有任何一邊被拒絕或被關閉連線。

2. **`error.code=="session_limit"` 實際觸發條件是「單一 session 內部待轉錄佇列滿了」，跟連線數量無關**。`tea_asr_service/src/tea_asr/api/stream.py:59`：`MAX_PENDING_SEGMENTS = 16`（同一個 session 內，segment 切好、排隊等模型轉錄的佇列上限），`stream.py:175`：`self._pending: asyncio.Queue = asyncio.Queue(MAX_PENDING_SEGMENTS)`。`stream.py:279-286`（`_enqueue`）：
   ```python
   except asyncio.QueueFull as exc:
       raise ApiError("session_limit", "等待辨識的片段已達上限。") from exc
   ```
   這是「模型推論跟不上、同一個 session 自己講話講太快導致排隊的 segment 超過16個」的情境（例如推論當下負載高、暫時性 GPU 忙碌），跟「第二個字幕來源搶佔連續 session 名額」是完全不同的兩件事。這個例外不在 `_handle_control` 的 try/except（`stream.py:706-719`，只包住控制訊息）保護範圍內，而是從 `_handle_frame`（處理音訊 binary frame）拋出，一路往上到 `run_stream` 的 `except ApiError as exc: await session.fail(exc)`（`stream.py:899-900`），`fail()` 會 emit `ErrorEvent(code="session_limit", retryable=error.retryable, ...)` 並 `await self._websocket.close(code=error.ws_close_code or 1011)`（`stream.py:807-816`）——`ws_close_code` 查表（`errors.py:36`）確實是 1013。**所以 client 端偵測到的 `code=="session_limit"` + close 1013 這兩個信號，在目前伺服器實作下，只可能來自「這個 session 自己的推論佇列塞爆」，不可能來自「另一個來源占用了唯一的連續 session 名額」**——因為後者伺服器根本不會發生（見第1點）。

3. **伺服器其實把 `session_limit` 標成 `retryable=True`，client 卻無條件覆蓋成 fatal**。`errors.py:42-49`：
   ```python
   RETRYABLE_CODES = frozenset({
       "queue_full", "session_limit", "model_loading",
       "inference_failed", "inference_timeout", "timeline_gap",
   })
   ```
   `ApiError.retryable` 預設就是 `code in RETRYABLE_CODES`，所以伺服器送出的 `ErrorEvent` 裡 `retryable` 欄位對 `session_limit` 是 `true`——伺服器自己認為這是「重試就可能好」的暫時性問題（佇列塞滿，等模型推論跟上、佇列清空後自然恢復）。但 `asr-client.cpp:710-716` 的邏輯是：
   ```cpp
   if (code == QLatin1String("session_limit")) {
       fatalNonRetryable_ = true;   // 無條件，不看 retryable 欄位
       ...
   } else if (!retryable) {
       fatalNonRetryable_ = true;
   }
   ```
   `session_limit` 分支完全不看伺服器送來的 `retryable` 值，直接判死刑。這代表：**只要單一字幕來源自己講話密度太高、暫時把伺服器單一 session 的轉錄佇列塞滿（跟其他來源完全無關，單一來源、單機、單 session 的場景下就能發生），這個修復就會把它永久判定為「已有其他來源在使用」並停止重試**——這是比原本「無限重試」更糟的行為：原本的無限16秒重試至少會在佇列清空後的下一次連線自動恢復；現在則是誤診後永久卡死，需要使用者手動點「全部重新連線」或改動 Properties 才能恢復，而顯示的訊息（「另一個字幕來源已在使用中」）還是錯的，會誤導使用者去找根本不存在的第二個來源。
4. **close 1013 的分支更是把三種不同原因混為一談**。`errors.py:35-37`：`queue_full`、`session_limit`、`slow_client` 三個完全不同的錯誤碼都映射到 close code 1013。`asr-client.cpp:605-611` 只看 `code==1013` 就判定「server is at capacity (only one live-subtitle connection allowed at a time)」——但 `slow_client` 的真正意思（`tea_asr_service/src/tea_asr/api/events.py:37`：`"事件佇列持續滿載，連線已中止。"`，且建構時明確傳 `retryable=False`）是「**這個 client 自己**消費 server 事件的速度跟不上，事件佇列持續滿載」，根源可能是 client 端處理迴圈卡住或網路吃緊，跟「另一來源占用連續 session」毫無關係，卻會被一樣的錯誤訊息誤導使用者去找假想中的第二個來源，反而忽略了真正該查的（自己的 event loop 是否被什麼卡住）。

**結論：主張1的修復是基於對伺服器協定的錯誤理解實作出來的。它既不能偵測到真正的「多來源違反 max_continuous_sessions=1」（因為伺服器根本不會為此送出這兩個信號），又會在單一來源、單一 session 內部暫時性佇列壅塞或 client 自身消費過慢時，把可恢復的暫時性錯誤誤判成需要使用者手動介入的永久失敗，且顯示的可操作訊息是錯的。這比 M2 驗收報告缺陷1原本的「無限重試、訊息模糊」風險組合更差，因為現在多了一種新的、會在正常使用中被觸發、且診斷訊息明確錯誤的失敗模式。M2 驗收報告缺陷1（多來源真正違反容量限制時完全無法自我診斷）事實上仍未被解決——因為伺服器目前不會拒絕多來源，兩個來源會一起連上並各自佔用資源（真正的風險其實是資源競爭/成本，不是被拒絕連線）。**

### 主張2：掉幀計數改為真的加總 —— **屬實**
- `src/asr-client.cpp:148-149`（`droppedFrames()`）：`tea_audio_tap_dropped_samples(tap_) + droppedPcmSamples_.load(...)`，兩個來源都真的加總。
- Ring buffer 溢位計數（`ring-buffer.h` 的 `dropped_samples`，前一輪已驗證為屬實）透過 `tea_audio_tap_dropped_samples`（`audio-tap.c:201-204`）串接。
- `pendingPcm_` 佇列溢位改成精確樣本數：`asr-client.cpp:796-800`，`evictedSamples = pendingPcm_.front().size()/sizeof(int16_t)`，`droppedPcmSamples_` 是 `std::atomic<uint64_t>`（`asr-client.hpp:158`）。
- `tea_asr_client_dropped_audio_frames()`（`asr-client.cpp:911-915`）不再寫死回傳0，真的呼叫 `droppedFrames()`。
- **涵蓋範圍檢查**：目前程式碼中會丟棄音訊樣本的兩個路徑（ring buffer 寫入端溢位 `ring-buffer.h:73`、`pendingPcm_` 佇列溢位 `asr-client.cpp:796-800`）都已計入。沒有發現第三個靜默丟棄點。**主張2驗證通過，且沒有遺漏的丟棄路徑。**

### 主張3：解構子改用帶 timeout 的 `QEventLoop`，逾時後 `terminate()` 兜底 —— **實作與自述一致，但引入了一個新的、更嚴重的極端風險**
- 實作確實如自述：`asr-client.cpp:58-93`，本地 `QEventLoop` + 2秒 `QTimer`，`doStop()` 完成則 `loop.quit()`；逾時記警告；`thread_.wait(2000)` 再逾時才 `thread_.terminate()` + `thread_.wait()`（無 timeout，等到底）。
- **評估**：這個改動確實消除了「呼叫端執行緒無限期卡死」這個 M2 驗收報告缺陷3指出的問題——2秒後一定會往下走。但 `QThread::terminate()`（POSIX 上是類似 `pthread_cancel` 的強制終止，Windows 上是 `TerminateThread`）是 Qt 官方文件明確警告「極度危險，只在極端情況使用，可能讓資源（含鎖）處於未釋放狀態」的操作。`doStop()`（`asr-client.cpp:231-253`）內部會呼叫 `tea_caption_state_reset()`（持有 `caption-state.c` 的 mutex）、`setStatus()`（持有內部 mutex）、以及 `QTcpSocket` 的 `close()`/`deleteLater()`（可能觸發 Qt 內部的堆積配置/解配置、系統呼叫）。如果 `terminate()` 恰好打斷在這些操作進行到一半的瞬間：
  - 若打斷在自家的 mutex 臨界區內，該 mutex 會永久保持鎖定狀態——但因為這個 `TeaAsrClient` 實例（連同其 `tap_`/`captions_`）緊接著就會被整個銷毀釋放，「洩漏一個鎖定的 mutex」本身影響有限（沒有其他執行緒會再去碰這塊已釋放的記憶體）。
  - **較嚴重的風險**：若打斷發生在 Qt/CRT 的堆積配置（`malloc`/`free`/`new`/`delete`）或系統呼叫中途，`TerminateThread`（Windows）或非同步 `pthread_cancel`（POSIX，若未設成 deferred-only cancellation points）有可能讓**行程層級**的堆積鎖（不是這個外掛自己的鎖，而是整個 OBS 行程共用的 C runtime heap lock）永久保持鎖定，導致 OBS 行程裡**任何其他地方**之後的記憶體配置全部卡死——這是比「OBS 主執行緒暫時卡住2秒後繼續」更嚴重的失敗模式：原本的死鎖至少侷限在使用者能觀察到「OBS 沒回應，需要強制結束」；`terminate()` 打中堆積鎖的話，結果可能是稍後在完全不相關的操作上詭異當機或損毀，難以歸因到這次刪除字幕來源的動作。
  - 機率評估：這個路徑只有在 `thread_` 事件迴圈在2秒內完全沒有機會處理排入的 `doStop()`（例如事件迴圈本身卡在別的地方，如 `QTcpSocket` 的網路 I/O 卡住不返回）才會走到，前一輪報告也認為是「邊界情況」。**判斷：這是「拿一個機率低但影響侷限的問題（呼叫端卡死，可被強制結束回復），換成一個機率同樣低、但影響可能擴散到整個行程且難以復原的問題（堆積損毀）」，不是單純的淨改善，是風險轉移，且沒有測試或機制驗證會不會真的打中堆積鎖。** 建議 M3 前至少評估是否能把 `terminate()` 拿掉，改成「逾時後放棄等待、直接洩漏這個 `TeaAsrClient` 物件（不刪除、不 terminate，讓 thread 自然跑到某個時間點結束或行程退出時一起清掉）」——洩漏一個物件比行程堆積損毀安全得多。

### 主張4：設定視窗改表格＋registry —— **大致屬實，未發現競態或生命週期問題；`PTHREAD_MUTEX_INITIALIZER` 在 Windows 上經查證確實可用（比修復者自述的信心更高）**
- 表格化與每秒刷新：`src/settings-dialog.cpp:118-136`（`QTimer` 1000ms interval，`refresh()` 呼叫 `tea_captions_source_for_each`）、「全部重新連線」按鈕（`settings-dialog.cpp:141-144`，呼叫 `tea_captions_source_reconnect_all()`）——與自述一致。
- **Registry 生命週期**：`captions-source.c:96-116`（`tea_registry_add`/`tea_registry_remove`，`pthread_mutex_lock(&g_registry_lock)` 保護鏈結串列增刪）、`tea_captions_source_create`（`captions-source.c:261`：`create` 完成後才 `tea_registry_add`）、`tea_captions_source_destroy`（`captions-source.c:269`：**先** `tea_registry_remove(ctx)` **再**呼叫 `tea_asr_client_stop`/`tea_asr_client_destroy`）。`tea_captions_source_for_each`（`captions-source.c:118-136`）在同一把鎖保護下走訪串列並讀取 `ctx->client` 的狀態——由於 `remove` 與 `for_each` 用同一把鎖，`for_each` 走訪到某個 `ctx` 時，該 `ctx` 保證還沒被 `destroy` 流程摸過（`remove` 先發生，`remove` 完成後 `destroy` 才會繼續往下釋放 `client`；而 `remove` 本身要等 `for_each` 釋放鎖才能拿到鎖）——**沒有發現 use-after-free 或迭代時被刪除來源造成的競態**。設定視窗每秒讀取時若某個來源恰好被刪除，該來源只是在下一次 `refresh()` 就從表格消失，不會有懸空指標存取。
- **`PTHREAD_MUTEX_INITIALIZER` 在 Windows 上的可用性**：修復者自述「只看過別的 plugin 用法、未實機驗證」，本次覆驗**實際找到了 OBS 官方 vendored 的 `w32-pthreads`（pthreads-win32）原始碼**（`/Users/c2leb/Codes/obs-plugins/omni-projector/.deps/obs-studio-31.1.1/deps/w32-pthreads/pthread.h:593-697`）：Windows 上 `pthread_mutex_t` 被 typedef 成一個指標型別，`PTHREAD_MUTEX_INITIALIZER` 定義為 `((pthread_mutex_t)(size_t)-1)`（一個 sentinel 指標值，不是真正配置好的 mutex 物件）；`pthread_mutex_lock()`（`pthread_mutex_lock.c:58-64`）在鎖定前會檢查 `*mutex >= PTHREAD_ERRORCHECK_MUTEX_INITIALIZER` 並呼叫 `ptw32_mutex_check_need_init()` 做**執行緒安全的延遲初始化（lazy static init）**——這是 pthreads-win32 這個成熟函式庫刻意設計的標準模式（`util/threading.h:45-51` 的註解「this may seem strange, but you can't use it unless it's an initializer」正是在講這個用法），跟 POSIX 上 `PTHREAD_MUTEX_INITIALIZER` 的語意一致。另外 CI 上 Windows 建置（含這次三個修復 commit，`gh run view 35421290068` 全綠）已經成功編譯連結這段程式碼。**結論：`PTHREAD_MUTEX_INITIALIZER` 在 OBS 的 Windows 建置環境下是可靠、經過驗證的用法，不是風險項；修復者自述的不確定性其實可以被現有證據打消，不需要在 M3 前額外處理。**（註：查證用的是另一個 sibling plugin 的 `.deps` 快取，OBS 版本可能與本專案 CI 實際使用的版本不完全相同，但 w32-pthreads 這部分是多年未變的成熟第三方函式庫，行為差異的可能性極低。）

### 主張5：close frame RFC 6455 §5.5.1 確認、16MiB payload 上限、去重表 16→256 —— **均屬實**
- Close frame 確認：`asr-client.cpp:598-602`（收到 server 主動 close 時，若 `handshakeDone_` 且 socket 仍連線中，呼叫 `sendCloseFrame(code)`）；`sendCloseFrame()`（`asr-client.cpp:461-467`）正確組出 2-byte big-endian close code payload並透過既有的 `sendWsFrame(0x8, ...)`（沿用既有的 masking/長度編碼邏輯，前一輪已驗證正確）送出。**符合 RFC 6455 §5.5.1「收到 close 後必須回送 close」的要求。**
- 16MiB 上限：`asr-client.hpp:167`：`kMaxIncomingFramePayloadBytes = 16u*1024u*1024u`；`asr-client.cpp:511-527` 在解析 payload 長度後、實際讀取 payload 前就檢查並提前中斷連線、清空緩衝——**在讀取巨大 payload 之前就擋下，不會真的先累積再檢查，防護有效。**
- 去重表 16→256：`caption-state.c` 的 `TEA_MAX_TRACKED_SEGMENTS` 從 16 改為 256（`git show a850c12`），其餘 round-robin 邏輯未變。純數值調整，無新邏輯風險，`256 * TEA_ID_BUF(64) ≈ 16KB`，記憶體成本可忽略。**這仍然只是把觸發窗口從3-4分鐘推到約1小時，不是真正的全 session 生命週期去重**（修復者自己也承認），但作為 M2→M3 過渡的暫時緩解是合理的。

### 回歸檢查（前一輪已驗證屬實的項目）—— **未發現回歸**
- 三個修復 commit 對 `src/audio-tap.c`、`src/ring-buffer.h`、`src/asr-client.cpp` 裡 `pumpAudio()` 的 flow-control/16-byte header 組裝邏輯**完全沒有觸碰**（`git show 4d689b0 --stat` 只改了 `asr-client.cpp/h/hpp`，且該 diff 裡 `pumpAudio()` 唯一的改動是 `pendingPcm_` 溢位計數方式，flow control/header 組裝那幾行原封不動）。
- 新增的 `obs_log` 呼叫（doStop 逾時警告、worker thread terminate 警告、payload 超限警告、close code 記錄）逐一檢查，**沒有一處把 token、PCM、prompt 或 transcript 文字帶入 log**，符合既有規範。
- 音訊擷取 callback（`tea_on_audio_capture`）與 ring buffer `trylock`-not-wait 邏輯本次三個 commit 完全沒有修改，維持前一輪驗證的屬實結論。

---

## 殘留缺陷清單（依嚴重度排序）

1. **【高，本次覆驗新發現】「多來源違反容量限制」偵測邏輯的核心前提與伺服器實際行為不符，且會產生新的誤判失敗模式**（`src/asr-client.cpp:598-611, 710-719`；對照 `tea-asr-service/src/tea_asr/api/app.py:285-292`、`src/tea_asr/api/stream.py:59, 279-286, 899-900`、`src/tea_asr/errors.py:35-49`）。
   - 觸發條件A（誤判為真但實際問題沒被解決）：兩個以上字幕來源同時連上同一台 server——伺服器目前完全不會拒絕，兩者都會成功建立 session 並各自佔用運算資源；不會出現 `session_limit`/1013，M2 缺陷1「多來源會壞掉且無法自我診斷」實質上仍未解決（差別只是「不會壞」而非「壞了但看得懂原因」）。
   - 觸發條件B（新引入的誤判）：**單一**字幕來源、單一 session，若說話密度高導致伺服器端待轉錄佇列（`MAX_PENDING_SEGMENTS=16`）暫時滿了，或 client 自己消費事件變慢觸發 `slow_client`，伺服器會送出 `session_limit`/`queue_full`/`slow_client` 其中之一並可能以 1013 關閉連線（其中 `session_limit` 伺服器本身標記 `retryable=true`）——client 端一律誤判為「另一個字幕來源已在使用中」，設 `fatalNonRetryable_=true` 永久停止重試，使用者必須手動點「全部重新連線」，且看到的訊息是錯誤的診斷。
   - 後果：M3 前應重新設計偵測邏輯——至少應尊重伺服器送來的 `retryable` 欄位，不要對 `session_limit`（伺服器標記為 retryable）無條件判死刑；且應認知到目前伺服器根本沒有 enforce `max_continuous_sessions`，這個防護目前是打在不存在的敵人身上，同時误伤了會在正常使用中發生的暫時性壅塞情境。

2. **【中，沿用前一輪、風險型態改變】解構子的 `terminate()` 兜底把「呼叫端卡死」的風險換成「可能造成行程級堆積鎖損毀」的風險**（`src/asr-client.cpp:58-93`）。
   - 觸發條件：`thread_` 事件迴圈在2秒內完全未處理 `doStop()`（例如卡在網路 I/O），且 `terminate()` 打斷點恰好落在 Qt/CRT 堆積配置或系統呼叫中途。
   - 後果：機率遠低於「一般情況」，但一旦發生，影響範圍可能不侷限於本外掛，而是整個 OBS 行程的記憶體配置子系統，且難以歸因除錯。建議評估以「逾時後放棄清理、洩漏物件」取代 `terminate()`。

3. **【低，本次覆驗新發現，測試覆蓋缺口】整個 repo 沒有任何自動化測試**（`find . -iname "*test*"` 無結果），這三個修復 commit 全靠人工程式碼閱讀驗證，沒有單元測試能在未來回歸時攔截（例如「session_limit 是否尊重 retryable 欄位」這種邏輯，寫一個 mock server 的單元測試即可攔住，但目前無此測試）。

4. **【低，沿用前一輪，範圍縮小】去重表容量 256 仍是固定大小、非真正全 session 週期去重**（`caption-state.c` `TEA_MAX_TRACKED_SEGMENTS=256`），只是把觸發窗口從分鐘級推到約一小時，修復者自己也承認留給 M3。

---

## 放行判斷

**不建議直接放行 M3。**

- 缺陷1是本次覆驗的核心發現，且性質上不是「沒做完」而是「做錯了方向」：修復依賴的伺服器信號（`session_limit`/1013）跟它宣稱要防護的場景（多來源違反 `max_continuous_sessions=1`）在目前 `tea-asr-service` 實作下沒有因果關係，反而會在正常單來源使用中因暫時性佇列壅塞而誤觸發，把可自行恢復的暫時錯誤變成需要使用者手動介入、且顯示錯誤診斷訊息的永久失敗。這直接影響規格驗收條件3（「server 狀態顯示要明確、可操作」）——現在顯示的訊息本身就是錯的。建議 M3 前：(a) 修正邏輯以尊重伺服器 `retryable` 欄位；(b) 若真的要防護「多來源」，應該直接找 `tea-asr-service` 團隊確認是否要在伺服器端補上 enforcement（目前完全沒有），而不是在 client 端猜測一個不存在的信號；(c) 若短期內伺服器不會補上 enforcement，至少在 commit message／文件裡誠實記錄「這個防護目前打不到真正的目標場景」，避免下一個接手的人誤以為問題已解決。
- 缺陷2（`terminate()` 風險轉移）建議記錄在案，不必然阻斷 M3，但應該讓決策者知道這是有意識的取捨而非無成本的修復。
- 主張2、4（部分）、5均查證屬實，是這輪修復紮實的部分，不需要再處理。

---

## 我無法驗證的項目（老實列出）

1. **`tea-asr-service` 實際部署行為**：本次查證完全基於閱讀 `tea-asr-service` 原始碼推論「伺服器不會 enforce `max_continuous_sessions`」，沒有實際啟動 server、開兩條 WebSocket 連線觀察真實行為（環境限制：不可修改該 repo，但閱讀與啟動是兩回事——本次也沒有嘗試在旁路啟動它來做黑盒驗證，因為這已超出「純程式碼閱讀 + CI 結果比對」的授權範圍，且會需要本機執行該服務）。若 `activity.sessions` 計數之外還有本次搜尋沒找到的 enforcement 路徑（例如反向代理層、部署層級的限制），本報告的核心結論會需要修正。已用多個關鍵字（`max_continuous_sessions`、`max_total_connections`、`Semaphore`、`too_many`、`429`）交叉搜尋過 `src/tea_asr/api/` 與 `src/tea_asr/*.py`，未發現其他 enforcement 路徑，但無法100%排除。
2. **`terminate()` 實際觸發機率與後果**：只能從 Qt/pthreads-win32 官方文件的警告推論風險方向，無法實測「多常發生」「打中堆積鎖的實際機率」，需要在真實 OBS 環境下用網路異常/GDB/WinDbg 之類工具製造邊界情況才能實測，不可行（不可本機建置）。
3. **實機 UI 行為**：設定視窗表格是否真的在 OBS 32.2.1 上正確渲染、reconnect 按鈕點擊後的實際使用者體驗、多來源同時連線時 UI 的即時觀感——需要真人在 OBS 操作，本次只讀程式碼路徑。
4. **`w32-pthreads` 版本一致性**：查證 `PTHREAD_MUTEX_INITIALIZER` 用的是另一個 sibling plugin（`omni-projector`）快取的 `obs-studio-31.1.1` vendored 原始碼，並非本專案實際建置用的 OBS 版本快取（本專案禁止本機 cmake 建置，故沒有自己的 `.deps`）。雖然 w32-pthreads 是多年未變的成熟第三方函式庫，但無法100%排除本專案鎖定的 OBS 版本用了不同分支。
5. **連續直播一小時的實測**：去重表擴容到256是否真的撐得住規格驗收條件4，只能從 `docs/04` 的 `max_segment_ms=14000` 靜態估算，未實測。
