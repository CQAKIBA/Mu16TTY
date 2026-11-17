# C版Windows配布物のビルド

## 必要なもの

- Windows x64
- Visual Studio 2022 Build ToolsのC/C++コンパイラとWindows SDK
- CMake 3.21以上
- PortAudioのWindows x64 DLLと同梱README

Visual Studio開発環境からリポジトリルートで実行します。

    cmake -S native -B build/native -A x64
    cmake --build build/native --config Release --target mu16tty_station

ZIP生成はPowerShellから実行します。

    .\packaging\build-native-portable.ps1 -PortAudioDirectory C:\path\to\portaudio-binaries

指定先にはlibportaudio64bit.dll、README.mdが必要です。
PortAudioDirectoryは必須です。通常版DLLの取得元はTHIRD_PARTY_NOTICES.mdを参照してください。
CMakeの既定パスと異なる環境では-CMakeで実行ファイルを指定してください。

dist/にZIPとSHA-256ファイルを生成します。
実行ファイル、モデムDLL、Web資産、PortAudio、説明を収録します。
利用者のconfig、data、debug.logはコピーしません。
