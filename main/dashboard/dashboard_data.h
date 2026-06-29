#ifndef DASHBOARD_DATA_H
#define DASHBOARD_DATA_H

#include <string>
#include <vector>

// 冰箱信息板的数据模型。字段对齐数据桥的 dashboard.json
// （date / quote / meals / perishable / inventory / weather）。
// 室内温湿度与时间不在此结构里——那是板子本地 SHTC3 / RTC 实时读的，
// 走 DashboardDisplay::SetIndoor / SetClock。
//
// 本结构刻意保持纯数据 + 无 ESP-IDF 依赖，便于 host 侧单测（M3 的
// ParseDashboardJson 会把 cJSON 解析结果填进来）。

struct DashboardItem {
    std::string name;
    std::string quantity;
};

struct DashboardData {
    std::string date;                  // 形如 "2026-06-29"（JSON 原值，仅留痕）
    std::string quote;                 // 每日一句

    // 三餐：空字符串表示当天该餐未排（板上显示「未排」）
    std::string breakfast;
    std::string lunch;
    std::string dinner;

    std::vector<DashboardItem> perishable;  // 易腐且在库（已按易腐优先排序前置）
    std::vector<DashboardItem> inventory;    // 全部在库

    // 天气：has_weather=false 表示 JSON 的 weather 字段为 null，板上显示「—」
    bool        has_weather = false;
    std::string weather_text;          // "晴"
    int         weather_temp = 0;       // 摄氏度
    std::string weather_icon;          // 和风图标码 "100"（Phase 1 暂不画图标）

    int stale_minutes = 0;             // >0 时顶角标「数据 N 分钟前」
};

// M2 用：造一份假数据，单验布局/渲染（不联网）。
inline DashboardData MakeFakeDashboardData() {
    DashboardData d;
    d.date = "2026-06-29";
    d.quote = "慢就是快，先把易腐的吃掉。";
    d.breakfast = "";                  // 故意留空，验「未排」分支
    d.lunch = "杂粮饭 + 辣椒炒肉";
    d.dinner = "鸡腿 + 贝果";
    d.perishable = {
        {"上海青", "1袋"},
        {"牛油果", "2个"},
    };
    d.inventory = {
        {"上海青", "1袋"},
        {"牛油果", "2个"},
        {"鸡蛋", "8个"},
        {"豆腐", "1盒"},
    };
    d.has_weather = true;
    d.weather_text = "晴";
    d.weather_temp = 31;
    d.weather_icon = "100";
    d.stale_minutes = 0;
    return d;
}

#endif // DASHBOARD_DATA_H
