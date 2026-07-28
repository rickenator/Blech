#include "bpe_tokenizer.h"
#include "bpe_encoder.h"
#if __has_include("vocab.h")
#include "vocab.h"
#else
#include "token_defs.h"
#endif

/* Special tokens: must be matched first, before byte-level BPE */
static const struct { const char *s; int id; int len; } SPECIALS[] = {
    {"<|endoftext|>", 0, 13},
    {"<|system|>",    1, 10},
    {"<|user|>",      2, 8},
    {"<|assistant|>", 3, 13},
    {"<|tool|>",      4, 8},
};

static int merge_ids(int *ids, int count) {
    /* Apply merge table; each merge reduces count by 1 when applied */
    for (int mi = 0; mi < BPE_MERGE_COUNT; mi++) {
        int left   = (int)BPE_MERGES[mi][0];
        int right  = (int)BPE_MERGES[mi][1];
        int merged = (int)BPE_MERGES[mi][2];
        for (int i = 0; i < count - 1; i++) {
            if (ids[i] == left && ids[i+1] == right) {
                ids[i] = merged;
                /* shift remaining left */
                for (int j = i + 1; j < count - 1; j++)
                    ids[j] = ids[j+1];
                count--;
            }
        }
    }
    return count;
}

int bpe_encode(const char *text, int *tokens_out, int max_tokens) {
    int count = 0;
    int i = 0;

    while (text[i] && count < max_tokens) {
        /* 1. Try special tokens first */
        int matched = 0;
        for (int s = 0; s < 5; s++) {
            if (strncmp(text + i, SPECIALS[s].s, SPECIALS[s].len) == 0) {
                tokens_out[count++] = SPECIALS[s].id;
                i += SPECIALS[s].len;
                matched = 1;
                break;
            }
        }
        if (matched) continue;

        /* 2. Emit byte-level base token */
        uint8_t byte = (uint8_t)text[i];
        int base_tok = (int)BPE_BYTE_TO_TOK[byte];
        if (base_tok == 0 && byte != 0) {
            /* unknown byte - use token 0 */
            tokens_out[count++] = 0;
        } else {
            tokens_out[count++] = base_tok;
        }
        i++;
    }
    if (text[i] && count >= max_tokens) return -1;

    /* 3. Apply merges */
    return merge_ids(tokens_out, count);
}
