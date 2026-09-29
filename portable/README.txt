P1 Image Editing - Portable Demo

適用環境：Windows 10 / 11，x64。

啟動
1. 將 ZIP 全部解壓縮到可寫入的資料夾，例如桌面。
2. 雙擊 ImageEditing.exe，啟動程式。
3. 在視窗下方的指令框輸入指令，每行按 Enter。

圖片放在程式旁的 Images 資料夾，輸出存到 Output 資料夾。
視窗模式會自動以 EXE 所在資料夾讀寫相對路徑，可以搬動整個資料夾。
Start.bat 保留作為選用的啟動輔助檔，平常直接雙擊 EXE 即可。
請保留 EXE 旁附帶的 DLL。

手動操作
load Images/wiz.tga
npr-paint-advanced
save Output/my-oil-paint.png

比較效果前先重新載入原圖：
load Images/wiz.tga
npr-cartoon
save Output/my-cartoon.png

load Images/wiz.tga
npr-watercolor
save Output/my-watercolor.png

批次示範
在程式的指令框輸入：
run demo.txt

五種效果會存到 Output：basic.png、advanced.png、advanced-bold.png、
cartoon.png、watercolor.png。批次完成後，視窗顯示最後的水彩效果。
重新執行會覆寫這五個同名輸出檔；其他圖片請使用不同的輸出檔名。
也可以在 portable 資料夾開啟終端機執行：
.\ImageEditing.exe -headless demo.txt
命令列的 -headless 模式以終端機的目前資料夾讀寫相對路徑。

放入自己的圖片
將 TGA、PNG、JPG/JPEG 圖片放到 Images，例如 Images/photo.jpg，然後輸入：
load Images/photo.jpg

路徑使用 Images/，開頭不用加 /。檔名及指令中的相對路徑不要含空白。
解壓縮的父資料夾可以含空白或中文；EXE 會處理資料夾路徑。
沒有復原功能，要恢復原圖請重新 load。腳本最後一行要保留換行。

常用指令
gray                         灰階
quant-unif / quant-pop       色彩量化
dither-fs / dither-color     黑白／彩色抖色
filter-box / filter-gauss    模糊
filter-edge / filter-enhance 邊緣／銳化
half / double / scale 1.5    縮放
rotate 30                   順時針旋轉
npr-paint                   基本圓形筆觸
npr-paint-advanced 1.7 42    油畫；筆刷倍率 0.5～3，種子可省略
npr-cartoon 1.7             卡通；強度 0.5～3
npr-watercolor 1.7 42       水彩；筆刷倍率 0.5～3，種子可省略

重建 portable
本壓縮檔供直接執行。修改程式後，請在原始碼專案資料夾中執行：
powershell -NoProfile -ExecutionPolicy Bypass -File .\Make-Portable.ps1

建置端需要 CMake 和 Visual Studio 2019 C++ x64 工具。
新的壓縮檔會產生在原始碼專案的 dist/P1-Demo-Windows-x64.zip。
Images 資料夾的內容也會一起打包；助教電腦不需要安裝建置工具。
