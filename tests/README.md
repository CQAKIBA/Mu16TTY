# テスト

CMakeでビルド後、リポジトリルートから実行します。

    cmake --build build/native --config Release
    ctest --test-dir build/native -C Release --output-on-failure

テストは固定パケットとのバイト一致、CRC破損の検出、40/20/10/4 symbol/sのUTF-8送受信を確認します。
音声デバイスは開きません。波形は実行時にメモリ内で生成します。

data/packet-v1.jsonと.binは同一の固定パケットです。
ヘッダはネットワークバイト順、CRCはCRC-32Cです。
このベクトルの組み立てとCRC計算はモデム関数とは独立に行っています。

実無線機、ノイズ耐性、HTTP認証、PTT機器との接続はこのテストの対象外です。
