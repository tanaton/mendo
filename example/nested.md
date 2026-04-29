# Nested blockquote / list サンプル

GitHub での表示を確認するための検証用サンプル。
mendo の `blockquote_group_stack` / `list_counter` をカウンタ化した場合に
影響しうるケースを並べる。

---

## 1. 単純なネスト blockquote

> hoge
> > huga

---

## 2. 外 → 内 → 外 に戻る blockquote

外側を抜けずに内側に入り、内側を抜けて外側に戻るケース。
`blockquote_group_stack` の top を使っている現状の挙動と、
単純カウンタ化した場合に差が出る最重要ケース。

> 外側 1 行目
>
> > 内側 1 行目
> > 内側 2 行目
>
> 外側 2 行目 (内側を抜けて外側に戻った)
> 外側 3 行目

---

## 3. 三段ネスト

> 第 1 階層
> > 第 2 階層
> > > 第 3 階層
> > 第 2 階層に戻る
> 第 1 階層に戻る

---

## 4. 順序ありリストの番号

1. one
2. two
3. three

ネストを挟んでも親リストの番号が継続することの確認:

1. parent A
   1. child A-1
   2. child A-2
2. parent B (親の番号が継続するか)
3. parent C

---

## 5. 順序なし → 順序あり混在

- ul item 1
  1. ol child 1
  2. ol child 2
- ul item 2
  1. ol child 1 (新しい ol なので 1 から)
  2. ol child 2

---

## 6. blockquote の中にリスト

> 引用の中のリスト
>
> 1. 引用内 ol-1
> 2. 引用内 ol-2
>    - 入れ子の ul
>    - 入れ子の ul
> 3. 引用内 ol-3 (番号継続)

---

## 7. リストの中に blockquote

1. リスト ol-1
   > リスト内 blockquote 1 行目
   > リスト内 blockquote 2 行目
2. リスト ol-2
   > 別の blockquote (新規 group になるか)

---

## 8. blockquote ネスト + リスト混在

> 外側 引用
>
> 1. 外側 引用内 ol-1
>    > 入れ子 引用
>    > > 三段目 引用
>    > 入れ子 引用に戻る
> 2. 外側 引用内 ol-2 (番号継続するか)
>
> 外側 引用 末尾

---

## 9. GitHub Alert (ネスト blockquote)

> [!NOTE]
> Alert 親
> > 内側 blockquote (Alert 伝播するか?)
>
> Alert 親の続き

---

## 10. 連続する別グループの blockquote

> 1 つ目の引用

段落で区切る

> 2 つ目の引用 (group が変わるか)
