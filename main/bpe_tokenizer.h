#pragma once
/* Byte-level BPE tokenizer matching the model's training tokenizer.
   Implements pre-tokenization (special tokens first), byte-to-base mapping,
   then a merge loop from the ranked merge table. */
#include <stdint.h>
#include <string.h>

/* Returns number of BPE tokens encoded into `tokens_out` (max `max_tokens`).
   Returns -1 on overflow. */
int bpe_encode(const char *text, int *tokens_out, int max_tokens);
