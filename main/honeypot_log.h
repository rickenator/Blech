#pragma once

void honeypot_log_init(void);
void honeypot_log_chat(const char *role, const char *content);
void honeypot_log_http(const char *method, const char *uri, int status);
void honeypot_log_provision(const char *action, const char *detail);
void honeypot_log_wifi(const char *event, const char *detail);
