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
./brokie health
```

Modes: `smartest`, `fastest`, `reliable`, `balanced`. API model IDs use `auto:MODE`;
`provider/model-id` pins a model. OpenAI base URL: `http://127.0.0.1:31817/v1`.
The launchers supply authentication and model settings automatically.

[Provider sign-up links and API bases](PROVIDERS.md), including image, audio, and search.
The server refreshes the signed model catalog hourly and checks keys every 15 minutes;
provider setup links refresh daily. `check` checks keys now; `health` shows cached results.
Use `/native/PROVIDER/PATH` for native APIs. Restart after editing keys/settings.
`BROKIE_OFFLINE=1` disables background downloads and checks. Provider quotas and charges apply.

[Configuration, scoring, API limits, and verification](GUIDE.md).

## Credits

Based on [FreeLLMAPI](https://github.com/tashfeenahmed/freellmapi) by Tashfeen Ahmed
(routing/catalog code) and [UltimateWrap](https://github.com/Osamious/ultimatewrap)
by Osama Hasoneh (vault/client workflows).
[MIT license](LICENSE) · [Upstream notices](THIRD_PARTY_NOTICES.md).
