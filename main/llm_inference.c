#include "llm_inference.h"

#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#define LLM_PROFILE
#define LLM_PROFILE_NOW() esp_timer_get_time()
#include "llm.h"
#include "bpe_tokenizer.h"

#if __has_include("vocab.h")
#include "vocab.h"
#define HAVE_VOCAB 1
#else
#define HAVE_VOCAB 0
#include "token_defs.h"
#endif

static const char *TAG = "llm";
static Model model;
static Scratch scratch;
static const void *model_base;
static esp_partition_mmap_handle_t model_map;
static bool loaded;
static int8_t *head_weights;
static float *head_scales;
static int8_t head_activations[1024];
static float head_activation_scale;
static int head_rows;
static int head_cols;
static int head_groups;
static TaskHandle_t head_worker;
static TaskHandle_t head_caller;
static float *head_output;
static int head_split;

static inline int32_t dot_int8(const int8_t *a, const int8_t *b, int n)
{
    int32_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (int32_t)a[i] * (int32_t)b[i];
    }
    return sum;
}

static void head_rows_range(float *output, int first, int last)
{
    int group_size = model.tok_emb.group;
    for (int row = first; row < last; row++) {
        const int8_t *weights =
            head_weights + (size_t)row * head_cols;
        float sum = 0.0f;
        for (int group = 0; group < head_groups; group++) {
            int begin = group * group_size;
            int end = begin + group_size;
            if (end > head_cols) {
                end = head_cols;
            }
            sum += (float)dot_int8(head_activations + begin,
                                   weights + begin, end - begin) *
                   head_scales[(size_t)row * head_groups + group];
        }
        output[row] = sum * head_activation_scale;
    }
}

static void head_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        head_rows_range(head_output, 0, head_split);
        xTaskNotifyGive(head_caller);
    }
}

static void head_matvec_int8(const QT *weights, const float *input,
                             float *output)
{
    (void)weights;
    quantize_act(input, head_cols, head_activations,
                 &head_activation_scale);
    head_output = output;
    head_split = head_rows / 2;
    head_caller = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(head_worker);
    head_rows_range(output, head_split, head_rows);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static bool stage_head(void)
{
    QT *head = &model.tok_emb;
    head_rows = head->rows;
    head_cols = head->cols;
    head_groups = head->n_groups;
    if (head_cols > (int)sizeof(head_activations)) {
        ESP_LOGE(TAG, "head dimension %d exceeds staging limit", head_cols);
        return false;
    }
    head_weights = heap_caps_malloc((size_t)head_rows * head_cols,
                                    MALLOC_CAP_SPIRAM);
    head_scales = heap_caps_malloc(
        (size_t)head_rows * head_groups * sizeof(float),
        MALLOC_CAP_SPIRAM);
    if (!head_weights || !head_scales) {
        ESP_LOGE(TAG, "int8 head staging allocation failed");
        return false;
    }
    for (int row = 0; row < head_rows; row++) {
        const uint8_t *source =
            head->codes + (size_t)row * head->row_bytes;
        int8_t *target = head_weights + (size_t)row * head_cols;
        for (int column = 0; column < head_cols; column++) {
            uint8_t packed = source[column >> 1];
            int code = (column & 1) ? (packed >> 4) : (packed & 0x0f);
            target[column] = (int8_t)(code - 8);
        }
        for (int group = 0; group < head_groups; group++) {
            head_scales[(size_t)row * head_groups + group] =
                half2float(head->scales[
                    (size_t)row * head_groups + group]);
        }
    }
    if (xTaskCreatePinnedToCore(head_worker_main, "llm_head", 4096, NULL,
                                5, &head_worker, 0) != pdPASS) {
        ESP_LOGE(TAG, "head worker creation failed");
        return false;
    }
    model.head_matvec = head_matvec_int8;
    ESP_LOGI(TAG, "staged %.2f MiB int8 head across both cores",
             ((size_t)head_rows * head_cols +
              (size_t)head_rows * head_groups * sizeof(float)) /
                 (1024.0 * 1024.0));
    return true;
}

static void *psram_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ESP_LOGE(TAG, "PSRAM allocation failed: %u bytes",
                 (unsigned)(count * size));
    }
    return ptr;
}

static bool allocate_scratch(const Cfg *c)
{
    int d = c->dim, l = c->n_layers, p = c->ple_dim;
    int f = c->ffn, v = c->vocab, s = c->seq_len;
    scratch.x = psram_calloc(d, sizeof(float));
    scratch.h = psram_calloc(f > d ? f : d, sizeof(float));
    scratch.qkv = psram_calloc(3 * d, sizeof(float));
    scratch.att = psram_calloc(d, sizeof(float));
    scratch.g1 = psram_calloc(f, sizeof(float));
    scratch.g2 = psram_calloc(p > f ? p : f, sizeof(float));
    scratch.ple = psram_calloc(l * p, sizeof(float));
    scratch.tmpP = psram_calloc(l * p, sizeof(float));
    scratch.trow = psram_calloc(l * p, sizeof(float));
    scratch.logits = psram_calloc(v, sizeof(float));
    scratch.scores = psram_calloc(s, sizeof(float));
    scratch.kcache = psram_calloc((size_t)l * s * d, sizeof(float));
    scratch.vcache = psram_calloc((size_t)l * s * d, sizeof(float));
    return scratch.x && scratch.h && scratch.qkv && scratch.att &&
           scratch.g1 && scratch.g2 && scratch.ple && scratch.tmpP &&
           scratch.trow && scratch.logits && scratch.scores &&
           scratch.kcache && scratch.vcache;
}

esp_err_t llm_init(void)
{
#if !HAVE_VOCAB
    ESP_LOGW(TAG, "main/vocab.h is missing");
    return ESP_ERR_NOT_FOUND;
#else
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        CONFIG_CHAT_MODEL_PARTITION_LABEL);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &model_base, &model_map);
    if (err != ESP_OK) {
        return err;
    }
    if (llm_load(model_base, &model) != 0) {
        esp_partition_munmap(model_map);
        model_base = NULL;
        return ESP_ERR_INVALID_VERSION;
    }
    if (model.c.n_layers > 32 || model.c.vocab < VOCAB_N ||
        model.c.n_heads <= 0 || model.c.dim % model.c.n_heads != 0) {
        esp_partition_munmap(model_map);
        model_base = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    model.tok_emb.rows = VOCAB_N;
    if (!allocate_scratch(&model.c)) {
        esp_partition_munmap(model_map);
        model_base = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (!stage_head()) {
        esp_partition_munmap(model_map);
        model_base = NULL;
        return ESP_ERR_NO_MEM;
    }
    loaded = true;
    ESP_LOGI(TAG, "model mapped at 0x%lx; free PSRAM=%u KiB",
             (unsigned long)part->address,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
#endif
}

#if HAVE_VOCAB
static int tokenize_greedy(const char *text, int *tokens, int max_tokens)
{
    size_t offset = 0, input_len = strlen(text);
    int count = 0;
    while (offset < input_len && count < max_tokens) {
        int best = -1;
        size_t best_len = 0;
        for (int token = 0; token < VOCAB_N; token++) {
            int begin = VOCAB_OFF[token];
            size_t len = (size_t)(VOCAB_OFF[token + 1] - begin);
            if (len > best_len && len <= input_len - offset &&
                memcmp(text + offset, VOCAB_BLOB + begin, len) == 0) {
                best = token;
                best_len = len;
            }
        }
        if (best < 0) {
            return -1;
        }
        tokens[count++] = best;
        offset += best_len;
    }
    return offset == input_len ? count : -1;
}

static size_t append_token(int token, char *out, size_t used, size_t capacity)
{
    if (token < 0 || token >= VOCAB_N || used >= capacity) {
        return used;
    }
    int begin = VOCAB_OFF[token];
    size_t len = (size_t)(VOCAB_OFF[token + 1] - begin);
    if (len > capacity - used - 1) {
        len = capacity - used - 1;
    }
    memcpy(out + used, VOCAB_BLOB + begin, len);
    out[used + len] = '\0';
    return used + len;
}
#else
/* CI stub — firmware will not tokenize correctly without vocab.h */
static size_t append_token(int token, char *out, size_t used, size_t capacity)
{
    (void)token;
    if (used + 3 < capacity) {
        out[used] = '?'; out[used+1] = '\0';
        return used + 1;
    }
    return used;
}
#endif

esp_err_t llm_generate(const char *prompt, char *response, size_t max_len)
{
#if !HAVE_VOCAB
    (void)prompt;
    snprintf(response, max_len, "tokenizer asset missing");
    return ESP_ERR_NOT_FOUND;
#else
    if (!loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    int tokens[CONFIG_CHAT_MAX_PROMPT_BYTES];
    int n_prompt = bpe_encode(prompt, tokens,
                                   CONFIG_CHAT_MAX_PROMPT_BYTES);
    if (n_prompt <= 0 || n_prompt >= model.c.seq_len) {
        snprintf(response, max_len,
                 "prompt cannot be encoded or is too long");
        return ESP_ERR_INVALID_ARG;
    }

    memset(scratch.kcache, 0, (size_t)model.c.n_layers *
           model.c.seq_len * model.c.dim * sizeof(float));
    memset(scratch.vcache, 0, (size_t)model.c.n_layers *
           model.c.seq_len * model.c.dim * sizeof(float));
    llm_profile_reset(&scratch);

    int pos = 0;
    int64_t started = esp_timer_get_time();
    for (int i = 0; i < n_prompt; i++) {
        llm_forward_ex(&model, tokens[i], pos++, &scratch,
                       i == n_prompt - 1);
        vTaskDelay(1);
    }
    int64_t prefill_ms = (esp_timer_get_time() - started) / 1000;
    int64_t decode_started = esp_timer_get_time();

    size_t used = 0;
    int generated = 0;
    response[0] = '\0';
    for (int step = 0;
         step < CONFIG_CHAT_MAX_GENERATED_TOKENS &&
         pos < model.c.seq_len && used + 1 < max_len;
         step++) {
        int best = 0;
        float best_logit = -FLT_MAX;
        for (int token = 0; token < VOCAB_N; token++) {
            if (scratch.logits[token] > best_logit) {
                best_logit = scratch.logits[token];
                best = token;
            }
        }
#ifdef TOKEN_ENDOFTEXT
        if (best == TOKEN_ENDOFTEXT) {
            break;
        }
#endif
#ifdef TOKEN_USER
        if (best == TOKEN_USER) {
            break;
        }
#endif
#ifdef TOKEN_ASSISTANT
        if (best == TOKEN_ASSISTANT) {
            break;
        }
#endif
#ifdef TOKEN_TOOL
        if (best == TOKEN_TOOL) {
            break;
        }
#endif
        used = append_token(best, response, used, max_len);
        llm_forward(&model, best, pos++, &scratch);
        generated++;
        vTaskDelay(1);
    }
    int64_t decode_ms = (esp_timer_get_time() - decode_started) / 1000;
    int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
    ESP_LOGI(TAG,
             "generation: prompt=%d generated=%d prefill=%lld ms "
             "decode=%lld ms %.2f tok/s total=%lld ms",
             n_prompt, generated, (long long)prefill_ms,
             (long long)decode_ms,
             decode_ms > 0 ? (generated * 1000.0) / decode_ms : 0.0,
             (long long)elapsed_ms);
    if (scratch.profile.calls) {
        double divisor = scratch.profile.calls * 1000.0;
        ESP_LOGI(TAG,
                 "profile ms/step: input=%.1f attn=%.1f ffn=%.1f "
                 "ple=%.1f head=%.1f",
                 scratch.profile.input_us / divisor,
                 scratch.profile.attn_us / divisor,
                 scratch.profile.ffn_us / divisor,
                 scratch.profile.ple_us / divisor,
                 scratch.profile.head_us / divisor);
    }
    return ESP_OK;
#endif
}


esp_err_t llm_generate_stream(const char *prompt,
                              llm_token_cb_t on_token, void *ctx,
                              char *full_response, size_t max_len)
{
    if (!loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    int tokens[CONFIG_CHAT_MAX_PROMPT_BYTES];
    int n_prompt = bpe_encode(prompt, tokens,
                                   CONFIG_CHAT_MAX_PROMPT_BYTES);
    if (n_prompt <= 0 || n_prompt >= model.c.seq_len) {
        snprintf(full_response, max_len,
                 "prompt too long");
        return ESP_ERR_INVALID_ARG;
    }
    memset(scratch.kcache, 0, (size_t)model.c.n_layers *
           model.c.seq_len * model.c.dim * sizeof(float));
    memset(scratch.vcache, 0, (size_t)model.c.n_layers *
           model.c.seq_len * model.c.dim * sizeof(float));
    llm_profile_reset(&scratch);
    int pos = 0;
    for (int i = 0; i < n_prompt; i++) {
        llm_forward_ex(&model, tokens[i], pos++, &scratch,
                       i == n_prompt - 1);
        vTaskDelay(1);
    }
    size_t used = 0;
    int generated = 0;
    full_response[0] = '\0';
    for (int step = 0;
         step < CONFIG_CHAT_MAX_GENERATED_TOKENS &&
         pos < model.c.seq_len && used + 1 < max_len;
         step++) {
        int best = 0;
        float best_logit = -3.402823466e+38F;
        for (int token = 0; token < VOCAB_N; token++) {
            if (scratch.logits[token] > best_logit) {
                best_logit = scratch.logits[token];
                best = token;
            }
        }
#ifdef TOKEN_ENDOFTEXT
        if (best == TOKEN_ENDOFTEXT) break;
#endif
#ifdef TOKEN_USER
        if (best == TOKEN_USER) break;
#endif
#ifdef TOKEN_ASSISTANT
        if (best == TOKEN_ASSISTANT) break;
#endif
#ifdef TOKEN_TOOL
        if (best == TOKEN_TOOL) break;
#endif
        size_t before = used;
        used = append_token(best, full_response, used, max_len);
        if (on_token && used > before) {
            char chunk[32];
            size_t clen = used - before;
            if (clen > 31) clen = 31;
            memcpy(chunk, full_response + before, clen);
            chunk[clen] = '\0';
            if (!on_token(chunk, ctx)) break;
        }
        llm_forward(&model, best, pos++, &scratch);
        generated++;
        vTaskDelay(1);
    }
    return ESP_OK;
}
const char *llm_get_info(void)
{
    static char info[128];
    if (!loaded) {
        return "PLE TinyLM not loaded";
    }
    snprintf(info, sizeof(info),
             "PLE TinyLM V=%d D=%d L=%d H=%d F=%d P=%d",
             model.c.vocab, model.c.dim, model.c.n_layers,
             model.c.n_heads, model.c.ffn, model.c.ple_dim);
    return info;
}
