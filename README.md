# Mu16TTY

音階でUTF-8メッセージを送受信する、実験的なアマチュア無線用16値FSKチャットソフトです。
日本語や絵文字を音声帯域で伝送し、ブラウザに履歴と受信状態を表示します。

現在の主な実装はWindows x64向けC版です。
通信方式と実装は初版公開に向けた調整中です。

## 主な機能

- 40 / 20 / 10 / 4 symbol/sの送受信
- 畳み込み符号とViterbi復号、CRC検査
- 先頭同期と途中同期、受信途中の文字表示
- 履歴一覧、詳細表示、入力波形・ウォーターフォール
- 履歴保存、重複受信とCRC不一致パケットの表示
- サウンドカード選択、RTS/DTRによるPTT制御
- HTTPの接続元IPv4制限とBasic認証

## Windowsで使う

配布用ファイル名は Mu16TTY-native-windows-x64.zip です。
プロジェクトURL: https://github.com/CQAKIBA/Mu16TTY

1. ZIPを展開し、Mu16TTY-native.exeを起動します。
2. コンソールに表示される http://127.0.0.1:ポート番号/ をブラウザで開きます。
3. デバイス情報の設定コンソールから自局と機器を設定します。

ブラウザは自動起動しません。初回の空きポートを保存します。

| コマンド | 内容 |
|---|---|
| show | 現在の設定 |
| my JA1XXX | 自局コールサインを設定（例示用） |
| sound in / sound out | 音声デバイス一覧 |
| sound in 1 / sound out 3 | デバイス指定（番号は環境依存） |
| sound in auto / sound out auto | システム既定 |
| ptt list | COMポート一覧 |
| ptt COM3 rts | PTTのポートと制御線 |
| ptt none | PTT無効 |
| frame | 送信前後の待ち時間など |
| loglevel all / loglevel valid | CRC不一致の表示切替 |
| http | HTTP設定とサブコマンド |

NOCALLのままではブラウザから送信できません。終了はCtrl+Cまたはコンソールの閉じるボタンで行います。
設定はconfig/、履歴はdata/に保存します。バックアップは終了後に行ってください。
HTTP設定はconfig/http.iniに保存され、パスワードは平文です。HTTP上のBasic認証は暗号化されません。

## 開発と仕様

- [C版ビルド方法](docs/build-native.md)
- [暫定通信方式仕様書](docs/Mu16TTY暫定通信方式仕様書.md)
- [テスト方法とデータ](tests/README.md)

| 場所 | 内容 |
|---|---|
| native/ | CモデムとWindowsサーバ |
| web/ | HTML/CSS/JavaScript |
| tests/ | 自動テスト |
| docs/ | 仕様・設計・実験記録 |
| packaging/ | 配布生成スクリプト |

## 公開準備

オリジナルのソースは[MIT License](LICENSE)で公開します。

Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO
Released under the MIT License.

第三者コンポーネントにはそれぞれのライセンスが適用されます。
第三者コンポーネントの説明と権利表記も確認してください。
