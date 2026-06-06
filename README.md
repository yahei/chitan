# chitan

小さい端末エミュレータ  
C言語, Xlib, Xft  
日本語インライン入力対応  

#### IMEの表示改善

Vimで日本語入力するとき右側の文字が押し出されていく機能を追加してみた  
ime.vimを~/.vim/pluginにコピーして使う  

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

設定ファイルは`$XDG_CONFIG_HOME/chitan/chitan.ini`  
または`~/.config/chitan/chitan.ini`です。  
引数のa,f,g,lと同様の設定に加えて色の設定ができます。  

`foreground` 文字色  
`background` 背景色  
`color*` パレットの*番目の色  

記述例  
```
# comment
alpha           = 0.95
font            = monospace:size=12
geometry        = 80x24+0+0
lines           = 1024
foreground      = #ffffff
background      = #000000
color10         = #00ff00
```

デフォルトのカラースキームは[Selenized black](https://github.com/jan-warchol/selenized)です。  
