# Setting up a local LLM (Gemma 4 E4B)

[日本語](LocalLLM.md)

MikuMikuDesktopPerch's "Talk" talks to an LLM server running on this computer. These are the steps to make Google's **Gemma 4 E4B** (Apache 2.0 license), which converses naturally in both Japanese and English, ready to use.

There are two ways. Either one is enough.

| Way | Suits you if | What you need |
|---|---|---|
| A. llama.cpp (`llama-server`) | You want it light, and want to set things in detail | The downloaded zip and a model file (about 4.6 GB) |
| B. Ollama | You want to be done quickly with an installer | Ollama itself and the model (downloaded automatically) |

## Rough requirements

- **GPU**: 6 GB of VRAM or more is comfortable (E4B at Q4_K_M with an 8192-token context). If that is not enough, use the one size smaller **E2B** (about 3.1 GB, around 2 GB of VRAM), or reduce the context to 4096
- **Disk**: about 5 GB per model (about 10 GB for Ollama)
- It runs on the CPU alone without a GPU, but replies take quite a while

## A. Running with llama.cpp

### 1. Install llama.cpp

1. Open [llama.cpp's releases page](https://github.com/ggml-org/llama.cpp/releases), and from the Assets of a **build named "b" and a number** such as `b11503` (shown as "Pre-release"), download the Windows zip that fits your GPU. The `v0.x.x` shown as "Latest" may have no files for Windows (these steps were checked with `b11503`)
   - **NVIDIA**: two files, `llama-b<number>-bin-win-cuda-13.4-x64.zip` and the CUDA runtime `cudart-llama-bin-win-cuda-13.4-x64.zip`. If it does not run because your GPU driver is old, use the `12.4` pair
   - **AMD / Intel**: `llama-b<number>-bin-win-vulkan-x64.zip`
   - **No GPU**: `llama-b<number>-bin-win-cpu-x64.zip`
2. Unpack it into a folder of your choice (for example `C:\LocalLLM\llama.cpp`). Unpack the cudart zip into the same folder

### 2. Download the model

From "Files and versions" of [unsloth/gemma-4-E4B-it-GGUF](https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF), download **`gemma-4-E4B-it-Q4_K_M.gguf`** (about 4.6 GB) and put it in `C:\LocalLLM\models`.

- The same file is also in [lmstudio-community/gemma-4-E4B-it-GGUF](https://huggingface.co/lmstudio-community/gemma-4-E4B-it-GGUF)
- Image input is not used, so `mmproj-*.gguf` is not needed
- For the smaller E2B, pick `gemma-4-E2B-it-Q4_K_M.gguf` the same way ([unsloth/gemma-4-E2B-it-GGUF](https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF))

### 3. Start the server

Make `C:\LocalLLM\start-gemma.bat` with the following, and double-click it to start (to stop, press Ctrl+C in its window).

```bat
@echo off
"%~dp0llama.cpp\llama-server.exe" -m "%~dp0models\gemma-4-E4B-it-Q4_K_M.gguf" --host 127.0.0.1 --port 18080 -ngl 99 -c 8192 -np 1
```

| Option | Meaning |
|---|---|
| `--host 127.0.0.1` | Accepts connections from this computer only. Do not make it `0.0.0.0` (other devices on the same network could use it too) |
| `--port 18080` | The port to listen on. Any number not used by something else will do |
| `-ngl 99` | Puts all layers on the GPU. If VRAM is not enough, lower the number, or make it `0` (CPU only) |
| `-c 8192` | The length it can handle at once (in tokens). `4096` if VRAM is not enough |
| `-np 1` | How many conversations are processed at once. The mascot has one conversation, so 1 is enough |

When the window shows `listening on http://127.0.0.1:18080`, it is ready. Open http://127.0.0.1:18080/health in a browser; if it shows `{"status":"ok"}`, it is running.

### 4. Set up MikuMikuDesktopPerch

1. Right-click the mascot → "Settings..." → Talking
2. Enter `http://127.0.0.1:18080/v1` as the server
3. Press "Get list"; the model's file name goes in (llama-server replies with the model it was started with, whatever this name is)
4. Press OK

## B. Running with Ollama

1. Install [Ollama](https://ollama.com/download). If it is already installed, update it to a new version that supports Gemma 4
2. Run this in a command prompt (the first time, the download takes a while)

   ```
   ollama pull gemma4:e4b
   ```

3. Under "Settings..." → Talking in MikuMikuDesktopPerch, check that the server is `http://127.0.0.1:11434/v1` (the default), pick `gemma4:e4b` from "Get list", and press OK

Ollama stays in the notification area and loads the model only when it is used. Left unused for a while, the model is unloaded from memory automatically.

## Notes

- **Thinking before answering**: Gemma 4 may put out what it thinks before answering. In talks with the mascot this only slows replies, so MikuMikuDesktopPerch sends llama.cpp the setting to answer without thinking (`chat_template_kwargs.enable_thinking=false`). If the thinking still gets into the text as `<think>`…`</think>`, it is not shown in the bubble
- **Language**: the app asks the LLM to reply in the language chosen under Display → Language in the settings (Japanese or English). Gemma 4 E4B handles both
- **Way of speaking**: under "Settings..." → Talking → "Persona...", you can write a name, personality and way of speaking for each model. Being concrete — how she refers to herself, favorite phrases, how she addresses you — makes it more convincing. The persona is sent every time she is talked to, so it must fit in the `-c` length above (`num_ctx` in Ollama) together with the instructions, memories and the recent conversation. At 4096, keep it to the essentials in a few hundred to a thousand characters (beyond it, the persona is the first thing lost)
- **Long memory**: the same model is also used to write out things worth remembering after a talk pauses (just one query in the background, which does not get in the way of replies). Even E4B picks up likes, plans, pet names and the like well enough. What it remembered can be checked and deleted under "Settings..." → Talking → "Memory..."
- **Feeling tags**: the LLM is asked to begin each reply with a feeling tag such as [smile] (［笑顔］ in Japanese), used for the expression. E4B keeps to it almost always. If it does not, the expression simply does not change
- **Voice**: with VOICEVOX or AivisSpeech running too, replies are spoken aloud ("Settings..." → Talking → "Voice" on, and the one in use chosen in the box below it). The voice supports Japanese only. When the LLM and the voice engine run on the same GPU, replies may be slower by the time it takes to make the voice
- **Characters the bubble cannot show**: emoji cannot be shown in the bubble's font, so they are removed before showing (♪ ★ ☆ are kept)
- **For safety**: only servers on this computer (`localhost`, `127.0.0.1`, `::1`) can be used. Your chats are not sent outside the computer
- **License**: Gemma 4 is released under the Apache 2.0 license. The GGUF files are conversions of Google's published model by volunteers

## When it does not work

| Symptom | What to check |
|---|---|
| "Could not connect to the LLM server" | Whether the server is running. Whether the server's port number is the one it was started on (18080 in the llama.cpp example, 11434 for Ollama) |
| "Could not get the list of models" | Whether the server address ends in `/v1` |
| "model '…' not found" (Ollama) | Whether you picked a name that "Get list" showed. Whether `ollama pull` has finished |
| Replies are very slow | Whether the GPU is used (llama.cpp with `-ngl 99`, and the CUDA or Vulkan build). If VRAM is not enough, part of it runs on the CPU and is slow, so use E2B or `-c 4096` |
| Replies are strange or cut off | Whether the model file's download stopped partway (whether its size matches what Hugging Face shows) |
