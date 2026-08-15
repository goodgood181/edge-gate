// 文件路径: src/net/http_server.cpp
// 职责: 极简 HTTP/1.1 服务器实现 —— 内置 Web 监控页(浏览器可打开)。
#include "http_server.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace es {

namespace {

// 本地帮助函数(与 cmd_server.cpp 同约定,零依赖): steady 毫秒时间戳
uint64_t nowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 内置监控页(自包含 HTML+CSS+JS,Canvas 曲线;1s 轮询 /api/snapshot)
const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EdgeGate 监控</title>
<style>
  body{background:#0f172a;color:#e2e8f0;font-family:'Segoe UI','Microsoft YaHei',sans-serif;margin:0;padding:20px}
  h1{font-size:20px;margin:0 0 4px}
  #dev{color:#38bdf8;font-size:14px}
  #status{color:#94a3b8;font-size:13px;margin-bottom:12px}
  .grid{display:grid;grid-template-columns:2fr 3fr;gap:14px}
  @media (max-width:800px){.grid{grid-template-columns:1fr}}
  table{border-collapse:collapse;font-size:13px;width:100%;background:#0b1220}
  th,td{border:1px solid #334155;padding:6px 8px;text-align:left}
  th{background:#1e293b;font-weight:600}
  td.g{color:#2ecc71} td.s{color:#f1c40f} td.b{color:#e74c3c}
  canvas{background:#161a24;border:1px solid #334155;border-radius:8px;width:100%}
</style>
</head>
<body>
<h1>EdgeGate 监控 <span id="dev"></span></h1>
<div id="status">连接中...</div>
<div class="grid">
  <div><table><thead><tr><th>ID</th><th>名称</th><th>值</th><th>单位</th><th>质量</th></tr></thead><tbody id="pts"></tbody></table></div>
  <div><canvas id="curves" width="720" height="360"></canvas></div>
</div>
<script>
var series = {};
var COLORS = ['#e74c3c','#3498db','#2ecc71','#f1c40f','#9b59b6','#1abc9c'];
async function refresh() {
  try {
    var r = await fetch('/api/snapshot');
    var d = await r.json();
    document.getElementById('dev').textContent = d.device || '';
    document.getElementById('status').textContent =
      '状态: ' + d.state + ' | MQTT: ' + (d.mqttConnected ? '已连接' : '未连接') +
      ' | tx=' + d.stats.tx + ' rx=' + d.stats.rx + ' timeout=' + d.stats.timeouts;
    var tbody = document.getElementById('pts');
    tbody.innerHTML = '';
    var now = Date.now();
    d.points.forEach(function(p, i) {
      var tr = document.createElement('tr');
      var qc = p.quality === 'good' ? 'g' : (p.quality === 'stale' ? 's' : 'b');
      tr.innerHTML = '<td>' + p.id + '</td><td>' + p.name + '</td><td>' +
        Number(p.value).toFixed(3) + '</td><td>' + p.unit + '</td><td class="' + qc + '">' +
        p.quality + '</td>';
      tbody.appendChild(tr);
      if (!series[p.id]) { series[p.id] = { color: COLORS[i % COLORS.length], pts: [] }; }
      if (p.quality !== 'bad') {
        series[p.id].pts.push({ x: now, y: p.value });
        if (series[p.id].pts.length > 300) series[p.id].pts.shift();
      }
    });
    draw(d);
  } catch (e) { /* 网关未就绪时静默重试 */ }
}
function draw(d) {
  var c = document.getElementById('curves');
  var ctx = c.getContext('2d');
  var W = c.width, H = c.height, L = 46, T = 20, B = 14;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#161a24'; ctx.fillRect(0, 0, W, H);
  // 量程
  var yMin = Infinity, yMax = -Infinity;
  for (var k in series) for (var i = 0; i < series[k].pts.length; ++i) {
    var v = series[k].pts[i].y;
    if (v < yMin) yMin = v; if (v > yMax) yMax = v;
  }
  if (!isFinite(yMin)) { yMin = 0; yMax = 1; }
  if (yMax - yMin < 1e-9) { yMax += 1; yMin -= 1; }
  var pad = (yMax - yMin) * 0.1; yMin -= pad; yMax += pad;
  // 网格 + 刻度
  ctx.strokeStyle = '#2a303e'; ctx.fillStyle = '#8a93a6'; ctx.font = '11px sans-serif';
  for (var i2 = 0; i2 <= 4; ++i2) {
    var gy = T + (H - T - B) * i2 / 4;
    ctx.beginPath(); ctx.moveTo(L, gy); ctx.lineTo(W, gy); ctx.stroke();
    ctx.fillText((yMax - (yMax - yMin) * i2 / 4).toFixed(1), 2, gy + 3);
  }
  // 折线
  for (var k2 in series) {
    var s = series[k2];
    if (s.pts.length < 2) continue;
    var x0 = s.pts[0].x, x1 = s.pts[s.pts.length - 1].x;
    var xspan = (x1 - x0) || 1;
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.6; ctx.beginPath();
    for (var j = 0; j < s.pts.length; ++j) {
      var fx = L + (s.pts[j].x - x0) / xspan * (W - L);
      var fy = (H - B) - (s.pts[j].y - yMin) / (yMax - yMin) * (H - T - B);
      if (j === 0) ctx.moveTo(fx, fy); else ctx.lineTo(fx, fy);
    }
    ctx.stroke();
  }
  // 图例
  var lx = L; ctx.font = '12px sans-serif';
  for (var k3 in series) {
    ctx.fillStyle = series[k3].color; ctx.fillText(k3, lx, 12); lx += ctx.measureText(k3).width + 24;
  }
}
setInterval(refresh, 1000);
refresh();
</script>
</body>
</html>)HTML";

const char* kNotFound = "404 Not Found";
const char* kBadRequest = "400 Bad Request";

bool setNonBlocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

} // namespace

bool HttpServer::start(uint16_t port, SnapshotProvider provider, std::string* err) {
    if (m_running.load()) {
        if (err) *err = "already running";
        return false;
    }
    if (!provider) {
        if (err) *err = "provider is null";
        return false;
    }
    m_provider = std::move(provider);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // 只绑回环: Web 页只读、无鉴权,不暴露到局域网(安全考虑,与 cmd_server 一致)
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        std::string e = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        if (err) *err = e;
        return false;
    }
    if (::listen(fd, 8) != 0) {
        std::string e = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        if (err) *err = e;
        return false;
    }
    setNonBlocking(fd);
    m_listener = fd;
    struct sockaddr_in got;
    socklen_t glen = sizeof(got);
    if (::getsockname(fd, (struct sockaddr*)&got, &glen) == 0) {
        m_port = ntohs(got.sin_port);
    } else {
        m_port = port;
    }
    if (::pipe(m_wakePipe) != 0) {
        ::close(fd);
        m_listener = -1;
        if (err) *err = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    setNonBlocking(m_wakePipe[0]);
    setNonBlocking(m_wakePipe[1]);
    m_running = true;
    m_thread = std::thread([this] { serverLoop(); });
    return true;
}

void HttpServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_wakePipe[1] >= 0) {
        const char c = 'x';
        const ssize_t wr = ::write(m_wakePipe[1], &c, 1);
        (void)wr;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_listener >= 0) {
        ::close(m_listener);
        m_listener = -1;
    }
    if (m_wakePipe[0] >= 0) {
        ::close(m_wakePipe[0]);
        m_wakePipe[0] = -1;
    }
    if (m_wakePipe[1] >= 0) {
        ::close(m_wakePipe[1]);
        m_wakePipe[1] = -1;
    }
}

uint16_t HttpServer::port() const { return m_port; }
bool HttpServer::running() const { return m_running.load(); }

void HttpServer::serverLoop() {
    while (m_running.load()) {
        // 重建 pollfd 集合(listener + 唤醒管道 + 客户端)
        struct pollfd fds[2 + kMaxClients];
        int nfds = 0;
        fds[nfds].fd = m_listener; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds;
        fds[nfds].fd = m_wakePipe[0]; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds;
        for (size_t i = 0; i < m_clients.size(); ++i) {
            fds[nfds].fd = m_clients[i].fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }
        int rc = ::poll(fds, nfds, kPollCapMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) {
            // 空闲超时检查
            const uint64_t now = nowMs();
            for (size_t i = 0; i < m_clients.size();) {
                if (now - m_clients[i].lastActivityMs >= kIdleTimeoutMs) {
                    closeClient(i);
                    continue;
                }
                ++i;
            }
            continue;
        }
        // 唤醒管道(停止)
        if (fds[1].revents & POLLIN) {
            char buf[16];
            while (::read(m_wakePipe[0], buf, sizeof(buf)) > 0) {}
            if (!m_running.load()) break;
        }
        // 新连接
        if (fds[0].revents & POLLIN) {
            while (static_cast<int>(m_clients.size()) < kMaxClients) {
                struct sockaddr_in ca;
                socklen_t clen = sizeof(ca);
                int cfd = ::accept(m_listener, (struct sockaddr*)&ca, &clen);
                if (cfd < 0) break;
                setNonBlocking(cfd);
                Client c;
                c.fd = cfd;
                c.lastActivityMs = nowMs();
                m_clients.push_back(c);
                ++m_activeClients;
            }
            if (m_clients.empty()) continue; // accept 完继续循环
        }
        // 客户端数据
        for (size_t i = 0; i < m_clients.size();) {
            const int idx = static_cast<int>(i);
            const bool hasData = (fds[2 + idx].revents & POLLIN) != 0;
            const bool hasErr = (fds[2 + idx].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (hasData) {
                char buf[2048];
                const ssize_t n = ::recv(m_clients[idx].fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    m_clients[idx].lastActivityMs = nowMs();
                    m_clients[idx].rxBuf.append(buf, static_cast<size_t>(n));
                    if (m_clients[idx].rxBuf.size() > kMaxRequest) {
                        (void)sendAll(m_clients[idx].fd, kBadRequest);
                        closeClient(i);
                        continue;
                    }
                    // 请求头完整(\r\n\r\n)则处理
                    if (m_clients[idx].rxBuf.find("\r\n\r\n") != std::string::npos) {
                        handleClient(i);
                    }
                    ++i;
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    closeClient(i);
                } else {
                    ++i;
                }
            } else if (hasErr) {
                closeClient(i);
            } else {
                ++i;
            }
        }
    }
    // 退出清理
    for (auto& c : m_clients) {
        ::close(c.fd);
        --m_activeClients;
    }
    m_clients.clear();
}

void HttpServer::handleClient(size_t idx) {
    Client& c = m_clients[idx];
    // 只处理 GET;解析请求行 "GET /path HTTP/1.1"
    std::string path;
    if (c.rxBuf.compare(0, 4, "GET ") == 0) {
        const size_t sp = c.rxBuf.find(' ', 4);
        if (sp != std::string::npos) {
            path = c.rxBuf.substr(4, sp - 4);
        }
    }
    std::string body;
    std::string ctype;
    if (path == "/" || path == "/index.html") {
        body = kIndexHtml;
        ctype = "text/html; charset=utf-8";
    } else if (path == "/api/snapshot") {
        body = m_provider ? m_provider() : "{}";
        ctype = "application/json; charset=utf-8";
    } else {
        body = kNotFound;
        ctype = "text/plain; charset=utf-8";
    }
    std::string resp = "HTTP/1.1 200 OK\r\n";
    if (body == kNotFound) {
        resp = "HTTP/1.1 404 Not Found\r\n";
    }
    char hdr[128];
    std::snprintf(hdr, sizeof(hdr), "Content-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                  body.size(), ctype.c_str());
    resp += hdr;
    resp += body;
    (void)sendAll(c.fd, resp);
    closeClient(idx);
}

void HttpServer::closeClient(size_t idx) {
    if (idx < m_clients.size()) {
        ::close(m_clients[idx].fd);
        --m_activeClients;
        m_clients.erase(m_clients.begin() + static_cast<ptrdiff_t>(idx));
    }
}

bool HttpServer::sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false; // EAGAIN 等: 客户端读得慢,直接放弃(Connection: close 语义)
    }
    return true;
}

} // namespace es
