# AMB82 離線語音 LED 控制

本專案使用 **Realtek AMB82-MINI（RTL8735B）** 的板載類比麥克風，完成可在開發板上獨立運作的離線語音控制系統。  
系統不依賴電腦端辨識，也不依賴雲端服務；使用者說出指定語音指令後，由 AMB82-MINI 直接擷取音訊、進行語音活動偵測與頻譜特徵比對，最後控制板載 LED。

目前支援：

- 「左邊開燈」：藍燈亮、綠燈熄。
- 「右邊開燈」：綠燈亮、藍燈熄。
- 無法辨識、非控制指令、聲音過短或信心不足：LED 維持原狀。
- 最後一次有效指令後 10 秒沒有新的有效指令：兩顆 LED 自動熄滅。
- 可透過 AMB82-MINI 自建 Wi-Fi 熱點進入板載網頁，查看辨識結果、執行結果與 GPIO 實際狀態。
- 網頁提供「暫停／繼續語音辨識」功能。

---

## 1. 最終實作方式

最終版本 **不使用 Realtek 自訂模型轉換，也不需要自訂 `.nb` 模型檔**。

AMB82-MINI 直接取得：

- 16 kHz
- 單聲道
- 16-bit PCM

並在板端完成以下流程：

1. 以 20 ms 音訊區塊計算平均絕對振幅。
2. 使用語音活動偵測（VAD）找出完整語音段。
3. 將語音分成 32 個時間區段。
4. 每個區段計算 12 個指定頻率帶的能量特徵。
5. 與板載麥克風收集的 `left / right` 平均頻譜範本計算距離。
6. 同時檢查最佳距離與左右距離差（margin）。
7. 若不符合門檻則輸出 `unknown`，不操作 LED。

最終頻譜範本由同一塊 AMB82-MINI 的板載麥克風收集：

- 左邊：6 筆
- 右邊：6 筆

樣本包含一般、較慢及較快語速。

---

## 2. 系統架構

```text
使用者語音
   │
   ▼
AMB82-MINI 板載類比麥克風
   │ 16 kHz PCM
   ▼
RawAudioSink / MMF
   │
   ▼
語音活動偵測 VAD
   │
   ▼
32 × 12 頻譜特徵擷取
   │
   ▼
left / right 範本距離 + margin 判斷
   │
   ├── LEFT    → 藍燈開 / 綠燈關
   ├── RIGHT   → 綠燈開 / 藍燈關
   └── UNKNOWN → LED 維持原狀
   │
   ▼
GPIO 實際狀態回讀
   │
   ▼
Wi-Fi AP + HTTP API + 板載網頁
   │
   ▼
手機 / 電腦瀏覽器
```

---

## 3. 功能與狀態規則

| 狀態 | 板端行為 | 網頁介面 |
|---|---|---|
| `left` | 藍燈開、綠燈關 | 辨識成功：左邊；指令已執行 |
| `right` | 綠燈開、藍燈關 | 辨識成功：右邊；指令已執行 |
| `unknown` | LED 維持原狀 | 無法辨識；LED 維持原狀 |
| `timeout` | 兩燈全關 | 10 秒沒有有效指令；已自動關燈 |
| `paused` | 停止辨識、清除未完成語音、兩燈全關 | 語音辨識已暫停 |
| 通訊失敗 | 板端維持目前狀態 | 顯示通訊失敗提示 |

有效的 `left / right` 指令會重新開始 10 秒計時。  
`unknown` 不重設計時。

---

## 4. 執行環境

### 硬體

- 開發板：Realtek AMB82-MINI（RTL8735B）
- 音訊輸入：板載類比麥克風 `USE_AUDIO_AMIC`
- 藍燈：`LED_B`（AMB82 variant 對應 `AMB_D23`）
- 綠燈：`LED_G`（AMB82 variant 對應 `AMB_D24`）
- USB 傳輸線

### Arduino 環境

本專案最終版本的已測試環境：

- Arduino IDE：2.x
- Realtek AmebaPro2 core：`4.0.9-build20250805`
- FQBN：`realtek:AmebaPro2:Ameba_AMB82-MINI`
- Serial Monitor：`115200 bps`
- Upload Speed：`2,000,000 bps`
- 最終版本不需要自訂 `.nb`
- 最終版本不需要 Realtek 線上模型轉換

> 本次開發時使用 `COM3`，實際執行時請選擇自己電腦上 AMB82-MINI 對應的 COM Port。

### Python 環境

Python 只用於資料整理、模型路線實驗與頻譜範本產生；**最終板端執行不需要 Python 或 TensorFlow**。

本次開發環境：

- Python：3.13.9
- NumPy：頻譜範本統計與報告
- TensorFlow 2.21：曾用於 YAMNet 路線實驗
- TensorFlow Hub 0.16.1：曾用於 YAMNet 路線實驗

若只需要重新建立目前使用的頻譜範本，主要需要：

```bash
pip install numpy
```

若要重現早期 YAMNet 實驗腳本，才需要額外安裝 TensorFlow / TensorFlow Hub 等套件。

---

## 5. 網路設定

AMB82-MINI 執行後會建立自己的 Wi-Fi AP：

```text
SSID：AMB82-Voice
Password：voice82x
IP：192.168.82.1
Subnet Mask：255.255.255.0
Wi-Fi Channel：6
HTTP Port：80
```

此 Wi-Fi 熱點用於連線至 AMB82-MINI 的板載網頁，**不提供網際網路連線**。

---

## 6. 專案主要檔案

```text
AMB82-Voice-Control/
│
├─ README.md
├─ AMB82_VoiceControl.ino
├─ RawAudioSink.h
├─ RawAudioSink.cpp
├─ spectral_templates.h
├─ page.h
│
├─ scripts/
│  ├─ Split-VoiceCommands.ps1
│  ├─ build_spectral_templates.py
│  ├─ build_live_templates.py
│  ├─ train_score_classifier.py
│  └─ 其他模型／資料處理腳本
│
├─ model/
│  └─ artifacts/
│     ├─ live_calibration.csv
│     └─ live_template_report.json
│
├─ dataset/
│  └─ manifest.csv
│
├─ PROJECT_DOCUMENTATION.md
└─ FULL_SOURCE_CODE.md
```

### 主要檔案用途

| 檔案 | 用途 |
|---|---|
| `AMB82_VoiceControl.ino` | 主程式、音訊處理、辨識、LED 控制、HTTP API、逾時與暫停邏輯 |
| `RawAudioSink.h/.cpp` | 從 AMB82 MMF 音訊管線取得 Raw PCM |
| `spectral_templates.h` | 左右指令的平均頻譜範本、距離與 margin 門檻 |
| `page.h` | 板載 Web 操作介面 |
| `scripts/build_live_templates.py` | 從板載校正 CSV 重建頻譜範本及門檻 |
| `scripts/build_spectral_templates.py` | 從 WAV 樣本建立頻譜範本 |
| `scripts/Split-VoiceCommands.ps1` | 將來源錄音依語音區段切成個別 WAV |
| `model/artifacts/live_calibration.csv` | 板載校正特徵原始資料 |
| `model/artifacts/live_template_report.json` | 範本距離與門檻報告 |
| `PROJECT_DOCUMENTATION.md` | 系統架構、操作、環境與 Codex 協作紀錄 |
| `FULL_SOURCE_CODE.md` | 專案完整文字版原始碼備份 |

---

## 7. 編譯與燒錄

### Arduino IDE

1. 使用 Arduino IDE 開啟：

```text
AMB82_VoiceControl.ino
```

2. 確認以下檔案與 `.ino` 位於相同 Arduino 專案資料夾：

```text
RawAudioSink.h
RawAudioSink.cpp
spectral_templates.h
page.h
```

3. 在 Arduino IDE 選擇 AMB82-MINI 開發板。

4. 確認 FQBN 對應：

```text
realtek:AmebaPro2:Ameba_AMB82-MINI
```

5. 選擇 AMB82-MINI 實際對應的 COM Port。

6. 編譯程式。

7. 讓開發板進入 Flash Mode。

8. Upload 至 AMB82-MINI。

9. 上傳完成後按 RESET。

10. 如需查看序列輸出，將 Serial Monitor 設定為：

```text
115200 baud
```

### Arduino CLI

本次專案使用的核心編譯方式：

```powershell
arduino-cli.exe compile `
  --fqbn realtek:AmebaPro2:Ameba_AMB82-MINI `
  --build-path <英文暫存路徑> `
  <專案路徑>
```

Realtek 工具鏈對中文路徑與 locale 較敏感。若遇到編譯問題，可將專案暫時放到純英文路徑，並視需要設定：

```text
LC_ALL=C
LANG=C
```

---

## 8. 執行與操作方式

### 8.1 啟動

1. 對 AMB82-MINI 供電。
2. 按一下 RESET。
3. 等待開發板建立 Wi-Fi 熱點。
4. 使用手機或電腦連線：

```text
SSID：AMB82-Voice
Password：voice82x
```

5. 在瀏覽器開啟：

```text
http://192.168.82.1
```

### 8.2 語音操作

建議距離板載麥克風約 **20–60 公分**。

說：

```text
左邊開燈
```

辨識成功：

```text
藍燈：開
綠燈：關
```

說：

```text
右邊開燈
```

辨識成功：

```text
藍燈：關
綠燈：開
```

網頁會顯示：

- 辨識結果
- 信心分數
- 指令執行情形
- 藍燈實際狀態
- 綠燈實際狀態

LED 狀態由 AMB82-MINI 的 GPIO 回讀結果提供，不是由瀏覽器自行推測。

頁面約每 700 ms 讀取一次開發板狀態。

### 8.3 無法辨識

以下情況會輸出 `unknown`，且不改變目前 LED：

- 聲音過短
- 語音與左右範本距離過大
- 左右距離過於接近
- 非控制指令
- 環境聲音

### 8.4 暫停／繼續

在網頁按下「暫停語音辨識」後：

- 停止接受新的語音指令
- 清除未完成語音
- 關閉兩顆 LED

再次按下「繼續語音辨識」即可恢復。

---

## 9. HTTP API

### `GET /`

取得板載 Web 操作頁面。

### `GET /api/state`

回傳目前辨識與 GPIO 狀態，例如：

```json
{
  "recognition": "left",
  "score": 58,
  "executed": true,
  "paused": false,
  "blue": true,
  "green": false,
  "sequence": 12
}
```

### `POST /api/pause`

切換暫停／繼續狀態。進入暫停模式時會停止辨識並關閉兩顆 LED。

---

## 10. 重新建立板載頻譜範本

最終版本使用 `spectral_templates.h` 內的頻譜範本。

若已重新收集 AMB82-MINI 板載麥克風校正資料，可使用：

```bash
python scripts/build_live_templates.py
```

腳本會讀取：

```text
model/artifacts/live_calibration.csv
```

並重新產生：

```text
spectral_templates.h
model/artifacts/live_template_report.json
```

目前最終範本的結構為：

- 32 個時間 frame
- 每個 frame 12 個頻率 band
- 共 384 個特徵
- left / right 各一組平均範本
- 使用最大距離與最小 margin 共同決定是否接受辨識結果

---

## 11. Codex / AI 協作摘要

本專案使用生成式 AI 協助開發、測試與除錯，主要過程包括：

1. 釐清 AMB82 離線辨識與 LED 控制需求。
2. 分析與切割左右語音資料。
3. 嘗試 YAMNet 自訂模型及 AMB82 內建 YAMNet 路線。
4. 發現 AMB82 Arduino API 的內建 YAMNet 僅能回傳一般聲音分類，無法直接辨識「左邊／右邊」中文詞義。
5. 改為實作 `RawAudioSink`，直接從 MMF 音訊管線取得 16 kHz PCM。
6. 建立 VAD、32×12 頻譜特徵與距離分類器。
7. 使用 AMB82 板載麥克風重新校正左右語音範本。
8. 完成 LED 控制、GPIO 回讀、Wi-Fi AP、HTTP API 與板載網頁。
9. 加入 `unknown`、10 秒 timeout、暫停／繼續與通訊錯誤處理。

> YAMNet / TensorFlow 相關腳本保留作為開發歷程與實驗紀錄；**最終板端執行路線為 Raw PCM + 頻譜範本比對，不依賴 YAMNet、自訂 `.nb` 或雲端推論。**

---

## 12. 已知限制

- 本系統是固定詞彙的聲學範本比對，不是通用中文語音轉文字。
- 發音方式、背景環境或麥克風距離差異很大時，系統會優先輸出 `unknown`。
- 目前範本針對已收集的語音與本專案使用的 AMB82-MINI 板載麥克風校正。
- 更換使用者、開發板或麥克風後，建議重新收集板載樣本並產生新的頻譜範本。
- 手機或電腦必須連線至 `AMB82-Voice` 才能開啟狀態網頁。
- `AMB82-Voice` 是區域 Wi-Fi 熱點，不提供網際網路。

---

## 13. 專案文件

詳細系統說明：

```text
PROJECT_DOCUMENTATION.md
```

完整文字版原始碼備份：

```text
FULL_SOURCE_CODE.md
```

GitHub Repository：

```text
<建立 GitHub Repository 後，在此填入專案網址>
```
