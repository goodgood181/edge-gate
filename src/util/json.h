// 文件路径: src/util/json.h
// 职责: 手写最小 JSON DOM(文档对象模型)—— 解析 / 序列化 / 查询构建,
//       零外部依赖,禁用异常(错误经 bool 返回值 + std::string* err 出参)。
// 典型用途: 解析 edge-gate.json 配置、生成遥测/告警 JSON 报文、命令服务器收发 JSON 命令。
//
// 设计要点:
// 1) std::variant 实现: 值语义 + 栈上存储,拷贝即深拷贝,节点生命周期自动管理,
//    比 void* + 手写内存管理安全得多;variant 的六种替代类型与 JSON 六种类型一一对应。
// 2) 错误定位策略: 解析器维护 pos/line/col 三态,错误消息含"字节偏移 + 行列号",
//    例如: "JSON 解析错误: 对象键后缺少 ':' @ 偏移 23 (第 2 行第 12 列)" ——
//    配置文件写错一眼定位,对比"parse failed"式的黑盒报错。
// 3) 递归下降解析 + 深度上限(512): 解析器天然递归,恶意/误写深层嵌套会栈溢出崩溃,
//    深度计数超限即报错 —— 输入不可信时设资源上限是基本功。
// 4) 转义对称: 解析支持 \" \\ \/ \b \f \n \r \t \uXXXX(含 UTF-16 代理对组合),
//    序列化为逆过程;非 ASCII 按 UTF-8 原样透传,不损失字节。
// 5) 数字分型: 无小数点/指数的字面量存 int64_t(保精度),其余存 double;
//    整数溢出自动落为 double;序列化 double 用"最短可往返"算法(1~17 位有效数字试凑),
//    精度不丢且输出干净(100.0 输出 "100" 而非 "1e+02")。
// 6) 便利语义(注释明示,防误解): 对象重复键后者覆盖;operator[] 对缺失键自动插入
//    Null、对非对象/非数组自动升级为对象/数组;越界下标自动补 Null —— 构建报文少样板。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace es {

// 最小 JSON DOM。值语义: 拷贝即深拷贝。
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json(); // Null
    Json(std::nullptr_t);
    Json(bool);
    Json(double);
    Json(int64_t);
    Json(int); // 便捷重载: 消除整数字面量在 int64_t/double/bool 间的重载歧义(见 json.cpp 注释)
    Json(const char* s);
    Json(const std::string& s);
    Json(const std::vector<Json>& arr);
    Json(const std::map<std::string, Json>& obj);

    // 解析;失败返回 Null 且 err 含"消息 + 字节偏移 + 行列号";成功则清空 err
    static Json parse(const std::string& text, std::string* err = nullptr);

    std::string dump(bool pretty = false) const; // 序列化(默认紧凑;pretty 为 2 空格缩进)

    [[nodiscard]] Type type() const;
    [[nodiscard]] bool isNull() const;

    bool asBool(bool def = false) const;          // bool / 数字(非 0 即真)
    double asNumber(double def = 0.0) const;      // int64 → double 无损(64 位内)
    int64_t asInt(int64_t def = 0) const;         // double 截断取整;越界(double 超 int64 范围)回默认值
    const std::string& asString(const std::string& def = "") const; // 非字符串返回 def 的引用

    [[nodiscard]] bool has(const std::string& key) const;
    Json get(const std::string& key, const Json& def = Json()) const; // 对象取值,缺省返回 def

    Json& operator[](const std::string& key); // 缺失键自动插入 Null;非对象自动升级为对象
    Json& operator[](size_t i);               // 越界自动补 Null(便于顺序构建数组)

    void pushBack(const Json& v);
    void set(const std::string& key, const Json& v);

    [[nodiscard]] size_t size() const;      // 数组/对象元素个数;其他类型 0
    const std::vector<Json>& items() const; // 数组元素(非数组返回空数组引用)
    std::vector<std::string> keys() const;  // 对象键(非对象返回空)

private:
    void dumpTo(std::string& out, int depth, bool pretty) const;

    // 六种 JSON 类型 ↔ variant 六种替代类型,一一对应(type() 的 index 映射见 json.cpp)
    using Variant = std::variant<std::nullptr_t, bool, double, int64_t,
                                 std::string, std::vector<Json>, std::map<std::string, Json>>;

    Variant m_value;
};

} // namespace es
