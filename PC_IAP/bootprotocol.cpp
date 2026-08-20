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
     * SOF 2 + CMD 2 + LEN 2 + RESERVE 4 + CRC 2
     * 一共12字节。
     */
    const quint16 totalLength =
        static_cast<quint16>(FrameFixedSize + data.size());

    QByteArray frame;

    /*
     * 提前申请内存，避免append过程中反复扩容。
     */
    frame.reserve(totalLength);

    /*
     * frame[0～1]：固定SOF帧头。
     */
    frame.append(static_cast<char>(SofByte0));
    frame.append(static_cast<char>(SofByte1));

    /*frame[2～3]：CMD。*/
    appendUint16LE(frame, command);

    /*
     * frame[4～5]：完整帧总长度，包含SOF和CRC。
     */
    appendUint16LE(frame, totalLength);

    /*
     * frame[6...]：DATA。
     */
    frame.append(data);

    /*
     * DATA后面的4字节RESERVE。
     */
    appendUint32LE(frame, reserveValue);

    /*
     * 此时frame中还没有CRC，
     * 因此直接计算当前所有字节即可，
     * 计算范围包含SOF帧头。
     */
    const quint16 crc = crc16Modbus(frame);

    /*
     * 最后追加2字节CRC。
     */
    appendUint16LE(frame, crc);

    return frame;
}

QByteArray BootProtocol::buildStartFrame(quint32 imageSize, quint16 imageCrc,quint32 imageVersion)
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
    return buildFrame(static_cast<quint16>(CmdStartUpdate), data, imageVersion);
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
    if (frame.size() != ResponseFrameSize)
    {
        return false;
    }

    /*frame[0～1]：固定SOF帧头。*/
    if ((static_cast<quint8>(frame.at(0)) != SofByte0) ||
        (static_cast<quint8>(frame.at(1)) != SofByte1))
    {
        return false;
    }

    /* frame[2～3]：ACK或者NACK命令。*/
    const quint16 responseCommand = readUint16LE(frame, CommandOffset);

    if ((responseCommand != CmdAck) &&
        (responseCommand != CmdNack))
    {
        return false;
    }

    /* frame[4～5]：应答帧声明的总长度。*/
    const quint16 totalLength = readUint16LE(frame, LengthOffset);

    if (totalLength != ResponseFrameSize)
    {
        return false;
    }

    const qsizetype crcOffset = ResponseFrameSize - FrameCrcSize;

    /* frame[14～15]：STM32发送的CRC。*/
    const quint16 receivedCrc = readUint16LE(frame, crcOffset);

    /*Qt重新计算包含SOF在内的前14字节CRC。*/
    const QByteArray crcData = frame.left(crcOffset);

    const quint16 calculatedCrc = crc16Modbus(crcData);

    if (receivedCrc != calculatedCrc)
    {
        return false;
    }

    /*所有基础检查成功后再填写输出结构体。*/
    response.responseCommand = responseCommand;

    /*frame[6～7]：原始请求命令。*/
    response.requestCommand = readUint16LE(frame, DataOffset);

    /* frame[8～9]：处理结果码。*/
    response.result = readUint16LE(frame, DataOffset + 2);

    /*frame[10～13]：RESERVE附加值。*/
    response.value = readUint32LE(frame, DataOffset + ResponseDataSize);

    return true;
}

qsizetype BootProtocol::findSof(const QByteArray &buffer, qsizetype from)
{
    QByteArray sof;
    sof.reserve(SofSize);
    sof.append(static_cast<char>(SofByte0));
    sof.append(static_cast<char>(SofByte1));

    return buffer.indexOf(sof, from);
}

bool BootProtocol::parseVersion(const QString &versionText, quint32 &versionValue)
{
    /*
     * 按小数点切割。
     * “1.2.3.4”会得到四部分：
     * “1”“2”“3”“4”。
     */
    const QStringList parts = versionText.trimmed().split('.');

    if(parts.size() != 4)
    {
        return false;
    }

    quint32 result = 0U;

    for (int index = 0; index < 4; index++)
    {
        bool ok = false;

        const uint partValue = parts.at(index).toUInt(&ok);

        /*
         * 每一级只占一个字节，
         * 所以范围只能是0～255。
         */
        if ((!ok) || (partValue > 255U))
        {
            return false;
        }
        const int shift = 24 - index * 8;

        result |= static_cast<quint32>(partValue) << shift;
    }

    versionValue = result;
    return true;
}

QString BootProtocol::formatVersion(quint32 versionValue)
{
    const quint32 major = (versionValue >> 24U) & 0xFFU;

    const quint32 minor = (versionValue >> 16U) & 0xFFU;

    const quint32 patch = (versionValue >> 8U) & 0xFFU;

    const quint32 build = versionValue & 0xFFU;

    return QString("%1.%2.%3.%4")
        .arg(major)
        .arg(minor)
        .arg(patch)
        .arg(build);
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
