#include "WebUiService.h"
#include "../reader/ReaderBookService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../display/DisplayService.h"
#include "../VinkPaperS3.h"
#include <SD.h>
#include <SPIFFS.h>
#include <Preferences.h>

namespace vink3 {

WebUiService g_webUi;

namespace {

String urlDecode(const String& s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '%' && i + 2 < s.length()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                return 0;
            };
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

String normalizePath(String path) {
    if (path.isEmpty()) path = "/";
    if (!path.startsWith("/")) path = "/" + path;
    int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);
    while (path.indexOf("//") >= 0) path.replace("//", "/");
    if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    return path;
}

bool isSafePath(const String& path) {
    if (path.isEmpty()) return false;
    for (size_t i = 0; i < path.length(); ++i) {
        const char c = path[i];
        if (static_cast<uint8_t>(c) < 0x20 || c == '\\') return false;
    }
    if (path.indexOf("..") >= 0) return false;
    return true;
}

bool queryValue(const char* uri, const char* key, String& out) {
    const char* q = strchr(uri, '?');
    if (!q) return false;
    String query(q + 1);
    String needle = String(key) + "=";
    int start = 0;
    while (start <= static_cast<int>(query.length())) {
        int amp = query.indexOf('&', start);
        if (amp < 0) amp = query.length();
        String part = query.substring(start, amp);
        if (part.startsWith(needle)) {
            out = urlDecode(part.substring(needle.length()));
            return true;
        }
        start = amp + 1;
    }
    return false;
}

String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (static_cast<uint8_t>(c) < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<uint8_t>(c)));
            out += buf;
        } else out += c;
    }
    return out;
}

String fileNameForEntry(const String& raw, const String& dir) {
    String name = raw;
    String parent = normalizePath(dir);
    if (parent != "/" && name.startsWith(parent + "/")) name = name.substring(parent.length() + 1);
    else if (parent == "/" && name.startsWith("/")) name = name.substring(1);
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    return name;
}

String formatBytes(size_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    if (bytes < 1024 * 1024) return String(bytes / 1024) + " KB";
    return String(bytes / (1024 * 1024)) + " MB";
}

bool ensureSd(httpd_req_t* req) {
    if (g_readerBook.ensureSdReadyForTransfer()) return true;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"SD not ready\"}");
    return false;
}

esp_err_t json(httpd_req_t* req, const String& body, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, body.c_str(), body.length());
}

esp_err_t errJson(httpd_req_t* req, const char* status, const char* msg) {
    return json(req, String("{\"error\":\"") + msg + "\"}", status);
}

bool mkdirRecursive(String path) {
    path = normalizePath(path);
    if (path == "/") return true;
    int pos = 1;
    while (pos <= static_cast<int>(path.length())) {
        int next = path.indexOf('/', pos);
        if (next < 0) next = path.length();
        String part = path.substring(0, next);
        if (part.length() > 1 && !SD.exists(part.c_str()) && !SD.mkdir(part.c_str())) return false;
        if (next == static_cast<int>(path.length())) break;
        pos = next + 1;
    }
    return true;
}

void ensureParentDir(const String& path) {
    int slash = path.lastIndexOf('/');
    if (slash <= 0) return;
    mkdirRecursive(path.substring(0, slash));
}

String pathFromWildcardUri(const char* uri) {
    String u(uri ? uri : "");
    int q = u.indexOf('?');
    if (q >= 0) u = u.substring(0, q);
    const String prefix = "/api/files/";
    if (u.startsWith(prefix)) return normalizePath(urlDecode(u.substring(prefix.length())));
    return "/";
}

String listFilesJson(String path) {
    path = normalizePath(path);
    String body = "{\"path\":\"" + jsonEscape(path) + "\",\"items\":[";
    File root = SD.open(path.c_str(), FILE_READ);
    bool first = true;
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            String name = fileNameForEntry(String(entry.name()), path);
            if (!name.isEmpty() && isSafePath(name)) {
                if (!first) body += ",";
                first = false;
                body += "{\"name\":\"" + jsonEscape(name) + "\",\"isDir\":";
                body += entry.isDirectory() ? "true" : "false";
                body += ",\"size\":" + String(entry.size()) + ",\"sizeText\":\"" + formatBytes(entry.size()) + "\"}";
            }
            File next = root.openNextFile();
            entry.close();
            entry = next;
        }
    }
    if (root) root.close();
    body += "]}";
    return body;
}

const char* mimeForPath(const String& path) {
    String lower = path;
    lower.toLowerCase();
    if (lower.endsWith(".html")) return "text/html";
    if (lower.endsWith(".txt")) return "text/plain; charset=utf-8";
    if (lower.endsWith(".json")) return "application/json";
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
    if (lower.endsWith(".png")) return "image/png";
    return "application/octet-stream";
}

static const char kHtml[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Vink 文件传输</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#f3f3f3;color:#222;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}.top{background:#111;color:#fff;padding:14px 16px;font-weight:700}.wrap{max-width:860px;margin:0 auto;padding:14px}.card{background:#fff;border-radius:10px;padding:14px;margin:0 0 12px;box-shadow:0 1px 4px #0002}.toolbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0}button,.btn{border:1px solid #ccc;background:#fff;border-radius:6px;padding:8px 10px;cursor:pointer;text-decoration:none;color:#222;font-size:14px}.primary{background:#111;color:#fff;border-color:#111}.danger{color:#b00020}.path{font-size:13px;color:#666;word-break:break-all}.item{display:flex;gap:8px;align-items:center;border-bottom:1px solid #eee;padding:9px 0}.name{flex:1;word-break:break-all}.meta{font-size:12px;color:#888;min-width:68px;text-align:right}.actions{display:flex;gap:4px;flex-wrap:wrap;justify-content:flex-end}.msg{display:none;margin-top:8px;padding:8px;border-radius:6px}.ok{background:#e2f5e7;color:#165c2d}.err{background:#fde7e7;color:#8a1f1f}input[type=file]{max-width:100%}h2{font-size:16px;margin:0 0 10px;color:#444}.row{display:flex;align-items:center;gap:8px;margin:9px 0}.row label{width:110px;color:#555}.row select,.row input[type=number],.row input[type=text],.row input[type=password]{flex:1;padding:8px;border:1px solid #ccc;border-radius:6px}.row span{font-size:12px;color:#888}.row input[type=checkbox]{width:22px;height:22px}
</style></head><body><div class="top">Vink-PaperS3 WebUI</div><div class="wrap"><div class="toolbar"><button onclick="show('files')">文件</button><button onclick="show('config')">配置</button><button onclick="show('info')">系统</button></div>
<div id="panelFiles">
  <div class="card"><h2>文件浏览器</h2><div class="path" id="path"></div><div class="toolbar"><button onclick="go('/')">SD 根目录</button><button onclick="ensureBooks()">打开 /books</button><button onclick="up()">上级目录</button><button onclick="load()">刷新</button><button onclick="mkdir()">新建目录</button></div><div id="list"></div></div>
  <div class="card"><h2>上传文件</h2><input type="file" id="files" multiple><div class="toolbar"><button class="primary" onclick="upload(cur)">上传到当前目录</button><button onclick="upload('/books')">上传到 /books</button></div><div style="font-size:13px;color:#666;line-height:1.6">你可以直接进入任意文件夹后上传 TXT。设备端书架会按 SD 目录读取，不需要 WebUI 单独维护书架。</div><div id="msg" class="msg"></div></div>
</div>
<div id="panelConfig" style="display:none">
  <div class="card"><h2>阅读排版</h2>
    <div class="row"><label>字体大小</label><select id="fontSize"><option value="16">16 px</option><option value="20">20 px</option><option value="32">32 px</option></select></div>
    <div class="row"><label>行间距</label><input type="number" id="lineSpacing" min="0" max="200"><span>0~200%</span></div>
    <div class="row"><label>段间距</label><input type="number" id="paragraphSpacing" min="0" max="100"><span>0~100%</span></div>
    <div class="row"><label>首行缩进</label><input type="number" id="indentFirstLine" min="0" max="4"><span>0~4 字</span></div>
    <div class="row"><label>左右边距</label><input type="number" id="marginLeft" min="0" max="120"><span>px</span><input type="hidden" id="marginRight"></div>
    <div class="row"><label>上边距</label><input type="number" id="marginTop" min="0" max="160"><span>px</span></div>
    <div class="row"><label>下边距</label><input type="number" id="marginBottom" min="0" max="160"><span>px</span></div>
    <div class="row"><label>两端对齐</label><input type="checkbox" id="justify"></div>
    <div class="row"><label>排版模式</label><select id="layoutPreset"><option value="0">原始</option><option value="1">优化</option><option value="2">紧凑</option></select></div>
  </div>
  <div class="card"><h2>刷新策略</h2>
    <div class="row"><label>全刷频率</label><select id="refreshStrategy"><option value="0">低</option><option value="1">中</option><option value="2">高</option></select><span>低=20页，中=10页，高=5页</span></div>
    <div class="row"><label>翻页档位</label><select id="pageTurnProfile"><option value="0">清晰</option><option value="1">均衡</option><option value="2">快速</option></select><span>清晰=64px，均衡=108px，快速=180px</span></div>
  </div>
  <div class="card"><h2>显示效果</h2>
    <div class="row"><label>抗锯齿</label><input type="checkbox" id="antiAlias"></div>
    <div class="row"><label>下划线</label><input type="checkbox" id="underline"></div>
    <div class="row"><label>翻页动画</label><input type="checkbox" id="pageTurnEffect"></div>
    <div class="row"><label>双击锁屏/解锁</label><input type="checkbox" id="doubleTapUnlock"></div>
  </div>
  <div class="card"><h2>WiFi（STA 模式）</h2><div class="row"><label>SSID</label><input type="text" id="wifiSsid" placeholder="路由器名称"></div><div class="row"><label>密码</label><input type="password" id="wifiPassword" placeholder="留空不修改"></div><div style="font-size:13px;color:#666;line-height:1.6">仅保存到本机配置；不会开机自动连接，不影响启动链路。</div></div>
  <div class="card"><h2>系统（占位）</h2><div class="row"><label>自动休眠</label><input type="checkbox" id="autoSleepEnabled" disabled></div><div class="row"><label>休眠时间</label><input type="number" id="autoSleepMinutes" disabled value="5"><span>分钟</span></div><div class="row"><label>锁屏图片</label><input type="text" id="lockScreenImagePath" disabled placeholder="占位"></div><div class="row"><label>启用锁屏</label><input type="checkbox" id="lockScreenEnabled" disabled></div><div class="row"><label>双击唤醒</label><input type="checkbox" id="lockScreenWakeOnDoubleClick" disabled checked></div><div class="row"><label>简体中文</label><input type="checkbox" id="simplifiedChinese" disabled checked></div><div style="font-size:13px;color:#666;line-height:1.6">这些系统项先占位，不保存、不影响启动链路。</div></div>
  <div class="card"><h2>备份 / 维护</h2><div class="toolbar"><a class="btn" href="/api/config/export" download="vink-config.json">下载完整配置</a><button onclick="ensureDirGo('/covers')">打开封面目录</button><button onclick="ensureDirGo('/lockscreen')">打开锁屏目录</button></div></div>
  <div class="card"><button class="primary" onclick="saveConfig()">保存到本机配置</button><div id="cfgMsg" class="msg"></div></div>
</div>
<div id="panelInfo" style="display:none"><div class="card"><h2>系统信息</h2><div id="info" style="font-size:13px;line-height:1.8;color:#555"></div></div></div>
</div><script>
let cur='/'; const $=id=>document.getElementById(id); const api='';
function show(tab){['Files','Config','Info'].forEach(n=>$('panel'+n).style.display=(n.toLowerCase()===tab?'block':'none')); if(tab==='config')loadConfig(); if(tab==='info')info();}
function enc(p){return encodeURIComponent(p).replace(/%2F/g,'%2F')}
function showMsg(t,ok=true){let m=$('msg');m.style.display='block';m.className='msg '+(ok?'ok':'err');m.textContent=t}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function join(d,n){return (d==='/'?'/':d+'/')+n}
function go(p){cur=p||'/';show('files');load()}
async function ensureDirGo(p){await fetch('/api/files?path='+encodeURIComponent(p),{method:'POST'});go(p)}
function up(){if(cur==='/')return;let p=cur.replace(/\/$/,'');let i=p.lastIndexOf('/');go(i<=0?'/':p.slice(0,i))}
async function load(){let r=await fetch(api+'/api/files?path='+encodeURIComponent(cur));let j=await r.json();cur=j.path||cur;$('path').innerHTML='当前路径：<b>'+esc(cur)+'</b>';let html='';if(cur!=='/')html+='<div class="item"><div class="name">📁 ..</div><div class="actions"><button onclick="up()">进入</button></div></div>';(j.items||[]).forEach(it=>{let p=join(cur,it.name);html+='<div class="item"><div class="name">'+(it.isDir?'📁 ':'📄 ')+esc(it.name)+'</div><div class="meta">'+(it.isDir?'目录':esc(it.sizeText))+'</div><div class="actions">'+(it.isDir?'<button onclick="go(\''+esc(p)+'\')">进入</button>':'<a class="btn" href="/api/files/'+enc(p)+'" download>下载</a>')+'<button onclick="ren(\''+esc(p)+'\')">重命名</button><button class="danger" onclick="del(\''+esc(p)+'\')">删除</button></div></div>'});$('list').innerHTML=html||'<div style="color:#999">空目录</div>'}
async function ensureBooks(){await fetch('/api/files?path='+encodeURIComponent('/books'),{method:'POST'});go('/books')}
async function mkdir(){let n=prompt('目录名');if(!n)return;let p=join(cur,n);let r=await fetch('/api/files?path='+encodeURIComponent(p),{method:'POST'});showMsg(r.ok?'目录已创建':'创建失败',r.ok);load()}
async function del(p){if(!confirm('删除 '+p+' ?'))return;let r=await fetch('/api/files/'+enc(p),{method:'DELETE'});showMsg(r.ok?'已删除':'删除失败',r.ok);load()}
async function ren(p){let n=prompt('新路径',p);if(!n||n===p)return;let r=await fetch('/api/files/rename?from='+encodeURIComponent(p)+'&to='+encodeURIComponent(n),{method:'POST'});showMsg(r.ok?'已重命名':'重命名失败',r.ok);load()}
async function upload(base){let fs=$('files').files;if(!fs.length){showMsg('请选择文件',false);return}await fetch('/api/files?path='+encodeURIComponent(base),{method:'POST'});for(const f of fs){let dest=join(base,f.name);let r=await fetch('/api/files/'+enc(dest),{method:'PUT',body:f});if(!r.ok){showMsg('上传失败：'+f.name,false);return}}showMsg('上传完成');$('files').value='';go(base)}
async function loadConfig(){let c=await (await fetch('/api/config')).json();['fontSize','lineSpacing','paragraphSpacing','indentFirstLine','marginLeft','marginRight','marginTop','marginBottom','layoutPreset','refreshStrategy','pageTurnProfile','wifiSsid','autoSleepMinutes','lockScreenImagePath'].forEach(id=>{if($(id)&&c[id]!==undefined)$(id).value=c[id]});['justify','antiAlias','underline','pageTurnEffect','doubleTapUnlock','autoSleepEnabled','lockScreenEnabled','lockScreenWakeOnDoubleClick','simplifiedChinese'].forEach(id=>{if($(id)&&c[id]!==undefined)$(id).checked=!!c[id]});if($('wifiPassword'))$('wifiPassword').placeholder=c.wifiPasswordSet?'已保存，留空不修改':'留空不设置'}
async function saveConfig(){let body=new URLSearchParams();['fontSize','lineSpacing','paragraphSpacing','indentFirstLine','marginLeft','marginRight','marginTop','marginBottom','layoutPreset','refreshStrategy','pageTurnProfile','wifiSsid'].forEach(id=>body.set(id,$(id).value));if($('wifiPassword').value)body.set('wifiPassword',$('wifiPassword').value);['justify','antiAlias','underline','pageTurnEffect','doubleTapUnlock'].forEach(id=>body.set(id,$(id).checked?'1':'0'));let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});let j=await r.json();let m=$('cfgMsg');m.style.display='block';m.className='msg '+(j.saved?'ok':'err');m.textContent=j.saved?'已保存到本机配置':'保存失败';if(j.saved)loadConfig()}
async function info(){let r=await fetch('/api/system/info');let i=await r.json();$('info').innerHTML='固件版本：'+esc(i.version)+'<br>访问地址：http://'+esc(i.ip)+'<br>可用内存：'+i.freeHeap+' B<br>PSRAM：'+i.freePsram+' B<br>Flash：'+i.flashSize+' B<br>SD：'+i.sdUsed+' / '+i.sdTotal+' B<br>运行时间：'+Math.floor(i.uptimeSeconds/60)+' 分钟'}
load();info();
</script></body></html>
)rawliteral";

esp_err_t rootHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t filesGet(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String path;
    queryValue(req->uri, "path", path);
    path = normalizePath(path);
    if (!isSafePath(path)) return errJson(req, "403 Forbidden", "Invalid path");
    return json(req, listFilesJson(path));
}

esp_err_t mkdirHandler(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String path;
    if (!queryValue(req->uri, "path", path)) return errJson(req, "400 Bad Request", "Missing path");
    path = normalizePath(path);
    if (!isSafePath(path)) return errJson(req, "403 Forbidden", "Invalid path");
    const bool ok = mkdirRecursive(path);
    return ok ? json(req, "{\"ok\":true}") : errJson(req, "500 Internal Server Error", "mkdir failed");
}

esp_err_t renameHandler(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String from, to;
    if (!queryValue(req->uri, "from", from) || !queryValue(req->uri, "to", to)) return errJson(req, "400 Bad Request", "Missing path");
    from = normalizePath(from);
    to = normalizePath(to);
    if (!isSafePath(from) || !isSafePath(to) || from == "/" || to == "/") return errJson(req, "403 Forbidden", "Invalid path");
    ensureParentDir(to);
    const bool ok = SD.rename(from.c_str(), to.c_str());
    return ok ? json(req, "{\"ok\":true}") : errJson(req, "500 Internal Server Error", "rename failed");
}

esp_err_t fileDownload(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String path = pathFromWildcardUri(req->uri);
    if (!isSafePath(path) || path == "/") return errJson(req, "403 Forbidden", "Invalid path");
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return errJson(req, "404 Not Found", "Not found");
    }
    httpd_resp_set_hdr(req, "Content-Type", mimeForPath(path));
    char buf[1024];
    while (f.available()) {
        size_t n = f.readBytes(buf, sizeof(buf));
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { f.close(); return ESP_FAIL; }
    }
    f.close();
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t filePut(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String path = pathFromWildcardUri(req->uri);
    if (!isSafePath(path) || path == "/") return errJson(req, "403 Forbidden", "Invalid path");
    ensureParentDir(path);
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return errJson(req, "500 Internal Server Error", "open failed");
    int remaining = req->content_len;
    char buf[1024];
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, min<int>(remaining, sizeof(buf)));
        if (n <= 0) { f.close(); SD.remove(path.c_str()); return errJson(req, "400 Bad Request", "upload interrupted"); }
        if (f.write(reinterpret_cast<const uint8_t*>(buf), n) != static_cast<size_t>(n)) {
            f.close(); SD.remove(path.c_str()); return errJson(req, "500 Internal Server Error", "write failed");
        }
        remaining -= n;
    }
    f.close();
    return json(req, "{\"ok\":true}");
}

bool deleteRecursive(const String& path) {
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (!f.isDirectory()) { f.close(); return SD.remove(path.c_str()); }
    File child = f.openNextFile();
    while (child) {
        String childName(child.name());
        child.close();
        if (!deleteRecursive(childName)) { f.close(); return false; }
        child = f.openNextFile();
    }
    f.close();
    return SD.rmdir(path.c_str());
}

esp_err_t fileDelete(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    String path = pathFromWildcardUri(req->uri);
    if (!isSafePath(path) || path == "/") return errJson(req, "403 Forbidden", "Invalid path");
    const bool ok = deleteRecursive(path);
    return ok ? json(req, "{\"ok\":true}") : errJson(req, "500 Internal Server Error", "delete failed");
}


bool readRequestBody(httpd_req_t* req, String& out, size_t maxLen = 2048) {
    if (req->content_len > maxLen) return false;
    out = "";
    out.reserve(req->content_len + 1);
    char buf[256];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, min<int>(remaining, sizeof(buf)));
        if (n <= 0) return false;
        out.concat(buf, n);
        remaining -= n;
    }
    return true;
}

bool formValue(const String& body, const char* key, String& out) {
    String needle = String(key) + "=";
    int start = 0;
    while (start <= static_cast<int>(body.length())) {
        int amp = body.indexOf('&', start);
        if (amp < 0) amp = body.length();
        String part = body.substring(start, amp);
        if (part.startsWith(needle)) {
            out = urlDecode(part.substring(needle.length()));
            return true;
        }
        start = amp + 1;
    }
    return false;
}

bool formHasKey(const String& body, const char* key) {
    String ignored;
    return formValue(body, key, ignored);
}

uint8_t formU8(const String& body, const char* key, uint8_t fallback, uint8_t lo, uint8_t hi) {
    String v;
    if (!formValue(body, key, v)) return fallback;
    int n = v.toInt();
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return static_cast<uint8_t>(n);
}

bool formBool(const String& body, const char* key, bool fallback) {
    String v;
    if (!formValue(body, key, v)) return fallback;
    return v == "1" || v == "true" || v == "on";
}

String localWifiSsid() {
    Preferences prefs;
    if (!prefs.begin("vink-wifi", true)) return String();
    String ssid = prefs.getString("ssid", "");
    prefs.end();
    return ssid;
}

bool localWifiPasswordSet() {
    Preferences prefs;
    if (!prefs.begin("vink-wifi", true)) return false;
    String password = prefs.getString("password", "");
    prefs.end();
    return !password.isEmpty();
}

bool saveLocalWifi(const String& ssid, const String* password) {
    Preferences prefs;
    if (!prefs.begin("vink-wifi", false)) return false;
    prefs.putString("ssid", ssid);
    if (password) prefs.putString("password", *password);
    prefs.end();
    return true;
}

String configJson() {
    const ReaderSettings& rs = g_readerText.settings();
    String body = "{";
    body += "\"fontSize\":" + String(g_readerText.readerFontSizeSetting()) + ",";
    body += "\"lineSpacing\":" + String(g_readerText.webLineSpacing()) + ",";
    body += "\"paragraphSpacing\":" + String(g_readerText.webParagraphSpacing()) + ",";
    body += "\"indentFirstLine\":" + String(g_readerText.webIndentFirstLine()) + ",";
    body += "\"marginLeft\":" + String(g_readerText.webMarginLeft()) + ",";
    body += "\"marginRight\":" + String(g_readerText.webMarginRight()) + ",";
    body += "\"marginTop\":" + String(g_readerText.webMarginTop()) + ",";
    body += "\"marginBottom\":" + String(g_readerText.webMarginBottom()) + ",";
    body += "\"justify\":" + String(g_readerText.webJustify() ? "true" : "false") + ",";
    body += "\"layoutPreset\":" + String(g_readerText.layoutPreset()) + ",";
    body += "\"pageMarginLevel\":" + String(g_readerText.pageMarginLevel()) + ",";
    body += "\"lineSpacingLevel\":" + String(g_readerText.lineSpacingLevel()) + ",";
    body += "\"antiAlias\":" + String(g_readerText.antiAliasEnabled() ? "true" : "false") + ",";
    body += "\"underline\":" + String(g_readerText.underlineEnabled() ? "true" : "false") + ",";
    body += "\"pageTurnEffect\":" + String(g_readerText.pageTurnEffectEnabled() ? "true" : "false") + ",";
    body += "\"doubleTapUnlock\":" + String(g_readerText.doubleTapUnlockEnabled() ? "true" : "false") + ",";
    body += "\"refreshStrategy\":" + String(static_cast<uint8_t>(g_displayService.readerRefreshStrategy())) + ",";
    body += "\"pageTurnProfile\":" + String(static_cast<uint8_t>(g_displayService.readerPageTurnProfile())) + ",";
    body += "\"wifiSsid\":\"" + jsonEscape(localWifiSsid()) + "\",";
    body += "\"wifiPasswordSet\":" + String(localWifiPasswordSet() ? "true" : "false") + ",";
    body += "\"autoSleepEnabled\":false,\"autoSleepMinutes\":5,";
    body += "\"lockScreenImagePath\":\"\",\"lockScreenEnabled\":false,\"lockScreenWakeOnDoubleClick\":true,";
    body += "\"simplifiedChinese\":true,";
    body += "\"formatting1\":" + String(rs.formatting1) + ",";
    body += "\"renderOpt1\":" + String(rs.renderOpt1) + ",";
    body += "\"spacing\":" + String(rs.spacing);
    body += "}";
    return body;
}

esp_err_t configGet(httpd_req_t* req) {
    return json(req, configJson());
}

esp_err_t configExport(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Type", "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"vink-config.json\"");
    String body = configJson();
    return httpd_resp_send(req, body.c_str(), body.length());
}

esp_err_t configPost(httpd_req_t* req) {
    String body;
    if (!readRequestBody(req, body)) return errJson(req, "400 Bad Request", "Invalid body");

    g_readerText.setLayoutPreset(formU8(body, "layoutPreset", g_readerText.layoutPreset(), 0, 2));
    const bool hasPageMarginLevel = formHasKey(body, "pageMarginLevel");
    if (hasPageMarginLevel) {
        g_readerText.setPageMarginLevel(formU8(body, "pageMarginLevel", g_readerText.pageMarginLevel(), 0, 3));
    }
    const bool hasLineSpacingLevel = formHasKey(body, "lineSpacingLevel");
    if (hasLineSpacingLevel) {
        g_readerText.setLineSpacingLevel(formU8(body, "lineSpacingLevel", g_readerText.lineSpacingLevel(), 0, 3));
    }
    g_readerText.setWebLayout(
        formU8(body, "fontSize", g_readerText.readerFontSizeSetting(), 16, 32),
        formU8(body, "lineSpacing", g_readerText.webLineSpacing(), 0, 200),
        formU8(body, "paragraphSpacing", g_readerText.webParagraphSpacing(), 0, 100),
        formU8(body, "indentFirstLine", g_readerText.webIndentFirstLine(), 0, 4),
        formU8(body, "marginLeft", g_readerText.webMarginLeft(), 0, 120),
        formU8(body, "marginRight", g_readerText.webMarginRight(), 0, 120),
        formU8(body, "marginTop", g_readerText.webMarginTop(), 0, 160),
        formU8(body, "marginBottom", g_readerText.webMarginBottom(), 0, 160),
        formBool(body, "justify", g_readerText.webJustify()));
    g_readerText.setAntiAlias(formBool(body, "antiAlias", g_readerText.antiAliasEnabled()));
    g_readerText.setUnderline(formBool(body, "underline", g_readerText.underlineEnabled()));
    g_readerText.setPageTurnEffect(formBool(body, "pageTurnEffect", g_readerText.pageTurnEffectEnabled()));
    g_readerText.setDoubleTapUnlock(formBool(body, "doubleTapUnlock", g_readerText.doubleTapUnlockEnabled()));
    const uint8_t refresh = formU8(body, "refreshStrategy", static_cast<uint8_t>(g_displayService.readerRefreshStrategy()), 0, 2);
    g_displayService.setReaderRefreshStrategy(static_cast<ReaderRefreshStrategy>(refresh));
    const uint8_t pageTurnProfile = formU8(body, "pageTurnProfile", static_cast<uint8_t>(g_displayService.readerPageTurnProfile()), 0, 2);
    g_displayService.setReaderPageTurnProfile(static_cast<ReaderPageTurnProfile>(pageTurnProfile));
    String ssid;
    if (formValue(body, "wifiSsid", ssid)) {
        String password;
        String* passwordPtr = formValue(body, "wifiPassword", password) && password.length() > 0 ? &password : nullptr;
        saveLocalWifi(ssid, passwordPtr);
    }
    g_readerBook.invalidatePaginationForLayoutChange();

    String resp = "{\"saved\":true,\"config\":" + configJson() + "}";
    return json(req, resp);
}

esp_err_t systemInfo(httpd_req_t* req) {
    if (!ensureSd(req)) return ESP_OK;
    char body[320];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"ip\":\"192.168.4.1\",\"freeHeap\":%u,\"freePsram\":%u,\"flashSize\":%u,\"sdTotal\":%llu,\"sdUsed\":%llu,\"uptimeSeconds\":%lu}",
             kVinkPaperS3FirmwareVersion,
             ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getFlashChipSize(),
             static_cast<unsigned long long>(SD.totalBytes()),
             static_cast<unsigned long long>(SD.usedBytes()),
             static_cast<unsigned long>(millis() / 1000));
    return json(req, body);
}

esp_err_t optionsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, nullptr, 0);
}

} // namespace

int WebUiService::registerHandlers(httpd_handle_t httpd) {
    if (!httpd) return ESP_FAIL;
    const httpd_uri_t routes[] = {
        { "/", HTTP_GET, rootHandler, nullptr },
        { "/index.html", HTTP_GET, rootHandler, nullptr },
        { "/api/files", HTTP_GET, filesGet, nullptr },
        { "/api/files", HTTP_POST, mkdirHandler, nullptr },
        { "/api/files/rename", HTTP_POST, renameHandler, nullptr },
        { "/api/config", HTTP_GET, configGet, nullptr },
        { "/api/config/export", HTTP_GET, configExport, nullptr },
        { "/api/config", HTTP_POST, configPost, nullptr },
        { "/api/files/*", HTTP_GET, fileDownload, nullptr },
        { "/api/files/*", HTTP_PUT, filePut, nullptr },
        { "/api/files/*", HTTP_DELETE, fileDelete, nullptr },
        { "/api/system/info", HTTP_GET, systemInfo, nullptr },
        { "/*", HTTP_OPTIONS, optionsHandler, nullptr },
    };
    for (const auto& route : routes) {
        esp_err_t err = httpd_register_uri_handler(httpd, &route);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

void WebUiService::unregisterHandlers(httpd_handle_t httpd) {
    if (!httpd) return;
    httpd_unregister_uri_handler(httpd, "/", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/index.html", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/api/files", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/api/files", HTTP_POST);
    httpd_unregister_uri_handler(httpd, "/api/files/rename", HTTP_POST);
    httpd_unregister_uri_handler(httpd, "/api/config", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/api/config/export", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/api/config", HTTP_POST);
    httpd_unregister_uri_handler(httpd, "/api/files/*", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/api/files/*", HTTP_PUT);
    httpd_unregister_uri_handler(httpd, "/api/files/*", HTTP_DELETE);
    httpd_unregister_uri_handler(httpd, "/api/system/info", HTTP_GET);
    httpd_unregister_uri_handler(httpd, "/*", HTTP_OPTIONS);
}

} // namespace vink3
