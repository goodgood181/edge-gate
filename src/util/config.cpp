// 文件路径: src/util/config.cpp
// 职责: config.h 的实现,设计要点见头文件注释。
#include "config.h"

#include <fstream>
#include <iterator>

namespace es {

namespace {

void setErr(std::string* err, const std::string& msg) {
    if (err) {
        *err = msg;
    }
}

} // namespace

Config::Config()
    : m_root(Json()) {} // 空配置树: 所有 getter 回退默认值

Config::Config(const Json& root)
    : m_root(root) {}

bool Config::loadFromFile(const std::string& path, Config* out, std::string* err) {
    if (!out) {
        setErr(err, "Config::loadFromFile: out 参数为空");
        return false;
    }

    std::ifstream f(path, std::ios::binary); // util 层允许 iostream(core 层禁,见契约)
    if (!f) {
        setErr(err, "无法打开配置文件: " + path);
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::string parseErr;
    Json root = Json::parse(text, &parseErr);
    if (!parseErr.empty()) {
        setErr(err, "配置文件解析失败: " + parseErr); // parseErr 已含偏移/行列定位
        return false;
    }
    if (root.type() != Json::Type::Object) {
        setErr(err, "配置文件根节点必须是 JSON 对象");
        return false;
    }

    *out = Config(root);
    if (err) {
        err->clear();
    }
    return true;
}

Config Config::fromJson(const Json& root) {
    return Config(root);
}

std::string Config::getString(const std::string& section, const std::string& key, const std::string& def) const {
    const Json v = m_root.get(section).get(key);
    // 返回值为 std::string: asString 返回的引用在拷贝为返回值时临时对象仍存活,无悬垂
    return v.asString(def);
}

int64_t Config::getInt(const std::string& section, const std::string& key, int64_t def) const {
    return m_root.get(section).get(key).asInt(def);
}

double Config::getDouble(const std::string& section, const std::string& key, double def) const {
    return m_root.get(section).get(key).asNumber(def);
}

bool Config::getBool(const std::string& section, const std::string& key, bool def) const {
    return m_root.get(section).get(key).asBool(def);
}

Json Config::getSection(const std::string& section) const {
    return m_root.get(section); // 不存在 → Null(调用方自行判断)
}

const Json& Config::root() const {
    return m_root;
}

} // namespace es
