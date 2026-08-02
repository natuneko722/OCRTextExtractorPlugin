# OCR文字抽出プラグイン Ver1.1

AviUtl2 用の汎用プラグイン（`.aux2`）です。画面上でドラッグした領域を GDI で取得し、Windows OCR (`Windows.Media.Ocr`) の日本語エンジンで文字抽出します。

## 実装済み

- 選択領域のキャプチャと位置・サイズ表示
- 日本語 Windows OCR による非同期認識
- 結果の編集、コピー、空白・改行の軽い整形
- 最新20件の履歴（アプリケーションデータに保存）
- 選択範囲のPNG保存（既定ではプロジェクトファイルと同じフォルダ）
- ⚙ボタンから変更できる画像保存先
- 保存画像のタイムライン配置（⚙の設定で有効化）
## ビルド

Visual Studio の「C++/WinRT」と Windows SDK をインストールしてから、Developer PowerShell で実行します。

```powershell
cmake -S . -B build -G Ninja
cmake --build build --config Release
```

生成される `OCRTextExtractorPlugin.aux2` を AviUtl2 の `Plugin` フォルダに配置してください。日本語 OCR 言語パックが未導入の場合は、Windows の「オプション機能」から日本語 OCR を追加してください。

現時点では仕様書で将来項目とされている翻訳、連続OCR、テキストオブジェクト作成は未実装です。
