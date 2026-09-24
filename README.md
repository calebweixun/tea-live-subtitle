# tea-live-subtitle

OBS Studio 外掛：把本機 [TEA ASR](https://github.com/calebweixun) 服務的即時語音辨識結果，
以原生 OBS 來源的方式疊在直播畫面上（字體、顏色、描邊、位置都用 OBS 原生機制調整，而不是
一個外掛硬塞的浮動視窗）。

完整的產品規格與協定細節記錄在 TEA ASR server 專案的
[`docs/08-obs-plugin.md`](https://github.com/calebweixun/tea-asr-service) 交接文件中。

## 目前狀態：Phase 1（骨架）

這個 repo 目前只做「讓外掛能被 OBS 實際載入」這件事，用來壓掉 C++/CMake/Qt/libobs
建置與載入流程的風險。**還沒有**音訊擷取、WebSocket 連線、字幕狀態機——這些留給 Phase 2。

Phase 1 完成的部分：

* 註冊一個「TEA 即時字幕」影像來源型別（`tea_live_subtitle_source`）。它用
  `obs_source_create_private()` 建立一個私有的 `text_ft2_source`（freetype2 文字來源）
  子來源來實際算繪文字，不重寫字型排版邏輯。屬性面板暴露該子來源的
  字型／顏色／描邊／陰影／自動換行／自訂寬度，外加外掛自己的最大行數／對齊／內距。
  位置一律交給 OBS 原生的 scene item transform。
  來源目前顯示一段佔位文字（「TEA ASR 尚未連線」），加入場景後就看得到東西。
* 在 Tools 選單掛一個「TEA ASR 字幕設定…」項目，開一個最小的 Qt 對話框
  （伺服器位址、連接埠、狀態列），證明 Qt 與 `obs-frontend-api` 有正確連結。
  這一步**還不接網路**。
* 所有面向使用者的字串走 `obs_module_text()` 與 `data/locale/{en-US,zh-TW}.ini`。

## 建置

**不要在本機執行 `cmake`。** 本機沒有配置完整的 OBS SDK，`cmake -S . -B build` 會把整包
OBS 原始碼下載到 `.deps/`，污染這個 repo 的 git 歷史與工作目錄。

所有建置都交給 GitHub Actions（`.github/workflows/push.yaml` → `build-project.yaml`），
在 push 到 `master` 時會自動跑 macOS / Windows / Ubuntu 三個平台。用
`gh run list --repo calebweixun/tea-live-subtitle` 追蹤結果。

## 測試

**CI（不需要 OBS）**：`.github/workflows/protocol-tests.yaml` 編譯並執行
`tests/asr-client-policy-test.cpp`（錯誤分類與重連退避）、`caption-layout-test.cpp`、
`caption-state-test.c` 與 `tests/*.py` 靜態契約檢查。

**端到端（本機，需要 tea-asr-service checkout 與桌面版 Qt6）**：用外掛真正的
`asr-client.cpp`＋`caption-state.c`，連到**目前版本**的 server app（server repo 的測試用
`FakeSupervisor`／`FakeVad`，不載入模型、不用 8327 port、不寫使用者的 `~/Library` 目錄），
驗證握手、授權、partial／final，以及 Host 被拒、token revoke／rotate、`rate_limited`、
連線上限（pre-accept 403）、`concurrent_session_limit`（4029）、`idle_timeout`（4408）、
server 重啟與 70 秒心跳，以及穩定字幕（`stable*` 情境：要求與收到 `transcript.stable`、畫面只增不改、
server 不宣告或拒絕 `stable` 時退回 partial、斷線重連不清空畫面）。這個建置只用 Qt，不需要 OBS SDK：

```sh
cmake -S tests/e2e -B /tmp/tea-e2e-build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build /tmp/tea-e2e-build
python3 tests/e2e/run_e2e.py --driver /tmp/tea-e2e-build/asr-client-e2e \
  --service-dir /path/to/tea-asr-service   # 全部約 8 分鐘；--only happy,bad_token 可挑選
```

## 與 server 的連線契約（外掛端）

* 每次連線先做**帶 token** 的 `GET /v1/capabilities` preflight，成功才開 WebSocket。server 在
  `accept()` 之前拒絕的 WS 連線（unauthenticated／rate_limited／forbidden_origin／session_limit）
  在線上一律是沒有內容的 `HTTP 403`，只有 HTTP 回應分辨得出原因。
* 退避：一般錯誤 1→2→4→8→16→30 秒（上限 30 秒、±10% jitter）；token 被拒 30／60 秒、最多 5 次後停止
  （每個字幕來源每 60 秒最多 2 次認證失敗，低於 server 的 10 次）；`rate_limited` 等 65 秒；
  Host 被拒 60 秒。token 檔為空或不存在時完全不連線，檔案一變動就立刻重試。沒選音訊來源時不佔用
  server 連線名額。
* 所有錯誤與斷線只出現在 Tools 對話框的狀態欄，不進字幕畫布。

### 穩定字幕（只增不改，`transcript.stable`）

來源屬性「穩定字幕（只增不改）」（設定鍵 `stable_captions`），**預設開啟**。契約以 server 的
`docs/04-api.md`「只增不改的穩定字幕流」為準。

* **要求**：preflight 的 `capabilities.features.stable_transcripts` 為 `true`（欄位不存在＝不支援）時，
  `session.start` 送 `"transcript_mode":"revisable","stable":{"agreement":2}`。server 沒宣告就不送 `stable`，
  直接用原本的 partial／final；宣告了卻在 `session.started` 前以 `protocol_error`／`unsupported_option` 拒絕時，
  下一次連線自動拿掉 `stable` 改用 partial（不當成致命錯誤，Tools 狀態會寫明），按「全部重新連線」或改設定才會再試。
* **顯示**：預設只顯示已提交的文字。「顯示未確定文字（較淡）」（`show_unstable_tail`，預設關閉）開啟時，
  同一段最新的 `transcript.partial` **以已提交文字開頭**時，才把多出來的部分以較淡的樣式接在後面；不是的話就不顯示
  尾巴，已提交的字不受影響；該段收到 final／收尾 stable／skipped／cancelled 或斷線時尾巴消失。每個 `segment_id`
  各自一行、各自保存，下一段的 stable 比上一段的 final 早到也不會互相覆蓋；行依 `segment_index` 排序。
* **接尾巴**：`text` 是該段完整的已提交文字，外掛只把比畫面多出的 UTF-8 bytes 接上。收到不以畫面文字開頭的值
  （違反契約，例如舊版或有 bug 的 server）時**不改字**、丟掉該值、在 OBS log 記一次，次數顯示在 Tools 對話框。
* **收尾**：`transcript.final` 若延伸已提交文字就立刻接上；否則保留已提交文字，等緊接著的收尾 stable。
  `final`／`diverged`／`abandoned` 是該段最後一則；`diverged` 保留已提交文字並接上 final 的尾巴，所以畫面可能與
  final 不同——**逐字稿與存檔只認 `transcript.final`**，stable 只供顯示。`segment.skipped`／`segment.error`／
  `session.cancelled` 不收回已顯示的字，只是該行不再增長。
* **斷線重連**：穩定字幕開啟時，斷線不清空畫面；最後顯示的內容凍結，新 session 的字幕接在下面捲上來。
  若 10 秒內沒有重新開始 session，就清空畫面，避免過期字幕假裝仍在直播。按「全部重新連線」或改設定仍會立即清空。
* 關閉此選項時行為與先前相同：單一 preview 行由 `transcript.partial` 整段替換、final 推入上方、斷線即清空。

## 畫面呈現（逐行渲染）

設計與取捨見 [`docs/phase-b-rendering.md`](docs/phase-b-rendering.md)。重點：

* 每一行各自排版：對齊（左／中／右）依每行實際寬度計算；固定寬度模式下超過可用寬度自動折行
  （中文逐字、英文在空白處、句讀不放行首），已經出現在畫面上的字不會因為後面的字到來而被擠到下一行。
* 「文字漸層上色／下色」就是 OBS 文字引擎的 `color1`／`color2`（上下漸層）；描邊與陰影固定為黑色。
* 選用功能（舊來源升級後預設都關閉，新建的來源會帶建議值）：文字底色、換句偵測時間、畫面最多行數、
  新文字淡入、舊字幕自動淡出（淡入淡出共用一個時間設定）、顯示未確定文字。
* 外觀設定在「屬性」視窗改了就套用（OBS 約 0.5 秒後呼叫 update），只重畫、不重連；伺服器位址／連接埠／
  Token／穩定字幕這類會送到 server 的設定，要按「套用連線設定」或關閉視窗才生效。

## 架構（規劃中，見交接規格）

```
src/
  plugin-main.c            # obs_module_load：註冊 source、Tools 選單
  captions-source.c/.h     # obs_source_info：屬性、render（Phase 1 已完成骨架）
  settings-dialog.cpp/.hpp # QDialog：server 位址、port、連線狀態（Phase 1 僅 UI 骨架）
  audio-tap.c/.h           # Phase 2：從指定音訊源取樣、resample、進 ring buffer
  asr-client.cpp/.hpp      # Phase 2：WebSocket client，協定狀態機、重連
```

本機環境事實：OBS Studio 32.2.1（macOS, Apple Silicon）；`buildspec.json` 目前沿用
`obs-plugintemplate` 預設的 `obs-studio` 版本 31.1.1 ——這個版本只影響 CI 建置環境下載
的 OBS 標頭檔/函式庫版本，跟本機安裝的 OBS 執行期版本無關，且 API（`obs_register_source`、
`text_ft2_source`、`obs_frontend_add_tools_menu_item` 等）在兩個版本間相容，因此沿用同一位
使用者已發布過的 [osc-mapper](https://github.com/calebweixun/osc-mapper) 外掛驗證過的配置。

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the
[obs-plugintemplate Wiki](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS).
