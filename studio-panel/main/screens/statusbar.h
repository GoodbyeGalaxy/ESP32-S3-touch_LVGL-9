#pragma once

void statusbar_init();
// ip_str: z.B. "192.168.1.42" — nur relevant wenn connected=true, sonst ignoriert
void statusbar_update_wifi(bool connected, const char *ip_str = nullptr);
void statusbar_update_time(const char *time_str);
