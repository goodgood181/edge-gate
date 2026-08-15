// 文件路径: tests/test_json.cpp
// 意图: 手写 JSON DOM 的契约 §14 全量单测 —— 解析/回环/错误定位/嵌套深度/
//       转义(含 \uXXXX 与 UTF-16 代理对)/数字分型与精度。
// 覆盖点:
//  - 基本对象/数组/嵌套解析与 get/has/items/keys 语义
//  - 序列化回环(Json 构建 → dump → parse → dump 稳定)
//  - 错误定位(err 含 "偏移"/"行"/"列")与典型语法错误
//  - 嵌套深度上限(512): 深层合法、超限报错
//  - 转义序列全集合与代理对组合(😀 = U+1F600 = \uD83D\uDE00)
//  - 数字: int64 保精度、double/指数、整数溢出落 double、类型换算语义
#include "framework.h"

#include "../src/util/json.h"

#include <string>

using es::Json;

ES_TEST(json_parse_basic_object)
{
    std::string err;
    Json v = Json::parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.5}}", &err);
    CHECK(err.empty());
    CHECK_EQ(static_cast<int>(v.type()), static_cast<int>(Json::Type::Object));
    CHECK(v.has("a"));
    CHECK(v.has("b"));
    CHECK(v.has("c"));
    CHECK(!v.has("nope"));
    CHECK_EQ(v.get("a").asInt(), static_cast<int64_t>(1));
    CHECK_EQ(v.get("c").get("d").asNumber(), 2.5);

    const Json& arr = v.get("b");
    CHECK_EQ(static_cast<int>(arr.type()), static_cast<int>(Json::Type::Array));
    CHECK_EQ(arr.size(), static_cast<size_t>(3));
    CHECK(arr.items()[0].asBool());
    CHECK(arr.items()[1].isNull());
    CHECK_EQ(arr.items()[2].asString(), std::string("x"));
    // keys 顺序与输入一致
    const std::vector<std::string> keys = v.keys();
    CHECK_EQ(keys.size(), static_cast<size_t>(3));
    CHECK_EQ(keys[0], std::string("a"));
    CHECK_EQ(keys[1], std::string("b"));
    CHECK_EQ(keys[2], std::string("c"));
}

ES_TEST(json_parse_whitespace_and_empty)
{
    std::string err;
    // 前导/尾随空白合法
    Json v = Json::parse("  \n\t{\"a\": 1} \r\n ", &err);
    CHECK(err.empty());
    CHECK(v.has("a"));
    // 空输入/非法输入 → Null + err
    CHECK(Json::parse("", &err).isNull() && !err.empty());
    CHECK(Json::parse("   ", &err).isNull() && !err.empty());
    // 根必须是单个值: 尾随字符报错
    err.clear();
    CHECK(Json::parse("{} extra", &err).isNull() && !err.empty());
    // 顶层标量也支持
    CHECK(Json::parse("42", &err).asInt() == 42 && err.empty());
    CHECK(Json::parse("\"str\"", &err).asString() == "str" && err.empty());
    CHECK(Json::parse("null", &err).isNull() && err.empty());
}

ES_TEST(json_parse_errors_with_position)
{
    std::string err;
    // 每个错误都应带"偏移 + 行列"定位信息
    CHECK(Json::parse("{\"a\":}", &err).isNull());
    CHECK(!err.empty());
    CHECK(err.find("偏移") != std::string::npos);
    CHECK(err.find("第") != std::string::npos && err.find("行") != std::string::npos);

    err.clear();
    CHECK(Json::parse("{", &err).isNull() && !err.empty());
    err.clear();
    CHECK(Json::parse("{\"a\" 1}", &err).isNull() && !err.empty());
    err.clear();
    CHECK(Json::parse("[1,2,]", &err).isNull() && !err.empty());
    err.clear();
    CHECK(Json::parse("tru", &err).isNull() && !err.empty());
    err.clear();
    CHECK(Json::parse("'single'", &err).isNull() && !err.empty());
    // 多行输入: 行号应 > 1
    err.clear();
    CHECK(Json::parse("{\n  \"a\": 1,\n  \"b\": tru\n}", &err).isNull());
    CHECK(err.find("第 3 行") != std::string::npos);
}

ES_TEST(json_nesting_depth_limit)
{
    std::string err;
    // 100 层嵌套数组: 合法
    std::string deep;
    for (int i = 0; i < 100; ++i)
    {
        deep += "[";
    }
    deep += "0";
    for (int i = 0; i < 100; ++i)
    {
        deep += "]";
    }
    Json v = Json::parse(deep, &err);
    CHECK(err.empty());
    CHECK_EQ(static_cast<int>(v.type()), static_cast<int>(Json::Type::Array));

    // 600 层嵌套: 超过 512 上限 → 报错(防栈溢出)
    err.clear();
    std::string tooDeep;
    for (int i = 0; i < 600; ++i)
    {
        tooDeep += "[";
    }
    tooDeep += "0";
    for (int i = 0; i < 600; ++i)
    {
        tooDeep += "]";
    }
    CHECK(Json::parse(tooDeep, &err).isNull());
    CHECK(!err.empty());
    CHECK(err.find("深度") != std::string::npos);
}

ES_TEST(json_escapes_roundtrip)
{
    std::string err;
    // 全部短转义: \" \\ \/ \b \f \n \r \t
    // JSON 文本 = "\"\\\/\b\f\n\r\t"(逐段拼接,避免 C++ 转义歧义)
    std::string raw = "\"";
    raw += "\\\"";  // JSON 转义: 双引号
    raw += "\\\\";  // JSON 转义: 反斜杠
    raw += "\\/";     // JSON 转义: 斜杠
    raw += "\\b\\f\\n\\r\\t"; // JSON 转义: 控制字符
    raw += "\"";
    Json v = Json::parse(raw, &err);
    CHECK(err.empty());
    const std::string expect = std::string("\"\\/\b\f\n\r\t", 8);
    CHECK_EQ(v.asString(), expect);
    // 转义对称: dump 后再解析仍一致
    CHECK(Json::parse(v.dump(), &err).asString() == expect);

    // \uXXXX 基本多语言平面
    CHECK_EQ(Json::parse("\"\\u4E2D\"", &err).asString(), std::string("\xE4\xB8\xAD"));
    // UTF-16 代理对: U+1F600(😀)→ UTF-8 4 字节 F0 9F 98 80
    const std::string smile = std::string("\xF0\x9F\x98\x80");
    CHECK_EQ(Json::parse("\"\\uD83D\\uDE00\"", &err).asString(), smile);
    CHECK(err.empty());
    // 孤立代理必须报错(不静默吞)
    err.clear();
    CHECK(Json::parse("\"\\uD83D\"", &err).isNull() && !err.empty());
    err.clear();
    CHECK(Json::parse("\"\\uDE00\"", &err).isNull() && !err.empty());
}

ES_TEST(json_numbers)
{
    std::string err;
    // 整数保精度(int64)
    CHECK_EQ(Json::parse("123456789012345678", &err).asInt(), static_cast<int64_t>(123456789012345678LL));
    CHECK(err.empty());
    // 负数/浮点/指数
    CHECK_NEAR(Json::parse("-2.5e-1", &err).asNumber(), -0.25, 1e-12);
    CHECK_NEAR(Json::parse("1e3", &err).asNumber(), 1000.0, 1e-9);
    CHECK_NEAR(Json::parse("0.1", &err).asNumber(), 0.1, 1e-12);
    // 整数溢出(超 int64)自动落 double
    CHECK_NEAR(Json::parse("99999999999999999999999999", &err).asNumber(), 1e26, 1e19);
    // 字面量分型: 无小数点的解析为 int64
    CHECK_EQ(Json::parse("42", &err).type(), Json::Type::Number);
    CHECK_EQ(Json::parse("42", &err).asInt(), static_cast<int64_t>(42));
    CHECK_NEAR(Json::parse("42", &err).asNumber(), 42.0, 1e-9);
    // asInt 对 double 截断
    CHECK_EQ(Json::parse("3.9", &err).asInt(), static_cast<int64_t>(3));
    // 数字 → bool 语义(非 0 即真)
    CHECK(Json::parse("0", &err).asBool() == false);
    CHECK(Json::parse("5", &err).asBool() == true);
}

ES_TEST(json_dump_roundtrip)
{
    // 用 API 构建 → dump 的字节级期望
    // 注意: Json 内部对象为 std::map,键按字典序输出(arr/b/f/i/n/s)
    Json obj;
    obj.set("b", true);
    obj.set("n", Json(nullptr));
    obj.set("i", static_cast<int64_t>(42));
    obj.set("f", 1.5);
    obj.set("s", "hi");
    Json arr;
    arr.pushBack(Json(static_cast<int64_t>(1)));
    arr.pushBack(Json(static_cast<int64_t>(2)));
    obj.set("arr", arr);
    // 期望: {"arr":[1,2],"b":true,"f":1.5,"i":42,"n":null,"s":"hi"}
    CHECK_EQ(obj.dump(), std::string("{\"arr\":[1,2],\"b\":true,\"f\":1.5,\"i\":42,\"n\":null,\"s\":\"hi\"}"));

    // 回环: dump → parse → dump 稳定
    std::string err;
    Json v = Json::parse(obj.dump(), &err);
    CHECK(err.empty());
    CHECK_EQ(v.dump(), obj.dump());
    // pretty 模式可再解析
    const std::string pretty = obj.dump(true);
    CHECK(pretty.find('\n') != std::string::npos);
    CHECK(Json::parse(pretty, &err).dump() == obj.dump());
}

ES_TEST(json_build_convenience)
{
    // 缺失键自动插入 Null;下标越界自动补 Null;非对象自动升级
    Json obj;
    CHECK_EQ(obj.type(), Json::Type::Null);
    CHECK(obj["k"].isNull());
    CHECK_EQ(obj.type(), Json::Type::Object);

    Json arr;
    arr[3] = Json(static_cast<int64_t>(9)); // 前 3 个位置补 Null
    CHECK_EQ(arr.size(), static_cast<size_t>(4));
    CHECK(arr.items()[0].isNull());
    CHECK_EQ(arr.items()[3].asInt(), static_cast<int64_t>(9));

    // set/get 语义
    Json o;
    o.set("a", Json(static_cast<int64_t>(1)));
    o.set("a", Json(static_cast<int64_t>(2))); // 重复键后者覆盖
    CHECK_EQ(o.get("a").asInt(), static_cast<int64_t>(2));
    CHECK(o.get("missing", Json(static_cast<int64_t>(-1))).asInt() == -1);
    // 默认值: 非字符串 asString 返回 def
    CHECK_EQ(o.asString("def"), std::string("def"));
}
