# Brokie

**Big model energy. Small wallet.**

One C file: automatic model routing, failover, encrypted keys, and streaming for
OpenAI clients and Claude Code. Linux, macOS, and Windows through WSL.

## Build and start

Install dependencies:

```sh
# Ubuntu / Debian / WSL
sudo apt install build-essential pkg-config libcurl4-openssl-dev libssl-dev
# macOS
brew install pkg-config curl openssl@3
```

```sh
git clone https://github.com/ykahalan/brokie.git
cd brokie
sh build.sh
./brokie init
./brokie sync
./brokie providers
./brokie key add google
./brokie serve
```

`init` asks for a vault password; `key add` asks for your provider key.
State lives in `~/.brokie`. No FreeLLMAPI installation is needed.

## Use

In another terminal:

```sh
./brokie claude smartest
./brokie run python3 fastest my_client.py
./brokie list reliable
./brokie stats
```

Modes: `smartest`, `fastest`, `reliable`, `balanced`. API model IDs use `auto:MODE`;
`provider/model-id` pins a model. OpenAI base URL: `http://127.0.0.1:31817/v1`.
The launchers supply authentication and model settings automatically.

[78 provider sign-up links and API bases](PROVIDERS.md), including image, audio, and search.
Use `/native/PROVIDER/PATH` for their native APIs. `sync` retrieves the signed catalog.
Restart the server after configuration changes. Provider quotas and charges still apply.

[Configuration, scoring, API limits, and verification](GUIDE.md).

## Credits

Based on [FreeLLMAPI](https://github.com/tashfeenahmed/freellmapi) by Tashfeen Ahmed
(routing/catalog code) and [UltimateWrap](https://github.com/Osamious/ultimatewrap)
by Osama Hasoneh (vault/client workflows).
[MIT license](LICENSE) · [Upstream notices](THIRD_PARTY_NOTICES.md).
