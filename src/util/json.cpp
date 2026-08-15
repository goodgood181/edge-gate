// 文件路径: src/util/json.cpp
// 职责: json.h 的实现 —— 手写递归下降 JSON DOM 解析器与序列化器。
// 解析器实现要点(设计总述见 json.h 文件头):
// 1) 逐字符推进 + pos/line/col 三态,任何语法错误都在"发生地"记录,消息含偏移与行列。
// 2) 深度上限 512: 递归下降天然怕深层嵌套(栈溢出),进入对象/数组时计数,超限即报错。
// 3) 数字解析不用 locale 相关函数(isdigit/atof 随 locale 漂移),手工判定字符集 +
//    strtoll/strtod;整数溢出(如 9223372036854775808)自动落为 double;非有限值报错。
// 4) \uXXXX 按 UTF-8 编码输出,正确处理 UTF-16 代理对;孤立/错配代理直接报错不静默吞。
// 5) Json(int) 便捷重载存在的原因: 32 位平台(ARM Cortex-A7)上 int64_t=long long,
//    int 字面量在 int64_t/double/bool 三个构造函数间同秩歧义,显式加 int 重载消除;
//    内部仍存 int64_t,不改变数值语义。
#include "json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace es {

namespace {

// ---------- 序列化小工具 ----------

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// 将 Unicode 码点按 UTF-8 编码追加到字符串(1~4 字节)
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 字符串序列化转义(解析转义的逆过程)
std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { // 其余控制字符 → \u00XX
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c)); // 非 ASCII 原样输出(UTF-8 透传)
                }
                break;
        }
    }
    return out;
}

// double → JSON 数字文本: 整数值走定点(避免 100.0 输出 "1e+02"),
// 其余取"最短可往返"表示(1~17 位有效数字试凑,strtod 还原一致即采用),精度不丢
std::string formatDouble(double v) {
    if (!std::isfinite(v)) {
        return "null"; // JSON 无 NaN/Infinity,防御性输出 null(注释说明取舍)
    }
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) {
            return buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "%.17g", v); // 兜底(理论不可达)
    return buf;
}

// 按需换行缩进(pretty 模式,2 空格/层)
void newlineIndent(std::string& out, int depth, bool pretty) {
    if (!pretty) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

// ---------- 递归下降解析器 ----------

constexpr int kMaxDepth = 512; // 嵌套深度上限,防恶意/误写深层嵌套导致栈溢出

class Parser {
public:
    explicit Parser(const std::string& text)
        : m_text(text) {}

    // 解析整个文档;失败返回 Null,err 含定位信息;成功清空 err
    Json run(std::string* err) {
        Json v = parseValue();
        if (!m_ok) {
            if (err) {
                *err = m_errMsg;
            }
            return Json();
        }
        skipWs();
        if (m_pos != m_text.size()) {
            fail("解析完成后仍有尾随字符");
            if (err) {
                *err = m_errMsg;
            }
            return Json();
        }
        if (err) {
            err->clear();
        }
        return v;
    }

private:
    bool eof() const {
        return m_pos >= m_text.size();
    }

    char peek() const {
        return eof() ? '\0' : m_text[m_pos];
    }

    void advance() {
        if (!eof()) {
            if (m_text[m_pos] == '\n') {
                ++m_line;
                m_col = 1;
            } else {
                ++m_col;
            }
            ++m_pos;
        }
    }

    // 记录错误: 消息含字节偏移与行列号,定位到出错字符
    void fail(const std::string& msg) {
        m_ok = false;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s @ 偏移 %zu (第 %zu 行第 %zu 列)",
                      msg.c_str(), m_pos, m_line, m_col);
        m_errMsg = buf;
    }

    void skipWs() {
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else {
                break;
            }
        }
    }

    bool enterDepth() {
        ++m_depth;
        if (m_depth > kMaxDepth) {
            fail("嵌套深度超过上限(512)");
            return false;
        }
        return true;
    }

    void leaveDepth() {
        --m_depth;
    }

    Json parseValue() {
        skipWs();
        if (eof()) {
            fail("输入意外结束(期望一个 JSON 值)");
            return Json();
        }
        const char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return Json(parseString());
            case 't': expectWord("true");  return Json(true);
            case 'f': expectWord("false"); return Json(false);
            case 'n': expectWord("null");  return Json(nullptr);
            default:
                if (c == '-' || isDigit(c)) {
                    return parseNumber();
                }
                fail(std::string("意外的字符 '") + c + "'");
                return Json();
        }
    }

    void expectWord(const char* w) {
        for (const char* p = w; *p; ++p) {
            if (eof() || peek() != *p) {
                fail(std::string("无效字面量,期望 ") + w);
                return;
            }
            advance();
        }
    }

    Json parseObject() {
        if (!enterDepth()) {
            return Json();
        }
        advance(); // '{'
        std::map<std::string, Json> obj;
        skipWs();
        if (peek() == '}') {
            advance();
            leaveDepth();
            return Json(obj);
        }
        while (true) {
            skipWs();
            if (peek() != '"') {
                fail("对象键必须是双引号字符串");
                return Json();
            }
            const std::string key = parseString();
            if (!m_ok) {
                return Json();
            }
            skipWs();
            if (peek() != ':') {
                fail("对象键后缺少 ':'");
                return Json();
            }
            advance();
            Json v = parseValue();
            if (!m_ok) {
                return Json();
            }
            obj[key] = std::move(v); // 重复键: 后者覆盖(与常见实现一致)
            skipWs();
            const char c = peek();
            if (c == ',') {
                advance();
                continue;
            }
            if (c == '}') {
                advance();
                leaveDepth();
                return Json(obj);
            }
            fail("对象中期望 ',' 或 '}'");
            return Json();
        }
    }

    Json parseArray() {
        if (!enterDepth()) {
            return Json();
        }
        advance(); // '['
        std::vector<Json> arr;
        skipWs();
        if (peek() == ']') {
            advance();
            leaveDepth();
            return Json(arr);
        }
        while (true) {
            skipWs();
            Json v = parseValue();
            if (!m_ok) {
                return Json();
            }
            arr.push_back(std::move(v));
            skipWs();
            const char c = peek();
            if (c == ',') {
                advance();
                continue;
            }
            if (c == ']') {
                advance();
                leaveDepth();
                return Json(arr);
            }
            fail("数组中期望 ',' 或 ']'");
            return Json();
        }
    }

    std::string parseString() {
        advance(); // '"'
        std::string out;
        while (true) {
            if (eof()) {
                fail("字符串未闭合");
                return out;
            }
            const char c = peek();
            if (c == '"') {
                advance();
                return out;
            }
            if (c == '\\') {
                advance();
                if (eof()) {
                    fail("转义序列不完整");
                    return out;
                }
                const char e = peek();
                advance();
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        const uint32_t cp = parseHex4();
                        if (!m_ok) {
                            return out;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 高位代理: 必须紧跟 \uXXXX 低位代理(UTF-16 代理对)
                            if (peek() == '\\' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == 'u') {
                                advance();
                                advance();
                                const uint32_t lo = parseHex4();
                                if (!m_ok) {
                                    return out;
                                }
                                if (lo < 0xDC00 || lo > 0xDFFF) {
                                    fail("\\u 低位代理无效(需在 0xDC00~0xDFFF)");
                                    return out;
                                }
                                appendUtf8(out, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
                            } else {
                                fail("高位代理后缺少低位代理 \\uXXXX");
                                return out;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("孤立低位代理 \\uXXXX");
                            return out;
                        } else {
                            appendUtf8(out, cp);
                        }
                        break;
                    }
                    default:
                        fail(std::string("无效转义序列 '\\") + e + "'");
                        return out;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                fail("字符串内出现未转义控制字符");
                return out;
            } else {
                out.push_back(c);
                advance();
            }
        }
    }

    uint32_t parseHex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) {
                fail("\\u 后缺少 4 位十六进制数字");
                return 0;
            }
            const char c = peek();
            uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                fail("\\u 后的字符不是十六进制数字");
                return 0;
            }
            v = v * 16 + d;
            advance();
        }
        return v;
    }

    Json parseNumber() {
        const size_t start = m_pos;
        if (peek() == '-') {
            advance();
        }
        if (eof() || !isDigit(peek())) {
            fail("数字格式错误(如 '-1'、'1.5')");
            return Json();
        }
        if (peek() == '0') {
            advance();
            if (!eof() && isDigit(peek())) {
                fail("数字不允许前导零(如 01)");
                return Json();
            }
        } else {
            while (!eof() && isDigit(peek())) {
                advance();
            }
        }
        bool isFloat = false; // 含小数点或指数 → 必须走 double
        if (!eof() && peek() == '.') {
            isFloat = true;
            advance();
            if (eof() || !isDigit(peek())) {
                fail("小数点后必须有数字(如 1.5)");
                return Json();
            }
            while (!eof() && isDigit(peek())) {
                advance();
            }
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            isFloat = true;
            advance();
            if (!eof() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            if (eof() || !isDigit(peek())) {
                fail("指数部分缺少数字(如 1e5)");
                return Json();
            }
            while (!eof() && isDigit(peek())) {
                advance();
            }
        }
        const std::string s = m_text.substr(start, m_pos - start);
        if (!isFloat) {
            errno = 0;
            char* endp = nullptr;
            const long long v = std::strtoll(s.c_str(), &endp, 10);
            if (endp == s.c_str() + s.size() && errno != ERANGE) {
                return Json(static_cast<int64_t>(v)); // 合法整数 → int64 分支,保精度
            }
            // 溢出(如 9223372036854775808)→ 落为 double(注释说明取舍)
        }
        const double d = std::strtod(s.c_str(), nullptr);
        if (!std::isfinite(d)) {
            fail("数字超出 double 可表示范围");
            return Json();
        }
        return Json(d);
    }

    const std::string& m_text;
    size_t m_pos = 0;
    size_t m_line = 1;
    size_t m_col = 1;
    int m_depth = 0;
    bool m_ok = true;
    std::string m_errMsg;
};

} // namespace

// ---------- 构造 ----------

Json::Json()
    : m_value(nullptr) {}

Json::Json(std::nullptr_t)
    : m_value(nullptr) {}

Json::Json(bool b)
    : m_value(b) {}

Json::Json(double d)
    : m_value(d) {}

Json::Json(int64_t i)
    : m_value(i) {}

Json::Json(int i) {
    // 见文件头注释: 消除 32 位平台整数字面量重载歧义;显式转型避免 variant 内部同秩歧义
    m_value = static_cast<int64_t>(i);
}

Json::Json(const char* s) {
    // 先显式构造 std::string 再赋给 variant: 若直接 m_value(s),variant 的转换构造函数
    // 会优先把 const char* 选成 bool(true) —— std::variant 的经典坑,必须绕开
    m_value = s ? std::string(s) : std::string();
}

Json::Json(const std::string& s)
    : m_value(s) {}

Json::Json(const std::vector<Json>& arr)
    : m_value(arr) {}

Json::Json(const std::map<std::string, Json>& obj)
    : m_value(obj) {}

// ---------- 解析与序列化 ----------

Json Json::parse(const std::string& text, std::string* err) {
    Parser p(text);
    return p.run(err);
}

std::string Json::dump(bool pretty) const {
    std::string out;
    dumpTo(out, 0, pretty);
    return out;
}

void Json::dumpTo(std::string& out, int depth, bool pretty) const {
    switch (type()) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += std::get<bool>(m_value) ? "true" : "false";
            break;
        case Type::Number:
            if (std::holds_alternative<int64_t>(m_value)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::get<int64_t>(m_value)));
                out += buf;
            } else {
                out += formatDouble(std::get<double>(m_value));
            }
            break;
        case Type::String:
            out.push_back('"');
            out += escapeString(std::get<std::string>(m_value));
            out.push_back('"');
            break;
        case Type::Array: {
            const auto& arr = std::get<std::vector<Json>>(m_value);
            if (arr.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                newlineIndent(out, depth + 1, pretty);
                arr[i].dumpTo(out, depth + 1, pretty);
            }
            newlineIndent(out, depth, pretty);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const auto& obj = std::get<std::map<std::string, Json>>(m_value);
            if (obj.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            size_t i = 0;
            for (const auto& kv : obj) {
                if (i++ > 0) {
                    out.push_back(',');
                }
                newlineIndent(out, depth + 1, pretty);
                out.push_back('"');
                out += escapeString(kv.first);
                out += "\":";
                if (pretty) {
                    out.push_back(' ');
                }
                kv.second.dumpTo(out, depth + 1, pretty);
            }
            newlineIndent(out, depth, pretty);
            out.push_back('}');
            break;
        }
    }
}

// ---------- 查询 ----------

Json::Type Json::type() const {
    // 与头文件 Variant 替代类型声明顺序一一对应:
    // 0 nullptr_t,1 bool,2 double,3 int64_t,4 string,5 vector,6 map
    switch (m_value.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2:
        case 3: return Type::Number;
        case 4: return Type::String;
        case 5: return Type::Array;
        default: return Type::Object;
    }
}

bool Json::isNull() const {
    return type() == Type::Null;
}

bool Json::asBool(bool def) const {
    switch (m_value.index()) {
        case 1: return std::get<bool>(m_value);
        case 2: return std::get<double>(m_value) != 0.0;
        case 3: return std::get<int64_t>(m_value) != 0;
        default: return def;
    }
}

double Json::asNumber(double def) const {
    switch (m_value.index()) {
        case 1: return std::get<bool>(m_value) ? 1.0 : 0.0;
        case 2: return std::get<double>(m_value);
        case 3: return static_cast<double>(std::get<int64_t>(m_value));
        default: return def;
    }
}

int64_t Json::asInt(int64_t def) const {
    switch (m_value.index()) {
        case 1: return std::get<bool>(m_value) ? 1 : 0;
        case 2: {
            const double v = std::get<double>(m_value);
            // double→int64 越界是未定义行为,宁可回默认值(禁异常工程原则: 不产生 UB)
            if (v >= -9.2233720368547758e18 && v <= 9.2233720368547758e18) {
                return static_cast<int64_t>(v);
            }
            return def;
        }
        case 3: return std::get<int64_t>(m_value);
        default: return def;
    }
}

const std::string& Json::asString(const std::string& def) const {
    if (type() == Type::String) {
        return std::get<std::string>(m_value);
    }
    return def; // 契约签名返回引用;def 为默认临时值时,引用在调用方的完整表达式内有效
}

bool Json::has(const std::string& key) const {
    return type() == Type::Object &&
           std::get<std::map<std::string, Json>>(m_value).count(key) > 0;
}

Json Json::get(const std::string& key, const Json& def) const {
    if (type() == Type::Object) {
        const auto& obj = std::get<std::map<std::string, Json>>(m_value);
        const auto it = obj.find(key);
        if (it != obj.end()) {
            return it->second;
        }
    }
    return def;
}

// ---------- 构建 ----------

Json& Json::operator[](const std::string& key) {
    if (type() != Type::Object) {
        m_value = std::map<std::string, Json>(); // 非对象自动升级为对象(便利语义,见头文件)
    }
    return std::get<std::map<std::string, Json>>(m_value)[key]; // 键缺失自动插入 Null
}

Json& Json::operator[](size_t i) {
    if (type() != Type::Array) {
        m_value = std::vector<Json>();
    }
    auto& arr = std::get<std::vector<Json>>(m_value);
    while (arr.size() <= i) {
        arr.emplace_back(); // 越界自动补 Null(便于顺序构建数组)
    }
    return arr[i];
}

void Json::pushBack(const Json& v) {
    if (type() != Type::Array) {
        m_value = std::vector<Json>();
    }
    std::get<std::vector<Json>>(m_value).push_back(v);
}

void Json::set(const std::string& key, const Json& v) {
    if (type() != Type::Object) {
        m_value = std::map<std::string, Json>();
    }
    std::get<std::map<std::string, Json>>(m_value)[key] = v;
}

size_t Json::size() const {
    if (type() == Type::Array) {
        return std::get<std::vector<Json>>(m_value).size();
    }
    if (type() == Type::Object) {
        return std::get<std::map<std::string, Json>>(m_value).size();
    }
    return 0;
}

const std::vector<Json>& Json::items() const {
    static const std::vector<Json> kEmpty; // 非数组时的空数组引用(C++11 起静态初始化线程安全)
    if (type() != Type::Array) {
        return kEmpty;
    }
    return std::get<std::vector<Json>>(m_value);
}

std::vector<std::string> Json::keys() const {
    std::vector<std::string> ks;
    if (type() == Type::Object) {
        for (const auto& kv : std::get<std::map<std::string, Json>>(m_value)) {
            ks.push_back(kv.first);
        }
    }
    return ks;
}

} // namespace es
