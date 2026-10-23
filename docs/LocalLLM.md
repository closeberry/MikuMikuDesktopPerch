# ローカルLLMの導入（Gemma 4 E4B）

MikuMikuDesktopPerch の「話しかける」は、このコンピューターで動くLLMのサーバーに話しかけます。ここでは、日本語の受け答えが自然な Google の **Gemma 4 E4B**（Apache 2.0 ライセンス）を使えるようにする手順をまとめます。

方法は2つあります。どちらか片方で十分です。

| 方法 | 向いている人 | 必要なもの |
|---|---|---|
| A. llama.cpp（`llama-server`） | 軽く動かしたい、設定を細かく決めたい | ダウンロードした zip とモデルファイル（約4.6GB） |
| B. Ollama | インストーラーで手早く済ませたい | Ollama 本体とモデル（自動でダウンロード） |

## 目安となる環境

- **GPU**：VRAMが6GB以上あると快適です（E4B の Q4_K_M、コンテキスト8192トークンのとき）。足りないときは、ひと回り小さい **E2B**（約3.1GB、VRAM 2GBほど）にするか、コンテキストを4096に減らします
- **ディスク**：モデル1つにつき約5GB（Ollama は10GBほど）
- GPUがなくても CPU だけで動きますが、返事にかなり時間がかかります

## A. llama.cpp で動かす

### 1. llama.cpp を入れる

1. [llama.cpp のリリースページ](https://github.com/ggml-org/llama.cpp/releases) を開き、`b11503` のような **「b」と番号のビルド**（「Pre-release」と表示されます）の Assets から、使っている GPU に合う Windows 用の zip をダウンロードします。「Latest」と表示される `v0.x.x` には Windows 用のファイルがないことがあります（この手順は `b11503` で確認しました）
   - **NVIDIA**：`llama-b<番号>-bin-win-cuda-13.4-x64.zip` と、CUDA ランタイムの `cudart-llama-bin-win-cuda-13.4-x64.zip` の2つ。GPU のドライバが古くて動かないときは、`12.4` の組み合わせにします
   - **AMD / Intel**：`llama-b<番号>-bin-win-vulkan-x64.zip`
   - **GPUなし**：`llama-b<番号>-bin-win-cpu-x64.zip`
2. 好きなフォルダ（例：`C:\LocalLLM\llama.cpp`）に展開します。cudart の zip も同じフォルダに展開します

### 2. モデルをダウンロードする

[unsloth/gemma-4-E4B-it-GGUF](https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF) の「Files and versions」から **`gemma-4-E4B-it-Q4_K_M.gguf`**（約4.6GB）をダウンロードし、`C:\LocalLLM\models` に置きます。

- 同じファイルは [lmstudio-community/gemma-4-E4B-it-GGUF](https://huggingface.co/lmstudio-community/gemma-4-E4B-it-GGUF) にもあります
- 画像の入力は使わないので、`mmproj-*.gguf` は要りません
- 小さい E2B を使うときは、同じ手順で `gemma-4-E2B-it-Q4_K_M.gguf` を選びます（[unsloth/gemma-4-E2B-it-GGUF](https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF)）

### 3. サーバーを起動する

次の内容で `C:\LocalLLM\start-gemma.bat` を作り、ダブルクリックで起動します（止めるときはそのウィンドウで Ctrl+C）。

```bat
@echo off
"%~dp0llama.cpp\llama-server.exe" -m "%~dp0models\gemma-4-E4B-it-Q4_K_M.gguf" --host 127.0.0.1 --port 18080 -ngl 99 -c 8192 -np 1
```

| オプション | 意味 |
|---|---|
| `--host 127.0.0.1` | このコンピューターからだけ接続を受け付けます。`0.0.0.0` にしないでください（同じネットワークの他の機器からも使えてしまいます） |
| `--port 18080` | 待ち受けるポート。ほかと重ならない番号なら何でも構いません |
| `-ngl 99` | 全部のレイヤーを GPU に載せます。VRAMが足りないときは数を減らすか、`0`（CPUだけ）にします |
| `-c 8192` | 一度に扱える長さ（トークン数）。VRAMが足りないときは `4096` に |
| `-np 1` | 同時に処理する会話の数。マスコットとの会話は1つなので1で十分です |

ウィンドウに `listening on http://127.0.0.1:18080` と出たら準備完了です。ブラウザで http://127.0.0.1:18080/health を開いて `{"status":"ok"}` と出れば動いています。

### 4. MikuMikuDesktopPerch の設定

1. マスコットを右クリック →「設定...」→「会話」
2. 接続先に `http://127.0.0.1:18080/v1` を入れます
3. 「一覧を取得」を押すと、モデルのファイル名が入ります（llama-server はこの名前に関係なく、起動したモデルで返事をします）
4. OK を押します

## B. Ollama で動かす

1. [Ollama](https://ollama.com/download) をインストールします。すでに入っているときは、Gemma 4 に対応した新しい版に更新してください
2. コマンドプロンプトで次を実行します（初回はダウンロードに時間がかかります）

   ```
   ollama pull gemma4:e4b
   ```

3. MikuMikuDesktopPerch の「設定...」→「会話」で、接続先が `http://127.0.0.1:11434/v1`（既定値）になっていることを確かめ、「一覧を取得」から `gemma4:e4b` を選んで OK を押します

Ollama はタスクトレイに常駐して、使うときだけモデルを読み込みます。しばらく使わないとモデルは自動でメモリから外れます。

## 補足

- **考えてから答える機能（thinking）**：Gemma 4 は、答える前に考える内容を出すことがあります。マスコットとの会話では返事が遅くなるだけなので、MikuMikuDesktopPerch は llama.cpp に「考えずに答える」指定（`chat_template_kwargs.enable_thinking=false`）を付けて送ります。もし考えた内容が `<think>`〜`</think>` として本文に混じっても、吹き出しには表示しません
- **口調**：「設定...」→「会話」→「人物設定...」で、名前と性格・話し方をモデルごとに書けます。「一人称」「語尾」「相手の呼び方」などを具体的に書くと、それらしくなります
- **吹き出しで表示できない文字**：絵文字は吹き出しのフォントで表示できないため、取り除いてから表示します（♪ ★ ☆ は残します）
- **安全のため**：接続先には、このコンピューター（`localhost`、`127.0.0.1`、`::1`）のサーバーしか指定できません。会話の内容はコンピューターの外に送られません
- **ライセンス**：Gemma 4 は Apache 2.0 ライセンスで公開されています。GGUF ファイルは、Google が公開したモデルを有志が変換したものです

## うまくいかないとき

| 症状 | 確かめること |
|---|---|
| 「LLMのサーバーに接続できませんでした」 | サーバーが起動しているか。接続先のポート番号が起動したものと同じか（llama.cpp の例は 18080、Ollama は 11434） |
| 「モデルの一覧を取得できませんでした」 | 接続先の末尾が `/v1` になっているか |
| 「model '…' not found」（Ollama） | 「一覧を取得」で出てきた名前を選んでいるか。`ollama pull` が終わっているか |
| 返事がとても遅い | GPUが使われているか（llama.cpp は `-ngl 99`、CUDA 版や Vulkan 版を使っているか）。VRAMが足りないと一部が CPU で動いて遅くなるので、E2B にするか `-c 4096` に |
| 返事がおかしい・途中で切れる | モデルファイルのダウンロードが途中で止まっていないか（サイズが Hugging Face の表示と同じか） |
