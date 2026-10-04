# 辨識提示（實驗）

在 OBS 裡**從清單選一個 server 上的字典**，它的對照表（常聽錯的字，例：盛家 → 聖經）就會套用在字幕上。字典集中在
TEA ASR app 的「字典」頁編輯（server 的字典 API），外掛只負責選。

早期版本在屬性視窗裡還有講道情境說明、專有詞、對照表與提示檔四個欄位；它們和 server 字典重複，而且 server 的模型
prompt 預設關閉（情境說明與專有詞根本不會送進模型），所以已從屬性視窗移除（見下方「舊的自訂詞」）。

## 契約（server 端，branch `agent/context-hints`）

* `GET /v1/capabilities` 的 `features.context_biasing`：`true` 才會送出任何提示。預設關閉，server 用
  `TEA_ASR_CONTEXT_HINTS=1` 開啟。開啟時 `features.context_limits` 帶上限：
  `max_domain_chars`、`max_hotwords`、`max_hotword_chars`、`max_replacements`、`max_replacement_chars`
  （目前 300／200／32／500／32）。外掛讀實際的值，不寫死。
* `session.start.context`（只在 `context_biasing` 為 `true` 時送；沒有任何提示就不送）：

  ```json
  {"profile": "church", "domain": "主日講道…", "hotwords": ["以弗所書"], "replacements": [{"from": "盛家", "to": "聖經"}]}
  ```

  只有這四個鍵，每個都可以省略；`to` 可以是空字串（刪掉）。超過上限、未知的鍵或不存在的 profile，server 會拒絕。
* `GET /v1/dictionaries`（與 preflight 相同的 token）回傳 server 上的字典檔
  `[{name, domain, hotwords_count, replacements_count}]`；`profile` 用 `name` 指定其中一個。server 讀不了的檔案
  （格式錯誤、讀不到、超過 1 MiB、symlink）也會列出，但只有 `{name, error}`，不會讓其他字典消失。
* server 先套用 profile，再套用 inline 欄位：inline 的 domain 取代 profile 的，專有詞取聯集，對照表以 `from`
  為準由 inline 蓋過 profile。
* `session.started.context = {profile?, domain_chars, hotwords_count, replacements_count, prompt_applied, prompt_tokens?}`：
  server 實際套用（profile 合併之後）的結果。`prompt_applied` 表示情境說明與專有詞有沒有送進模型：server 另外要開
  `TEA_ASR_CONTEXT_PROMPT=1` 才會送（預設關閉——真實模型的測試裡 prompt 讓準確率變差）；沒送時對照表照常生效。
  `prompt_tokens` 只在 prompt 已套用時才可能出現。
* 對照表是 server 對 partial 與 final 做的精確字串取代：`text` 是取代後的字、`raw_text` 保留聽到的字，warnings 帶
  `replacements_applied`；final 的 `warnings` 也可能帶 `context_echo`（結果裡出現情境說明的長段原文）。
* 字典檔是 server 支援目錄下的 TOML：`<support>/dictionaries/<name>.toml`，內容是 `domain`、`hotwords` 與
  `[[replacements]] from／to`（server repo 的 `docs/examples/dictionaries/church.example.toml`）。
* 拒絕的格式：未知或格式錯誤的 profile、以及功能關閉時帶了 `context`，都是 `error` `unsupported_option`
  （例：「server dictionary profile is unavailable or invalid.」），接著 close 1008；context 裡有未知的鍵是
  `protocol_error`。

## 屬性視窗：「辨識提示（實驗）」群組

整組是**連線類設定**（跟伺服器位址一樣）：開始連線時送出，按「套用連線設定」或關閉視窗才生效，不會即時套用。
預設不選字典，也就是不送任何提示。

* **字典**（`hints_profile`）：下拉選單，「（不使用）」加上 `GET /v1/dictionaries` 的字典（顯示名稱與對照數）。server
  讀不了的字典顯示為「（無法使用）」且不能選，原因寫在狀態列。清單在屬性視窗打開時非同步取得（不會卡住 UI），拿到後
  視窗自動重新整理；15 秒內不重複要，「重新整理清單與狀態」按鈕會立刻再要一次。目前選的字典即使不在清單裡（還沒連線、
  server 暫時拿不到清單）也保留在選單裡，不會被清掉。以前可輸入的選單存成文字的「（不使用）」會在載入時改成空值。
* 說明文字：「要編輯字典內容，請到 TEA ASR app 的『字典』頁」。

### 狀態列

屬性視窗打開時（以及按「套用連線設定」、「重新整理清單與狀態」時）計算：

* server 支援狀態：支援／這台 server 不支援（`context_biasing` 欄位不存在）／server 沒有開啟（欄位是 `false`；
  提示用 `TEA_ASR_CONTEXT_HINTS=1` 開啟）／尚未連線。
* 連線之後：server 回報的套用結果，例如「已套用：字典 church，對照 116」（server 開了 model prompt 時再加上送進模型的
  專有詞數）；或 server 拒絕的原因。
* server 的字典清單（名稱與對照數）；讀不了的字典逐一列出原因；或「無法取得」。

### 舊的自訂詞

設定鍵 `hints_domain`、`hints_hotwords`、`hints_replacements`、`hints_file` 已不在屬性視窗裡，但**已經存了值的來源照舊
送出**（和字典一起，規則與截斷見下面兩節），不會因為升級而改變辨識結果。這種來源的狀態列會多一行「此來源另有舊的自訂詞
（情境說明、專有詞、對照表或提示檔），仍會和字典一併送出」，附上依 server 上限算出的數量與注意事項，並出現
「清除舊的自訂詞」按鈕：按下後清掉這四個值並立即以新設定重新連線。選擇移除而不是收進「進階」：使用者的字典集中管理，
這四個欄位與字典重複、情境說明與專有詞在預設設定下根本不會送進模型；留著只會讓人以為要填。

舊的欄位格式（仍然有效）：

| 設定鍵 | 格式 |
|---|---|
| `hints_domain` | 多行文字，前後空白會去掉 |
| `hints_hotwords` | 一行一個；空白行與 `#` 開頭的行略過；重複的只送一次 |
| `hints_replacements` | 一行一組：`錯字 => 正字`、`錯字=>正字` 或 `錯字<Tab>正字`；`#` 開頭的行、以及空白後的 ` #…` 是註解（`C 井 => C#` 的 `#` 會保留）；`=>` 後面空白＝刪掉；同一個錯字以後面那行為準 |
| `hints_file` | UTF-8 文字檔（可有 BOM），`[專有詞]`／`[對照表]`（或 `[hotwords]`／`[replacements]`）兩段，每次連線重新讀取，在欄位之前合併 |

## 截斷與驗證（`src/recognition-hints.h`）

送出前依 server 宣告的上限處理，server 不會收到它會拒絕的內容：

* 情境說明超過字數上限時從結尾截斷（以字、不是 byte 計算）。
* 單個專有詞、或對照的任一邊超過長度上限時，整項不送並列出（截斷的詞就是另一個詞了）。
* 數量超過上限時保留前面的（提示檔的在前，所以檔案優先於欄位）。
* 有截斷時 OBS 記錄檔寫一行 `asr-client: WARN recognition hints cut to the server's limits: ...`，只有數字。

提示內容屬於 prompt，**不寫進 OBS 記錄檔**：記錄檔裡的 `session.start` 把 `context` 換成數量；只有使用者自己開啟的
「記錄辨識事件」檔案保留完整的 `session.start`（它本來就含逐字稿）。

## server 拒絕時

`session.started` 之前收到的 `error` 若與提示有關（`unsupported_option`／`protocol_error` 且訊息沒有指向
`segmentation`／`stable`，或任何提到 context／profile／dictionary／hotwords／replacements 的錯誤，例如不存在的
profile），外掛不當成致命錯誤：下一次連線不帶 `context`，其他設定（穩定字幕、換句偵測時間）照舊，Tools 狀態、
狀態列與 OBS 記錄檔（`WARN server rejected the recognition hints (<code>)`）說明原因。修改提示後按「套用連線設定」，或在 Tools 對話框按
「全部重新連線」，會再試一次（設定沒變時「套用連線設定」不會重新連線）。

## 記錄檔

| 訊息 | 意思 |
|---|---|
| `capabilities: ... context_biasing=on｜off on the server｜not offered` | preflight 看到的支援狀態 |
| `sending recognition hints: profile=yes domain=N chars hotwords=N replacements=N` | 這次 session.start 帶的量 |
| `recognition hints applied: profile=... domain=N chars hotwords=N replacements=N prompt_applied=0/1 prompt_tokens=N` | server 在 `session.started` 回報的套用結果 |
| `WARN recognition hints cut to the server's limits: ...` | 有東西被截掉或不送 |
| `WARN N recognition hint replacement line(s) are not "from => to"` | 對照表有格式錯誤的行 |
| `WARN the recognition hints file cannot be read (...)` | 提示檔讀不到 |
| `WARN server rejected the recognition hints (<code>); reconnecting without them` | server 拒絕，改為不帶提示 |
| `the server offers N recognition hint dictionaries` / `GET /v1/dictionaries failed (...)` | 字典清單 |

## 測試

* `tests/recognition-hints-test.c`（CI）：專有詞與對照表的三種格式、註解、空白行、CRLF、提示檔段落、合併順序
  （檔案 → 欄位）、截斷到上限、拒絕原因的判斷、「舊的自訂詞」的判斷（`tea_hints_has_legacy()`）。
* e2e（`tests/e2e/run_e2e.py`）：`hints_on`（送出的 `context` 格式正確且等於設定、讀回 echo、抓到字典清單，帶 `error` 的字典標成無法使用並附原因）、
  `hints_limits`（依小上限截斷並寫 WARN）、`hints_rejected`（不存在的 profile → 不帶提示重連、其他設定保留）、
  `hints_off`（對 `--service-dir` 裡的 server 原樣測：功能關閉時不送 `context`）、`hints_absent`（沒有這個欄位的 server）。
  `fake_asr_server.py --emulate-context`／`--reject-context-field`／`--hide-context-capability` 在不修改 server app
  的情況下模擬契約。
* 對真正的 server 實作（不模擬）：`hints_real_profile_only`（屬性視窗現在送出的樣子：只有選的字典；echo 是字典與
  對照數，對照真的套用在 final 上）、`hints_real`（舊的自訂詞仍一併送出：capabilities 與上限、`/v1/dictionaries` 的
  真實格式（含 server 讀不了、以 `error` 列出的字典）、context 被接受、
  echo 含 `prompt_applied=false`、profile 與欄位的對照都真的套用在 final 上且帶 `replacements_applied`、畫面上
  只出現取代後的字、穩定字幕契約不受影響）、`hints_real_prompt`（`TEA_ASR_CONTEXT_PROMPT=1` → `prompt_applied=true`）、
  `hints_real_unknown_profile`（真實的 `unsupported_option` 拒絕 → 不帶提示重連）。這四個情境自己用
  `TEA_ASR_CONTEXT_HINTS=1` 啟動 server，並把字典放在 harness 的支援目錄；`--service-dir` 裡的 server 沒有這個功能時
  就略過。harness 把環境變數 `TEA_ASR_CONTEXT_HINTS`／`TEA_ASR_CONTEXT_PROMPT` 傳給 server 設定，
  所以整個 e2e 也可以在開啟的狀態下跑。
