Mu16TTY v0.1.0 C版 Windows x64

ZIPを展開しMu16TTY-native.exeを起動します。
コンソールのhttp://127.0.0.1:ポート番号/をブラウザで開いてください。
初回の空きポートは保存されます。ブラウザは自動起動しません。

設定コンソールでmy JA1XXXのように自局を設定します。
NOCALLのままではブラウザから送信できません。
sound in / sound outで一覧、sound in 1のように番号を指定します。
autoでシステム既定を選べます。ptt listでCOM一覧、ptt COM3 rtsでPTTを設定します。
showで全設定、frameとhttpで各設定の使い方を表示します。

設定はconfig/、履歴はdata/に保存します。
HTTPパスワードはconfig/http.iniに平文保存されます。
HTTP上のBasic認証は暗号化されません。
移行やバックアップは終了後に行ってください。
終了はCtrl+Cまたはコンソールの閉じるボタンで行います。

Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO
Released under the MIT License.
オリジナルソースのライセンス全文はLICENSEを参照してください。
第三者コンポーネントにはそれぞれのライセンスが適用されます。

通信方式は同梱のMu16TTY通信方式仕様書.mdを参照してください。
