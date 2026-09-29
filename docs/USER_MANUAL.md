# P1 操作說明

## 啟動與讀寫圖片

### Portable 版本

在原始碼專案的 `ImageEditing-master` 資料夾開啟 PowerShell，執行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Make-Portable.ps1
```

建置端需要 CMake 和 Visual Studio 2019 C++ x64 工具。產生的
`dist/P1-Demo-Windows-x64.zip` 包含 Release 執行檔、runtime DLL、`Images`
與選用的啟動輔助檔，適用於 Windows 10/11 x64。

將 ZIP 完整解壓縮到可寫入的資料夾，雙擊 `ImageEditing.exe`。圖片統一放在程式旁的
`Images`，指令使用相對路徑，開頭不用加 `/`：

```text
load Images/wiz.tga
npr-paint-advanced
save Output/my-oil-paint.png
```

視窗模式會自動以 EXE 所在資料夾為工作目錄，整個 portable 資料夾可以直接搬動。
`Start.bat` 保留作為選用的啟動輔助檔。輸入 `run demo.txt`
會把五種 NPR 結果存到 `Output`；再次執行會覆寫同名示範輸出檔。
以下開發版範例中的 `load wiz.tga`，在 portable 版請改成 `load Images/wiz.tga`。

命令列批次測試可在 portable 資料夾執行 `.\ImageEditing.exe -headless demo.txt`。
`-headless` 模式保留終端機的目前工作目錄，腳本與圖片路徑以該目錄為準。

### 本機開發版本

建置後的執行檔位於 `build/Debug/ImageEditing.exe`。本機 VS Code 的工作目錄已設成 `E:/Development/NTUST-Computer graphics projects/P1`，因此可以直接讀取該資料夾的 `wiz.tga`；不需要搬動 exe。

在程式視窗底下的指令框逐行輸入，每一行按 Enter：

```text
load wiz.tga
npr-paint-advanced
save my-oil-paint.png
```

輸出在工作目錄 `P1` 裡。圖片支援 TGA、PNG、JPG/JPEG；有透明度時建議存 PNG 或 TGA。每次操作會修改目前的圖片，比較不同方法前要重新 `load` 原圖。

## Basic NPR 與 Advance NPR

| 方法 | 指令 | 效果 |
| --- | --- | --- |
| Basic NPR | `npr-paint` | 大、中、小圓形筆觸，呈現點描效果 |
| Advance NPR，預設 | `npr-paint-advanced` | 沿影像輪廓延伸的曲線油畫筆觸 |
| 較細的筆刷 | `npr-paint-advanced 0.7` | 較能保留細節 |
| 較粗的筆刷 | `npr-paint-advanced 1.7` | 大色塊與較明顯的長筆觸 |
| 指定筆刷和亂數種子 | `npr-paint-advanced 1.7 42` | 改變筆觸顏色擾動與覆蓋順序 |
| 卡通／賽璐璐 | `npr-cartoon` | 參考《聖騎士之戰 -奮戰-》的色塊、分層明暗與描邊 |
| 加強卡通化 | `npr-cartoon 1.7` | 較少色階、較明顯描邊 |
| 水彩 | `npr-watercolor` | 參考《Disco Elysium》的繪畫感：暈染、紙紋與局部乾筆 |
| 較大水彩筆刷 | `npr-watercolor 1.7 42` | 調整暈染尺度與不均勻顏料分布 |

完整語法為 `npr-paint-advanced [brush-scale [seed]]`；方括號表示可省略，實際輸入不加方括號。筆刷倍率接受 0.5～3.0，預設 1.0；種子接受 0～4294967295 的整數，預設 1337。相同輸入、參數與執行檔會得到相同的進階 NPR 結果。基本版的圓點排列仍會隨每次執行改變。

水彩語法是 `npr-watercolor [brush-scale [seed]]`，參數範圍和預設值相同。卡通語法是 `npr-cartoon [strength]`，強度接受 0.5～3.0，預設 1.0；卡通不需要亂數種子。這三種是作用於輸入圖片的獨立 NPR 演算法，遊戲名稱表示視覺方向。

筆刷半徑會隨圖片短邊調整，並限制在合理範圍；很小的圖片可能看不出倍率差異。處理期間 GUI 可能短暫沒有回應，完成後會顯示結果。`wiz.tga` 的各種預設效果在本機 Debug 版本可於數秒內完成；大圖會較久。

比較指令：

```text
load wiz.tga
npr-paint
save my-basic.png
load wiz.tga
npr-paint-advanced
save my-advanced.png
load wiz.tga
npr-paint-advanced 1.7
save my-advanced-bold.png
load wiz.tga
npr-cartoon
save my-cartoon.png
load wiz.tga
npr-watercolor
save my-watercolor.png
```

本機 `P1/npr-demo.txt` 已準備好上述五張成果的示範，可在 GUI 輸入 `run npr-demo.txt`。它會把結果存到專案的 `examples/npr` 資料夾。可攜的專案版腳本是 `examples/npr/demo.txt`，需以 `ImageEditing-master` 為工作目錄執行。

## 其他功能

以下指令都在 `load` 後使用；表格中的 `N`、`s`、`angle`、`file` 要換成實際數值或檔名。

| 類別 | 指令 | 說明 |
| --- | --- | --- |
| 灰階 | `gray` | 轉為亮度灰階 |
| 色彩量化 | `quant-unif`、`quant-pop` | 均勻量化、熱門色彩量化 |
| 黑白抖色 | `dither-thresh`、`dither-bright`、`dither-rand` | 固定閾值、保留平均亮度、隨機抖色 |
| 黑白抖色 | `dither-cluster`、`dither-fs` | 群聚式閾值、Floyd–Steinberg 誤差擴散 |
| 彩色抖色 | `dither-color` | RGB 色盤與誤差擴散 |
| 模糊 | `filter-box`、`filter-bartlett`、`filter-gauss` | 5×5 濾波 |
| 自訂模糊 | `filter-gauss-n N` | N 必須是正奇數，例如 `filter-gauss-n 9` |
| 邊緣與銳化 | `filter-edge`、`filter-enhance` | 高通、細節增強 |
| 縮放 | `half`、`double`、`scale s` | 減半、加倍、正倍率縮放，例如 `scale 1.5` |
| 旋轉 | `rotate angle` | 順時針角度，保留原畫布尺寸，例如 `rotate 30` |
| 合成 | `comp-over file`、`comp-in file`、`comp-out file`、`comp-atop file`、`comp-xor file` | 與另一張同尺寸圖片進行 Alpha 合成 |
| 差異 | `diff file` | 與同尺寸圖片的絕對 RGB 差異 |
| 批次 | `run file` | 逐行執行文字腳本 |

檔名／相對路徑不可含空白，腳本最後一行須保留換行。沒有復原功能；要恢復原圖請重新 `load`。

## 成果圖與繳交資料

已附程式實際輸出的 PNG，先看[三種進階效果比較](../examples/npr/comparison.png)：

1. [Basic NPR](../examples/npr/basic.png)
2. [Advance NPR，預設](../examples/npr/advanced.png)
3. [Advance NPR，粗筆刷](../examples/npr/advanced-bold.png)
4. [卡通／賽璐璐](../examples/npr/cartoon.png)
5. [水彩](../examples/npr/watercolor.png)

這些是演算法輸出圖，並非應用程式視窗截圖。評分表列的「3 snap shots」可依下列流程準備：每次 `load wiz.tga`，分別執行 `npr-paint-advanced`、`npr-cartoon`、`npr-watercolor`，完成後截取含影像與指令框的完整視窗，讓方法與效果能對照。

新版評分表要求原始碼、操作說明、技術文件與三張截圖。本文件為操作說明；[技術說明](NPR_TECHNICAL.md) 包含基本功能概述與進階 NPR 的實作。繳交前補上自己的姓名、學號，並依課程要求附上截圖及原始碼。附圖使用作業提供的 `wiz.tga`。
