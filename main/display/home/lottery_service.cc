// lottery_service.cc —— 双色球开奖实现(字符串提取,非 cJSON)。
#include "lottery_service.h"

#include <cstdio>
#include <cstring>

#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "lottery";

LotteryService& LotteryService::Get() {
    static LotteryService instance;
    return instance;
}

void LotteryService::Start(const std::string& base_url) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/api/ssq/history";
}

// 从原始 JSON 文本提取一期;非 cJSON——30KB 响应超 8KB 缓冲。
// Python json.dumps 默认在冒号/逗号后加空格("code": "xxx","reds": [1,2]),
// 搜索用带空格版本(2026-11-02 实锤:无空格模式解析 0 期)。
static const char* json_str(const char* p, const char* key_sp, const char* key_ns,
                             const char* end) {
    // 先试带空格,再试紧凑
    const char* r = strstr(p, key_sp);
    if (r != nullptr && r < end) return r + strlen(key_sp);
    r = strstr(p, key_ns);
    if (r != nullptr && r < end) return r + strlen(key_ns);
    return nullptr;
}

static int extract_issues(const char* body, int body_len, LotteryService::Issue* out, int max) {
    int n = 0;
    const char* p = body;
    const char* end = body + body_len;

    while (n < max) {
        // 期号
        p = json_str(p, "\"code\": \"", "\"code\":\"", end);
        if (p == nullptr) break;
        char* code = out[n].code;
        int ci = 0;
        while (p < end && *p != '"' && ci < 7) code[ci++] = *p++;
        code[ci] = '\0';
        if (ci < 4) break;

        // 日期 "date": "2026-11-01" → 取 MM-DD
        const char* dp = json_str(p, "\"date\": \"", "\"date\":\"", end);
        if (dp != nullptr && dp + 10 < end) {
            snprintf(out[n].date, sizeof(out[n].date), "%.2s-%.2s", dp + 5, dp + 8);
        } else {
            out[n].date[0] = '\0';
        }

        // 红球
        p = json_str(p, "\"reds\": [", "\"reds\":[", end);
        if (p == nullptr) break;
        for (int r = 0; r < 6; r++) {
            out[n].reds[r] = (int)strtol(p, (char**)&p, 10);
            if (*p == ',' || *p == ' ') p++;
        }

        // 蓝球
        p = json_str(p, "\"blue\": ", "\"blue\":", end);
        if (p == nullptr) break;
        out[n].blue = (int)strtol(p, (char**)&p, 10);

        // 销售额 "sales": N
        const char* sp = json_str(p, "\"sales\":", "\"sales\":", end);
        out[n].sales = sp ? strtol(sp, nullptr, 10) : 0;

        // 奖池 "pool": N
        sp = json_str(p, "\"pool\":", "\"pool\":", end);
        out[n].pool = sp ? strtol(sp, nullptr, 10) : 0;

        // 一等奖:找 grades 后第一个 "num": N
        const char* gp = json_str(p, "\"grades\":", "\"grades\":", end);
        out[n].prize1_count = 0;
        out[n].prize1_money = 0;
        if (gp != nullptr) {
            const char* mp = strstr(gp, "\"num\":");
            if (mp != nullptr && mp < end) {
                mp += 6;
                if (*mp == ' ') mp++;
                out[n].prize1_count = (int)strtol(mp, nullptr, 10);
            }
            const char* mm = strstr(gp, "\"money\":");
            if (mm != nullptr && mm < end) {
                mm += 7;
                if (*mm == ' ') mm++;
                out[n].prize1_money = strtol(mm, nullptr, 10);
            }
        }

        n++;
    }
    return n;
}

void LotteryService::Poll() {
    ESP_LOGI(TAG, "lottery poll (url=%s)", base_url_.empty() ? "(空)" : base_url_.c_str());
    if (base_url_.empty()) return;
    // 30 期≈15KB 超共享 8KB;堆碎片后 16KB 分配也失败(2026-11-02 实锤
    // largest blk 8KB)。方案:同一条 HTTP 连接内两轮读取(8KB×2),
    // 各轮独立解析,跨轮去重(C3 内存红线的标准做法)。
    char* body = poller_http::body;   // 共享 8KB
    const size_t kCap = sizeof(poller_http::body);
    LotteryService::Issue parsed[kMaxIssues];
    int n = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = base_url_.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return;
    bool conn_ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            conn_ok = true;
            // ── 轮 1: 读 8KB → 解析 ──
            int len = 0;
            int nr;
            while ((nr = esp_http_client_read(client, body + len, kCap - len - 1)) > 0) {
                len += nr;
                if (len >= (int)kCap - 1) break;
            }
            body[len] = '\0';
            // 跳过 "latest" 字段(含与 issues[0] 相同的首期,会产生重复)
            const char* issues_start = strstr(body, "\"issues\":");
            if (issues_start != nullptr) {
                n = extract_issues(issues_start, len - (issues_start - body),
                                   parsed, kMaxIssues);
            } else {
                n = extract_issues(body, len, parsed, kMaxIssues);
            }

            // ── 轮 2: 覆写缓冲 → 继续解析(同连接继续读) ──
            if (n > 0 && n < kMaxIssues) {
                int len2 = 0;
                while ((nr = esp_http_client_read(client, body + len2, kCap - len2 - 1)) > 0) {
                    len2 += nr;
                    if (len2 >= (int)kCap - 1) break;
                }
                body[len2] = '\0';
                if (len2 > 100) {   // 有足够数据
                    int n2 = extract_issues(body, len2, parsed + n, kMaxIssues - n);
                    // 去重:轮 2 首期可能=轮 1 尾期(跨轮边界截断)
                    if (n2 > 0 && n > 0 &&
                        strcmp(parsed[n - 1].code, parsed[n].code) == 0) {
                        // 首期重复,后续前移一位
                        for (int i = n; i < n + n2 - 1; i++) parsed[i] = parsed[i + 1];
                        n2--;
                    }
                    n += n2;
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!conn_ok || n < 1) {
        ESP_LOGW(TAG, "双色球查询/解析失败(n=%d),保留旧值", n);
        return;
    }
    memcpy(issues_, parsed, sizeof(Issue) * n);
    count_ = n;
    valid_ = true;
    version_.fetch_add(1);
    ESP_LOGI(TAG, "双色球 %d 期,最新 %s", n, issues_[0].code);
}
