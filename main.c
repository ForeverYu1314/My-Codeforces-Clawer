#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>     
#include <io.h>           // 提供 _access 函数（检查文件是否存在）
#include <curl/curl.h>  
#include "cJSON.h"       

#define API_BASE "https://codeforces.com/api/"
#define USER_AGENT "CFClawer/2.3"
#define MAX_HANDLES 100    // 最多同时处理的用户数

typedef struct {
    char handle[50];     
    int rating;           
    int maxRating;        
    char rank[50];        // 头衔字符串
    char avatar[200];     // 头像的地址
} UserInfo;

// 一场比赛的 Rating 变化
typedef struct {
    int contestId;   
    char contestName[200];
    int rank;            
    int oldRating;     
    int newRating;   
    long long updateTime; // Rating 更新时间（Unix 时间戳）
} RatingChange;

// 一次提交
typedef struct {
    int contestId;       
    char index[5];        // 题目编号
    char name[200];       // 题目名称
    int rating;           // 题目难度，若无则为 0
    char verdict[30];     // 评测结果
    long long creationTime; // 提交时间
    double points;        // 该次提交获得的分数
} Submission;

typedef struct {
    int contestId;       
    long long startTime;  
    int duration;         // 比赛时长（秒）
} ContestInfo;

// 多用户总览页中一行所需的数据
typedef struct {
    UserInfo user;        // 用户基础信息
    int contestCount;     // 参加的比赛总次数
    int maxRating;        
    int recent180Contests;// 近 180 天的比赛次数
    int recent180MaxRating; // 近 180 天的最高 Rating
} MultiUserSummary;



 //根据 Rating 返回对应的 CSS 颜色代码（Codeforces 段位颜色）。
 //返回值：颜色字符串，如 "#77ff77"
static const char* rating_color(int rating) {
    if (rating < 1200) return "#cccccc";
    if (rating < 1400) return "#77ff77";
    if (rating < 1600) return "#77ddbb";
    if (rating < 1900) return "#aaaaff";
    if (rating < 2100) return "#ff88ff";
    if (rating < 2300) return "#ffcc88";
    if (rating < 2400) return "#ffbb55";
    if (rating < 2600) return "#ff7777";
    if (rating < 3000) return "#ff3333";
    return "#aa0000"; 
}


 //根据 Rating 返回对应的头衔名称。
static const char* rank_name(int rating) {
    if (rating < 1200) return "Newbie";
    if (rating < 1400) return "Pupil";
    if (rating < 1600) return "Specialist";
    if (rating < 1900) return "Expert";
    if (rating < 2100) return "Candidate Master";
    if (rating < 2300) return "Master";
    if (rating < 2400) return "International Master";
    if (rating < 2600) return "Grandmaster";
    if (rating < 3000) return "International Grandmaster";
    return "Legendary Grandmaster";
}

 //判断时间戳 t 是否在最近 days 天内。 
static int within_days(long long t, int days) {
    long long now = (long long)time(NULL);      // 获取当前时间戳
    return t >= now - (long long)days * 86400;  
}

// 用于接收 HTTP 响应的动态缓冲区
struct string_buffer {
    char *data;      // 缓冲区指针
    size_t size;     // 已使用的大小
};

/*
  libcurl 回调函数，每收到一块数据就会调用，将数据追加到 string_buffer 中。
  contents：指向收到数据的指针
  size：每个数据单元的大小
  nmemb：数据单元的数量
  userp： struct string_buffer 的地址
  返回：实际接收到的字节数
 */
static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;                            // 本次收到的总字节数
    struct string_buffer *mem = (struct string_buffer *)userp; // 强转回缓冲区结构体
    // 动态扩大缓冲区，多留一个字节用于存放字符串结束符 '\0'
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;                     // 内存不足时返回 0 会导致 curl 报错
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);  // 追加数据到缓冲区末尾
    mem->size += realsize;
    mem->data[mem->size] = 0;              // 末尾补 0 方便当作 C 字符串使用
    return realsize;
}

/*
  发送 GET 请求并返回解析好的 cJSON 对象。
  url：完整的 API 地址
  返回：如果请求成功且 API 状态为 "OK"，返回 cJSON 指针；否则返回 NULL。
 */
static cJSON* api_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl init failed\n");
        return NULL;
    }

    struct string_buffer buf = {0};        
    curl_easy_setopt(curl, CURLOPT_URL, url);               
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);  
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb); 
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);      
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        

    CURLcode res = curl_easy_perform(curl);  // 执行网络请求
    curl_easy_cleanup(curl);                 // 清理 curl 句柄，释放资源

    if (res != CURLE_OK) {                   // 返回码不是 OK 说明请求失败
        fprintf(stderr, "curl: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    // 将接收到的字符串解析为 JSON 对象
    cJSON *json = cJSON_Parse(buf.data);
    free(buf.data);                          // 释放原始字符串缓冲区
    if (!json) {
        fprintf(stderr, "JSON parse error\n");
        return NULL;
    }

    // 检查 Codeforces API 的通用返回格式: {"status":"OK", "result":...}
    cJSON *status = cJSON_GetObjectItem(json, "status");
    if (!status || strcmp(status->valuestring, "OK") != 0) {
        cJSON *comment = cJSON_GetObjectItem(json, "comment");  // 错误详情
        fprintf(stderr, "API: %s\n", comment ? comment->valuestring : "unknown error");
        cJSON_Delete(json);  // 释放 JSON 对象
        return NULL;
    }
    Sleep(500);   
    return json;  // 调用者负责 cJSON_Delete
}


UserInfo get_user_info(const char *handle) {
    UserInfo user = {0};  
    char url[256];
    // 拼接请求 URL
    snprintf(url, sizeof(url), API_BASE "user.info?handles=%s", handle);
    cJSON *json = api_get(url);
    if (!json) return user;
    cJSON *result = cJSON_GetObjectItem(json, "result");
    if (cJSON_GetArraySize(result) > 0) {
        cJSON *u = cJSON_GetArrayItem(result, 0);
        cJSON *h = cJSON_GetObjectItem(u, "handle");
        if (h) strncpy(user.handle, h->valuestring, 49); 
        cJSON *r = cJSON_GetObjectItem(u, "rating");
        user.rating = r ? r->valueint : 0;  
        cJSON *mr = cJSON_GetObjectItem(u, "maxRating");
        user.maxRating = mr ? mr->valueint : 0;
        cJSON *rk = cJSON_GetObjectItem(u, "rank");
        if (rk) strncpy(user.rank, rk->valuestring, 49);
        else if (user.rating) strncpy(user.rank, rank_name(user.rating), 49);
        else strcpy(user.rank, "Unrated");  
        cJSON *av = cJSON_GetObjectItem(u, "avatar");
        if (av) strncpy(user.avatar, av->valuestring, 199);
    }
    cJSON_Delete(json);
    return user;
}

//获取用户的所有 Rating 变化记录。
RatingChange* get_user_rating(const char *handle, int *count) {
    *count = 0;
    char url[256];
    snprintf(url, sizeof(url), API_BASE "user.rating?handle=%s", handle);
    cJSON *json = api_get(url);
    if (!json) return NULL;

    cJSON *result = cJSON_GetObjectItem(json, "result");
    int n = cJSON_GetArraySize(result);  // 数组长度
    RatingChange *arr = malloc(n * sizeof(RatingChange));
    if (!arr) { cJSON_Delete(json); return NULL; }

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(result, i);
        RatingChange *rc = &arr[i];
        rc->contestId = cJSON_GetObjectItem(item, "contestId")->valueint;
        strncpy(rc->contestName, cJSON_GetObjectItem(item, "contestName")->valuestring, 199);
        rc->rank = cJSON_GetObjectItem(item, "rank")->valueint;
        rc->oldRating = cJSON_GetObjectItem(item, "oldRating")->valueint;
        rc->newRating = cJSON_GetObjectItem(item, "newRating")->valueint;
        rc->updateTime = (long long)cJSON_GetObjectItem(item, "ratingUpdateTimeSeconds")->valuedouble;
    }
    *count = n;
    cJSON_Delete(json);
    return arr;
}

// 获取用户的所有提交记录（自动分页，最多取完）。
Submission *get_user_status(const char *handle, int *count)
{
        char url[256];
        cJSON *json, *result;
        int n;
        Submission *all_subs = NULL;
        *count = 0;
        snprintf(url, sizeof(url),"https://codeforces.com/api/user.status?handle=%s",handle);
        json = api_get(url);
        if (!json) return NULL;
        result = cJSON_GetObjectItem(json, "result");
        n = cJSON_GetArraySize(result);
        if (n == 0) {
                cJSON_Delete(json);
                return NULL;
        }
    
        //一次性分配动态内存，避免在循环中频繁使用 realloc
        all_subs = malloc(n * sizeof(Submission));
        if (!all_subs) {
                cJSON_Delete(json);
                return NULL;
        }
        for (int i = 0; i < n; i++) {
                cJSON *sub = cJSON_GetArrayItem(result, i);
                cJSON *prob = cJSON_GetObjectItem(sub, "problem");
                Submission *s = &all_subs[i];

                s->contestId = 0;
                if (cJSON_GetObjectItem(sub, "contestId")) {
                        s->contestId = cJSON_GetObjectItem(sub,
                                        "contestId")->valueint;
                }

                if (prob) {
                        cJSON *idx = cJSON_GetObjectItem(prob, "index");
                        strncpy(s->index, idx ? idx->valuestring : "?", 4);
                        cJSON *name = cJSON_GetObjectItem(prob, "name");
                        strncpy(s->name, name ? name->valuestring : "", 199);
                        cJSON *rating = cJSON_GetObjectItem(prob, "rating");
                        s->rating = rating ? rating->valueint : 0;
                }

                cJSON *ver = cJSON_GetObjectItem(sub, "verdict");
                strncpy(s->verdict, ver ? ver->valuestring : "UNKNOWN", 29);
                s->creationTime = (long long)cJSON_GetObjectItem(sub,
                                "creationTimeSeconds")->valuedouble;
                cJSON *pts = cJSON_GetObjectItem(sub, "points");
                s->points = pts ? pts->valuedouble : 0.0;
        }
        *count = n;
        cJSON_Delete(json);
        return all_subs;
}

ContestInfo* get_contest_list(int *cnt) {
    *cnt = 0;
    cJSON *json = api_get(API_BASE "contest.list?gym=false");  //排除非正式赛
    if (!json) return NULL;

    cJSON *result = cJSON_GetObjectItem(json, "result");
    int n = cJSON_GetArraySize(result);
    ContestInfo *arr = malloc(n * sizeof(ContestInfo));
    if (!arr) { cJSON_Delete(json); return NULL; }

    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_GetArrayItem(result, i);
        arr[i].contestId = cJSON_GetObjectItem(c, "id")->valueint;
        arr[i].startTime = (long long)cJSON_GetObjectItem(c, "startTimeSeconds")->valuedouble;
        arr[i].duration = cJSON_GetObjectItem(c, "durationSeconds")->valueint;
    }
    *cnt = n;
    cJSON_Delete(json);
    return arr;
}

ContestInfo* find_contest(ContestInfo *list, int cnt, int id) {
    for (int i = 0; i < cnt; i++)
        if (list[i].contestId == id) return &list[i];
    return NULL;
}


//读取多用户列表文件，每行一个 handle。
static char** read_handle_list(const char *filename, int *count) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("fopen"); return NULL; }  // perror 输出系统错误信息

    char **handles = malloc(MAX_HANDLES * sizeof(char*));
    if (!handles) { fclose(f); return NULL; }

    char line[128];
    *count = 0;
    while (fgets(line, sizeof(line), f) && *count < MAX_HANDLES) {
        line[strcspn(line, "\r\n")] = 0;    // 去掉行尾的换行符（Windows 可能 \r\n）
        if (line[0] == '#' || line[0] == '\0') continue; // 跳过注释和空行
        handles[*count] = _strdup(line);    // _strdup ：Windows 下的字符串复制函数，等价于 malloc+strcpy
        (*count)++;
    }
    fclose(f);
    return handles;
}

//生成多用户总览页面（index.html）。
static void generate_index(MultiUserSummary *summaries, int n, const char *dir) {
    char path[260];
    snprintf(path, sizeof(path), "%s/index.html", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    // 写入 HTML 头部和 CSS 样式
    fputs("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>CF Users</title>"
          "<style>body{background:#1a1a1a;color:#fff;font-family:Arial}"
          "table{border-collapse:collapse;width:100%}"
          "th,td{padding:10px;border:1px solid #444;text-align:center}"
          "a{text-decoration:none}</style></head><body><h1>Codeforces Users</h1><table>\n", f);
    
    // 表头
    fputs("<tr><th>Handle</th><th>Rating</th><th>Rank</th><th>Contests</th><th>Max Rating</th><th>180d Ct</th><th>180d Max</th></tr>\n", f);

    // 每行一个用户
    for (int i = 0; i < n; i++) {
        MultiUserSummary *s = &summaries[i];
        fprintf(f, "<tr><td><a href='%s.html' style='color:%s;font-weight:bold'>%s</a></td>"
                "<td style='color:%s'>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
                s->user.handle, rating_color(s->user.rating), s->user.handle,
                rating_color(s->user.rating), s->user.rating,
                s->user.rank, s->contestCount, s->maxRating,
                s->recent180Contests, s->recent180MaxRating);
    }
    fputs("</table></body></html>", f);
    fclose(f);
}

// 记录用户通过题目的信息，用于难度统计与去重
typedef struct {
    int contest_id;
    char index[5];
    int rating;
    long long pass_time;
} passed_t;

// 记录单场比赛中单道题的通过状态（赛内/补题）
typedef struct {
    char idx[5];
    int in_contest_ac;
    int upsolved_ac;
} prob_status_t;

//比较逻辑：优先按比赛 ID 升序，若 ID 相同则按题目编号字典序升序 
static int compare_passed(const void *a, const void *b) {
    const passed_t *pa = (const passed_t *)a;
    const passed_t *pb = (const passed_t *)b;

    if (pa->contest_id != pb->contest_id)
        return pa->contest_id - pb->contest_id;

    return strcmp(pa->index, pb->index);
}

// 比较逻辑：按题目编号字典序升序排列 (如 A, B, C, D1, D2)
static int compare_prob_status(const void *a, const void *b) {
    const prob_status_t *pa = (const prob_status_t *)a;
    const prob_status_t *pb = (const prob_status_t *)b;
    return strcmp(pa->idx, pb->idx);
}

// 生成单用户详细分析页面
static void generate_user_page(const char *handle, const char *dir) {
    printf("Processing user: %s\n", handle);

    UserInfo info = get_user_info(handle);
    if (info.handle[0] == 0) {
        fprintf(stderr, "Failed to get info for %s\n", handle);
        return;
    }
    int rating_count;
    RatingChange *ratings = get_user_rating(handle, &rating_count);
    int sub_count;
    Submission *subs = get_user_status(handle, &sub_count);
    int clist_cnt;
    ContestInfo *clist = get_contest_list(&clist_cnt);

    int max_rating = info.maxRating;
    int contest_all = rating_count;
    int recent_180 = 0, recent_180_max = 0;

    for (int i = 0; i < rating_count; i++) {
        if (within_days(ratings[i].updateTime, 180)) {
            recent_180++;
            if (ratings[i].newRating > recent_180_max)
                recent_180_max = ratings[i].newRating;
        }
    }

    typedef struct {
        int rating; int all, y1, y180, y30;
    } bin_t;
    bin_t bins[60];
    int bin_count = 0;
    for (int r = 800; r <= 3500; r += 100)
        bins[bin_count++] = (bin_t){r, 0, 0, 0, 0};

    passed_t *passed = malloc(sub_count * sizeof(passed_t));
    if (!passed) {
        free(ratings); free(subs); free(clist);
        return;
    }

    int passed_cnt = 0;
    for (int i = 0; i < sub_count; i++) {
        if (strcmp(subs[i].verdict, "OK") == 0) {
            passed[passed_cnt].contest_id = subs[i].contestId;
            strncpy(passed[passed_cnt].index, subs[i].index, 4);
            passed[passed_cnt].index[4] = '\0';
            passed[passed_cnt].rating = subs[i].rating;
            passed[passed_cnt].pass_time = subs[i].creationTime;
            passed_cnt++;
        }
    }

    qsort(passed, passed_cnt, sizeof(passed_t), compare_passed);

    int unique_cnt = 0;
    for (int i = 0; i < passed_cnt; i++) {
        if (i > 0 &&
            passed[i].contest_id == passed[i - 1].contest_id &&
            strcmp(passed[i].index, passed[i - 1].index) == 0) {
            continue; 
        }
        passed[unique_cnt++] = passed[i];
    }
    passed_cnt = unique_cnt;

    for (int i = 0; i < passed_cnt; i++) {
        if (passed[i].rating <= 0) continue;
        int bin = (passed[i].rating - 800) / 100;
        if (bin < 0 || bin >= bin_count) continue;
        bins[bin].all++;
        if (within_days(passed[i].pass_time, 365)) bins[bin].y1++;
        if (within_days(passed[i].pass_time, 180)) bins[bin].y180++;
        if (within_days(passed[i].pass_time, 30))  bins[bin].y30++;
    }
    free(passed);
    
    char path[260];
    snprintf(path, sizeof(path), "%s/%s.html", dir, handle);
    FILE *f = fopen(path, "w");
    if (!f) { free(ratings); free(subs); free(clist); return; }

    fputs("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>", f);
    fprintf(f, "%s - CF Analysis</title>", handle);
    fputs("<script src='https://cdn.jsdelivr.net/npm/echarts@5.4.3/"
          "dist/echarts.min.js'></script>"
          "<style>body{background:#f5f5f5;font-family:Segoe UI,Arial;"
          "margin:20px}.card{background:white;border-radius:10px;padding:20px;"
          "margin:10px 0;box-shadow:0 2px 8px rgba(0,0,0,0.1)}"
          ".avatar{width:80px;border-radius:50%;vertical-align:middle}"
          ".handle-name{font-size:28px;font-weight:bold;margin-left:20px}"
          ".stat-box{display:inline-block;text-align:center;margin:20px;"
          "padding:15px;border-radius:8px;background:#eee}"
          ".stat-num{font-size:24px;font-weight:bold}"
          ".stat-label{font-size:14px;color:#666}"
          "table{border-collapse:collapse;width:100%;background:white;"
          "border-radius:8px;overflow:hidden}"
          "th,td{padding:8px 12px;border-bottom:1px solid #ddd;"
          "text-align:center}.ac{color:green}.upsolved{color:orange}"
          ".positive{color:green;font-weight:bold}"
          ".negative{color:red;font-weight:bold}"
          ".tab{display:inline-block;padding:10px 20px;cursor:pointer;"
          "background:#ddd;margin-right:5px;border-radius:5px 5px 0 0}"
          ".tab.active{background:white;font-weight:bold}"
          "#chart{height:400px;width:100%}</style></head><body>\n", f);

    fputs("<div class='card'>", f);
    if (info.avatar[0])
        fprintf(f, "<img src='%s' class='avatar'>", info.avatar);
    fprintf(f, "<span class='handle-name' style='color:%s'>%s</span>",
            rating_color(info.rating), info.handle);
    fputs("<div style='margin-top:15px'>", f);

    fprintf(f, "<div class='stat-box'><div class='stat-num' style='color:%s'>"
            "%d</div><div class='stat-label'>Rating</div></div>",
            rating_color(info.rating), info.rating);
    fprintf(f, "<div class='stat-box'><div class='stat-num'>%s</div>"
            "<div class='stat-label'>Rank</div></div>", info.rank);
    fprintf(f, "<div class='stat-box'><div class='stat-num'>%d</div>"
            "<div class='stat-label'>Contests</div></div>", contest_all);
    fprintf(f, "<div class='stat-box'><div class='stat-num' style='color:%s'>"
            "%d</div><div class='stat-label'>Max Rating</div></div>",
            rating_color(max_rating), max_rating);
    fprintf(f, "<div class='stat-box'><div class='stat-num'>%d</div>"
            "<div class='stat-label'>180d Contests</div></div>", recent_180);
    fprintf(f, "<div class='stat-box'><div class='stat-num' style='color:%s'>"
            "%d</div><div class='stat-label'>180d Max</div></div>",
            rating_color(recent_180_max), recent_180_max);
    fputs("</div></div>", f);

    fputs("<div class='card'><h2>Contest History</h2><table>"
          "<tr><th>Contest</th><th>Date</th><th>Before</th><th>After</th>"
          "<th>Change</th><th>Rank</th><th>In Contest</th><th>Upsolved</th>"
          "</tr>\n", f);

    for (int i = rating_count - 1; i >= 0; i--) {
        RatingChange *rc = &ratings[i];
        int change = rc->newRating - rc->oldRating;
        char date[20];
        time_t t = rc->updateTime;
        strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));

        fprintf(f, "<tr><td>%s</td><td>%s</td>", rc->contestName, date);
        fprintf(f, "<td style='color:%s'>%d</td>",
                rating_color(rc->oldRating), rc->oldRating);
        fprintf(f, "<td style='color:%s'>%d</td>",
                rating_color(rc->newRating), rc->newRating);

        if (change > 0)
            fprintf(f, "<td class='positive'>+%d</td>", change);
        else if (change < 0)
            fprintf(f, "<td class='negative'>%d</td>", change);
        else
            fprintf(f, "<td style='color:gray'>0</td>");

        fprintf(f, "<td>%d</td>", rc->rank);

        long long contest_start = 0, contest_end = 0;
        ContestInfo *ci = find_contest(clist, clist_cnt, rc->contestId);
        if (ci) {
            contest_start = ci->startTime;
            contest_end = contest_start + ci->duration;
        }

        prob_status_t probs[26];
        int prob_count = 0;

        for (int s = 0; s < sub_count; s++) {
            if (subs[s].contestId != rc->contestId) continue;

            int found = -1;
            for (int p = 0; p < prob_count; p++) {
                if (strcmp(probs[p].idx, subs[s].index) == 0) {
                    found = p; break;
                }
            }
            if (found == -1 && prob_count < 26) {
                found = prob_count++;
                strncpy(probs[found].idx, subs[s].index, 4);
                probs[found].in_contest_ac = 0;
                probs[found].upsolved_ac = 0;
            }
            if (found == -1) continue;

            if (strcmp(subs[s].verdict, "OK") == 0) {
                if (subs[s].creationTime <= contest_end)
                    probs[found].in_contest_ac = 1;
                else
                    probs[found].upsolved_ac = 1;
            }
        }
        //题目按顺序显示
        if (prob_count > 0) {
            qsort(probs, prob_count, sizeof(prob_status_t),
                  compare_prob_status);
        }

        char in_contest_str[200] = "";
        char upsolved_str[200] = "";

        for (int p = 0; p < prob_count; p++) {
            if (probs[p].in_contest_ac) {
                if (strlen(in_contest_str) > 0) strcat(in_contest_str, " ");
                strcat(in_contest_str, probs[p].idx);
            } else if (probs[p].upsolved_ac) {
                if (strlen(upsolved_str) > 0) strcat(upsolved_str, " ");
                strcat(upsolved_str, probs[p].idx);
            }
        }

        fprintf(f, "<td class='ac'>%s</td>",
                strlen(in_contest_str) ? in_contest_str : "-");
        fprintf(f, "<td class='upsolved'>%s</td>",
                strlen(upsolved_str) ? upsolved_str : "-");
        fputs("</tr>\n", f);
    }
    fputs("</table></div>", f);

    fputs("<div class='card'><h2>Problem Difficulty Distribution</h2>"
          "<div id='tabs'><span class='tab active' onclick='switchTab(0)'>"
          "All</span><span class='tab' onclick='switchTab(1)'>1 Year</span>"
          "<span class='tab' onclick='switchTab(2)'>180 Days</span>"
          "<span class='tab' onclick='switchTab(3)'>30 Days</span></div>"
          "<div id='chart'></div></div>", f);

    fputs("<script>", f);
    fprintf(f, "var all=[");
    for (int i = 0; i < bin_count; i++)
        fprintf(f, "%d%s", bins[i].all, i==bin_count-1 ? "" : ",");
    fprintf(f, "];\nvar y1=[");
    for (int i = 0; i < bin_count; i++)
        fprintf(f, "%d%s", bins[i].y1, i==bin_count-1 ? "" : ",");
    fprintf(f, "];\nvar y180=[");
    for (int i = 0; i < bin_count; i++)
        fprintf(f, "%d%s", bins[i].y180, i==bin_count-1 ? "" : ",");
    fprintf(f, "];\nvar y30=[");
    for (int i = 0; i < bin_count; i++)
        fprintf(f, "%d%s", bins[i].y30, i==bin_count-1 ? "" : ",");
    fprintf(f, "];\nvar x=[");
    for (int i = 0; i < bin_count; i++)
        fprintf(f, "'%d'%s", bins[i].rating, i==bin_count-1 ? "" : ",");

    fprintf(f, "];\nvar sets=[all,y1,y180,y30];\n"
            "var myChart=echarts.init(document.getElementById('chart'));"
            "var opt={tooltip:{trigger:'axis'},xAxis:{data:x},yAxis:{},"
            "grid:{left:40,right:20},"
            "series:[{type:'bar',data:all,itemStyle:{color:'#5470c6'}}]};"
            "myChart.setOption(opt);"
            "function switchTab(idx){"
            "var tabs=document.querySelectorAll('.tab');"
            "for(var i=0;i<tabs.length;i++)"
            "tabs[i].classList.toggle('active',i==idx);"
            "opt.series[0].data=sets[idx];"
            "myChart.setOption(opt,true);"
            "}\n</script>");
    fputs("</body></html>", f);
    fclose(f);

    free(ratings);
    free(subs);
    free(clist);
    printf("Done: %s\n", path);
}

int main(int argc, char *argv[]) {
    const char *input = NULL;       // 输入的用户名或文件名
    const char *output_dir = "output";   // 输出目录，默认为output
    char buffer[100];

    if (argc >= 2) {
        input = argv[1];            // 第一个参数作为输入
    } else {
        printf("Enter Codeforces handle: ");
        if (fgets(buffer, sizeof(buffer), stdin)) {
            buffer[strcspn(buffer, "\r\n")] = 0; // 去除末尾换行符
            input = buffer;
        }
    }
    if (argc >= 3) output_dir = argv[2];  // 第二个参数为输出目录

    if (!input || input[0] == '\0') {
        fprintf(stderr, "No handle provided.\n");
        return 1;
    }

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", output_dir);
    system(cmd);

    //  判断输入是文件（多用户）还是单个 handle
    int is_file = (_access(input, 0) == 0) ? 1 : 0;
    if (is_file && strchr(input, '.') == NULL) is_file = 0;

    if (is_file) {
        int count;
        char **handles = read_handle_list(input, &count);
        if (!handles) {
            fprintf(stderr, "Cannot read list file.\n");
            return 1;
        }
        MultiUserSummary *summaries = malloc(count * sizeof(MultiUserSummary));
        // 逐一处理每个用户
        for (int i = 0; i < count; i++) {
            UserInfo u = get_user_info(handles[i]);
            int rc; RatingChange *rt = get_user_rating(handles[i], &rc);
            summaries[i].user = u;
            summaries[i].contestCount = rc;
            summaries[i].maxRating = u.maxRating;
            // 计算近180天比赛数和最高分
            int r180 = 0, r180max = 0;
            for (int j = 0; j < rc; j++) {
                if (within_days(rt[j].updateTime, 180)) {
                    r180++;
                    if (rt[j].newRating > r180max) r180max = rt[j].newRating;
                }
            }
            summaries[i].recent180Contests = r180;
            summaries[i].recent180MaxRating = r180max;
            free(rt);
            // 生成个人页面
            generate_user_page(handles[i], output_dir);
            free(handles[i]);
            Sleep(1000);  // 用户间暂停 1 秒，减轻 API 压力
        }
        free(handles);
        // 生成总览页
        generate_index(summaries, count, output_dir);
        free(summaries);
    } else {
        generate_user_page(input, output_dir);
    }

    printf("All done. Open index.html or %s.html in browser.\n", input);
    return 0;
}
