#include "app_http_server.h"
#include "app_dsl.h"
#include "app_wifi_ntp.h"
#include "app_settings.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "ui_terminal.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_http_server";

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_running = false;

typedef struct {
    char ssid[33];
    char pass[65];
    char tz_str[48];
    char tz_label[24];
    char ntp[64];
} portal_connect_req_t;

/* ---------- Captive Portal DNS Server (192.168.4.1) ---------- */

static void dns_server_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (s_dns_running) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        int pos = 12;
        bool ptr = false;
        while (pos < len && buf[pos] != 0) {
            if ((buf[pos] & 0xC0) == 0xC0) {
                pos += 2;
                ptr = true;
                break;
            }
            pos += buf[pos] + 1;
        }
        if (!ptr) {
            if (pos >= len || buf[pos] != 0) continue;
            pos++;
        }
        int q_end = pos + 4;
        if (q_end > len || q_end + 16 > (int)sizeof(buf)) continue;

        buf[2] = 0x81; buf[3] = 0x80;
        buf[4] = 0x00; buf[5] = 0x01;
        buf[6] = 0x00; buf[7] = 0x01;
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;
        static const uint8_t ans[16] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, 192, 168, 4, 1
        };
        memcpy(&buf[q_end], ans, sizeof(ans));
        sendto(sock, buf, q_end + sizeof(ans), 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- Helper to read request body ---------- */

static char *recv_body_alloc(httpd_req_t *req, size_t max_len, int *out_len)
{
    int total = req->content_len;
    if (total <= 0 || total > (int)max_len) {
        return NULL;
    }
    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) return NULL;

    int recvd = 0;
    while (recvd < total) {
        int r = httpd_req_recv(req, buf + recvd, total - recvd);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        recvd += r;
    }
    buf[recvd] = '\0';
    if (out_len) *out_len = recvd;
    return buf;
}

static esp_err_t send_json_resp(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* ---------- Embedded Web Console & Wi-Fi Portal ---------- */

static const char CONSOLE_HTML[] =
"<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>AI Ask-Me 提示终端控制台</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif;"
"background:#090E17;color:#F1F5F9;min-height:100vh;padding:18px;display:flex;justify-content:center}"
".wrap{width:100%;max-width:780px}"
".hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;flex-wrap:wrap;gap:8px}"
".hdr h1{font-size:20px;color:#38BDF8}"
".tabs{display:flex;gap:8px}"
".tab{background:#111A2E;border:1px solid #233352;color:#94A3B8;padding:7px 14px;border-radius:8px;cursor:pointer;font-size:13px}"
".tab.act{background:#0284C7;border-color:#38BDF8;color:#fff;font-weight:600}"
".card{background:#111A2E;border:1px solid #233352;border-radius:14px;padding:14px;margin-bottom:14px}"
".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:10px}"
".btn{background:#1E293B;border:1px solid #334155;color:#E2E8F0;padding:7px 12px;border-radius:8px;cursor:pointer;font-size:13px}"
".btn.pri{background:#2563EB;border-color:#60A5FA;color:#fff;font-weight:600}"
".btn.ok{background:#16A34A;border-color:#4ADE80;color:#fff;font-weight:600}"
".btn.warn{background:#D97706;border-color:#FBBF24;color:#fff}"
"textarea,input,select{width:100%;background:#090E17;border:1px solid #233352;border-radius:8px;color:#F1F5F9;padding:9px;font-size:13px;font-family:Consolas,monospace}"
"textarea{min-height:170px;resize:vertical}"
".shot{width:100%;border-radius:10px;border:1px solid #334155;background:#000;image-rendering:auto}"
".res{background:#090E17;border:1px solid #233352;border-radius:8px;padding:10px;font-family:Consolas,monospace;font-size:12px;color:#7DD3FC;white-space:pre-wrap;min-height:48px}"
".ap-list{max-height:180px;overflow-y:auto;display:flex;flex-direction:column;gap:6px;margin-bottom:10px}"
".ap{display:flex;justify-content:space-between;padding:8px 10px;border:1px solid #233352;border-radius:8px;background:#090E17;cursor:pointer}"
".ap.sel{border-color:#38BDF8;background:#13243A}"
".hide{display:none}"
"</style></head><body><div class='wrap'>"
"<div class='hdr'><h1>AI Ask-Me 交互提示终端</h1>"
"<div class='tabs'><button class='tab act' onclick='swTab(0)'>AI DSL 调试台</button>"
"<button class='tab' onclick='swTab(1)'>Wi-Fi 配网与设置</button></div></div>"
"<div id='p0'>"
"<div class='card'><div class='row'><span style='font-size:13px;color:#94A3B8'>实时屏幕预览 (640×172):</span>"
"<button class='btn' onclick='refreshShot()'>刷新截图</button>"
"<button class='btn warn' onclick='postJson(\"/api/dismiss\",{})'>收起回到待机</button></div>"
"<img id='shotImg' class='shot' src='/screenshot.bmp'></div>"
"<div class='card'><div class='row'>"
"<button class='btn' onclick='loadTpl(\"text\")'>模板: 确认提示</button>"
"<button class='btn' onclick='loadTpl(\"kv\")'>模板: KV 参数</button>"
"<button class='btn' onclick='loadTpl(\"choice\")'>模板: 多选胶囊</button>"
"<button class='btn' onclick='loadTpl(\"number\")'>模板: 数值调节</button>"
"<button class='btn' onclick='loadTpl(\"progress\")'>模板: 执行进度</button>"
"</div><textarea id='dslJson'></textarea>"
"<div class='row' style='margin-top:10px'>"
"<button class='btn pri' onclick='sendDsl(\"/api/ask\")'>发送 /api/ask (等待屏幕点击)</button>"
"<button class='btn ok' onclick='sendDsl(\"/api/status\")'>发送 /api/status (异步状态)</button>"
"<button class='btn' onclick='postJson(\"/api/sound\",{preset:\"ask\"})'>试听提示音</button>"
"</div><div id='dslRes' class='res'>等待发送请求...</div></div></div>"
"<div id='p1' class='hide'>"
"<div class='card'><div class='row'><span style='font-size:13px;color:#94A3B8'>音频与显示设置</span></div>"
"<div class='row'><label>音量:</label><input id='volInp' type='number' min='0' max='100' value='78' style='width:80px'>"
"<label>亮度:</label><input id='briInp' type='number' min='10' max='100' value='90' style='width:80px'>"
"<button class='btn pri' onclick='saveAudioDisp()'>应用设置</button></div></div>"
"<div class='card'><div class='row'><span style='font-size:13px;color:#94A3B8'>Wi-Fi 热点列表</span>"
"<button class='btn' onclick='scanWifi()'>扫描附近 Wi-Fi</button></div>"
"<div class='ap-list' id='apList'></div>"
"<div class='row'><input id='ssid' placeholder='Wi-Fi 名称 (SSID)'></div>"
"<div class='row'><input id='pass' type='password' placeholder='Wi-Fi 密码 (无密码留空)'></div>"
"<button class='btn ok' onclick='connectWifi()'>保存并连接 Wi-Fi</button>"
"<div id='wifiRes' class='res' style='margin-top:10px'>提示: 如处于配网模式请点击扫描。</div></div>"
"</div></div><script>"
"const TPLS={"
"text:{state:'asking',agent:'Claude Code',title:'是否执行生产环境数据库迁移？',subtitle:'prisma migrate deploy',layout:'text',body:'检测到 2 个待执行迁移脚本，将修改 users 表索引并添加审计字段。',callout:'警告：操作将短暂锁定 users 表约 3 秒',buttons:[{id:'approve',label:'立即执行',style:'success',is_default:false},{id:'dryrun',label:'仅预览 SQL',style:'primary'},{id:'reject',label:'取消操作',style:'danger'}],timeout_sec:60,sound:'ask'},"
"kv:{state:'asking',agent:'Codex CLI',title:'请审查即将执行的 Shell 命令',subtitle:'高风险权限申请',layout:'kv',kv:[{k:'命令',v:'rm -rf ./dist && npm run build',highlight:true},{k:'工作目录',v:'/srv/app-backend'},{k:'影响文件',v:'142 个构建产物'},{k:'耗时预估',v:'约 18 秒'}],buttons:[{id:'allow',label:'允许执行',style:'success'},{id:'deny',label:'拒绝',style:'danger'}],timeout_sec:45,sound:'warning'},"
"choice:{state:'asking',agent:'Antigravity',title:'请选择需要运行的测试套件',subtitle:'支持多选后点击右侧提交',layout:'choice',multi:true,choices:[{id:'unit',label:'单元测试',selected:true},{id:'api',label:'接口集成',selected:true},{id:'e2e',label:'端到端 UI',selected:false},{id:'perf',label:'压测基准',selected:false}],buttons:[{id:'run',label:'开始测试',style:'primary',is_default:true},{id:'skip',label:'跳过',style:'secondary'}],timeout_sec:60,sound:'ask'},"
"number:{state:'asking',agent:'AutoBuilder',title:'请确认并行构建线程数',subtitle:'点击中栏 - / + 微调数值',layout:'number',value:8,min:1,max:32,step:2,unit:'核',buttons:[{id:'ok',label:'应用并发数',style:'primary',is_default:true},{id:'cancel',label:'取消',style:'secondary'}],timeout_sec:60,sound:'info'},"
"progress:{state:'running',agent:'Deploy Bot',title:'正在编译并烧录 ESP32-S3 固件',subtitle:'COM5 · 921600bps',layout:'progress',percent:68,step_text:'正在链接 LVGL 静态库与字库...',steps:[{label:'字库',done:true},{label:'编译',active:true},{label:'烧录'},{label:'验证'}],sound:'info'}"
"};"
"function loadTpl(k){document.getElementById('dslJson').value=JSON.stringify(TPLS[k],null,2);}"
"function swTab(i){document.getElementById('p0').classList.toggle('hide',i!==0);document.getElementById('p1').classList.toggle('hide',i!==1);document.querySelectorAll('.tab').forEach((t,idx)=>t.classList.toggle('act',idx===i));if(i===1)scanWifi();}"
"function refreshShot(){document.getElementById('shotImg').src='/screenshot.bmp?t='+Date.now();}"
"function postJson(u,o){return fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json()).then(d=>{setTimeout(refreshShot,180);return d;});}"
"function sendDsl(ep){var el=document.getElementById('dslRes');el.textContent='请求已发送，等待终端响应...';setTimeout(refreshShot,220);"
"fetch(ep,{method:'POST',headers:{'Content-Type':'application/json'},body:document.getElementById('dslJson').value})"
".then(r=>r.json()).then(d=>{el.textContent=JSON.stringify(d,null,2);setTimeout(refreshShot,180);}).catch(e=>{el.textContent='ERR: '+e;});}"
"function saveAudioDisp(){postJson('/api/config',{volume:+document.getElementById('volInp').value,brightness:+document.getElementById('briInp').value}).then(d=>alert('已保存'));}"
"function scanWifi(){var l=document.getElementById('apList');l.innerHTML='扫描中...';fetch('/api/scan').then(r=>r.json()).then(aps=>{l.innerHTML='';aps.forEach(a=>{var d=document.createElement('div');d.className='ap';d.innerHTML='<span>'+a.ssid+'</span><span>'+a.rssi+'dBm</span>';d.onclick=()=>{document.getElementById('ssid').value=a.ssid;document.querySelectorAll('.ap').forEach(x=>x.classList.remove('sel'));d.classList.add('sel');};l.appendChild(d);});});}"
"function connectWifi(){var s=document.getElementById('ssid').value.trim();var p=document.getElementById('pass').value;if(!s)return;postJson('/api/connect',{ssid:s,pass:p}).then(d=>{document.getElementById('wifiRes').textContent=JSON.stringify(d);});}"
"loadTpl('text');setInterval(refreshShot,2000);"
"</script></body></html>";

/* ---------- HTTP Endpoint Handlers ---------- */

static esp_err_t handle_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, CONSOLE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_ask_post(httpd_req_t *req)
{
    char *body = recv_body_alloc(req, 8192, NULL);
    if (!body) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"invalid or empty body\"}");
    }

    dsl_request_t *dreq = calloc(1, sizeof(dsl_request_t));
    if (!dreq) {
        free(body);
        return send_json_resp(req, "{\"ok\":false,\"err\":\"oom\"}");
    }

    char err_msg[64] = {0};
    esp_err_t perr = app_dsl_parse_json(body, true, dreq, err_msg, sizeof(err_msg));
    free(body);
    if (perr != ESP_OK) {
        free(dreq);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", err_msg);
        return send_json_resp(req, resp);
    }

    uint32_t gen = 0;
    app_dsl_submit_request(dreq, &gen);
    ui_terminal_request_refresh();

    bool wait = dreq->wait;
    uint32_t max_wait_ms = (dreq->timeout_sec > 0) ? ((uint32_t)dreq->timeout_sec * 1000U + 2000U) : 120000U;
    char req_id[DSL_STR_ID_LEN];
    strlcpy(req_id, dreq->req_id, sizeof(req_id));
    free(dreq);

    if (!wait) {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"req_id\":\"%s\",\"status\":\"pending\"}", req_id);
        return send_json_resp(req, resp);
    }

    dsl_response_t *dresp = calloc(1, sizeof(dsl_response_t));
    if (!dresp) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"oom\"}");
    }
    app_dsl_wait_response(gen, max_wait_ms, dresp);
    ui_terminal_request_refresh();

    char *json_out = app_dsl_response_to_json(dresp);
    free(dresp);
    if (!json_out) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"json encode failed\"}");
    }
    esp_err_t ret = send_json_resp(req, json_out);
    free(json_out);
    return ret;
}

static esp_err_t handle_status_post(httpd_req_t *req)
{
    char *body = recv_body_alloc(req, 8192, NULL);
    if (!body) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"invalid or empty body\"}");
    }

    dsl_request_t *dreq = calloc(1, sizeof(dsl_request_t));
    if (!dreq) {
        free(body);
        return send_json_resp(req, "{\"ok\":false,\"err\":\"oom\"}");
    }

    char err_msg[64] = {0};
    esp_err_t perr = app_dsl_parse_json(body, false, dreq, err_msg, sizeof(err_msg));
    free(body);
    if (perr != ESP_OK) {
        free(dreq);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", err_msg);
        return send_json_resp(req, resp);
    }

    uint32_t gen = 0;
    app_dsl_submit_request(dreq, &gen);
    ui_terminal_request_refresh();

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"req_id\":\"%s\",\"state\":\"%s\"}",
             dreq->req_id, app_dsl_state_to_str(dreq->state));
    free(dreq);
    return send_json_resp(req, resp);
}

static esp_err_t handle_sound_post(httpd_req_t *req)
{
    char *body = recv_body_alloc(req, 2048, NULL);
    if (!body) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"empty body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return send_json_resp(req, "{\"ok\":false,\"err\":\"invalid json\"}");
    }

    const cJSON *j_vol = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if (cJSON_IsNumber(j_vol)) {
        int v = (int)j_vol->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        bsp_audio_set_volume((uint8_t)v);
        app_settings_set_volume((uint8_t)v);
    }

    const cJSON *j_mute = cJSON_GetObjectItemCaseSensitive(root, "mute");
    if (cJSON_IsBool(j_mute)) {
        bool m = cJSON_IsTrue(j_mute);
        bsp_audio_set_mute(m);
        app_settings_set_muted(m);
    }

    int repeat = 1;
    const cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(root, "repeat");
    if (cJSON_IsNumber(j_rep)) {
        repeat = (int)j_rep->valuedouble;
    }

    const cJSON *j_mel = cJSON_GetObjectItemCaseSensitive(root, "melody");
    const cJSON *j_pre = cJSON_GetObjectItemCaseSensitive(root, "preset");
    if (cJSON_IsString(j_mel) && j_mel->valuestring && j_mel->valuestring[0]) {
        bsp_audio_play_melody(j_mel->valuestring, repeat);
    } else if (cJSON_IsString(j_pre) && j_pre->valuestring && j_pre->valuestring[0]) {
        bsp_audio_play_preset(bsp_audio_preset_from_str(j_pre->valuestring), repeat);
    }

    cJSON_Delete(root);
    return send_json_resp(req, "{\"ok\":true}");
}

static esp_err_t handle_dismiss_post(httpd_req_t *req)
{
    app_dsl_dismiss("api");
    ui_terminal_request_refresh();
    return send_json_resp(req, "{\"ok\":true}");
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char *body = recv_body_alloc(req, 1024, NULL);
    if (!body) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"empty body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_resp(req, "{\"ok\":false,\"err\":\"invalid json\"}");
    }

    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if (cJSON_IsNumber(jv)) {
        uint8_t v = (uint8_t)((jv->valuedouble < 0) ? 0 : (jv->valuedouble > 100 ? 100 : jv->valuedouble));
        bsp_audio_set_volume(v);
        app_settings_set_volume(v);
    }
    const cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(jb)) {
        uint8_t b = (uint8_t)((jb->valuedouble < 10) ? 10 : (jb->valuedouble > 100 ? 100 : jb->valuedouble));
        bsp_display_set_brightness(b);
        app_settings_set_brightness(b);
    }
    cJSON_Delete(root);
    return send_json_resp(req, "{\"ok\":true}");
}

static esp_err_t handle_result_get(httpd_req_t *req)
{
    char query[128] = {0};
    char req_id[DSL_STR_ID_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "req_id", req_id, sizeof(req_id));
    }

    dsl_response_t resp;
    if (req_id[0] != '\0') {
        if (!app_dsl_get_result_by_id(req_id, &resp)) {
            return send_json_resp(req, "{\"ok\":false,\"err\":\"req_id not found\"}");
        }
    } else {
        if (!app_dsl_get_last_response(&resp, NULL, 0)) {
            return send_json_resp(req, "{\"ok\":true,\"status\":\"none\"}");
        }
    }

    char *json_out = app_dsl_response_to_json(&resp);
    if (!json_out) return send_json_resp(req, "{\"ok\":false}");
    esp_err_t ret = send_json_resp(req, json_out);
    free(json_out);
    return ret;
}

static esp_err_t handle_state_get(httpd_req_t *req)
{
    net_status_t net;
    app_wifi_get_status(&net);

    dsl_request_t dreq;
    uint32_t rem_ms = 0;
    bool waiting = false;
    bool active = app_dsl_get_active_snapshot(&dreq, &rem_ms, NULL, &waiting);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "device", "ESP32-S3-Touch-LCD-3.49-V2");
    cJSON_AddStringToObject(root, "ip", net.ip_str);
    cJSON_AddStringToObject(root, "ssid", net.ssid);
    cJSON_AddNumberToObject(root, "rssi", net.rssi);
    cJSON_AddNumberToObject(root, "battery_pct", bsp_power_get_battery_pct());
    cJSON_AddNumberToObject(root, "volume", bsp_audio_get_volume());
    cJSON_AddBoolToObject(root, "muted", bsp_audio_is_muted());
    cJSON_AddBoolToObject(root, "active", active);
    if (active) {
        cJSON_AddStringToObject(root, "req_id", dreq.req_id);
        cJSON_AddStringToObject(root, "state", app_dsl_state_to_str(dreq.state));
        cJSON_AddStringToObject(root, "layout", app_dsl_layout_to_str(dreq.layout));
        cJSON_AddBoolToObject(root, "waiting", waiting);
        cJSON_AddNumberToObject(root, "remaining_ms", rem_ms);
    }

    char *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t ret = send_json_resp(req, json_out ? json_out : "{\"ok\":false}");
    if (json_out) free(json_out);
    return ret;
}

static esp_err_t handle_scan_get(httpd_req_t *req)
{
    wifi_ap_record_t raw_aps[20];
    uint16_t raw_cnt = 0;
    app_wifi_scan_networks(raw_aps, 20, &raw_cnt);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < raw_cnt; i++) {
        if (raw_aps[i].ssid[0] == '\0') continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)raw_aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", raw_aps[i].rssi);
        cJSON_AddBoolToObject(o, "auth", raw_aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    char *json_out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    esp_err_t ret = send_json_resp(req, json_out ? json_out : "[]");
    if (json_out) free(json_out);
    return ret;
}

static void delayed_connect_task(void *arg)
{
    portal_connect_req_t *r = (portal_connect_req_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(600));
    app_wifi_stop_portal_and_connect(r->ssid, r->pass, r->tz_str, r->tz_label, r->ntp);
    free(r);
    vTaskDelete(NULL);
}

static esp_err_t handle_connect_post(httpd_req_t *req)
{
    char *body = recv_body_alloc(req, 512, NULL);
    if (!body) return send_json_resp(req, "{\"ok\":false,\"err\":\"empty\"}");

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_resp(req, "{\"ok\":false,\"err\":\"bad json\"}");

    portal_connect_req_t *creq = calloc(1, sizeof(portal_connect_req_t));
    if (!creq) {
        cJSON_Delete(root);
        return send_json_resp(req, "{\"ok\":false,\"err\":\"oom\"}");
    }

    const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "pass");
    if (cJSON_IsString(js) && js->valuestring) strlcpy(creq->ssid, js->valuestring, sizeof(creq->ssid));
    if (cJSON_IsString(jp) && jp->valuestring) strlcpy(creq->pass, jp->valuestring, sizeof(creq->pass));
    strlcpy(creq->tz_str, "CST-8", sizeof(creq->tz_str));
    strlcpy(creq->tz_label, "UTC+8 北京", sizeof(creq->tz_label));
    strlcpy(creq->ntp, "ntp.aliyun.com", sizeof(creq->ntp));
    cJSON_Delete(root);

    if (creq->ssid[0] == '\0') {
        free(creq);
        return send_json_resp(req, "{\"ok\":false,\"err\":\"empty ssid\"}");
    }

    send_json_resp(req, "{\"ok\":true}");
    xTaskCreate(delayed_connect_task, "portal_conn", 4096, creq, 4, NULL);
    return ESP_OK;
}

static esp_err_t handle_screenshot_get(httpd_req_t *req)
{
    uint8_t *bmp = NULL;
    size_t bmp_len = 0;
    esp_err_t err = bsp_display_capture_bmp(&bmp, &bmp_len);
    if (err != ESP_OK || !bmp || bmp_len == 0) {
        if (bmp) heap_caps_free(bmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, (const char *)bmp, (ssize_t)bmp_len);
    heap_caps_free(bmp);
    return ret;
}

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "", 0);
}

esp_err_t app_http_server_start(void)
{
    if (s_httpd != NULL) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 10240;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/",                   .method = HTTP_GET,  .handler = handle_root_get},
        {.uri = "/api/ask",            .method = HTTP_POST, .handler = handle_ask_post},
        {.uri = "/api/status",         .method = HTTP_POST, .handler = handle_status_post},
        {.uri = "/api/sound",          .method = HTTP_POST, .handler = handle_sound_post},
        {.uri = "/api/dismiss",        .method = HTTP_POST, .handler = handle_dismiss_post},
        {.uri = "/api/config",         .method = HTTP_POST, .handler = handle_config_post},
        {.uri = "/api/result",         .method = HTTP_GET,  .handler = handle_result_get},
        {.uri = "/api/state",          .method = HTTP_GET,  .handler = handle_state_get},
        {.uri = "/api/scan",           .method = HTTP_GET,  .handler = handle_scan_get},
        {.uri = "/api/connect",        .method = HTTP_POST, .handler = handle_connect_post},
        {.uri = "/screenshot.bmp",     .method = HTTP_GET,  .handler = handle_screenshot_get},
        {.uri = "/api/screenshot.bmp", .method = HTTP_GET,  .handler = handle_screenshot_get},
        {.uri = "/generate_204",       .method = HTTP_GET,  .handler = handle_redirect},
        {.uri = "/hotspot-detect.html",.method = HTTP_GET,  .handler = handle_redirect},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP API server listening on port 80 (/api/ask, /api/status, /api/sound)");
    return ESP_OK;
}

esp_err_t app_portal_start(void)
{
    if (!s_dns_running) {
        s_dns_running = true;
        xTaskCreate(dns_server_task, "portal_dns", 4096, NULL, 3, &s_dns_task);
    }
    return app_http_server_start();
}

void app_portal_stop(void)
{
    s_dns_running = false;
}
