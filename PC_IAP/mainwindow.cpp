#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "bootprotocol.h"

#include <QFile>
#include <QFileDialog>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , serialPort(new QSerialPort(this))
    , ackTimer(new QTimer(this))
{
    ui->setupUi(this);

    connect(ui->pushButtonRefreshPort, &QPushButton::clicked, this, &MainWindow::refreshSerialPorts);

    /*打开/关闭串口按钮。*/
    connect(ui->pushButtonOpenSerial, &QPushButton::clicked, this, &MainWindow::toggleSerialPort);

    /*选择BIN文件按钮。*/
    connect(ui->pushButtonBrowseBin, &QPushButton::clicked, this, &MainWindow::selectBinFile);

    /*清空日志按钮。*/
    connect(ui->pushButtonClearLog, &QPushButton::clicked, ui->plainTextEditLog, &QPlainTextEdit::clear);

    /*点击“开始升级”后发送START。*/
    connect(ui->pushButtonStartUpgrade, &QPushButton::clicked, this, &MainWindow::startUpgrade);

    /*USART1收到数据时读取ACK/NACK。*/
    connect(serialPort, &QSerialPort::readyRead, this, &MainWindow::onSerialReadyRead);

    /*ACK定时器采用单次触发模式。*/
    ackTimer->setSingleShot(true);

    /*定时器超时时调用onAckTimeout()。*/
    connect(ackTimer, &QTimer::timeout, this, &MainWindow::onAckTimeout);

    /*软件启动时立即扫描一次串口*/
    refreshSerialPorts();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::refreshSerialPorts()
{
    const QString previousPort = ui->comboBoxPort->currentData().toString();

    ui->comboBoxPort->clear();

    /*
     * 获取电脑当前存在的串口。
     */
    const QList<QSerialPortInfo>ports = QSerialPortInfo :: availablePorts();

    for (const QSerialPortInfo &portInfo : ports)
    {
        QString displayText = portInfo.portName();

        if (!portInfo.description().isEmpty())
        {
            displayText += " - " + portInfo.description();
        }

        /*
         * displayText给用户看；
         * portName作为隐藏数据保存，例如COM9。
         */
        ui->comboBoxPort->addItem(displayText,portInfo.portName());
    }

    if (ports.isEmpty())
    {
        ui->comboBoxPort->addItem("未检测到可用串口");
        ui->pushButtonOpenSerial->setEnabled(false);

        ui->plainTextEditLog->appendPlainText(
            "[串口] 未检测到可用串口");

        return;
    }

    ui->pushButtonOpenSerial->setEnabled(true);

    /*
     * 尝试恢复刷新前选中的串口。
     */
    const int previousIndex =
        ui->comboBoxPort->findData(previousPort);

    if (previousIndex >= 0)
    {
        ui->comboBoxPort->setCurrentIndex(previousIndex);
    }

    ui->plainTextEditLog->appendPlainText(
        QString("[串口] 检测到 %1 个串口")
            .arg(ports.size()));
}

void MainWindow::toggleSerialPort()
{
    if(serialPort->isOpen())
    {
        const QString portName = serialPort->portName();

        serialPort->close();

        ui->comboBoxPort->setEnabled(true);
        ui->comboBoxBaudRate->setEnabled(true);
        ui->pushButtonRefreshPort->setEnabled(true);

        ui->pushButtonOpenSerial->setText("打开串口");

        ui->labelConnectionStatus->setText("● 串口未连接");

        ui->labelConnectionStatus->setStyleSheet(
            "color: #334155;"
            "background: #e2e8f0;"
            "border-radius: 13px;"
            "padding: 5px 13px;"
            "font-weight: 600;");

        /*
         * 串口关闭后不允许开始升级。
         */
        ui->pushButtonStartUpgrade->setEnabled(false);

        ui->plainTextEditLog->appendPlainText(
            QString("[串口] 已关闭 %1").arg(portName));
        responseBuffer.clear();
        return;
    }

    /*
     * currentData保存的是COM9这样的真实端口名。
     */
    const QString portName = ui->comboBoxPort->currentData().toString();

    if (portName.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[串口] 没有选择有效串口");
        return;
    }

    const qint32 baudRate = ui->comboBoxBaudRate->currentText().toInt();

    /*
     * 配置为与STM32 USART1一致的115200、8N1。
     */
    serialPort->setPortName(portName);
    serialPort->setBaudRate(baudRate);
    serialPort->setDataBits(QSerialPort::Data8);
    serialPort->setParity(QSerialPort::NoParity);
    serialPort->setStopBits(QSerialPort::OneStop);
    serialPort->setFlowControl(QSerialPort::NoFlowControl);

    /*
     * 需要同时发送协议帧和接收ACK/NACK，
     * 所以使用ReadWrite方式打开。
     */
    if (!serialPort->open(QIODevice::ReadWrite))
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[串口] 打开 %1 失败：%2")
                .arg(portName)
                .arg(serialPort->errorString()));

        return;
    }

    /*
     * 打开成功后锁定串口参数，
     * 防止通信中途修改COM口或波特率。
     */
    ui->comboBoxPort->setEnabled(false);
    ui->comboBoxBaudRate->setEnabled(false);
    ui->pushButtonRefreshPort->setEnabled(false);

    ui->pushButtonOpenSerial->setText("关闭串口");

    ui->labelConnectionStatus->setText(
        QString("● %1 已连接").arg(portName));

    ui->labelConnectionStatus->setStyleSheet(
        "color: #166534;"
        "background: #dcfce7;"
        "border-radius: 13px;"
        "padding: 5px 13px;"
        "font-weight: 600;");

    ui->plainTextEditLog->appendPlainText(
        QString("[串口] %1 打开成功，波特率 %2")
            .arg(portName)
            .arg(baudRate));

    /*
 * 如果已经选择了BIN文件，则允许开始升级。
 */
    ui->pushButtonStartUpgrade->setEnabled(!firmwareData.isEmpty());
}

void MainWindow::selectBinFile()
{
    /*
     * 弹出文件选择窗口，只显示BIN文件。
     */
    const QString filePath = QFileDialog::getOpenFileName(
                                            this,
                                        "选择APP固件",
                                         QString(),
                                        "BIN固件 (*.bin);;所有文件 (*.*)");

    /*用户点击取消时，路径为空。*/
    if (filePath.isEmpty())
    {
        return;
    }

    QFile file(filePath);

    /*使用只读方式打开BIN文件。*/
    if (!file.open(QIODevice::ReadOnly))
    {
        ui->plainTextEditLog->appendPlainText(QString("[固件] 文件打开失败：%1").arg(file.errorString()));
        return;
    }

    /*一次性读取整个BIN文件。*/
    const QByteArray data = file.readAll();

    file.close();

    if (data.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[固件] BIN文件为空");
        return;
    }

    /*文件读取成功后再替换原来的固件数据。*/
    firmwareData = data;

    /*计算整个BIN文件的CRC16。*/
    const quint16 imageCrc = BootProtocol::crc16Modbus(firmwareData);

    /*更新界面。*/
    ui->lineEditBinPath->setText(filePath);
    ui->labelFileSizeValue->setText( QString("%1 字节").arg(firmwareData.size()));

    /*把CRC显示成4位大写十六进制。*/
    const QString crcText = QString::number(imageCrc, 16).rightJustified(4, '0').toUpper();

    ui->labelFileCrcValue->setText( "0x" + crcText);
    ui->plainTextEditLog->appendPlainText(QString("[固件] 已加载：%1").arg(filePath));
    ui->plainTextEditLog->appendPlainText(QString("[固件] 大小：%1 字节").arg(firmwareData.size()));
    ui->plainTextEditLog->appendPlainText(QString("[固件] CRC16：0x%1").arg(crcText));

    /*
     * 串口已打开并且BIN已加载时，
     * 才允许点击“开始升级”。
     */
    ui->pushButtonStartUpgrade->setEnabled(serialPort->isOpen());
}

void MainWindow::startUpgrade()
{
    sendStartFrame();
}

void MainWindow::sendStartFrame()
{
    /*必须先打开协议串口。*/
    if (!serialPort->isOpen())
    {
        ui->plainTextEditLog->appendPlainText("[升级] 协议串口尚未打开");
        return;
    }

    /*必须先选择有效BIN文件。*/
    if (firmwareData.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[升级] 尚未加载BIN文件");
        return;
    }

    quint32 imageVersion  = 0u;

    if(!BootProtocol::parseVersion(ui->lineEditFirmwareVersion->text(),imageVersion))
    {
        ui->labelUpgradeStatusValue->setText("固件版本格式错误");
        ui->plainTextEditLog->appendPlainText(
            "[升级] 固件版本格式错误，请使用1.2.3.4格式，"
            "每一级范围为0～255");
        return;
    }

    /*
     * 0.0.0.0保留为“没有版本信息”，
     * 不允许作为正式固件版本。
     */
    if (imageVersion == 0U)
    {
        ui->labelUpgradeStatusValue->setText("固件版本不能为0.0.0.0");
        ui->plainTextEditLog->appendPlainText("[升级] 固件版本不能为0.0.0.0");
        return;
    }

    const quint32 imageSize = static_cast<quint32>(firmwareData.size());
    const quint16 imageCrc = BootProtocol::crc16Modbus(firmwareData);

    /*每张DATA帧最多携带256字节BIN数据。*/
    constexpr quint32 packetDataSize = 256U;

    /*一次新的升级从第0包开始。*/
    currentPacketSequence = 0U;

    /*
     * 对BIN大小进行256字节向上取整，
     * 计算需要发送的DATA总包数。
     */
    totalPacketCount = (imageSize + packetDataSize - 1U) / packetDataSize;

    /*一次新升级开始时，初始化DATA进度显示。*/
    ui->labelPacketProgressValue->setText(QString("0 / %1").arg(totalPacketCount));
    ui->progressBarUpgrade->setRange(0, 100);
    ui->progressBarUpgrade->setValue(0);
    ui->progressBarUpgrade->setFormat("%p% · 正在升级");
    ui->plainTextEditLog->appendPlainText(QString("[升级] BIN大小=%1字节，DATA总包数=%2").arg(imageSize).arg(totalPacketCount));

    /*根据目标、整个BIN大小和整个BIN CRC,构造完整START协议帧。*/
    const QByteArray startFrame = BootProtocol::buildStartFrame(imageSize,imageCrc,imageVersion);

    if (startFrame.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[升级] START组帧失败");
        return;
    }

    /*开始一次新的请求前清空旧应答数据。*/
    responseBuffer.clear();

    /*
     * 把START帧交给Qt串口发送缓冲区。
     *
     * write()返回成功接受的字节数；
     * 返回负数表示写入失败。
     */
    const qint64 written = serialPort->write(startFrame);

    /*write()只是把数据放进Qt发送缓存,必须确认17个字节全部成功进入发送缓存。*/
    if (written != startFrame.size())
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[升级] START写入发送缓存失败：" "需要发送%1字节，实际写入%2字节，错误：%3")
                .arg(startFrame.size()).arg(written).arg(serialPort->errorString()));
        return;
    }

    /*立即启动底层串口发送。*/
    if (!serialPort->flush())
    {
        ui->plainTextEditLog->appendPlainText(QString("[升级] START刷新发送缓存失败：%1")
                .arg(serialPort->errorString()));
        return;
    }

    /*等待电脑真正向串口驱动写出数据。*/
    if (!serialPort->waitForBytesWritten(1000))
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[升级] START物理发送超时：%1").arg(serialPort->errorString()));
        return;
    }

    /*这是一次新的START请求，重发次数从0开始*/
    retryCount = 0;

    /*保存当前正在等待应答的完整START帧*/
    pendingFrame = startFrame;

    /*记录当前等待的是START应答，用于识别ACK/NACK以及显示日志。*/
    pendingCommand = static_cast<quint16>(BootProtocol::CmdStartUpdate);

    /*开始等待STM32的START应答。*/
    ackTimer->start(AckTimeoutMs);

    ui->plainTextEditLog->appendPlainText(QString("[发送确认] START共%1字节，已交给串口驱动").arg(written));

    /*等待START应答期间锁定固件和目标，防止用户中途更换。*/
    ui->pushButtonStartUpgrade->setEnabled(false);
    ui->labelUpgradeStatusValue->setText("等待START应答");
    ui->labelLastResponseValue->setText("--");
    ui->plainTextEditLog->appendPlainText(QString("[发送] START，目标=自动选择，大小=%1，CRC=0x%2")
                                              .arg(imageSize)
                                              .arg(QString::number(imageCrc, 16)
                                              .rightJustified(4, '0')
                                              .toUpper()));
    ui->plainTextEditLog->appendPlainText(
        QString("[升级] 固件版本=%1，编码值=0x%2")
            .arg(ui->lineEditFirmwareVersion->text().trimmed())
            .arg(QString::number(imageVersion, 16)
                     .rightJustified(8, '0')
                     .toUpper()));

    ui->plainTextEditLog->appendPlainText("[发送] " + QString::fromLatin1(startFrame.toHex(' ').toUpper()));

}

void MainWindow::onSerialReadyRead()
{
    /* 读取本次已经到达的所有字节，
     * 追加到原有缓存末尾。*/
    responseBuffer.append(serialPort->readAll());
    constexpr qsizetype responseFrameSize = 14;

    /*
     * 数据不足14字节时先不解析，
     * 等下一次readyRead继续追加。
     */
    while (responseBuffer.size() >= responseFrameSize)
    {
        /* 从缓存最前面取出一张14字节应答。*/
        const QByteArray frame = responseBuffer.left(responseFrameSize);

        BootProtocol::ResponseInfo response;

        /* 检查长度、ACK/NACK命令和CRC。*/
        if (!BootProtocol::parseResponse(frame,response))
        {
            ui->plainTextEditLog->appendPlainText("[接收] 应答帧错误：" +
                QString::fromLatin1(frame.toHex(' ').toUpper()));

            /*
             * 当前没有帧头字段。
             * 解析失败时先移除一个字节，
             * 尝试从下一个位置重新同步。
             */
            responseBuffer.remove(0, 1);
            continue;
        }

        /*
         * 成功解析一张应答后，
         * 从缓存中移除完整14字节。
         */
        responseBuffer.remove(0,responseFrameSize);

        ui->plainTextEditLog->appendPlainText("[接收] " +
            QString::fromLatin1(frame.toHex(' ').toUpper()));

        /* 判断ACK还是NACK。*/
        if (response.responseCommand == BootProtocol::CmdAck)
        {
            ui->labelLastResponseValue->setText("ACK");
            ui->plainTextEditLog->appendPlainText(QString("[应答] ACK，原命令=0x%1，" "结果=0x%2，附加值=%3").arg(QString::number(response.requestCommand,16).rightJustified(4, '0').toUpper()).arg(QString::number(response.result,16).rightJustified(4, '0').toUpper()).arg(response.value));

            if ((response.requestCommand == BootProtocol::CmdStartUpdate) &&
                (response.result         == BootProtocol::ResultOk))
            {

                /*
                 * START ACK的value是STM32实际选择的槽位。
                 * Qt只显示结果，不参与选择。
                 */
                if (response.value == BootProtocol::TargetA)
                {
                    ui->plainTextEditLog->appendPlainText("[升级] STM32自动选择A区接收固件");
                }
                else if (response.value == BootProtocol::TargetB)
                {
                    ui->plainTextEditLog->appendPlainText("[升级] STM32自动选择B区接收固件");
                }
                else
                {
                    ui->plainTextEditLog->appendPlainText(QString("[升级] STM32返回未知目标槽位：%1").arg(response.value));
                    ui->labelUpgradeStatusValue->setText("START目标应答错误");
                    return;
                }
                /*START ACK已经到达，停止START应答定时器*/
                ackTimer->stop();
                pendingFrame.clear();
                pendingCommand = 0U;
                retryCount = 0;

                ui->plainTextEditLog->appendPlainText("[升级] STM32已接受START命令");
                /*
                 * START成功后，
                 * currentPacketSequence当前等于0，
                 * 因此这里发送第0包。
                 */
                if (!sendCurrentDataPacket())
                {
                    ui->labelUpgradeStatusValue->setText("DATA发送失败");
                    return;
                }

            }

            else if ((response.requestCommand == BootProtocol::CmdDataPacket) &&
                     (response.result         == BootProtocol::ResultOk))
            {


                if(response.value != currentPacketSequence)
                {
                    ui->labelUpgradeStatusValue->setText("DATA应答序号错误");
                    ui->plainTextEditLog->appendPlainText(
                        QString("[升级] DATA应答序号错误：""期望%1，实际%2").arg(currentPacketSequence).arg(response.value));
                    return;
                }

                ackTimer->stop();
                pendingFrame.clear();
                pendingCommand = 0U;
                retryCount = 0;

                const quint32 completedPackets = currentPacketSequence + 1U;

                ui->plainTextEditLog->appendPlainText(
                    QString(
                        "[升级] DATA包%1写入成功，""完成%2/%3包")
                        .arg(currentPacketSequence)
                        .arg(completedPackets)
                        .arg(totalPacketCount));

                /*更新数据包数量显示。*/
                ui->labelPacketProgressValue->setText(QString("%1 / %2").arg(completedPackets).arg(totalPacketCount));

                /*计算DATA发送百分比。*/
                const int progress = static_cast<int>(completedPackets * 100U / totalPacketCount);
                ui->progressBarUpgrade->setValue(progress);

                /*当前包收到正确ACK后，才允许序号加1*/
                currentPacketSequence++;

                /*
                 * 如果还有DATA包没有发送，
                 * 就发送新的currentPacketSequence。
                 */
                if (currentPacketSequence < totalPacketCount)
                {
                    if (!sendCurrentDataPacket())
                    {
                        ui->labelUpgradeStatusValue->setText("DATA发送失败");
                        return;
                    }
                }
                else
                {
                    ui->progressBarUpgrade->setValue(100);
                    ui->progressBarUpgrade->setFormat("%p% · DATA发送完成");

                    ui->plainTextEditLog->appendPlainText(
                        QString(
                            "[升级] 全部DATA发送完成，"
                            "总包数=%1，BIN大小=%2字节")
                            .arg(totalPacketCount)
                            .arg(firmwareData.size()));

                    /* DATA传输完成后发送END帧。*/
                    if (!sendEndFrame())
                    {
                        ui->labelUpgradeStatusValue->setText("END发送失败");
                        return;
                    }
                }
            }

            /*处理STM32返回的END成功应答。*/
            else if ((response.requestCommand == BootProtocol::CmdEndUpdate) &&
                     (response.result         == BootProtocol::ResultOk))
            {
                /*
                 * STM32通过ACK附加值返回实际接收的数据包数量。
                 *
                 * Qt发送了totalPacketCount包，
                 * STM32也必须确认收到相同数量。
                 */
                if (response.value != totalPacketCount)
                {
                    ui->labelUpgradeStatusValue->setText("END应答包数错误");

                    ui->plainTextEditLog->appendPlainText(QString(
                            "[升级] END应答包数错误："
                            "Qt发送%1包，STM32确认%2包")
                            .arg(totalPacketCount)
                            .arg(response.value));

                    return;
                }

                ackTimer->stop();
                pendingFrame.clear();
                pendingCommand = 0U;
                retryCount = 0;
                /*
                 * END成功只表示：
                 *
                 * 1. 固件已经完整写入备份槽位；
                 * 2. STM32已经完成整个镜像CRC校验；
                 * 3. Flag已经记录pending_slot。
                 *
                 * 新固件此时还没有安装到Run区。
                 */
                ui->labelUpgradeStatusValue->setText("固件接收成功，等待设备复位安装");

                ui->labelPacketProgressValue->setText(
                    QString("%1 / %2")
                        .arg(totalPacketCount)
                        .arg(totalPacketCount));

                ui->progressBarUpgrade->setValue(100);
                ui->progressBarUpgrade->setFormat( "%p% · 等待设备安装");

                ui->plainTextEditLog->appendPlainText(
                    QString(
                        "[升级] 固件接收与CRC校验成功，"
                        "共%1包，BIN大小=%2字节")
                        .arg(totalPacketCount)
                        .arg(firmwareData.size()));

                ui->plainTextEditLog->appendPlainText("[升级] STM32已记录待安装固件，""请复位设备完成安装");

                /*
                 * 当前固件还没有安装，
                 * 不允许在同一次Bootloader会话中再次升级。
                 */
                ui->pushButtonStartUpgrade->setEnabled(false);

                /*
                 * 暂时允许重新选择文件也没有意义，
                 * 等设备复位后再开始下一轮升级。
                 */
                ui->pushButtonBrowseBin->setEnabled(true);
            }
        }
        else
        {
            /*只有当NACK对应当前正在等待的命令时，才结束当前ACK等待。*/
            if (response.requestCommand == pendingCommand)
            {
                ackTimer->stop();
                pendingFrame.clear();
                pendingCommand = 0;
                retryCount = 0;
            }
            ui->labelLastResponseValue->setText("NACK");
            if ((response.requestCommand == BootProtocol::CmdStartUpdate) &&
                (response.result == BootProtocol::BOOT_RESULT_VERSION_ERROR))
            {
                const QString currentVersion = BootProtocol::formatVersion(response.value);
                ui->labelUpgradeStatusValue->setText(QString("版本过低，设备当前版本为%1").arg(currentVersion));
                ui->plainTextEditLog->appendPlainText(QString("[版本拒绝] 请求固件版本低于设备当前版本%1").arg(currentVersion));
            }
            else
            {
                ui->labelUpgradeStatusValue->setText("START失败");
                ui->plainTextEditLog->appendPlainText(
                    QString("[应答] NACK，原命令=0x%1，"
                        "错误=0x%2，附加值=%3")
                        .arg(QString::number(response.requestCommand,16)
                                .rightJustified(4, '0')
                                .toUpper())
                        .arg(QString::number(response.result,16)
                                .rightJustified(4, '0')
                                .toUpper())
                        .arg(response.value));
                ui->progressBarUpgrade->setValue(0);
                ui->progressBarUpgrade->setFormat("0% · 版本拒绝");
            }
            /* NACK后允许用户重新选择并重试。*/
            ui->pushButtonBrowseBin->setEnabled(true);
            ui->progressBarUpgrade->setValue(0);
            ui->progressBarUpgrade->setFormat("0% · 升级失败");
            ui->pushButtonStartUpgrade->setEnabled(serialPort->isOpen() && !firmwareData.isEmpty());
        }
    }
}

bool MainWindow::sendCurrentDataPacket()
{
    /*必须已经打开协议串口。*/
    if (!serialPort->isOpen())
    {
        ui->plainTextEditLog->appendPlainText("[升级] DATA发送失败：串口未打开");
        return false;
    }

    /*
     * 当前包序号不能超过总包数。
     *
     * 例如总共22包时，
     * 合法序号是0～21。
     */
    if (currentPacketSequence >= totalPacketCount)
    {
        ui->plainTextEditLog->appendPlainText(
            QString(
                "[升级] DATA包序号越界："
                "当前=%1，总包数=%2")
                .arg(currentPacketSequence)
                .arg(totalPacketCount));

        return false;
    }

    constexpr qsizetype packetDataSize = 256;

    /*
     * 根据包序号计算这一包在完整BIN中的起始位置。
     *
     * 第0包：0 × 256 = 0
     * 第1包：1 × 256 = 256
     * 第2包：2 × 256 = 512
     */
    const qsizetype offset = static_cast<qsizetype>(currentPacketSequence) * packetDataSize;

    /*
     * 从完整BIN中截取当前包的数据。
     *
     * 最后一包不足256字节时，
     * mid()只返回实际剩余数据。
     */
    const QByteArray packetData = firmwareData.mid(offset,packetDataSize);

    if (packetData.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText(
            QString(
                "[升级] DATA截取失败："
                "序号=%1，偏移=%2")
                .arg(currentPacketSequence)
                .arg(offset));

        return false;
    }

    /*
     * 使用当前BIN数据和当前包序号
     * 构造完整DATA协议帧。
     */
    const QByteArray dataFrame = BootProtocol::buildDataFrame(packetData,currentPacketSequence);

    if (dataFrame.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText(QString("[升级] DATA包%1组帧失败").arg(currentPacketSequence));
        return false;
    }

    /*
     * 把完整DATA帧放入Qt串口发送缓存。
     */
    const qint64 written = serialPort->write(dataFrame);

    if (written != dataFrame.size())
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[升级] DATA包%1发送失败："
                "需要%2字节，实际%3字节，错误：%4")
                .arg(currentPacketSequence)
                .arg(dataFrame.size())
                .arg(written)
                .arg(serialPort->errorString()));

        return false;
    }

    /*立即尝试把Qt内部缓存交给串口驱动。*/
    if (!serialPort->flush())
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[升级] DATA包%1刷新发送缓存失败：%2")
                .arg(currentPacketSequence)
                .arg(serialPort->errorString()));

        return false;
    }

    /*等待Qt内部待发送数据全部交给串口驱动。*/
    while (serialPort->bytesToWrite() > 0)
    {
        if (!serialPort->waitForBytesWritten(1000))
        {
            ui->plainTextEditLog->appendPlainText(
                QString("[升级] DATA包%1发送超时：%2")
                    .arg(currentPacketSequence)
                    .arg(serialPort->errorString()));

            return false;
        }
    }

    /*
     * 当前DATA帧已经成功交给串口驱动。
     *
     * 这是一个新的DATA请求，
     * 重发次数从0开始。
     */
    retryCount = 0;

    /*
     * 保存当前完整DATA帧。
     *
     * 如果本包ACK丢失，
     * 超时函数将重新发送完全相同的DATA帧，
     * 包括相同的数据、序号和CRC。
     */
    pendingFrame = dataFrame;

    /* 当前等待的是DATA命令的应答。*/
    pendingCommand = static_cast<quint16>(BootProtocol::CmdDataPacket);

    /* 开始等待本包DATA的ACK。*/
    ackTimer->start(AckTimeoutMs);

    ui->labelUpgradeStatusValue->setText(QString("DATA包%1已发送，等待ACK").arg(currentPacketSequence));
    ui->plainTextEditLog->appendPlainText(
        QString(
            "[发送] DATA包%1，"
            "BIN偏移=%2，数据=%3字节，完整帧=%4字节")
            .arg(currentPacketSequence)
            .arg(offset)
            .arg(packetData.size())
            .arg(dataFrame.size()));

    return true;
}

bool MainWindow::sendEndFrame()
{
    /*发送前检查串口。*/
    if (!serialPort->isOpen())
    {
        ui->plainTextEditLog->appendPlainText("[升级] END发送失败：串口未打开");
        return false;
    }

    /*如果总包数为0，说明升级参数不正常。*/
    if (totalPacketCount == 0U)
    {
        ui->plainTextEditLog->appendPlainText("[升级] END发送失败：DATA总包数为0");
        return false;
    }

    /*
     * 构造完整END帧。
     *
     * END没有DATA，
     * totalPacketCount放在RESERVE字段中。
     */
    const QByteArray endFrame = BootProtocol::buildEndFrame(totalPacketCount);

    if (endFrame.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[升级] END组帧失败");
        return false;
    }

    /*把END帧写入Qt串口发送缓存。*/
    const qint64 written = serialPort->write(endFrame);

    if (written != endFrame.size())
    {
        ui->plainTextEditLog->appendPlainText(
            QString(
                "[升级] END发送失败："
                "需要%1字节，实际写入%2字节，错误：%3")
                .arg(endFrame.size())
                .arg(written)
                .arg(serialPort->errorString()));

        return false;
    }

    /*尽快把Qt发送缓存交给串口驱动。*/
    if (!serialPort->flush())
    {
        ui->plainTextEditLog->appendPlainText(
            QString("[升级] END刷新发送缓存失败：%1")
                .arg(serialPort->errorString()));

        return false;
    }

    /*
     * 等待Qt中的待发送数据全部交给串口驱动。
     */
    while (serialPort->bytesToWrite() > 0)
    {
        if (!serialPort->waitForBytesWritten(1000))
        {
            ui->plainTextEditLog->appendPlainText(
                QString("[升级] END发送超时：%1")
                    .arg(serialPort->errorString()));

            return false;
        }
    }

    /*
     * END已经成功交给串口驱动。
     *
     * 这是一个新的END请求，
     * 重发次数从0开始。
     */
    retryCount = 0;

    /*
     * 保存完整END帧。
     *
     * 如果END ACK丢失，
     * 超时函数会重新发送完全相同的END帧。
     */
    pendingFrame = endFrame;

    /*记录当前等待的是END应答。*/
    pendingCommand = static_cast<quint16>(BootProtocol::CmdEndUpdate);

    /*启动END ACK等待。*/
    ackTimer->start(AckTimeoutMs);

    ui->labelUpgradeStatusValue->setText("END已发送，等待ACK");

    ui->plainTextEditLog->appendPlainText(
        QString(
            "[发送] END，总包数=%1，完整帧=%2字节")
            .arg(totalPacketCount)
            .arg(endFrame.size()));

    ui->plainTextEditLog->appendPlainText(
        "[发送] " +
        QString::fromLatin1(
            endFrame.toHex(' ').toUpper()));

    return true;


}

void MainWindow::onAckTimeout()
{
    /*
     * 如果没有保存待应答帧，
     * 说明当前超时状态不正常。
     */
    if (pendingFrame.isEmpty())
    {
        ui->plainTextEditLog->appendPlainText("[超时] 没有找到待重发协议帧");
        return;
    }

    /*串口已经关闭，无法继续重发。*/
    if (!serialPort->isOpen())
    {
        ui->labelUpgradeStatusValue->setText("应答超时，串口已关闭");
        ui->plainTextEditLog->appendPlainText("[超时] 串口已经关闭，升级终止");
        pendingFrame.clear();
        retryCount = 0;
        return;
    }

    /*
     * 最多允许重发3次。
     */
    if (retryCount >= MaxRetryCount)
    {
        ui->labelUpgradeStatusValue->setText("START应答超时，升级终止");
        ui->plainTextEditLog->appendPlainText("[升级失败] START连续3次重发后仍未收到应答");

        /*当前请求结束，清除待重发帧和重发次数。*/
        pendingFrame.clear();
        retryCount = 0;

        /*恢复界面，允许用户重新尝试。*/
        ui->pushButtonBrowseBin->setEnabled(true);
        ui->pushButtonStartUpgrade->setEnabled(serialPort->isOpen() && !firmwareData.isEmpty());
        return;
    }

    /* 准备执行一次重发。*/
    retryCount++;

    /*重新发送之前保存的完整START帧。*/
    const qint64 written = serialPort->write(pendingFrame);

    if (written != pendingFrame.size())
    {
        ui->labelUpgradeStatusValue->setText("START重发失败");

        ui->plainTextEditLog->appendPlainText(
            QString(
                "[重发失败] 需要发送%1字节，"
                "实际写入%2字节，错误：%3")
                .arg(pendingFrame.size())
                .arg(written)
                .arg(serialPort->errorString()));

        return;
    }

    /*
     * 尽快把Qt发送缓存交给串口驱动。
     */
    if (!serialPort->flush())
    {
        ui->labelUpgradeStatusValue->setText("START重发失败");
        ui->plainTextEditLog->appendPlainText(
            QString("[重发失败] 刷新发送缓存失败：%1")
                .arg(serialPort->errorString()));

        return;
    }

    /*等待Qt中的数据全部交给串口驱动。*/
    while (serialPort->bytesToWrite() > 0)
    {
        if (!serialPort->waitForBytesWritten(1000))
        {
            ui->labelUpgradeStatusValue->setText("START重发超时");
            ui->plainTextEditLog->appendPlainText(QString("[重发失败] 串口发送超时：%1").arg(serialPort->errorString()));
            return;
        }
    }

    /* 根据pendingCommand判断当前重发的是哪种帧。*/
    QString commandName;

    switch (pendingCommand)
    {

        case BootProtocol::CmdStartUpdate:
            commandName = "START";
            break;

        case BootProtocol::CmdDataPacket:
            commandName = "DATA";
            break;

        case BootProtocol::CmdEndUpdate:
            commandName = "END";
            break;

        default:
            commandName = QString("未知命令0x%1").arg(QString::number(pendingCommand,16) .rightJustified(4, '0').toUpper());
            break;
    }

    ui->labelUpgradeStatusValue->setText(QString("%1第%2次重发，等待ACK").arg(commandName).arg(retryCount));

    ui->plainTextEditLog->appendPlainText(
        QString(
            "[重发] %1第%2/%3次，"
            "完整帧=%4字节")
            .arg(commandName)
            .arg(retryCount)
            .arg(MaxRetryCount)
            .arg(pendingFrame.size()));

    /*本次重发已经完成，重新等待3秒。*/
    ackTimer->start(AckTimeoutMs);
}
