#include "honeypot_log.h"
#include <stdarg.h>
#include <stdio.h>
#include "esp_timer.h"

static uint64_t hp_t0;

void honeypot_log_init(void) {
    hp_t0 = (uint64_t)(esp_timer_get_time() / 1000LL);
    printf("{\"t\":\"boot\",\"ts\":0}\n");
    fflush(stdout);
}

static uint64_t hp_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000LL) - hp_t0;
}

static void _hp_printf(const char *type, const char *fmt, ...) {
    printf("{\"t\":\"%s\",\"ts\":%llu", type, (unsigned long long)hp_ms());
    if (fmt && fmt[0]) {
        printf(",");
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("}\n");
    fflush(stdout);
}

void honeypot_log_chat(const char *role, const char *content) {
    printf("{\"t\":\"chat\",\"ts\":%llu,\"role\":\"%s\",\"content\":\"",
           (unsigned long long)hp_ms(), role);
    for (const char *p = content; *p; p++) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if (*p == '\n') { printf("\\n"); continue; }
        if (*p == '\r') { printf("\\r"); continue; }
        putchar(*p);
    }
    printf("\"}\n");
    fflush(stdout);
}

void honeypot_log_http(const char *method, const char *uri, int status) {
    _hp_printf("http", "\"method\":\"%s\",\"uri\":\"%s\",\"status\":%d", method, uri, status);
}

void honeypot_log_provision(const char *action, const char *detail) {
    _hp_printf("provision", "\"action\":\"%s\",\"detail\":\"%s\"", action, detail ? detail : "");
}

void honeypot_log_wifi(const char *event, const char *detail) {
    _hp_printf("wifi", "\"event\":\"%s\",\"detail\":\"%s\"", event, detail ? detail : "");
}
