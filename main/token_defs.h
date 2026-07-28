#pragma once
/* Minimal token constants for build — full vocab.h is generated separately */
#define VOCAB_N 4096
#define TOKEN_ENDOFTEXT 0
#define TOKEN_SYSTEM 1
#define TOKEN_USER 2
#define TOKEN_ASSISTANT 3
#define TOKEN_TOOL 4

/* Stub arrays for CI builds without real vocab.h — firmware won't tokenize
   correctly without a model anyway, but it compiles and links. */
#if !HAVE_VOCAB
static const unsigned char VOCAB_BLOB[1] = {0};
static const int VOCAB_OFF[2] = {0, 1};
#endif
