#ifndef DASHBOARD_JSON_H
#define DASHBOARD_JSON_H

#include <string>
#include <vector>
#include "cJSON.h"
#include "dashboard_data.h"

// 把数据桥的 dashboard.json 解析成 DashboardData。
// 容错：字段缺失/为 null 给安全默认（meals null→空字符串→板上「未排」；
// weather null→has_weather=false→板上「—」）。解析失败返回 false。
// header-only inline，便于后续 host 侧测（cJSON 在 host 也可用）。
inline bool ParseDashboardJson(const char* json, DashboardData& out) {
    if (json == nullptr) return false;
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) return false;

    auto get_str = [](cJSON* obj, const char* key) -> std::string {
        cJSON* v = cJSON_GetObjectItem(obj, key);
        return (v != nullptr && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    };

    out.date  = get_str(root, "date");
    out.quote = get_str(root, "quote");

    cJSON* meals = cJSON_GetObjectItem(root, "meals");
    if (meals != nullptr && cJSON_IsObject(meals)) {
        out.breakfast = get_str(meals, "breakfast");
        out.lunch     = get_str(meals, "lunch");
        out.dinner    = get_str(meals, "dinner");
    }

    auto parse_items = [&](const char* key, std::vector<DashboardItem>& vec) {
        cJSON* arr = cJSON_GetObjectItem(root, key);
        if (arr == nullptr || !cJSON_IsArray(arr)) return;
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            DashboardItem di;
            di.name = get_str(it, "name");
            di.quantity = get_str(it, "quantity");
            if (!di.name.empty()) vec.push_back(di);
        }
    };
    parse_items("perishable", out.perishable);
    parse_items("inventory",  out.inventory);

    cJSON* w = cJSON_GetObjectItem(root, "weather");
    if (w != nullptr && cJSON_IsObject(w)) {
        out.has_weather   = true;
        out.weather_text  = get_str(w, "text");
        out.weather_icon  = get_str(w, "icon");
        cJSON* temp = cJSON_GetObjectItem(w, "temp");
        if (temp != nullptr && cJSON_IsNumber(temp)) out.weather_temp = temp->valueint;
    }

    cJSON_Delete(root);
    return true;
}

#endif // DASHBOARD_JSON_H
