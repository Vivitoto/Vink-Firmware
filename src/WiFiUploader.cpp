#include "WiFiUploader.h"
#include <ElegantOTA.h>
#include <SPIFFS.h>

// Web 文件管理页面（存储在 PROGMEM 中）
static const char FILE_MANAGER_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Vink-PaperS3 文件管理</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--text:#1f2937;--muted:#6b7280;--line:#e5e7eb;--primary:#2563eb;--danger:#dc2626;--ok:#16a34a;--ink:#111827}
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:var(--bg);color:var(--text)}
.wrap{max-width:980px;margin:0 auto;padding:18px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}h1{font-size:24px;margin:12px 0}.card{background:var(--card);border-radius:16px;padding:18px;margin:14px 0;box-shadow:0 4px 18px rgba(15,23,42,.08)}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}.stat{background:#f8fafc;border:1px solid var(--line);border-radius:14px;padding:14px}.stat b{display:block;font-size:20px;color:var(--ink);margin-top:6px}.nav{display:flex;gap:8px;flex-wrap:wrap}.nav a{text-decoration:none;border-radius:999px;padding:9px 12px;background:#eef2ff;color:#3730a3}
.breadcrumb{display:flex;flex-wrap:wrap;gap:6px;align-items:center;font-size:14px;color:var(--muted)}.crumb{border:0;background:#eef2ff;color:#3730a3;border-radius:999px;padding:7px 10px;cursor:pointer}.crumb:hover{background:#e0e7ff}
.toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}.btn{border:0;border-radius:10px;padding:10px 13px;font-size:15px;cursor:pointer;background:#111827;color:#fff}.btn.primary{background:var(--primary)}.btn.danger{background:var(--danger)}.btn.light{background:#e5e7eb;color:#111827}.btn:disabled{opacity:.5;cursor:not-allowed}
.drop{border:2px dashed #cbd5e1;border-radius:14px;padding:20px;text-align:center;color:var(--muted);margin-top:14px}.drop.drag{border-color:var(--primary);background:#eff6ff}input[type=file]{display:none}
.status{min-height:22px;margin-top:12px;font-size:14px}.ok{color:var(--ok)}.err{color:var(--danger)}
.list{overflow:hidden;border:1px solid var(--line);border-radius:14px}.row{display:grid;grid-template-columns:minmax(0,1fr) 105px 82px;gap:10px;align-items:center;padding:12px 14px;border-bottom:1px solid var(--line)}.row:last-child{border-bottom:0}.name{display:flex;align-items:center;gap:10px;min-width:0}.name a,.folder{color:var(--text);text-decoration:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.folder{border:0;background:transparent;font:inherit;cursor:pointer;padding:0;text-align:left}.folder:hover,.name a:hover{color:var(--primary)}.size{color:var(--muted);font-size:13px;text-align:right}.actions{text-align:right}.empty{padding:28px;text-align:center;color:var(--muted)}
@media(max-width:620px){.wrap{padding:10px}h1{font-size:21px}.card{padding:14px;border-radius:12px}.row{grid-template-columns:1fr;gap:8px}.size,.actions{text-align:left}.toolbar .btn{flex:1}.top{display:block}.grid{grid-template-columns:1fr 1fr}}
</style>
</head>
<body>
<div class="wrap">
  <div class="top"><h1>📚 Vink-PaperS3 管理中心</h1><div class="nav"><a href="/update">固件OTA</a><a href="#files" onclick="refreshList()">文件</a></div></div>
  <div class="card"><div id="stats" class="grid"><div class="stat">设备IP<b>加载中</b></div><div class="stat">固件空间<b>-</b></div><div class="stat">SPIFFS资源<b>-</b></div><div class="stat">运行内存<b>-</b></div></div></div>
  <div id="files" class="card">
    <div id="breadcrumb" class="breadcrumb"></div>
    <div class="toolbar">
      <label class="btn primary" for="fileInput">上传文件</label>
      <input id="fileInput" type="file" onchange="uploadSelected()">
      <button class="btn light" onclick="newFolder()">新建文件夹</button>
      <button class="btn light" onclick="goUp()">返回上级</button>
    </div>
    <div id="drop" class="drop">拖拽 .txt / .epub 文件到这里上传，或点击“上传文件”</div>
    <div id="status" class="status"></div>
  </div>
  <div class="card"><div id="fileList" class="list"><div class="empty">加载中...</div></div></div>
</div>
<script>
let currentPath = '/books';
const $ = id => document.getElementById(id);
function enc(s){return encodeURIComponent(s)}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function joinPath(base,name){return (base==='/'?'':base)+'/'+name}
function parentPath(p){if(!p||p==='/')return '/'; const i=p.lastIndexOf('/'); return i<=0?'/':p.slice(0,i)}
function fmtSize(n){if(n===0)return '0 B'; const u=['B','KB','MB','GB']; let i=0,v=n; while(v>=1024&&i<u.length-1){v/=1024;i++} return (i? v.toFixed(1):v)+' '+u[i]}
async function loadStatus(){try{const s=await (await fetch('/api/status')).json();$('stats').innerHTML='<div class="stat">设备IP<b>'+esc(s.ip)+'</b></div><div class="stat">固件空间<b>'+fmtSize(s.freeSketch)+'</b></div><div class="stat">SPIFFS资源<b>'+fmtSize(s.spiffsUsed)+' / '+fmtSize(s.spiffsTotal)+'</b></div><div class="stat">运行内存<b>'+fmtSize(s.freeHeap)+'</b></div>'}catch(e){}}
function setStatus(msg, cls=''){ $('status').className='status '+cls; $('status').textContent=msg||'' }
function renderBreadcrumb(){
  const parts=currentPath.split('/').filter(Boolean); let html='<button class="crumb" onclick="openPath(\'/\')">根目录</button>'; let p='';
  parts.forEach(part=>{p+='/'+part; html+=' / <button class="crumb" onclick="openPath(\''+esc(p)+'\')">'+esc(part)+'</button>'});
  $('breadcrumb').innerHTML=html;
}
async function loadList(){
  renderBreadcrumb(); setStatus(''); $('fileList').innerHTML='<div class="empty">加载中...</div>';
  try{
    const resp=await fetch('/api/list?path='+enc(currentPath));
    if(!resp.ok) throw new Error(await resp.text());
    const items=await resp.json();
    items.sort((a,b)=>(b.isDir-a.isDir)||a.name.localeCompare(b.name));
    if(!items.length){$('fileList').innerHTML='<div class="empty">当前目录为空</div>';return;}
    $('fileList').innerHTML=items.map(it=>{
      const full=joinPath(currentPath,it.name); const icon=it.isDir?'📁':'📄';
      const link=it.isDir?'<button class="folder" onclick="openPath(\''+esc(full)+'\')">'+esc(it.name)+'</button>':'<a href="/download?path='+enc(full)+'">'+esc(it.name)+'</a>';
      return '<div class="row"><div class="name"><span>'+icon+'</span>'+link+'</div><div class="size">'+(it.isDir?'文件夹':fmtSize(it.size))+'</div><div class="actions"><button class="btn danger" onclick="deleteItem(\''+esc(full)+'\')">删除</button></div></div>';
    }).join('');
  }catch(e){$('fileList').innerHTML='<div class="empty">加载失败</div>'; setStatus('加载失败: '+e.message,'err')}
}
function openPath(p){currentPath=p||'/books'; loadList()}
function goUp(){openPath(parentPath(currentPath))}
function refreshList(){loadList()}
async function uploadFile(file){
  if(!file)return; const fd=new FormData(); fd.append('file',file); setStatus('上传中: '+file.name);
  try{const resp=await fetch('/upload?path='+enc(currentPath),{method:'POST',body:fd}); const text=await resp.text(); if(!resp.ok)throw new Error(text); setStatus(text,'ok'); loadList()}catch(e){setStatus('上传失败: '+e.message,'err')}
}
function uploadSelected(){uploadFile($('fileInput').files[0]); $('fileInput').value=''}
async function deleteItem(path){
  if(!confirm('确定删除？\n'+path))return;
  try{const resp=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+enc(path)}); const text=await resp.text(); if(!resp.ok)throw new Error(text); setStatus(text,'ok'); loadList()}catch(e){setStatus('删除失败: '+e.message,'err')}
}
async function newFolder(){
  const name=prompt('文件夹名称'); if(!name)return; const path=joinPath(currentPath,name.replace(/[\\/]/g,'').trim());
  try{const resp=await fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+enc(path)}); const text=await resp.text(); if(!resp.ok)throw new Error(text); setStatus(text,'ok'); loadList()}catch(e){setStatus('创建失败: '+e.message,'err')}
}
const drop=$('drop');
['dragenter','dragover'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add('drag')}));
['dragleave','drop'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove('drag')}));
drop.addEventListener('drop',e=>{const f=e.dataTransfer.files[0]; if(f)uploadFile(f)});
loadStatus();
loadList();
</script>
</body>
</html>
)=====";

WebFileManager::WebFileManager() : _server(nullptr), _running(false), _newUpload(false), _uploadOk(false) {
}

bool WebFileManager::start(const char* ssid, const char* password) {
    if (_running) return true;
    
    Serial.printf("[WiFi] Connecting to %s...\n", ssid);
    WiFi.begin(ssid, password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        retries++;
        Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection failed");
        WiFi.disconnect();
        return false;
    }
    
    Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    if (!SPIFFS.begin(true)) {
        Serial.println("[FS] SPIFFS mount failed");
    }
    
    _server = new WebServer(8080);
    _server->on("/", HTTP_GET, [this]() { handleRoot(); });
    _server->on("/upload", HTTP_POST, [this]() {
        if (_uploadOk && _uploadError.length() == 0) {
            _server->send(200, "text/plain", "上传成功: " + _lastUploadName);
        } else {
            String error = _uploadError.length() ? _uploadError : String("上传失败");
            _server->send(400, "text/plain", error);
        }
        _uploadOk = false;
        _uploadError = "";
    }, [this]() { handleUpload(); });
    _server->on("/list", HTTP_GET, [this]() { handleList(); });
    _server->on("/api/list", HTTP_GET, [this]() { handleApiList(); });
    _server->on("/api/delete", HTTP_POST, [this]() { handleApiDelete(); });
    _server->on("/api/mkdir", HTTP_POST, [this]() { handleApiMkdir(); });
    _server->on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
    _server->on("/download", HTTP_GET, [this]() { handleDownload(); });
    ElegantOTA.begin(_server);
    _server->onNotFound([this]() { handleNotFound(); });
    _server->begin();
    
    _running = true;
    Serial.println("[WiFi] HTTP server started on port 8080");
    return true;
}

void WebFileManager::stop() {
    if (!_running) return;
    _server->stop();
    delete _server;
    _server = nullptr;
    WiFi.disconnect();
    _running = false;
    Serial.println("[WiFi] Server stopped");
}

String WebFileManager::getIP() const {
    if (!_running) return "";
    return WiFi.localIP().toString();
}

void WebFileManager::handleClient() {
    if (_server) {
        _server->handleClient();
        ElegantOTA.loop();
    }
}

void WebFileManager::handleRoot() {
    _server->send_P(200, "text/html", FILE_MANAGER_PAGE);
}

String WebFileManager::normalizePath(const String& rawPath) const {
    String path = rawPath.length() ? rawPath : String(BOOKS_DIR);
    path.trim();
    if (!path.startsWith("/")) path = "/" + path;
    while (path.indexOf("//") >= 0) path.replace("//", "/");
    if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    if (path.indexOf("..") >= 0) return "";
    return path;
}

String WebFileManager::jsonEscape(const String& value) const {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

bool WebFileManager::deleteRecursive(const String& path) {
    File target = SD.open(path.c_str());
    if (!target) return false;
    bool isDir = target.isDirectory();
    if (!isDir) {
        target.close();
        return SD.remove(path.c_str());
    }

    File child = target.openNextFile();
    while (child) {
        String childPath = String(child.name());
        if (!childPath.startsWith("/")) childPath = path + "/" + childPath;
        bool childIsDir = child.isDirectory();
        child.close();
        if (childIsDir) {
            if (!deleteRecursive(childPath)) {
                target.close();
                return false;
            }
        } else if (!SD.remove(childPath.c_str())) {
            target.close();
            return false;
        }
        child = target.openNextFile();
    }
    target.close();
    return SD.rmdir(path.c_str());
}

void WebFileManager::handleUpload() {
    HTTPUpload& upload = _server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        _uploadOk = false;
        _uploadError = "";
        String filename = upload.filename;
        int slash = filename.lastIndexOf('/');
        int backslash = filename.lastIndexOf('\\');
        int cut = max(slash, backslash);
        if (cut >= 0) filename = filename.substring(cut + 1);
        String lower = filename;
        lower.toLowerCase();
        if (!(lower.endsWith(".txt") || lower.endsWith(".epub"))) {
            _uploadError = "只支持 .txt / .epub 文件";
            return;
        }

        String dir = normalizePath(_server->arg("path"));
        if (dir.length() == 0) dir = BOOKS_DIR;
        File dirFile = SD.open(dir.c_str());
        if (!dirFile || !dirFile.isDirectory()) dir = BOOKS_DIR;
        if (dirFile) dirFile.close();
        
        String path = dir + "/" + filename;
        Serial.printf("[WiFi] Upload start: %s\n", path.c_str());
        
        if (SD.exists(path.c_str())) {
            SD.remove(path.c_str());
        }
        
        _uploadFile = SD.open(path.c_str(), FILE_WRITE);
        if (!_uploadFile) {
            _uploadError = "无法创建文件";
            return;
        }
        
        _uploadOk = true;
        _uploadPath = path;
        _lastUploadName = filename;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_uploadOk && _uploadFile) {
            _uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (_uploadFile) {
            _uploadFile.close();
        }
        if (_uploadOk) {
            _newUpload = true;
            Serial.printf("[WiFi] Upload complete: %s (%d bytes)\n", _uploadPath.c_str(), upload.totalSize);
        }
    }
}

void WebFileManager::handleList() {
    String json = "[";
    File root = SD.open(BOOKS_DIR);
    if (root) {
        File file = root.openNextFile();
        bool first = true;
        while (file) {
            String name = file.name();
            if (!file.isDirectory() && (name.endsWith(".txt") || name.endsWith(".epub"))) {
                int slash = name.lastIndexOf('/');
                if (slash >= 0) name = name.substring(slash + 1);
                if (!first) json += ",";
                first = false;
                json += "\"" + jsonEscape(name) + "\"";
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    }
    json += "]";
    _server->send(200, "application/json", json);
}

void WebFileManager::handleApiList() {
    String path = normalizePath(_server->arg("path"));
    if (path.length() == 0) {
        _server->send(400, "text/plain", "Invalid path");
        return;
    }

    File root = SD.open(path.c_str());
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        _server->send(404, "text/plain", "Directory not found");
        return;
    }

    String json = "[";
    bool first = true;
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.length() > 0) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((uint32_t)file.size()) + ",\"isDir\":" + String(file.isDirectory() ? "true" : "false") + "}";
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    json += "]";
    _server->send(200, "application/json", json);
}

void WebFileManager::handleApiDelete() {
    String path = normalizePath(_server->arg("path"));
    if (path.length() == 0 || path == "/" || path == String(BOOKS_DIR)) {
        _server->send(400, "text/plain", "Invalid path");
        return;
    }
    if (!SD.exists(path.c_str())) {
        _server->send(404, "text/plain", "Path not found");
        return;
    }
    if (!deleteRecursive(path)) {
        _server->send(500, "text/plain", "删除失败");
        return;
    }
    _server->send(200, "text/plain", "删除成功");
}

void WebFileManager::handleApiMkdir() {
    String path = normalizePath(_server->arg("path"));
    if (path.length() == 0 || path == "/") {
        _server->send(400, "text/plain", "Invalid path");
        return;
    }
    if (SD.exists(path.c_str())) {
        _server->send(409, "text/plain", "目录已存在");
        return;
    }
    if (!SD.mkdir(path.c_str())) {
        _server->send(500, "text/plain", "创建目录失败");
        return;
    }
    _server->send(200, "text/plain", "目录已创建");
}

void WebFileManager::handleApiStatus() {
    size_t spiffsTotal = 0;
    size_t spiffsUsed = 0;
    if (SPIFFS.begin(true)) {
        spiffsTotal = SPIFFS.totalBytes();
        spiffsUsed = SPIFFS.usedBytes();
    }

    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"sketchSize\":" + String(ESP.getSketchSize()) + ",";
    json += "\"freeSketch\":" + String(ESP.getFreeSketchSpace()) + ",";
    json += "\"spiffsTotal\":" + String(spiffsTotal) + ",";
    json += "\"spiffsUsed\":" + String(spiffsUsed);
    json += "}";
    _server->send(200, "application/json", json);
}

void WebFileManager::handleDownload() {
    String path = normalizePath(_server->arg("path"));
    if (path.length() == 0 || path == "/") {
        _server->send(400, "text/plain", "Invalid path");
        return;
    }
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        _server->send(404, "text/plain", "File not found");
        return;
    }
    String name = path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    _server->sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    _server->streamFile(file, "application/octet-stream");
    file.close();
}

void WebFileManager::handleNotFound() {
    _server->send(404, "text/plain", "Not Found");
}
