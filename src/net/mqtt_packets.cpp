// src/net/mqtt_packets.cpp
// 职责: MQTT 3.1.1 编解码实现。全部函数只操作内存,不依赖任何 IO/线程;
//       解码遵循"先结构后内容"的校验顺序: 类型 → 标志 → 剩余长度 → 字段,
//       任何一步失败都只返回 false + err,绝不抛异常。
#include "mqtt_packets.h"

namespace es::mqtt {
namespace {

// 大端序 2 字节 → uint16(MQTT 多数字段为大端,与 Modbus 的低字节在前相反)
uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// 读一个 2 字节长度前缀字符串(MQTT UTF-8 字符串格式)。
// pos/end 界定"可变头+载荷"区间的读取游标;成功时 *next 指向下一字段起点。
// 失败: 长度前缀不足 2 字节,或声明长度超出剩余字节 —— 均属协议错误。
bool readStr(const uint8_t* buf, size_t pos, size_t end, std::string* out, size_t* next)
{
    if (pos > end || end - pos < 2)
    {
        return false;
    }
    const uint16_t slen = readU16(buf + pos);
    pos += 2;
    if (static_cast<size_t>(slen) > end - pos)
    {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(buf + pos), slen);
    *next = pos + static_cast<size_t>(slen);
    return true;
}

// 追加 2 字节大端整数
void appendU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

// 追加 MQTT UTF-8 字符串: [长度 2B 大端][字节]。字符串超 65535 会被截断,
// 项目内报文远小于该上限,由调用方(本客户端)自行保证。
void appendUtf(std::vector<uint8_t>& v, const std::string& s)
{
    appendU16(v, static_cast<uint16_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

// 组装完整报文: 固定头(类型<<4|标志) + 剩余长度 + 可变头/载荷。
// body 超 268435455 字节(2^28-1)时编码失败 —— 实际报文不可能达到,防御性处理。
// 注意: encodeRemainingLength 会清空输出容器,故剩余长度先编码到临时容器,
// 再拼到已写入类型字节的固定头之后(避免误清类型字节)。
void buildPacket(std::vector<uint8_t>* out, uint8_t typeFlags, const std::vector<uint8_t>& body)
{
    out->clear();
    out->push_back(typeFlags);
    std::vector<uint8_t> rl;
    if (!encodeRemainingLength(static_cast<uint32_t>(body.size()), &rl))
    {
        out->clear();  // 长度超上限: 返回空报文(现实中不可达,见函数头注释)
        return;
    }
    out->insert(out->end(), rl.begin(), rl.end());
    out->insert(out->end(), body.begin(), body.end());
}

// 1 字节 → 两位十六进制(仅用于错误信息可读性)
std::string byteHex(uint8_t v)
{
    const char* hex = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    return s;
}

// —— 以下为 decodePacket 的内部解析器(按报文类型分派) ——

// CONNACK: 可变头 [确认标志 1B][返回码 1B],剩余长度恒为 2
bool decodeConnack(const uint8_t* buf, size_t vh, size_t rl, Packet* out, std::string* err)
{
    if (rl != 2)
    {
        *err = "CONNACK 剩余长度必须为 2";
        return false;
    }
    const uint8_t ackFlags = buf[vh];  // bit0 = sessionPresent(会话已被服务端保留)
    if ((ackFlags & 0xFE) != 0)
    {
        *err = "CONNACK 确认标志保留位非 0";
        return false;
    }
    // 契约 Packet 无 sessionPresent 字段: 复用 packetId 承载(见头文件映射约定)
    out->packetId = (ackFlags & 0x01) ? 1 : 0;
    out->returnCode = buf[vh + 1];
    if (out->returnCode > 5)
    {
        *err = "CONNACK 返回码非法(合法范围 0x00..0x05)";
        return false;
    }
    return true;
}

// PUBLISH: 可变头 [topic][packetId?],余下全部为载荷
bool decodePublish(const uint8_t* buf, size_t vh, size_t end, uint8_t flags, Packet* out,
                   std::string* err)
{
    out->dup = (flags & 0x08) != 0;
    out->qos = (flags >> 1) & 0x03;  // QoS=3 已在外层标志校验拒绝
    out->retain = (flags & 0x01) != 0;
    size_t pos = vh;
    std::string topic;
    size_t next = 0;
    if (!readStr(buf, pos, end, &topic, &next))
    {
        *err = "PUBLISH 主题长度字段越界";
        return false;
    }
    pos = next;
    if (topic.empty())
    {
        *err = "PUBLISH 主题为空(主题至少 1 字符,MQTT-4.7.3-1)";
        return false;
    }
    out->topic = topic;
    if (out->qos > 0)
    {
        if (end - pos < 2)
        {
            *err = "PUBLISH 缺少包 ID";
            return false;
        }
        out->packetId = readU16(buf + pos);
        if (out->packetId == 0)
        {
            *err = "PUBLISH 包 ID 为 0";
            return false;
        }
        pos += 2;
    }
    // 余下全部是载荷(允许空载荷);QoS0 的 dup 置位规范不允许,此处兼容接收
    out->payload.assign(reinterpret_cast<const char*>(buf + pos), end - pos);
    return true;
}

// PUBACK/PUBREC/PUBREL/PUBCOMP/UNSUBACK: 可变头仅 [包 ID],剩余长度恒为 2
bool decodeAck(const uint8_t* buf, size_t vh, size_t rl, Packet* out, std::string* err)
{
    if (rl != 2)
    {
        *err = "该报文剩余长度必须为 2(仅含包 ID)";
        return false;
    }
    out->packetId = readU16(buf + vh);
    if (out->packetId == 0)
    {
        *err = "包 ID 为 0";
        return false;
    }
    return true;
}

// SUBSCRIBE: 可变头 [包 ID],载荷 [(topic,qos)…];包 ID 必须非 0(MQTT-2.2.1)
bool decodeSubscribe(const uint8_t* buf, size_t vh, size_t end, size_t rl, Packet* out,
                     std::string* err)
{
    if (rl < 6)  // 2 包 ID + 至少 1 个主题项(2 长度 + ≥1 主题 + 1 QoS)
    {
        *err = "SUBSCRIBE 剩余长度过短(无主题项)";
        return false;
    }
    out->packetId = readU16(buf + vh);
    if (out->packetId == 0)
    {
        *err = "SUBSCRIBE 包 ID 为 0(包 ID 必须非 0,MQTT-2.2.1)";
        return false;
    }
    size_t pos = vh + 2;
    while (pos < end)
    {
        std::string topic;
        size_t next = 0;
        if (!readStr(buf, pos, end, &topic, &next))
        {
            *err = "SUBSCRIBE 主题长度字段越界";
            return false;
        }
        pos = next;
        if (topic.empty())
        {
            *err = "SUBSCRIBE 主题为空";
            return false;
        }
        if (end - pos < 1)
        {
            *err = "SUBSCRIBE 缺少请求 QoS 字节";
            return false;
        }
        const uint8_t q = buf[pos];
        if (q > 2)
        {
            *err = "SUBSCRIBE 请求 QoS=3 非法";
            return false;
        }
        out->topics.push_back(topic);
        // 映射约定: payload 第 i 字节 = 第 i 个主题的请求 QoS(见头文件)
        out->payload.push_back(static_cast<char>(q));
        pos += 1;
    }
    return true;
}

// SUBACK: 可变头 [包 ID],载荷为每主题 1 字节授权 QoS(0x00/0x01/0x02)或 0x80 失败
bool decodeSuback(const uint8_t* buf, size_t vh, size_t end, size_t rl, Packet* out,
                  std::string* err)
{
    if (rl < 3)  // 2 包 ID + ≥1 返回码
    {
        *err = "SUBACK 剩余长度过短(无返回码)";
        return false;
    }
    out->packetId = readU16(buf + vh);
    if (out->packetId == 0)
    {
        *err = "SUBACK 包 ID 为 0";
        return false;
    }
    for (size_t i = vh + 2; i < end; ++i)
    {
        const uint8_t code = buf[i];
        if (code > 2 && code != 0x80)
        {
            *err = "SUBACK 返回码非法(合法: 0x00/0x01/0x02/0x80)";
            return false;
        }
        // 映射约定: 授权 QoS 以十进制串存入 topics(0x80 → "128" = 订阅失败)
        out->topics.push_back(std::to_string(code));
    }
    return true;
}

// UNSUBSCRIBE: 可变头 [包 ID],载荷 [topic…]
bool decodeUnsubscribe(const uint8_t* buf, size_t vh, size_t end, size_t rl, Packet* out,
                       std::string* err)
{
    if (rl < 5)  // 2 包 ID + 至少 1 个主题项(2 长度 + ≥1 主题)
    {
        *err = "UNSUBSCRIBE 剩余长度过短(无主题)";
        return false;
    }
    out->packetId = readU16(buf + vh);
    if (out->packetId == 0)
    {
        *err = "UNSUBSCRIBE 包 ID 为 0";
        return false;
    }
    size_t pos = vh + 2;
    while (pos < end)
    {
        std::string topic;
        size_t next = 0;
        if (!readStr(buf, pos, end, &topic, &next))
        {
            *err = "UNSUBSCRIBE 主题长度字段越界";
            return false;
        }
        pos = next;
        if (topic.empty())
        {
            *err = "UNSUBSCRIBE 主题为空";
            return false;
        }
        out->topics.push_back(topic);
    }
    return true;
}

// PINGREQ/PINGRESP/DISCONNECT: 无可变头无载荷,剩余长度必须为 0
bool decodeNoPayload(size_t rl, std::string* err)
{
    if (rl != 0)
    {
        *err = "该报文剩余长度必须为 0";
        return false;
    }
    return true;
}

// CONNECT(客户端→服务端;fake_broker 用它解析握手):
// 可变头 [协议名][级别 0x04][标志][keepalive 2B],载荷 [clientId][will][username][password]
bool decodeConnect(const uint8_t* buf, size_t vh, size_t end, size_t rl, Packet* out,
                   std::string* err)
{
    // 最小长度: 可变头 10B + clientId 长度前缀 2B
    if (rl < 12)
    {
        *err = "CONNECT 剩余长度过短";
        return false;
    }
    size_t pos = vh;
    // 1) 协议名: [0x00 0x04 'M' 'Q' 'T' 'T'](格式与 UTF-8 字符串相同)
    std::string proto;
    size_t next = 0;
    if (!readStr(buf, pos, end, &proto, &next))
    {
        *err = "CONNECT 协议名长度越界";
        return false;
    }
    pos = next;
    if (proto != "MQTT")
    {
        *err = "协议名非法(必须为 \"MQTT\")";
        return false;
    }
    // 2) 协议级别: 本栈只支持 MQTT 3.1.1 的 0x04
    if (pos >= end)
    {
        *err = "CONNECT 缺少协议级别字节";
        return false;
    }
    if (buf[pos] != 0x04)
    {
        *err = "协议级别非法(仅支持 MQTT 3.1.1 级别 0x04)";
        return false;
    }
    ++pos;
    // 3) 连接标志(OASIS 位序,与契约注释的简化位序不同,以规范为准):
    //    bit0 保留必须 0;bit1 CleanSession;bit2 Will;bit3-4 WillQoS;
    //    bit5 WillRetain;bit6 Password;bit7 UserName
    if (pos >= end)
    {
        *err = "CONNECT 缺少标志字节";
        return false;
    }
    const uint8_t cf = buf[pos];
    ++pos;
    if ((cf & 0x01) != 0)
    {
        *err = "CONNECT 标志保留位(bit0)非 0(MQTT-3.1.2-11)";
        return false;
    }
    const bool cleanSession = (cf & 0x02) != 0;
    const bool willFlag = (cf & 0x04) != 0;
    const uint8_t willQos = static_cast<uint8_t>((cf >> 3) & 0x03);
    const bool willRetain = (cf & 0x20) != 0;
    const bool passwordFlag = (cf & 0x40) != 0;
    const bool usernameFlag = (cf & 0x80) != 0;
    if (!willFlag && (willQos != 0 || willRetain))
    {
        *err = "未置 Will 标志却设置了 WillQoS/WillRetain(MQTT-3.1.2-13/15)";
        return false;
    }
    if (willFlag && willQos == 3)
    {
        *err = "Will QoS=3 非法(MQTT-3.1.2-14)";
        return false;
    }
    if (passwordFlag && !usernameFlag)
    {
        *err = "Password 标志置位但 UserName 标志未置位(MQTT-3.1.2-22)";
        return false;
    }
    // 4) keepalive(2B 大端): 本模块不消费,仅跳过(客户端自行持有配置)
    if (end - pos < 2)
    {
        *err = "CONNECT 缺少 keepalive 字段";
        return false;
    }
    pos += 2;
    // 5) 载荷顺序固定: clientId → [willTopic willPayload] → [username] → [password]
    std::string clientId;
    if (!readStr(buf, pos, end, &clientId, &next))
    {
        *err = "CONNECT clientId 长度越界";
        return false;
    }
    pos = next;
    if (clientId.empty() && !cleanSession)
    {
        *err = "cleanSession=0 时 clientId 不得为空(MQTT-3.1.3-7)";
        return false;
    }
    std::string willTopic;
    std::string willPayload;
    if (willFlag)
    {
        if (!readStr(buf, pos, end, &willTopic, &next) || willTopic.empty())
        {
            *err = "Will 主题缺失或为空";
            return false;
        }
        pos = next;
        if (!readStr(buf, pos, end, &willPayload, &next))
        {
            *err = "Will 载荷长度越界";
            return false;
        }
        pos = next;
    }
    if (usernameFlag)
    {
        std::string username;
        if (!readStr(buf, pos, end, &username, &next))
        {
            *err = "用户名长度越界";
            return false;
        }
        pos = next;
    }
    if (passwordFlag)
    {
        std::string password;
        if (!readStr(buf, pos, end, &password, &next))
        {
            *err = "密码长度越界";
            return false;
        }
        pos = next;
    }
    if (pos != end)
    {
        *err = "CONNECT 载荷存在多余字节";
        return false;
    }
    // 映射约定(见头文件): topics[0]=clientId;topic=willTopic;payload=willPayload
    out->topics.push_back(clientId);
    out->topic = willTopic;
    out->payload = willPayload;
    return true;
}

}  // namespace

bool encodeRemainingLength(uint32_t v, std::vector<uint8_t>* out)
{
    if (out == nullptr || v > 268435455u)  // 4 字节上限 2^28-1
    {
        return false;
    }
    out->clear();
    // 除 128 取余: 低 7 位为数据,商 > 0 则置续延位继续 —— 与解码的乘 128 累加对称
    do
    {
        uint8_t b = static_cast<uint8_t>(v % 128);
        v /= 128;
        if (v > 0)
        {
            b |= 0x80;
        }
        out->push_back(b);
    } while (v > 0);
    return true;
}

bool decodeRemainingLength(const uint8_t* buf, size_t len, size_t* used, uint32_t* out)
{
    if (buf == nullptr || used == nullptr || out == nullptr)
    {
        return false;
    }
    *used = 0;
    *out = 0;
    uint32_t value = 0;
    uint32_t multiplier = 1;
    size_t i = 0;
    // 最多 4 字节;uint32 累加不会溢出: 4 字节全取 0x7F 时恰为 2^28-1
    while (i < len && i < 4)
    {
        const uint8_t b = buf[i];
        value += static_cast<uint32_t>(b & 0x7F) * multiplier;
        ++i;
        if ((b & 0x80) == 0)  // 无续延位 → 编码结束
        {
            *used = i;
            *out = value;
            return true;
        }
        multiplier *= 128;
    }
    *used = i;
    // 失败区分(见头文件): *used == 4 → 已读满 4 字节仍有续延位 = 协议错误;
    // *used < 4 → 输入耗尽 = 还需更多字节(非错误)。
    return false;
}

std::vector<uint8_t> encodeConnect(const ConnectOptions& opt)
{
    // L2 防御性入参校验: willQos 只允许 0/1/2;置 Password 标志要求同时置 UserName(规范)
    if (opt.willQos > 2 || (!opt.password.empty() && opt.username.empty())) {
        return {};   // 配置错误: 返回空报文,连接必然失败——显式失败优于静默错包
    }
    // —— 可变头 ——
    std::vector<uint8_t> body;
    appendUtf(body, "MQTT");      // 协议名: 0x00 0x04 'M' 'Q' 'T' 'T'
    body.push_back(0x04);         // 协议级别: MQTT 3.1.1
    uint8_t flags = 0;
    // 标志位按 OASIS 规范(见头文件注释,与契约注释的位序描述不同,以规范为准):
    // bit1 CleanSession / bit2 Will / bit3-4 WillQoS / bit5 WillRetain /
    // bit6 Password / bit7 UserName;bit0 保留恒为 0
    if (opt.cleanSession)
    {
        flags |= 0x02;
    }
    const bool hasWill = !opt.willTopic.empty();
    if (hasWill)
    {
        flags |= 0x04;                                             // bit2 Will
        flags |= static_cast<uint8_t>((opt.willQos & 0x03) << 3);  // bit3-4 WillQoS
        if (opt.willRetain)
        {
            flags |= 0x20;  // bit5 WillRetain
        }
    }
    if (!opt.password.empty())
    {
        flags |= 0x40;  // bit6 Password(规范要求同时置 UserName,调用方成对提供)
    }
    if (!opt.username.empty())
    {
        flags |= 0x80;  // bit7 UserName
    }
    body.push_back(flags);
    appendU16(body, opt.keepaliveSec);  // keepalive 2B 大端

    // —— 载荷(顺序固定: clientId → will → username → password) ——
    appendUtf(body, opt.clientId);
    if (hasWill)
    {
        appendUtf(body, opt.willTopic);
        appendUtf(body, opt.willPayload);
    }
    if (!opt.username.empty())
    {
        appendUtf(body, opt.username);
    }
    if (!opt.password.empty())
    {
        appendUtf(body, opt.password);
    }

    std::vector<uint8_t> out;
    buildPacket(&out, 0x10, body);  // 0x10 = Connect<<4 | 0000
    return out;
}

std::vector<uint8_t> encodePublish(const std::string& topic, const std::string& payload,
                                   uint8_t qos, bool retain, uint16_t packetId)
{
    // 可变头: [topic][packetId?];载荷 = 消息内容
    std::vector<uint8_t> body;
    appendUtf(body, topic);
    if (qos > 0)
    {
        appendU16(body, packetId);  // QoS1/2 必须携带包 ID(QoS0 不允许有)
    }
    body.insert(body.end(), payload.begin(), payload.end());

    // 固定头: 0x30 | dup(0) | qos<<1 | retain;重发时由客户端以同包 ID 重新编码置 dup
    uint8_t typeFlags = static_cast<uint8_t>(PacketType::Publish) << 4;
    typeFlags |= static_cast<uint8_t>((qos & 0x03) << 1);
    if (retain)
    {
        typeFlags |= 0x01;
    }
    std::vector<uint8_t> out;
    buildPacket(&out, typeFlags, body);
    return out;
}

std::vector<uint8_t> encodePuback(uint16_t packetId)
{
    std::vector<uint8_t> body;
    appendU16(body, packetId);  // 回显 PUBLISH 的包 ID → 完成 QoS1 确认
    std::vector<uint8_t> out;
    buildPacket(&out, 0x40, body);  // 0x40 = Puback<<4 | 0000
    return out;
}

std::vector<uint8_t> encodeSubscribe(
    uint16_t packetId, const std::vector<std::pair<std::string, uint8_t>>& topics)
{
    std::vector<uint8_t> body;
    appendU16(body, packetId);  // 订阅也是请求/应答,包 ID 必须非 0
    for (const auto& t : topics)
    {
        appendUtf(body, t.first);  // 主题(支持 +/# 通配符,匹配语义在 broker 侧)
        body.push_back(static_cast<uint8_t>(t.second & 0x03));  // 请求 QoS 0/1/2
    }
    std::vector<uint8_t> out;
    buildPacket(&out, 0x82, body);  // 0x82 = Subscribe<<4 | 0010(固定标志)
    return out;
}

std::vector<uint8_t> encodeUnsubscribe(uint16_t packetId, const std::vector<std::string>& topics)
{
    std::vector<uint8_t> body;
    appendU16(body, packetId);
    for (const auto& t : topics)
    {
        appendUtf(body, t);
    }
    std::vector<uint8_t> out;
    buildPacket(&out, 0xA2, body);  // 0xA2 = Unsubscribe<<4 | 0010(固定标志)
    return out;
}

std::vector<uint8_t> encodePingreq()
{
    // 0xC0 00: 空载荷心跳;keepalive 周期内空闲即发,服务端回 PINGRESP
    std::vector<uint8_t> out;
    buildPacket(&out, 0xC0, std::vector<uint8_t>());
    return out;
}

std::vector<uint8_t> encodeDisconnect()
{
    // 0xE0 00: 优雅断开;broker 收到后丢弃本会话的 Will 遗嘱
    std::vector<uint8_t> out;
    buildPacket(&out, 0xE0, std::vector<uint8_t>());
    return out;
}

bool decodePacket(const uint8_t* buf, size_t len, size_t* consumed, Packet* out,
                  std::string* err)
{
    if (consumed != nullptr)
    {
        *consumed = 0;
    }
    if (err != nullptr)
    {
        err->clear();  // 数据不足时 err 必须保持为空(调用方据此判断"还需更多字节")
    }
    if (buf == nullptr || out == nullptr || consumed == nullptr)
    {
        if (err != nullptr)
        {
            *err = "入参为空";
        }
        return false;
    }
    *out = Packet();  // 复位,避免上次解码的字段残留

    // —— 固定头第 1 字节: 类型(高 4 位)+ 标志(低 4 位) ——
    if (len < 2)
    {
        return false;  // 至少还需 1 字节剩余长度 → 数据不足
    }
    const uint8_t type = buf[0] >> 4;
    const uint8_t flags = buf[0] & 0x0F;
    if (type < 1 || type > 14)
    {
        *err = "非法报文类型 0x" + byteHex(type) + "(合法范围 1..14)";
        return false;
    }
    out->type = static_cast<PacketType>(type);

    // 标志位校验(先于剩余长度,尽早拒绝): PUBLISH 低 4 位 = dup|qos<<1|retain,
    // PUBREL/SUBSCRIBE/UNSUBSCRIBE 固定 0010,其余固定 0000
    switch (static_cast<PacketType>(type))
    {
        case PacketType::Publish:
            if (((flags >> 1) & 0x03) == 3)
            {
                *err = "PUBLISH QoS=3 非法(规范保留)";
                return false;
            }
            break;
        case PacketType::Pubrel:
        case PacketType::Subscribe:
        case PacketType::Unsubscribe:
            if (flags != 0x02)
            {
                *err = "固定头标志必须为 0010(PUBREL/SUBSCRIBE/UNSUBSCRIBE)";
                return false;
            }
            break;
        default:
            if (flags != 0x00)
            {
                *err = "固定头标志必须为 0000";
                return false;
            }
            break;
    }

    // —— 剩余长度(变长,最多 4 字节;失败时区分"协议错误"与"数据不足") ——
    size_t used = 0;
    uint32_t rl = 0;
    if (!decodeRemainingLength(buf + 1, len - 1, &used, &rl))
    {
        if (used == 4)
        {
            *err = "剩余长度编码超过 4 字节上限";
            return false;
        }
        return false;  // 数据不足: err 保持为空
    }
    const size_t end = 1 + used + static_cast<size_t>(rl);  // 完整报文终点(不含)
    if (len < end)
    {
        return false;  // 可变头/载荷未到齐 → 数据不足
    }
    *consumed = end;

    const size_t vh = 1 + used;  // 可变头起点 = 固定头之后

    switch (static_cast<PacketType>(type))
    {
        case PacketType::Connect:
            return decodeConnect(buf, vh, end, static_cast<size_t>(rl), out, err);
        case PacketType::Connack:
            return decodeConnack(buf, vh, static_cast<size_t>(rl), out, err);
        case PacketType::Publish:
            return decodePublish(buf, vh, end, flags, out, err);
        case PacketType::Puback:
        case PacketType::Pubrec:
        case PacketType::Pubrel:
        case PacketType::Pubcomp:
        case PacketType::Unsuback:
            return decodeAck(buf, vh, static_cast<size_t>(rl), out, err);
        case PacketType::Subscribe:
            return decodeSubscribe(buf, vh, end, static_cast<size_t>(rl), out, err);
        case PacketType::Suback:
            return decodeSuback(buf, vh, end, static_cast<size_t>(rl), out, err);
        case PacketType::Unsubscribe:
            return decodeUnsubscribe(buf, vh, end, static_cast<size_t>(rl), out, err);
        case PacketType::Pingreq:
        case PacketType::Pingresp:
        case PacketType::Disconnect:
            return decodeNoPayload(static_cast<size_t>(rl), err);
    }
    return false;  // 不可达(类型 1..14 已在上方全覆盖校验)
}

}  // namespace es::mqtt
