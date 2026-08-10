#ifndef BOOTPROTOCOL_H
#define BOOTPROTOCOL_H

#include <QByteArray>
#include <QtGlobal>
#include <QString>
#include <QStringList>

/*
 * Bootloader协议算法类。
 *
 * 只负责CRC、组帧和解析应答，
 * 不负责界面和串口操作。
 */
class BootProtocol
{
public:
    /*
     * 计算Modbus CRC16。
     * 算法和STM32中的
     * Boot_CRC16_Modbus()完全一致。
     */
    static quint16 crc16Modbus(const QByteArray &data);

    /*
     * 协议命令。
     * 必须与STM32端Boot_CmdTypeDef保持一致。
     */
    enum Command : quint16
    {
        CmdStartUpdate = 0x0002,
        CmdDataPacket  = 0x0003,
        CmdEndUpdate   = 0x0004,

        CmdAck         = 0x8000,
        CmdNack        = 0x8001
    };

    /*
     * START帧中的升级目标模式。
     * 数值必须与STM32端Update_TargetTypeDef一致。
     */
        enum UpdateTarget : quint8
        {
            TargetNone = 0U,
            TargetA    = 1U,
            TargetB    = 2U,
            TargetAuto = 3U
        };

    /*
     * STM32返回的处理结果。
     * 必须与Boot_ResultTypeDef一致。
     */
    enum Result : quint16
    {
        ResultOk               = 0x0000,
        ResultFrameError       = 0x0001,
        ResultStateError       = 0x0002,
        ResultImageTooLarge    = 0x0003,
        ResultSequenceError    = 0x0004,
        ResultDataTooLarge     = 0x0005,
        ResultFlashError       = 0x0006,
        ResultPacketCountError = 0x0007,
        ResultImageSizeError   = 0x0008,
        ResultImageCrcError    = 0x0009,
        ResultUnknownCommand   = 0x000A,

        /*请求安装的固件版本不符合升级策略*/
        BOOT_RESULT_VERSION_ERROR = 0x000BU
    };

    /*一张ACK/NACK解析后的结果。*/
    struct ResponseInfo
    {
        /*CmdAck或者CmdNack。*/
        quint16 responseCommand = 0U;

        /*这张应答对应的原始请求命令。*/
        quint16 requestCommand = 0U;

        /*Result枚举中的处理结果码。*/
        quint16 result = 0U;

        /* RESERVE中的4字节附加值。*/
        quint32 value = 0U;
    };

    /* 构造一张通用请求帧：*/
    static QByteArray buildFrame( quint16 command,const QByteArray &data,quint32 reserveValue);

    /* 构造START升级帧。*/
    static QByteArray buildStartFrame(quint32 imageSize,quint16 imageCrc,quint32 imageVersion);

    /* 构造一张DATA固件数据帧。*/
    static QByteArray buildDataFrame(const QByteArray &packetData,quint32 sequence);

    /* 构造END升级结束帧。*/
    static QByteArray buildEndFrame(quint32 packetCount);

    /*解析一张固定14字节的ACK/NACK。*/
    static bool parseResponse(const QByteArray &frame, ResponseInfo &response);

    /*将“1.2.3.4”转换为0x01020304*/
    static bool parseVersion(const QString &versionText,quint32 &versionValue);
    /*将0x01020304转换成“1.2.3.4”*/
    static QString formatVersion(quint32 versionValue);

private:

    /*按小端格式向数组末尾追加16位数据。*/
    static void appendUint16LE(QByteArray &buffer, quint16 value);

    /*按小端格式向数组末尾追加32位数据。*/
    static void appendUint32LE(QByteArray &buffer, quint32 value);

    /*从buffer指定位置读取一个小端16位数据*/
    static quint16 readUint16LE(const QByteArray &buffer,qsizetype offset);

    /*从buffer指定位置读取一个小端32位数据*/
    static quint32 readUint32LE(const QByteArray &buffer,qsizetype offset);
};

#endif // BOOTPROTOCOL_H