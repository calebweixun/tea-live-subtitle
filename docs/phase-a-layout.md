# Phase A｜字幕資料流與固定寬度

> 更新：渲染已改為逐行（見 `phase-b-rendering.md`）。下文「native FreeType child 收到
> `custom_width`／`word_wrap`」的做法已被取代——那正是對齊無效的原因。畫布只畫 transcript、
> 寬度模式與舊 scene 遷移規則仍然有效。

這份文件記錄 OBS plugin 第一階段的畫布契約。完整協定規格仍在
`tea-asr-service/docs/08-obs-plugin.md`；本 repo 不修改 server repo。

## 畫布只畫 transcript

字幕狀態機只保存 `transcript.partial`、`transcript.final` 及其 session/segment
去重資訊。`tea_caption_state_render()` 沒有字幕時回傳空字串；它不接收
connecting、model loading/unavailable、token、reconnect、4029 或其他 server error
文字。

這些 diagnostics 仍由 `TeaAsrClient::statusText()` 提供給 Tools 設定視窗。來源畫布
若啟用 `Show waiting prompt when empty`，只會由來源自己顯示固定的中性
`Waiting for captions...`／`等待字幕…`；錯誤狀態不會取代這個提示。

## 寬度模式

`Layout Mode` 有兩種值：

- `Auto`：保留既有 scene 的 `text_ft2_source` 行為；`custom_width` 與 child source
  的實際寬度繼續決定來源尺寸。
- `Fixed Width`：`Caption Box Width` 是包含左右 padding 的外框總寬度。傳給 native
  FreeType child 的內容寬度為 `caption_width - 2 * padding`，並強制啟用 `word_wrap`。
  外層 `get_width()` 固定回傳該外框寬度，不會因字幕長短改變。

Phase A 只負責固定寬度與 native wrapping，不重寫 FreeType 排版，也不承諾嚴格的
visual two-line clipping。現有 `Max Lines` 仍是字幕狀態機的 logical final/partial
行數；嚴格最多雙行會在後續 layout phase 另行設計。

新 source 的 `Layout Mode` 預設為 `Fixed Width`，預設外框寬度為 960px。外層設定會
保存 `layout_schema_version=1`；載入沒有這個 marker、也沒有使用者明確寫入
`layout_mode` 的舊 scene 時，migration policy 會保留 `Auto`，因此舊有
`custom_width` 不會被覆寫。舊 source 只要在內容面板明確選擇一次模式，就會寫入
目前 schema marker。

private `text_ft2_source` 會先收到 OBS settings 的 effective defaults，再套用使用者
值。這一步很重要：`obs_data_apply()` 本身只複製 user values，若直接從空的
`obs_data_t` apply，新的平台 CJK font default 不會傳到 child，會退回 child 自己的
字型 default。

## 驗收重點

1. `TeaAsrClient::setStatus()` 的文字只出現在 Tools/source diagnostics，不出現在 OBS
   畫布。
2. Fixed mode 下，短句、長句、繁體中文及英文變化都不改變 source 的 outer width。
3. Fixed mode 下 native `text_ft2_source` 收到 `custom_width = outer - padding*2`
   及 `word_wrap=true`。
4. 新 source 預設為 Fixed 960px；未設定 marker 的舊 scene 維持 Auto 及原本
   `custom_width` 語意。
5. 新 source 的 effective CJK font default 會實際傳給 private child，既有使用者字型
   不被覆寫。
