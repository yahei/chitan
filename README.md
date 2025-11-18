# chitan

小さい端末エミュレータ  
C言語, Xlib, Xft  
日本語インライン入力対応  

## 使い方

#### インストール

```
make install
```
terminfoもインストールされます。  

#### アンインストール

```
make uninstall
```
terminfoは削除されません。  
消したい場合は手動でファイルを削除してください。  

### 引数

`-a` 背景の不透明度を0.0から1.0で設定  
`-f` フォントを"monospace:size=12"のような形式で指定  
`-g` ウィンドウの大きさと位置を"80x24+0+0"のような形式で指定  
`-h` ヘルプを表示  
`-l` バッファの行数を設定  
`-v` バージョンを表示  
`-e` 起動時に実行するコマンド  

### 設定

xrdbを使用しています。  
引数のa,f,g,lと同様の設定に加えて色の設定ができます。  

`chitan.foreground` 文字色  
`chitan.background` 背景色  
`chitan.color*` パレットの*番目の色  

記述例  
```
! --- chitan ---
chitan.alpha:           0.95
chitan.font:            monospace:size=12
chitan.geometry:        80x24+0+0
chitan.lines:           1024
chitan.foreground:      #ffffff
chitan.background:      #000000
chitan.color10:         #00ff00
```

デフォルトのカラースキームは[Selenized black](https://github.com/jan-warchol/selenized)です。  
