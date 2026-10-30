# Provider directory

79 providers from FreeLLMAPI's key manager, search registry, and catalog extras; updated 2026-10-30.
This is a directory of access paths, not a guarantee of ongoing free service. The notes are
upstream descriptions; confirm current eligibility, limits, and billing at the linked provider.

Start with `brokie providers`: it shows signup links, API bases, whether a key was added,
and whether the provider appears in your last signed catalog. Then run `brokie key add ID`.
Use `--anonymous` for optional-key providers. Cloudflare also prompts for its account ID.

`chat` supports the existing Chat Completions router and native pass-through. `native` uses
`/native/ID/PATH` with that provider's request format; it is excluded from automatic chat routing.
Transport support does not reproduce FreeLLMAPI's individual provider adapters or validate every
model/parameter combination. [Native API examples and limits](GUIDE.md#native-apis).

| ID | Register / get a key | API base | Access | Notes |
|---|---|---|---|---|
| `aclide` | [ACLIDE](https://aclide.com/en/dashboard/api-keys) | `https://aclide.com/v1` | native | shared monthly credits |
| `agnes` | [Agnes AI](https://platform.agnes-ai.com) | `https://apihub.agnes-ai.com/v1` | chat | free key |
| `aihorde` | [AI Horde](https://aihorde.net/register) | `https://oai.aihorde.net/v1` | native | no key needed, slow |
| `ainative` | [AINative Studio](https://ainative.studio) | `https://api.ainative.studio/api/v1` | chat | free key |
| `aion` | [Aion Labs](https://www.aionlabs.ai) | `https://api.aionlabs.ai/v1` | chat | free key |
| `airforce` | [Api.Airforce](https://api.airforce) | `https://api.airforce/v1` | chat | daily free requests; 1 per minute |
| `anyapi` | [AnyAPI](https://anyapi.ai) | `https://api.anyapi.ai/v1` | chat | free key |
| `apinex` | [APINex](https://apinex.bond/keys) | `https://api.apinex.bond/v1` | chat | shared daily free chat and decisions |
| `bai` | [B.AI](https://b.ai) | `https://api.b.ai/v1` | chat | promotional free model |
| `bazaarlink` | [BazaarLink](https://bazaarlink.ai) | `https://bazaarlink.ai/api/v1` | chat | free key |
| `beatapi` | [BeatAPI](https://beatapi.io) | `https://api.beatapi.io/v1` | native | JEV decisions; temporary free offer |
| `blaze` | [BlazeAPI](https://blazeapi.org/dashboard) | `https://api.blazeapi.org/paid/v1` | chat | daily free tokens; Discord verification |
| `blockrun` | [BlockRun](https://user.blockrun.ai/dashboard/keys) | `https://api.blockrun.ai/v1` | chat | zero-priced models |
| `brave` | [Brave Search](https://api-dashboard.search.brave.com) | `https://api.search.brave.com/res/v1` | native | Check current free credits and billing requirements. |
| `cerebras` | [Cerebras](https://cloud.cerebras.ai) | `https://api.cerebras.ai/v1` | chat | Check current free-plan eligibility. |
| `clod` | [CLōD](https://newapp.clod.io) | `https://api.clod.io/v1` | chat | shared daily free requests |
| `cloudflare` | [Cloudflare Workers AI](https://dash.cloudflare.com) | `https://api.cloudflare.com/client/v4/accounts/YOUR_ACCOUNT_ID/ai/v1` | chat | Check current free-plan eligibility. |
| `cohere` | [Cohere](https://dashboard.cohere.com/api-keys) | `https://api.cohere.ai/compatibility/v1` | chat | Check current free-plan eligibility. |
| `dreamprompting` | [DreamPrompting](https://dreamprompting.com) | `https://dreamprompting.com/api/v1` | chat | rolling 24h free tokens and requests |
| `electronhub` | [ElectronHub](https://app.electronhub.ai) | `https://api.electronhub.ai/v1` | chat | shared weekly credits |
| `exa` | [Exa](https://dashboard.exa.ai/api-keys) | `https://api.exa.ai` | native | Check current free credits and billing requirements. |
| `experiential` | [Experiential Labs](https://platform.experientiallabs.ai) | `https://api.experientiallabs.ai/v1` | chat | shared monthly credits |
| `firecrawl` | [Firecrawl](https://www.firecrawl.dev/app/api-keys) | `https://api.firecrawl.dev/v2` | native | Check current free credits and billing requirements. |
| `freeai` | [Free.ai](https://free.ai/account/#api-keys) | `https://api.free.ai` | native | shared daily free TTS tokens |
| `gizmo` | [Gizmo](https://gizmoplatforms.com/developers) | `https://api.gizmoplatforms.com/api/v1` | chat | monthly free-model requests |
| `google` | [Google AI Studio](https://aistudio.google.com/apikey) | `https://generativelanguage.googleapis.com/v1beta/openai` | chat | Check current free-plan eligibility. |
| `groq` | [Groq](https://console.groq.com/keys) | `https://api.groq.com/openai/v1` | chat | Check current free-plan eligibility. |
| `huggingface` | [HuggingFace Router](https://huggingface.co/settings/tokens) | `https://router.huggingface.co/v1` | chat | Check current free-plan eligibility. |
| `inferbase` | [Inferbase](https://inferbase.ai) | `https://api.inferbase.ai/v1` | chat | shared monthly and daily allowance |
| `iteracompute` | [IteraCompute](https://iteracompute.com/free/) | `https://api.iteracompute.com/v1` | chat | 30M free tokens / month, shared |
| `jamai` | [JamAI Base](https://cloud.jamaibase.com) | `https://api.jamaibase.com/api/v1` | chat | monthly ELLM allowance |
| `keenable` | [Keenable](https://app.keenable.ai) | `https://api.keenable.ai/v1` | native | Check current free credits and billing requirements. |
| `kenari` | [Kenari](https://kenari.id) | `https://kenari.id/v1` | chat | shared daily free chat and images |
| `kilo` | [Kilo Gateway](https://app.kilo.ai) | `https://api.kilo.ai/api/gateway/v1` | chat | no key needed |
| `kiosapi` | [KiosAPI](https://kiosapi.id/dashboard/keys) | `https://api.kiosapi.id/v1` | chat | shared daily free chat |
| `linkup` | [Linkup](https://app.linkup.so) | `https://api.linkup.so/v1` | native | Check current free credits and billing requirements. |
| `llm7` | [LLM7](https://llm7.io) | `https://api.llm7.io/v1` | chat | anon ok |
| `llmtr` | [LLMTR](https://llmtr.com) | `https://llmtr.com/v1` | chat | daily free-model quotas |
| `logfare` | [Logfare](https://logfare.ai) | `https://logfare.ai/v1` | chat | fair-use free models |
| `longcat` | [LongCat](https://longcat.chat/platform) | `https://api.longcat.chat/openai/v1` | chat | free daily, email signup ok |
| `lucidity` | [Lucidity Composite](https://composite.lucidity.sh) | `https://composite.lucidity.sh/v1` | chat | daily free-model requests |
| `mistral` | [Mistral](https://console.mistral.ai/api-keys/) | `https://api.mistral.ai/v1` | chat | Check current free-plan eligibility. |
| `modelscope` | [ModelScope](https://modelscope.cn/my/myaccesstoken) | `https://api-inference.modelscope.cn/v1` | chat | free key, needs Aliyun cn binding |
| `moondream` | [Moondream](https://moondream.ai/c/cloud/api-keys) | `https://api.moondream.ai/v1` | chat | $5 shared monthly credits |
| `nara` | [NaraRouter](https://router.bynara.id) | `https://router.bynara.id/v1` | chat | free key |
| `navy` | [NavyAI](https://api.navy) | `https://api.navy/v1` | chat | free key |
| `nvidia` | [NVIDIA NIM](https://build.nvidia.com/settings/api-keys) | `https://integrate.api.nvidia.com/v1` | chat | Check current free-plan eligibility. |
| `ollama` | [Ollama Cloud](https://ollama.com/settings/keys) | `https://ollama.com/v1` | chat | Check current free-plan eligibility. |
| `onomeo` | [onomeo](https://onomeo.com/dashboard) | `https://onomeo.com/v1` | chat | 30 free calls / 5 hours, shared |
| `opencode` | [OpenCode Zen](https://opencode.ai/auth) | `https://opencode.ai/zen/v1` | chat | paid models only |
| `openrouter` | [OpenRouter](https://openrouter.ai/keys) | `https://openrouter.ai/api/v1` | chat | Check current free-plan eligibility. |
| `orcarouter` | [OrcaRouter](https://www.orcarouter.ai) | `https://api.orcarouter.ai/v1` | chat | free key |
| `ovh` | [OVH AI Endpoints](https://endpoints.ai.cloud.ovh.net) | `https://oai.endpoints.kepler.ai.cloud.ovh.net/v1` | chat | no key needed |
| `parallel` | [Parallel](https://platform.parallel.ai) | `https://api.parallel.ai/v1` | native | Check current free credits and billing requirements. |
| `pinecone` | [Pinecone](https://app.pinecone.io) | `https://api.pinecone.io` | native | Cataloged embeddings; confirm Starter allowance. |
| `plugsky` | [Plugsky](https://plugsky.com/dashboard) | `https://plugsky.com/v1` | chat | free chat aliases |
| `pollinations` | [Pollinations](https://enter.pollinations.ai) | `https://gen.pollinations.ai/v1` | chat | Check current free-plan eligibility. |
| `qianfan` | [Baidu Qianfan](https://console.bce.baidu.com/qianfan/overview) | `https://qianfan.baidubce.com/v2` | chat | free ERNIE, needs cn real-name |
| `radeon` | [AMD Radeon Cloud](https://developer.amd.com.cn/radeon/tokenfactory) | `https://developer.amd.com.cn/radeon/api/v1` | chat | free shared models |
| `reka` | [Reka](https://platform.reka.ai) | `https://api.reka.ai/v1` | chat | prepaid credits |
| `requesty` | [Requesty](https://www.requesty.ai) | `https://router.requesty.ai/v1` | chat | free key |
| `router9` | [Router9](https://www.router9.com) | `https://api.router9.com/v1` | chat | shared monthly credits |
| `routeway` | [Routeway](https://routeway.ai) | `https://api.routeway.ai/v1` | chat | free key |
| `sail` | [Sail Research](https://app.sailresearch.com) | `https://api.sailresearch.com/v1` | native | $5 monthly with payment method |
| `sealion` | [SEA-LION](https://sea-lion.ai) | `https://api.sea-lion.ai/v1` | chat | free key |
| `septor` | [Septor Labs](https://septorlabs.com/dashboard) | `https://api.septorlabs.com/v1` | chat | daily free-model quota |
| `serpsearch` | [SerpSearch](https://serpsearch.com) | `https://api.serpsearch.com/api/v1` | native | Check current free credits and billing requirements. |
| `siliconflow` | [SiliconFlow](https://siliconflow.com) | `https://api.siliconflow.com/v1` | chat | image + TTS |
| `simplellm` | [SimpleLLM](https://app.simplellm.eu) | `https://api.simplellm.eu/v1` | chat | shared hourly and daily free quota |
| `speechify` | [Speechify](https://platform.speechify.ai) | `https://api.speechify.ai/v1` | native | monthly free TTS characters |
| `speka` | [Speka](https://speka.me/dashboard/keys) | `https://speka.me/v1` | chat | $1 shared monthly credits |
| `tavily` | [Tavily](https://app.tavily.com) | `https://api.tavily.com` | native | Check current free credits and billing requirements. |
| `typhoon` | [Typhoon](https://playground.opentyphoon.ai) | `https://api.opentyphoon.ai/v1` | chat | free research API |
| `unorouter` | [UnoRouter](https://unorouter.com) | `https://api.unorouter.com/v1` | chat | free key |
| `volcengine` | [Volcengine Ark](https://console.volcengine.com/ark) | `https://ark.cn-beijing.volces.com/api/v3` | chat | free daily, needs cn real-name |
| `waterfall` | [Waterfall](https://getwaterfall.org) | `https://api.getwaterfall.org/v1` | chat | community free models |
| `xfyun` | [iFlytek Spark](https://console.xfyun.cn) | `https://spark-api-open.xf-yun.com/v1` | chat | free Lite, needs cn real-name |
| `xkiro` | [xKiro](https://xkiro.com) | `https://api.xkiro.com/v1` | chat | free key |
| `zhipu` | [Zhipu AI](https://z.ai/manage-apikey/apikey-list) | `https://api.z.ai/api/paas/v4` | chat | Z.ai |

Custom OpenAI-compatible and native endpoints also work: supply an explicit base URL.

The signed catalog refreshes model/service availability, quotas, and platform names. It does not
publish signup URLs, API bases, or API keys. A daily GitHub job updates Brokie's
[provider directory](providers.json) from upstream; the server checks its cache hourly and
downloads it when older than 24 hours. `providers` and `key add` refresh stale metadata too.
Bundled defaults work if updates fail. Metadata updates never rewrite configured credential URLs.
New catalog-only providers are shown with a request for their documented base URL until their
setup metadata is available. New protocols can still require adapter changes.
The complete signed payload, including media/search sections, stays in `~/.brokie/catalog.json`.

Sources: [FreeLLMAPI key manager](https://github.com/tashfeenahmed/freellmapi/blob/main/client/src/components/keys/shared.tsx),
[provider registry](https://github.com/tashfeenahmed/freellmapi/blob/main/server/src/providers/index.ts),
[search registry](https://github.com/tashfeenahmed/freellmapi/blob/main/server/src/lib/search-platforms.ts).
Search signup pages are linked directly to the providers. Original MIT attribution is in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
