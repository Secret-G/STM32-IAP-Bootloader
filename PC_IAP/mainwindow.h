#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPortInfo>
#include <QSerialPort>
#include <QByteArray>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void refreshSerialPorts();
    void toggleSerialPort();
    void selectBinFile();

    /*点击“开始升级”。*/
    void startUpgrade();

    /*根据已经确定的A/B目标，构造并发送START升级帧。*/
    void sendStartFrame();

    /* 协议串口收到数据时调用。*/
    void onSerialReadyRead();

    /*根据currentPacketSequence，从firmwareData截取并发送当前DATA包。*/
    bool sendCurrentDataPacket();

    /*构造并发送END帧。*/
    bool sendEndFrame();

    /*等待STM32应答超时时调用。*/
    void onAckTimeout();

private:
    Ui::MainWindow *ui;

    QSerialPort *serialPort;

    /*保存从BIN文件读取出来的全部固件数据。*/
    QByteArray firmwareData;

    /*串口应答接收缓存。*/
    QByteArray responseBuffer;

    /*当前正在发送或者等待ACK的DATA包序号。*/
    quint32 currentPacketSequence = 0U;

    /*整个BIN需要发送的DATA总包数。*/
    quint32 totalPacketCount = 0U;

    /*
     * ACK等待定时器。
     * 每次发送START、DATA或者END后启动；
     * 收到正确ACK/NACK后停止。
     */
    QTimer *ackTimer;

    /*
     * 保存当前正在等待应答的完整协议帧。
     *
     * 超时以后直接重新发送这一张帧，
     * 保证重发内容与第一次完全相同。
     */
    QByteArray pendingFrame;

    /*
     * 当前等待应答的原始请求命令。
     *
     * 例如：
     * 0x0002 = START
     * 0x0003 = DATA
     * 0x0004 = END
     */
    quint16 pendingCommand = 0U;

    /*
     * 当前协议帧已经重发的次数。
     *
     * 第一次正常发送不算重发，
     * 第一次超时后重新发送才变成1。
     */
    int retryCount = 0;

    /*
     * 等待ACK的最长时间，单位毫秒。
     *
     * 使用3000ms是因为START过程中
     * STM32可能需要擦除较大的Flash扇区。
     */
    static constexpr int AckTimeoutMs = 3000;

    /*一张协议帧最多重发3次。*/
    static constexpr int MaxRetryCount = 3;
};



#endif // MAINWINDOW_H

