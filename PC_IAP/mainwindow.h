#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPortInfo>
#include <QSerialPort>
#include <QByteArray>

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

    /*点击“开始升级”，发送START帧。*/
    void startUpgrade();

    /* 协议串口收到数据时调用。*/
    void onSerialReadyRead();

    /*
     * 根据currentPacketSequence，
     * 从firmwareData截取并发送当前DATA包。
     */
    bool sendCurrentDataPacket();

    /*构造并发送END帧。*/
    bool sendEndFrame();

private:
    Ui::MainWindow *ui;

    QSerialPort *serialPort;

    /*
     * 保存从BIN文件读取出来的全部固件数据。
     */
    QByteArray firmwareData;

    /*
     * 串口应答接收缓存。
     *
     * ACK/NACK可能被分成多次readyRead到达，
     * 因此不能认为readAll()一次就是完整14字节。
     */
    QByteArray responseBuffer;

    /*
     * 当前正在发送或者等待ACK的DATA包序号。
     */
    quint32 currentPacketSequence = 0U;

    /*
     * 整个BIN需要发送的DATA总包数。
     */
    quint32 totalPacketCount = 0U;
};



#endif // MAINWINDOW_H

