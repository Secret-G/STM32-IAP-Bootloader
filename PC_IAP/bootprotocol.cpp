#include "bootprotocol.h"

quint16 BootProtocol::crc16Modbus(const QByteArray &data)
{
    quint16 crc = 0xFFFFU;

    /*
     * QByteArray中的单个元素是char，
     * char可能是有符号类型，因此必须转为quint8。
     */
    for (char value : data)
    {
        const quint8 byte = static_cast<quint8>(value);

        crc ^= byte;

        /*
         * 每个字节处理8个二进制位。
         */
        for (quint8 bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 0x0001U) != 0U)
            {
                crc = static_cast<quint16>((crc >> 1U) ^ 0xA001U);
            }
            else
            {
                crc = static_cast<quint16>(crc >> 1U);
            }
        }
    }

    return crc;
}



QByteArray BootProtocol::buildFrame(quint16 command, const QByteArray &data, quint32 reserveValue)
{
    if(data.size() > 256)
    {
        return QByteArray();
    }

    /*
     * 固定字段：
     * CMD 2 + LEN 2 + RESERVE 4 + CRC 2
     * 一共10字节。
     */
    constexpr quint16 fixedSize = 10U;

    const quint16 totalLength = static_cast<quint16>(fixedSize + data.size());

    QByteArray frame;

    /*
     * 提前申请内存，避免append过程中反复扩容。
     */
    frame.reserve(totalLength);

    /*
     * frame[0～1]：CMD。
     */
    appendUint16LE(frame, command);

    /*
     * frame[2～3]：完整帧总长度。
     */
    appendUint16LE(frame, totalLength);

    /*
     * frame[4...]：DATA。
     */
    frame.append(data);

    /*
     * DATA后面的4字节RESERVE。
     */
    appendUint32LE(frame, reserveValue);

    /*
     * 此时frame中还没有CRC，
     * 因此直接计算当前所有字节即可。
     */
    const quint16 crc = crc16Modbus(frame);

    /*
     * 最后追加2字节CRC。
     */
    appendUint16LE(frame, crc);

    return frame;
}

QByteArray BootProtocol::buildStartFrame(quint32 imageSize, quint16 imageCrc)
{


    QByteArray data;

    if (imageSize == 0U)
    {
        return QByteArray();
    }

    /*
     * START的DATA固定7字节：
     * DATA[1～4] 整个BIN大小
     * DATA[5～6] 整个BIN CRC16
     */
    data.reserve(7);

    data.append(static_cast<char>(TargetAuto));

    appendUint32LE(data, imageSize);
    appendUint16LE(data, imageCrc);

    /*
     * START帧暂时不使用RESERVE，
     * 因此传入0。
     */
    return buildFrame(static_cast<quint16>(CmdStartUpdate), data, 0U);
}

QByteArray BootProtocol::buildDataFrame(const QByteArray &packetData, quint32 sequence)
{
    /*
     * DATA帧必须携带真实BIN数据，
     * 不允许发送空数据包。
     */
    if (packetData.isEmpty())
    {
        return QByteArray();
    }

    /*
     * 根据当前协议设计，
     * 每张DATA帧最多携带256字节BIN数据。
     */
    if (packetData.size() > 256)
    {
        return QByteArray();
    }

    /*
     * DATA帧格式：
     *
     * CMD       = CmdDataPacket，即0x0003
     * DATA      = 当前包BIN数据
     * RESERVE   = 当前包序号
     * CRC       = buildFrame自动计算
     */
    return buildFrame(CmdDataPacket, packetData, sequence);
}

QByteArray BootProtocol::buildEndFrame(quint32 packetCount)
{
    /*一个有效升级至少应该存在一张DATA帧。*/
    if (packetCount == 0U)
    {
        return QByteArray();
    }

    /*END帧没有DATA内容，*/
    return buildFrame(CmdEndUpdate, QByteArray(),packetCount);
}

bool BootProtocol::parseResponse(const QByteArray &frame, ResponseInfo &response)
{

    /* ACK/NACK固定为14字节。*/
    constexpr qsizetype responseFrameSize = 14;
    constexpr qsizetype crcSize = 2;

    if (frame.size() != responseFrameSize)
    {
        return false;
    }

    /* frame[0～1]：ACK或者NACK命令。*/
    const quint16 responseCommand = readUint16LE(frame, 0);

    if ((responseCommand != CmdAck) &&
        (responseCommand != CmdNack))
    {
        return false;
    }

    /* frame[2～3]：应答帧声明的总长度。*/
    const quint16 totalLength = readUint16LE(frame, 2);

    if (totalLength != responseFrameSize)
    {
        return false;
    }

    /* frame[12～13]：STM32发送的CRC。*/
    const quint16 receivedCrc = readUint16LE(frame, 12);

    /* Qt重新计算前12字节CRC。*/
    const QByteArray crcData = frame.left(responseFrameSize - crcSize);

    const quint16 calculatedCrc = crc16Modbus(crcData);

    if (receivedCrc != calculatedCrc)
    {
        return false;
    }

    /*所有基础检查成功后再填写输出结构体。*/
    response.responseCommand = responseCommand;

    /*frame[4～5]：原始请求命令。*/
    response.requestCommand = readUint16LE(frame, 4);

    /* frame[6～7]：处理结果码。*/
    response.result = readUint16LE(frame, 6);

    /*frame[8～11]：RESERVE附加值。*/
    response.value = readUint32LE(frame, 8);

    return true;
}

void BootProtocol::appendUint16LE(QByteArray &buffer, quint16 value)
{
    /*
     * 小端格式：低字节在前，高字节在后。
     */
    buffer.append(static_cast<char>(value & 0x00FFU));
    buffer.append(static_cast<char>((value >> 8U) & 0x00FFU));
}

void BootProtocol::appendUint32LE(QByteArray &buffer, quint32 value)
{
    buffer.append(static_cast<char>(value & 0x000000FFUL));

    buffer.append(static_cast<char>((value >> 8U) & 0x000000FFUL));

    buffer.append(static_cast<char>((value >> 16U) & 0x000000FFUL));

    buffer.append(static_cast<char>((value >> 24U) & 0x000000FFUL));
}

quint16 BootProtocol::readUint16LE(const QByteArray &buffer, qsizetype offset)
{
    const quint16 low = static_cast<quint8>(buffer.at(offset));

    const quint16 high = static_cast<quint8>(buffer.at(offset + 1));

    return static_cast<quint16>(low | (high << 8U));
}

quint32 BootProtocol::readUint32LE(const QByteArray &buffer, qsizetype offset)
{
    const quint32 byte0 =static_cast<quint8>(buffer.at(offset));

    const quint32 byte1 =static_cast<quint8>(buffer.at(offset + 1));

    const quint32 byte2 =static_cast<quint8>(buffer.at(offset + 2));

    const quint32 byte3 =static_cast<quint8>(buffer.at(offset + 3));

    return byte0 | (byte1 << 8U) | (byte2 << 16U) | (byte3 << 24U);
}