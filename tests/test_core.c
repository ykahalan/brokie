#define main brokie_cli_main
#include "../brokie.c"
#undef main

int main(void) {
    const char escaped[] = "{\"\\u0061\":1}";
    Json escaped_json = json(escaped, strlen(escaped));
    if (escaped_json.bad || fieldnum(&escaped_json, 0, "a", 0) != 1) die("escaped JSON key test failed");
    jfree(&escaped_json);
    const char *good[] = {"{}", "[]", "{\"a\":[true,false,null,-1.25e+2]}", "\"\\uD83D\\uDE80\""},
               *bad[] = {"", "[1,]", "{\"a\" 1}", "{\"a\":}", "01", "true false", "\"\\q\""};
    for (int i = 0; i < 4; i++) {
        Json j = json(good[i], strlen(good[i]));
        if (j.bad) die("JSON positive test failed");
        jfree(&j);
    }
    for (int i = 0; i < 7; i++) {
        Json j = json(bad[i], strlen(bad[i]));
        if (!j.bad) die("JSON negative test failed");
        jfree(&j);
    }
    Stats s = {0};
    if (fabs(speed(&s) - .6) > .00001 || fabs(headroom(.1) - .55) > .00001 ||
        fabs(weights("smartest", 2) - .55) > .00001)
        die("scoring test failed");
    const int health[][3] = {{1, 200, H_PUBLIC},  {1, 404, H_PUBLIC},  {1, 401, H_INVALID}, {1, 403, H_REJECTED},
                             {1, 402, H_LIMITED}, {1, 429, H_LIMITED}, {1, 503, H_DOWN},    {0, 0, H_DOWN}};
    for (size_t i = 0; i < sizeof(health) / sizeof(*health); i++)
        if (response_health(health[i][0], health[i][1]) != health[i][2]) die("HTTP health classification failed");
    Key key = {0};
    snprintf(key.url, sizeof(key.url), "https://example.test/v1/chat/completions");
    char url[256];
    const char *paths[][2] = {{"/models", "https://example.test/v1/models"},
                              {"//account", "https://example.test/v1//account"}};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++)
        if (!key_url(url, sizeof(url), &key, paths[i][0], 0) || strcmp(url, paths[i][1]))
            die("relative API URL failed");
    if (!key_url(url, sizeof(url), &key, "//account", 1) || strcmp(url, "https://example.test/account") ||
        key_url(url, 8, &key, "/models", 0))
        die("root or bounded API URL failed");
    key.native = 1;
    if (!key_url(url, sizeof(url), &key, "/audio", 0) || strcmp(url, "https://example.test/v1/chat/completions/audio"))
        die("native API base changed");
    Model model[2];
    int count = 0;
    const char *configs[] = {
        "{\"models\":[{\"id\":1,\"provider\":\"local\",\"model\":\"test\",\"enabled\":false}]}",
        "{\"profile\":\"coding\",\"profiles\":[{\"name\":\"coding\",\"models\":[1]}],\"models\":[]}"};
    for (int i = 0; i < 2; i++) {
        Json cfg = json(configs[i], strlen(configs[i]));
        if (parse_models(&cfg, model, &count) != !i || (!i && (count != 1 || model[0].enabled)))
            die("model enable flags or removed profile guard failed");
        jfree(&cfg);
    }
    Buf directory = {0};
    put(&directory,
        "{\"providers\":[{\"id\":\"electronhub\",\"url\":\"https://example.test/v1\",\"signup\":\"https://example.test\"}]}");
    if (!provider_load(&directory)) die("provider metadata load failed");
    snprintf(key.provider, sizeof(key.provider), "electronhub");
    if (strcmp(key_probe(&key), "/user/me")) die("bundled safe probe lost during metadata overlay");
    drop(&directory);
    put(&directory,
        "{\"providers\":[{\"id\":\"partial\",\"url\":\"https://example.test\",\"signup\":\"https://example.test\"},{\"id\":\"bad\",\"url\":\"http://unsafe.test\"}]}");
    if (provider_load(&directory) || provider_find("partial") || strcmp(key_probe(&key), "/user/me"))
        die("failed metadata update replaced working probes");
    drop(&directory);
    puts("Core parser, scoring and health tests passed.");
    return 0;
}
