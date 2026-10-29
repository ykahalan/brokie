# Brokie

**Big model energy. Small wallet.**

A tiny LLM gateway for stretching free-tier quotas, based on the code and ideas in
[FreeLLMAPI](https://github.com/tashfeenahmed/freellmapi) by **Tashfeen Ahmed** and
[UltimateWrap](https://github.com/Osamious/ultimatewrap) by **Osama Hasoneh**.

A standalone, single-file C gateway for OpenAI-compatible clients and Claude Code.
Runs on Linux, macOS, and Windows through WSL. Requires libcurl and OpenSSL 3.
No Node server, Python runtime, database, FreeLLMAPI installation, or CCR gateway is required.

The complete implementation is `brokie.c`. `build.sh` is only a build convenience.
This folder contains no credentials, local migration scripts, or personal configuration.

## Build

Ubuntu / Debian, including Ubuntu in WSL:

```sh
sudo apt install build-essential pkg-config libcurl4-openssl-dev libssl-dev
sh build.sh
```

macOS, with Xcode Command Line Tools and Homebrew:

```sh
brew install pkg-config curl openssl@3
sh build.sh
```

Compile from source on each platform. Linux/WSL has been verified locally;
GitHub Actions builds and checks both Ubuntu and Apple Silicon macOS.

## Start from scratch

```sh
./brokie init
./brokie key add google https://generativelanguage.googleapis.com/v1beta/openai
./brokie sync
./brokie list smartest
./brokie serve
```

`init` prompts for a vault password; `key add` prompts for the provider key without echoing it.
Other commands that need keys prompt for the vault password. `BROKIE_PASSWORD` can supply
the password for headless execution; the program removes it from its environment after unlocking.

State defaults to `~/.brokie`. To keep an isolated setup, prefix every command with
`--dir /absolute/path/to/state` (the parent directory must already exist).

For example, the gateway base URLs are:

- OpenAI clients: `http://127.0.0.1:31817/v1`
- Claude Code: `http://127.0.0.1:31817`

Run the server in one terminal, then launch clients in another:

```sh
./brokie claude smartest
./brokie claude fastest -p 'Reply with exactly hello.'
./brokie run python3 reliable my_client.py
```

The launcher supplies the gateway credential in the child's environment. It does not edit the
client's settings. OpenAI clients receive `OPENAI_API_KEY`, `OPENAI_BASE_URL`, and `OPENAI_MODEL`.
For example, a Python OpenAI client can use `model=os.environ['OPENAI_MODEL']`.
Claude Code receives its base URL, authentication token, model and subagent model aliases.

The endpoint is loopback-only. With WSL, these URLs refer to **WSL's** localhost.
Native Windows clients require working Windows/WSL localhost forwarding.
Run Claude Code inside WSL when localhost forwarding is unavailable.

## Routing

Requests accept `auto:smartest`, `auto:fastest`, `auto:reliable`, `auto:balanced`, or `auto`.
An explicit `provider/model-id` pins a model without substituting a different model.

| Mode | Reliability | Speed | Intelligence |
|---|---:|---:|---:|
| smartest | .35 | .10 | .55 |
| fastest | .35 | .55 | .10 |
| reliable | .70 | .15 | .15 |
| balanced | .50 | .25 | .25 |

Reliability uses Beta(1 + successes, 1 + failures) Thompson sampling with a two-day half-life.
The list command uses the mean for stable display. Speed blends throughput and first-byte latency.
Intelligence uses the catalog tier and rank: `tier*1000 - 31*sqrt(rank)`, normalized over the chain.
Quota headroom reduces the combined score below 20% remaining; exhausted routes and cooldowns are
excluded. Keys are selected using their observed reliability and speed. The same opening user
message stays on a model for 30 minutes when possible; set `sticky_seconds` to zero to disable this.

```sh
./brokie mode reliable     # changes the default; restart the server afterwards
./brokie list fastest
```

The service records operational statistics, not prompts, responses, or conversation histories.

## Keys and models

Provider names are arbitrary labels; the URL and model records determine how requests are sent.
Use an upstream **OpenAI-compatible Chat Completions** endpoint. Examples:

```sh
./brokie key add mistral https://api.mistral.ai/v1
./brokie key add openrouter https://openrouter.ai/api/v1
./brokie key add local http://127.0.0.1:8080/v1 --anonymous
./brokie model add local my-model
./brokie discover mistral
```

For Cloudflare, use provider `cloudflare`, base URL
`https://api.cloudflare.com/client/v4/accounts/YOUR_ACCOUNT_ID/ai/v1`, and enter only the API token.
`discover` prints upstream model IDs; it does not automatically enable unknown or paid models.
Adding another key for the same provider enables key rotation. Disable or relabel keys in JSON.

Edit `config.json` and restart the service. A manually added model looks like:

```json
{
  "id": 1,
  "provider": "local",
  "model": "my-model",
  "enabled": true,
  "tools": true,
  "vision": false,
  "context": 32768,
  "tier": 2,
  "rank": 50,
  "rpm": 0,
  "rpd": 0,
  "tpm": 0,
  "tpd": 0,
  "source": "user"
}
```

IDs must be unique positive integers. A zero quota/context means unknown. Tiers are Small=1,
Medium=2, Large=3, Frontier=4. Only mark tools/vision when the upstream model supports them.
An optional `key_id` binds a model to one credential, which is useful for custom endpoints.

Profiles are simple subsets: `"profile":"coding"` plus
`"profiles":[{"name":"coding","models":[1,2]}]`. An empty profile selects all enabled models.

## Optional public catalog

```sh
./brokie sync
```

This fetches `https://api.freellmapi.co/v1/latest`, checks the exact response bytes against the
pinned Ed25519 public key, and merges chat metadata into local configuration. The public service
currently returns its delayed monthly tier. For an existing premium license, use `brokie license`
to store it encrypted; the service decides entitlement. No provider keys are sent to the catalog host.

Sync preserves local keys, profiles, disabled entries, and models marked `source:user`. Catalog-owned
models missing from the new feed are removed. To hold a catalog field locally, add an `overrides`
array listing any of `tools`, `vision`, `context`, `rank`, `rpm`, `rpd`, `tpm`, `tpd` to that model.
Use `enabled:false` to keep a catalog model disabled across syncs; simply deleting it allows it to return.
Restart a running server after syncing. Sync is an explicit command, not a background scheduler.

Once populated, local configuration works offline from the catalog host. You can maintain models
manually and never use this feed. Inference still goes directly to your configured providers.

## State and credentials

- `config.json`: editable metadata; no API keys.
- `vault.bin`: AES-256-GCM encrypted credentials; PBKDF2-HMAC-SHA256, 600,000 iterations, random salt/IV.
- `stats.json`: success/speed counters, quota usage and cooldowns; no conversation text.
- `catalog.json`: the last verified public catalog, if used.

The state directory and new files use owner-only permissions on POSIX filesystems.
The vault password is required to recover the keys; the source does not persist the password.
Keep model/key IDs stable when editing because statistics reference those IDs.

## Implemented boundaries

- `/v1/models`, `/v1/chat/completions`, `/v1/messages`, and `/v1/messages/count_tokens`.
- Incremental text streaming, images, standard client tool calls/results, multiple tools,
  and Claude Code's mid-conversation system messages. Anthropic tool argument events are buffered
  until the call is complete, then emitted as normal tool-use blocks.
- Failover before an answer begins. A broken committed stream reports an error and must be retried
  by the client; it cannot safely restart a partial answer.
- Six attempts by default; bounded requests and 16 concurrent connections.

This is a deliberately limited implementation, not a full clone of either original project.
No Responses API, embeddings, media generation, native Anthropic server tools, or reasoning-trace
round-tripping. Token counting is an estimate. Quotas use fixed UTC minute/day windows and per-key,
per-model counters; account-wide provider quotas and provider-specific quirks are not fully modeled.
Statistics persist across restarts; sticky-session memory does not.

Claude Code may warn that the virtual model ID is unknown and assume a 200k context window.
The gateway independently checks the selected model's configured context before dispatch.

## Verification and provenance

Verified in Ubuntu WSL: strict warning-free C build, 25 offline protocol checks, ASan/UBSan checks,
real catalog signature verification, live OpenAI text and Anthropic tool-call responses, and a real
Claude Code prompt. GitHub Actions also passed the build and all 25 offline checks on
Ubuntu and Apple Silicon macOS.

## Credits and code provenance

Brokie is based on these two MIT-licensed projects:

- **[FreeLLMAPI](https://github.com/tashfeenahmed/freellmapi)** — by **Tashfeen Ahmed**.
  Its router and scoring code are the basis for Brokie's strategy weights, Beta reliability
  sampling, throughput/latency blend, intelligence tier/rank calculation, and quota headroom.
  Brokie also implements its signed catalog retrieval protocol and uses its published catalog key.
  References: [scoring.ts](https://github.com/tashfeenahmed/freellmapi/blob/main/server/src/services/scoring.ts),
  [router.ts](https://github.com/tashfeenahmed/freellmapi/blob/main/server/src/services/router.ts),
  [catalog-sync.ts](https://github.com/tashfeenahmed/freellmapi/blob/main/server/src/services/catalog-sync.ts).
- **[UltimateWrap](https://github.com/Osamious/ultimatewrap)** — by **Osama Hasoneh**.
  Its shared-vault, provider/catalog, and client-launch workflows informed Brokie's local
  credential separation, editable metadata, model listing, and Claude Code launcher.
  References: [keysync](https://github.com/Osamious/ultimatewrap/tree/master/keysync),
  [menu](https://github.com/Osamious/ultimatewrap/tree/master/menu),
  [refresh](https://github.com/Osamious/ultimatewrap/tree/master/refresh).

The implementation adapts these foundations into a single C file with direct provider calls.
The original authors deserve credit for the routing work, catalog, tooling, and design.
Their original MIT copyright and permission notices are preserved in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Brokie is also [MIT licensed](LICENSE).

The gateway itself has no usage charge. Provider quotas, promotional free tiers, and any charges
from your chosen providers remain under their control.
