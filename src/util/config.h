// 文件路径: src/util/config.h
// 职责: 配置访问门面 —— 从 JSON 文件加载配置,按 "section.key" 两级取值并带默认值;
//       未配置/类型不匹配时静默回退默认值(禁异常工程: 配置缺失不致命)。
// 典型用途: main 启动时 loadFromFile 解析 edge-gate.json,Gateway 各模块按 section 取值。
//
// 设计要点:
// 1) 双层取值模型: 配置按 section(device/log/serial/mqtt/cmdServer/jsonl/points)组织,
//    getXxx(section, key, def) 避免拍平后键名拼写灾难,与契约 §12 schema 一一对应。
// 2) 宽松类型换算: asInt/asDouble/asBool 在 int/double/bool 之间做有意义换算
//    (数字非 0 即 true、整数 ↔ 浮点),配置里写 9600 还是 9600.0 都能读对。
// 3) 失败策略: 文件打不开/JSON 语法错/根非对象 → loadFromFile 返回 false + err;
//    单个键缺失或类型不匹配 → 返回默认值不报错 —— "整体严格、局部宽容",
//    网关宁可带默认参数启动,也不因一个可选键崩溃。
#pragma once

#include <cstdint>
#include <string>

#include "json.h"

namespace es {

class Config {
public:
    Config(); // 默认构造(空配置树);契约的 loadFromFile(path, Config* out, ...) 用法要求可默认构造

    // 从文件加载并解析;失败返回 false 且 err 给出原因(含 JSON 错误定位);成功清空 err
    static bool loadFromFile(const std::string& path, Config* out, std::string* err);
    static Config fromJson(const Json& root); // 从已解析的 Json 构造(测试/内存配置)

    std::string getString(const std::string& section, const std::string& key, const std::string& def) const;
    int64_t getInt(const std::string& section, const std::string& key, int64_t def) const;
    double getDouble(const std::string& section, const std::string& key, double def) const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;
    Json getSection(const std::string& section) const; // 整个 section(不存在 → Null)
    [[nodiscard]] const Json& root() const;

private:
    explicit Config(const Json& root);

    Json m_root; // 持有完整配置树(Json 值语义,拷贝即深拷贝)
};

} // namespace es
