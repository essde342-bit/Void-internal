#pragma once

struct Config {
    // ESP
    bool box       = true;
    bool skeleton  = false;
    bool health    = true;
    bool name      = true;
    bool teamcheck = true;

    // Aimbot
    bool  aimbot           = false;
    bool  aimbot_show_fov  = true;
    float aimbot_fov       = 5.0f;   // градусы
    float aimbot_smooth    = 3.0f;   // 1.0 = мгновенно
    int   aimbot_bone      = 0;      // 0=head, 1=neck, 2=chest
    int   aimbot_key       = 0x06;   // VK_XBUTTON2 (Mouse5)
    bool  aimbot_vis_check = true;
};

extern Config g_cfg;
