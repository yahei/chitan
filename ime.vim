vim9script

# --- モード切替時にDECSET/RST ---

# これが有効のとき、ターミナルエミュレータ側はPreeditを自分で書かず
# OSCでこちらに送ってくる

augroup IMEReport
	autocmd!
	autocmd InsertEnter  * :call echoraw("\x1b[?4160h")
	autocmd InsertLeave  * :call echoraw("\x1b[?4160l")
augroup END



# --- Preeditの受信 ---

var head: number
var tail: number
var text: string

def OSCReceive(): void
	# UTF-8の文字に <80> が含まれていると <80><fe>X に置換されて壊れるみたい
	# こうして <fe>X を取り除くと直る
	var osc = substitute(v:termosc, "\xfeX", '', 'g')

	if osc[2] == 'p' && 2 <= count(osc, ';')
		var list = matchlist(osc, '\vp([0-9]*);([0-9]*);(\p*)')
		head = str2nr(list[1])
		tail = str2nr(list[2])
		text = list[3]
		PrintPreedit()
	endif
enddef
autocmd! TermResponseAll * call OSCReceive()



# --- Preeditの表示 ---

## プロパティタイプ
highlight Attr1 cterm=reverse
highlight Attr2 cterm=underline
prop_type_delete('prop1')
prop_type_delete('prop2')
prop_type_add('prop1', {'highlight': 'Attr1'})
prop_type_add('prop2', {'highlight': 'Attr2'})

## テキストプロパティとしてPreeditを表示する
def PrintPreedit(): void
	# 本来はカーソルを表示するが、反転表示で代用
	if (head == tail)
		tail += 1
	endif

	prop_clear(line('.'))

	for i in range(strchars(text))
		var prop = (head <= i && i < tail) ? 'prop1' : 'prop2'
		prop_add(line('.'), col('.'), {'type': prop, 'text': text[i]})
	endfor

	winrestview(winsaveview())
enddef

## undoしたときにテキストプロパティが復活してしまうので消す
def Clear(): void
	prop_clear(getpos('.')[1])
	winrestview(winsaveview())
enddef
autocmd IMEReport TextChanged * :call Clear()


#
# 適当に決めたOSCの形式
#
# <ESC> ] p Pn1 ; Pn2 ; Text <BEL>
#
# Pn1 Pn2: カーソルの先頭と終端の位置
#          Pn1 < Pn2 ならその範囲を反転表示する
#          Pn1 == Pn2 ならその位置にカーソルを表示する
#          Pn1 > Pn2 ならカーソルの表示も反転表示もしない
#
# Text: 表示する文字列(UTF-8)
#
#
# 例: \e]p0;3;日本語入力\a
#
