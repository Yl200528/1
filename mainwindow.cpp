#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QDebug>
#include <QModbusRtuSerialClient>
#include <QModbusTcpClient>
#include <QModbusReply>
#include <QDateTime>
#include <QSerialPort>

// ================= 构造函数 =================
// 构造函数负责初始化窗口对象、绑定协议切换信号，以及设置 ASCII 超时定时器。
// 这里把 modbusClient 先置空，是因为 TCP/RTU 的具体客户端对象要在 connectModbus() 里按协议动态创建，
// 如果提前创建会让协议切换逻辑变复杂，而且也更容易出现信号重复绑定的问题。
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , modbusClient(nullptr) // 先置空，等连接时按当前协议动态创建
    , asciiSerialPort(nullptr)
    , asciiResponseTimer(new QTimer(this))
{
    ui->setupUi(this); // 生成界面控件并绑定基础布局

    connect(ui->comboBox_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProtocolChanged); // 用户切换协议时触发状态更新
    asciiResponseTimer->setSingleShot(true); // ASCII 超时只触发一次，不需要循环定时
    connect(asciiResponseTimer, &QTimer::timeout, this, &MainWindow::onAsciiTimeout); // 连接超时处理槽

    setupSerialPortUi(); // 初始化串口、波特率和校验位控件
    setupModbusUi(); // 初始化功能码、地址和写入参数控件

    connect(ui->comboBox_funcCode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
                onModbusStateChanged(modbusClient ? modbusClient->state() : QModbusDevice::UnconnectedState);
            });
    // 注意：此时 modbusClient 还是 nullptr，所以不会在这里直接连接信号。
    // 真正的连接绑定放在 connectModbus() 中，避免对象尚未创建时访问空指针。
}

MainWindow::~MainWindow()
{
    if (modbusClient) {
        modbusClient->disconnectDevice(); // 断开 Modbus 设备连接
        delete modbusClient; // 释放客户端对象
    }
    if (asciiSerialPort)
        asciiSerialPort->close(); // 关闭 ASCII 串口
    delete ui; // 释放 UI 对象
}

// ================= 初始化界面逻辑 =================
// 程序启动时，需要把所有串口和协议参数初始化好，保证用户一打开界面就能直接选择端口和参数，
// 而不是先手动填写一堆默认值。这样可以降低误操作和“参数未初始化”的问题。
void MainWindow::setupSerialPortUi()
{
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        ui->comboBox_port->addItem(info.portName()); // 自动枚举当前可用串口
    }

    ui->comboBox_baudRate->addItems({"1200", "2400", "4800", "9600", "19200",
                              "38400", "57600", "115200"});
    ui->comboBox_baudRate->setCurrentText("9600"); // 默认常用波特率

    ui->comboBox_parity->addItems({"无校验", "偶校验", "奇校验"});
    ui->comboBox_parity->setCurrentText("无校验");

    ui->comboBox_stopBits->addItems({"1", "1.5", "2"});
    ui->comboBox_stopBits->setCurrentText("1");

    ui->comboBox_dataBits->addItems({"8", "7", "6", "5"});
    ui->comboBox_dataBits->setCurrentText("8");

    ui->comboBox_protocol->setCurrentIndex(0); // 默认首选协议
    onProtocolChanged(ui->comboBox_protocol->currentIndex()); // 立即更新界面状态

    // TCP 默认值
    ui->lineEdit_ip->setText("127.0.0.1");
    ui->lineEdit_port->setText("502");

    // 初始化 UI 状态（此时未连接）
    onModbusStateChanged(QModbusDevice::UnconnectedState);
}

void MainWindow::setupModbusUi()
{
    ui->comboBox_funcCode->addItem("01 ", 1);
    ui->comboBox_funcCode->addItem("02 ", 2);
    ui->comboBox_funcCode->addItem("03 ", 3);
    ui->comboBox_funcCode->addItem("04 ", 4);
    ui->comboBox_funcCode->addItem("05 ", 5);
    ui->comboBox_funcCode->addItem("06 ", 6);
    ui->comboBox_funcCode->addItem("10 ", 16);

    ui->lineEdit_slaveId->setText("1"); // 默认从站地址为 1
    ui->lineEdit_startAddr->setText("0"); // 默认起始寄存器地址为 0
    ui->lineEdit_quantity->setText("1"); // 默认读取一个寄存器
    ui->lineEdit_writeValue->setText("0"); // 默认写入值为 0
}

// ================= 核心连接逻辑 =================
// 这里把 “协议分支 + 连接参数 + 状态绑定” 封装成统一入口，
// 这样外部只需要调用 connectModbus()，不需要自己判断当前是 TCP、RTU 还是 ASCII。
bool MainWindow::connectModbus()
{
    // 1. 清理旧的 client，避免信号重复绑定和资源泄漏
    if (modbusClient) {
        modbusClient->disconnectDevice();
        modbusClient->deleteLater();
        modbusClient = nullptr;
    }
    if (asciiSerialPort) {
        asciiSerialPort->close();
        asciiSerialPort->deleteLater();
        asciiSerialPort = nullptr;
    }

    QString protocol = ui->comboBox_protocol->currentText();
    qDebug() << "========== 尝试连接 ==========";
    qDebug() << "当前协议:" << protocol;

    if (protocol == "ASCII" || protocol == "文本模式")
        return connectAscii(); // ASCII 模式单独走串口逻辑，不走 QModbusClient

    // 2. 根据协议实例化不同的 Client
    if (protocol == "TCP") {
        // ========== TCP 模式 ==========
        modbusClient = new QModbusTcpClient(this); // 通过 TCP 连接远程设备
        modbusClient->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter, ui->lineEdit_ip->text());
        modbusClient->setConnectionParameter(
            QModbusDevice::NetworkPortParameter, ui->lineEdit_port->text().toInt());
        qDebug() << "目标IP:" << ui->lineEdit_ip->text() << "端口:" << ui->lineEdit_port->text();
    }
    else {
        // ========== RTU 模式 ==========
        modbusClient = new QModbusRtuSerialClient(this); // 通过串口进行 RTU 通信

        modbusClient->setConnectionParameter(
            QModbusDevice::SerialPortNameParameter, ui->comboBox_port->currentText());

        QSerialPort::Parity parity = QSerialPort::NoParity;
        QString parityText = ui->comboBox_parity->currentText();
        if (parityText == "偶校验" || parityText == "Even")
            parity = QSerialPort::EvenParity;
        else if (parityText == "奇校验" || parityText == "Odd")
            parity = QSerialPort::OddParity;
        modbusClient->setConnectionParameter(QModbusDevice::SerialParityParameter, parity);

        modbusClient->setConnectionParameter(
            QModbusDevice::SerialBaudRateParameter, ui->comboBox_baudRate->currentText().toInt());

        modbusClient->setConnectionParameter(
            QModbusDevice::SerialDataBitsParameter,
            static_cast<QSerialPort::DataBits>(ui->comboBox_dataBits->currentText().toInt()));

        QSerialPort::StopBits stopBits = QSerialPort::OneStop;
        QString stopText = ui->comboBox_stopBits->currentText();
        if (stopText == "1.5") stopBits = QSerialPort::OneAndHalfStop;
        else if (stopText == "2") stopBits = QSerialPort::TwoStop;
        modbusClient->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, stopBits);
        qDebug() << "目标串口:" << ui->comboBox_port->currentText() << "波特率:" << ui->comboBox_baudRate->currentText();
    }

    // 3. 通用设置
    modbusClient->setTimeout(1000); // 请求超时 1 秒，避免长时间阻塞
    modbusClient->setNumberOfRetries(3); // 失败后重试 3 次，提高稳定性

    // 4. 重新绑定信号，因为这是新创建的客户端对象
    connect(modbusClient, &QModbusClient::stateChanged, this, &MainWindow::onModbusStateChanged);
    connect(modbusClient, &QModbusClient::errorOccurred, this, &MainWindow::onModbusErrorOccurred);

    // 5. 发起连接
    return modbusClient->connectDevice();
}

void MainWindow::disconnectModbus()
{
    if (modbusClient)
        modbusClient->disconnectDevice(); // 断开设备连接
    if (asciiResponseTimer->isActive())
        asciiResponseTimer->stop(); // 停止 ASCII 超时计时器
    asciiReceiveBuffer.clear(); // 清空缓存区，避免残留帧干扰下一次通信
    if (asciiSerialPort) {
        asciiSerialPort->close(); // 关闭 ASCII 串口
        onModbusStateChanged(QModbusDevice::UnconnectedState); // 强制更新界面状态
    }
}

// ================= 状态与 UI 联动 =================
// 连接状态和控件可用性必须同步，否则用户可能在连接中修改串口参数，导致状态和设备配置不一致。
// 这里通过统一状态槽来更新按钮文本和界面禁用状态，能最大程度减少“配置错乱”的风险。
void MainWindow::onModbusStateChanged(int state)
{
    bool connected = (state == QModbusDevice::ConnectedState);
    ui->pushButton_openClose->setText(connected ? "断开连接" : "连接/断开");

    // 已连接时锁定协议切换，避免中途切换协议导致对象失效
    ui->comboBox_protocol->setEnabled(!connected);

    bool isTcp = (ui->comboBox_protocol->currentText() == "TCP");
    bool isAsciiText = (ui->comboBox_protocol->currentText() == "文本模式");

    int funcCode = ui->comboBox_funcCode->currentData().toInt();
     bool isWriteFunc = (funcCode == 5 || funcCode == 6 || funcCode == 16);
    // 串口控件：RTU/ASCII 模式且未连接时才允许编辑
    ui->label_port->setEnabled(!isTcp && !connected);
    ui->comboBox_port->setEnabled(!isTcp && !connected);
    ui->label_baudRate->setEnabled(!isTcp && !connected);
    ui->comboBox_baudRate->setEnabled(!isTcp && !connected);
    ui->label_parity->setEnabled(!isTcp && !connected);
    ui->comboBox_parity->setEnabled(!isTcp && !connected);
    ui->label_dataBits->setEnabled(!isTcp && !connected);
    ui->comboBox_dataBits->setEnabled(!isTcp && !connected);
    ui->label_stopBits->setEnabled(!isTcp && !connected);
    ui->comboBox_stopBits->setEnabled(!isTcp && !connected);

    // TCP 控件：TCP 模式且未连接时才允许编辑
    ui->label_2->setEnabled(isTcp && !connected);       // IP 标签
    ui->lineEdit_ip->setEnabled(isTcp && !connected);   // IP 输入框
    ui->label_3->setEnabled(isTcp && !connected);       // 端口标签
    ui->lineEdit_port->setEnabled(isTcp && !connected); // 端口输入框

    if (!isAsciiText) {
        // 正常 Modbus 模式且未连接：启用基础参数
        ui->label_slaveId->setEnabled(true);
        ui->lineEdit_slaveId->setEnabled(true);
        ui->label_funcCode->setEnabled(true);
        ui->comboBox_funcCode->setEnabled(true);
        ui->label_startAddr->setEnabled(true);
        ui->lineEdit_startAddr->setEnabled(true);

        // 核心逻辑：读操作启用数量，写操作启用写入值
        ui->label_quantity->setEnabled(!isWriteFunc);
        ui->lineEdit_quantity->setEnabled(!isWriteFunc);

        ui->label_writeValue->setEnabled(isWriteFunc);
        ui->lineEdit_writeValue->setEnabled(isWriteFunc);
    } else {
        // 文本模式，或者已连接状态：Modbus 参数全部禁用
        ui->label_slaveId->setEnabled(false);
        ui->lineEdit_slaveId->setEnabled(false);
        ui->label_funcCode->setEnabled(false);
        ui->comboBox_funcCode->setEnabled(false);
        ui->label_startAddr->setEnabled(false);
        ui->lineEdit_startAddr->setEnabled(false);

        ui->label_quantity->setEnabled(false);
        ui->lineEdit_quantity->setEnabled(false);
        ui->label_writeValue->setEnabled(false);
        ui->lineEdit_writeValue->setEnabled(false);
    }
}

void MainWindow::onModbusErrorOccurred(QModbusDevice::Error error)
{
    Q_UNUSED(error);
    if (modbusClient && modbusClient->error() != QModbusDevice::NoError) {
        qDebug() << "Modbus error:" << modbusClient->errorString();
    }
}

// 协议切换时需要同步更新输入字段，ASCII 文本和 Modbus 帧使用的字段并不一致。
void MainWindow::onProtocolChanged(int index)
{
    Q_UNUSED(index);

    const bool isAsciiText = ui->comboBox_protocol->currentText() == "文本模式";
    ui->label_writeValue->setText(isAsciiText ? "发送文本" : "写入值");
    ui->lineEdit_writeValue->setPlaceholderText(
        isAsciiText ? "输入  文本" : QString());
    ui->label_slaveId->setEnabled(!isAsciiText);
    ui->lineEdit_slaveId->setEnabled(!isAsciiText);
    ui->label_funcCode->setEnabled(!isAsciiText);
    ui->comboBox_funcCode->setEnabled(!isAsciiText);
    ui->label_startAddr->setEnabled(!isAsciiText);
    ui->lineEdit_startAddr->setEnabled(!isAsciiText);
    ui->label_quantity->setEnabled(!isAsciiText);
    ui->lineEdit_quantity->setEnabled(!isAsciiText);

    // 用户主动切换协议时，若当前已有连接，需要先断开，避免旧连接和新协议参数相互干扰。
    if (modbusClient && modbusClient->state() == QModbusDevice::ConnectedState) {
        disconnectModbus();
    }
    if (asciiSerialPort && asciiSerialPort->isOpen())
        disconnectModbus();

    // 刷新 UI 状态，确保界面和设备状态保持一致
    onModbusStateChanged(QModbusDevice::UnconnectedState);
}

// ================= 工具函数：校验和与报文构造 =================
// Modbus 协议在 RTU/ASCII 两种传输层上都依赖校验值，校验错误会直接导致设备拒绝响应。
// CRC16 适用于 RTU，LRC 适用于 ASCII；两者都要严格按照 Modbus 规范实现，否则采样报文会被设备判定无效。
quint16 MainWindow::crc16Modbus(const QByteArray &data)
{
    quint16 crc = 0xFFFF; // CRC16 初始值
    for (unsigned char byte : data) {
        crc ^= static_cast<quint16>(byte); // 逐字节异或
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001) crc = static_cast<quint16>((crc >> 1) ^ 0xA001); // 生成多项式校验
            else crc >>= 1; // 直接右移
        }
    }
    return crc; // 返回最终 CRC 值
}

quint8 MainWindow::lrcModbus(const QByteArray &data)
{
    quint8 sum = 0; // LRC 累加和初始值
    for (unsigned char byte : data)
        sum = static_cast<quint8>(sum + byte); // 对字节逐个累加
    return static_cast<quint8>(-sum); // 取反码作为 LRC
}

QByteArray MainWindow::buildModbusAsciiFrame(int slaveId, int funcCode, int startAddr,
                                             int quantity, const QVector<quint16> &values)
{
    // ASCII 请求帧格式是「: + 数据 + LRC + \r\n」；
    // 这里先生成标准 Modbus 请求帧，再去掉末尾 CRC16，替换为 ASCII 专用 LRC，
    // 这样既复用了报文编排逻辑，又保证 ASCII 和 RTU 的字段结构一致。
    QByteArray frame = buildModbusRequestFrame(slaveId, funcCode, startAddr, quantity, values);
    frame.chop(2); // 去掉 RTU 末尾的 CRC16
    frame.append(static_cast<char>(lrcModbus(frame))); // 替换成 ASCII 的 LRC
    return ":" + frame.toHex().toUpper() + "\r\n"; // 以 ASCII 十六进制字符串发出
}

bool MainWindow::connectAscii()
{
    // ASCII 模式不走 QModbusClient，而是直接使用 QSerialPort，
    // 因为 ASCII 帧需要单独处理起始符、LRC 校验和分包处理，不是传统 RTU 的纯串口封装。
    asciiSerialPort = new QSerialPort(this);
    asciiSerialPort->setPortName(ui->comboBox_port->currentText());
    asciiSerialPort->setBaudRate(ui->comboBox_baudRate->currentText().toInt());

    QSerialPort::Parity parity = QSerialPort::NoParity;
    const QString parityText = ui->comboBox_parity->currentText();
    if (parityText == "偶校验" || parityText == "Even")
        parity = QSerialPort::EvenParity;
    else if (parityText == "奇校验" || parityText == "Odd")
        parity = QSerialPort::OddParity;
    asciiSerialPort->setParity(parity);
    asciiSerialPort->setDataBits(
        static_cast<QSerialPort::DataBits>(ui->comboBox_dataBits->currentText().toInt()));

    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    const QString stopText = ui->comboBox_stopBits->currentText();
    if (stopText == "1.5")
        stopBits = QSerialPort::OneAndHalfStop;
    else if (stopText == "2")
        stopBits = QSerialPort::TwoStop;
    asciiSerialPort->setStopBits(stopBits);

    connect(asciiSerialPort, &QSerialPort::readyRead, this, &MainWindow::onAsciiReadyRead);
    connect(asciiSerialPort, &QSerialPort::errorOccurred,
            this, &MainWindow::onAsciiErrorOccurred);
    if (!asciiSerialPort->open(QIODevice::ReadWrite))
        return false;

    asciiReceiveBuffer.clear();
    onModbusStateChanged(QModbusDevice::ConnectedState);
    return true;
}

void MainWindow::onAsciiReadyRead()
{
    if (!asciiSerialPort)
        return;
    const QByteArray received = asciiSerialPort->readAll();
    if (ui->comboBox_protocol->currentText() == "文本模式") {
        // 文本模式不走 Modbus 解析，而是直接把字符原样显示在 RX 日志里。
        logRx(QString("[%1] RX← %2")
                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                       QString::fromLatin1(received)));
        return;
    }
    asciiReceiveBuffer.append(received); // 缓存串口收到的数据，等待完整 ASCII 帧组装

    while (true) {
        const int start = asciiReceiveBuffer.indexOf(':'); // 查找帧起始符 ':'
        if (start < 0) {
            asciiReceiveBuffer.clear(); // 没有起始符则丢弃脏数据
            return;
        }
        if (start > 0)
            asciiReceiveBuffer.remove(0, start); // 去掉前面多余的无效字节

        const int end = asciiReceiveBuffer.indexOf("\r\n"); // 找到一帧的结束标志
        if (end < 0)
            return; // 当前未接收完一帧，等待后续数据
        const QByteArray line = asciiReceiveBuffer.left(end + 2); // 取出完整帧
        asciiReceiveBuffer.remove(0, end + 2); // 处理完后从缓存中移除
        handleAsciiResponse(line); // 交给解析函数校验和解码
    }
}

void MainWindow::handleAsciiResponse(const QByteArray &frame)
{
    // ASCII 帧结构：':' + 数据 + LRC + '\r\n'，
    // 这里先做帧合法性校验，再核对 LRC 和从站/功能码是否和当前请求一致，避免误处理无效响应。
    const QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    QByteArray body = frame.mid(1, frame.size() - 3);
    if (body.isEmpty() || (body.size() % 2) != 0) {
        logRx(QString("[%1] RX← 无效的 Modbus ASCII 帧: %2")
                  .arg(timeStr, QString::fromLatin1(frame.trimmed())));
        return;
    }

    const QByteArray data = QByteArray::fromHex(body);
    if (data.size() < 3 || data.toHex().toUpper() != body.toUpper()) {
        logRx(QString("[%1] RX← 无效的 Modbus ASCII 十六进制数据: %2")
                  .arg(timeStr, QString::fromLatin1(frame.trimmed())));
        return;
    }
    if (lrcModbus(data) != 0) {
        logRx(QString("[%1] RX← LRC 校验失败: %2")
                  .arg(timeStr, QString::fromLatin1(frame.trimmed())));
        return;
    }

    const int slaveId = static_cast<quint8>(data.at(0));
    const int funcCode = static_cast<quint8>(data.at(1));
    const int expectedFunc = asciiPendingFuncCode;
    const int expectedSlave = asciiPendingSlaveId;
    const QByteArray payload = data.left(data.size() - 1);
    logRx(QString("[%1] RX← %2").arg(timeStr, QString::fromLatin1(frame.trimmed())));

    if (slaveId != expectedSlave || (funcCode != expectedFunc && funcCode != (expectedFunc | 0x80))) {
        logRx(QString("[%1] 响应地址或功能码与请求不匹配").arg(timeStr));
        return;
    }
    asciiResponseTimer->stop();

    if (funcCode & 0x80) {
        const int exceptionCode = payload.size() > 2
                                      ? static_cast<quint8>(payload.at(2))
                                      : -1;
        logRx(QString("[%1] Modbus 异常响应，异常码: %2")
                  .arg(timeStr,exceptionCode < 0 ? QStringLiteral("无效") :
                                           QString::number(exceptionCode, 16).toUpper()));
        return;
    }

    QStringList decoded;
    if (funcCode >= 1 && funcCode <= 4) {
        if (payload.size() < 3) {
            logRx(QString("[%1] 响应数据长度无效").arg(timeStr));
            return;
        }
        const int byteCount = static_cast<quint8>(payload.at(2));
        const int expectedByteCount = (funcCode == 1 || funcCode == 2)
                                          ? (asciiPendingQuantity + 7) / 8
                                          : asciiPendingQuantity * 2;
        if (payload.size() != byteCount + 3 || byteCount != expectedByteCount) {
            logRx(QString("[%1] 响应字节数无效").arg(timeStr));
            return;
        }
        if (funcCode == 1 || funcCode == 2) {
            // 线圈/离散输入返回的是位图，需要逐位解析到 0/1 的值列表。
            for (int bit = 0; bit < asciiPendingQuantity; ++bit) {
                const int byteIndex = 3 + bit / 8;
                decoded.append(QString::number(
                    (static_cast<quint8>(payload.at(byteIndex)) >> (bit % 8)) & 0x01));
            }
        } else {
            // 寄存器返回的是 16 位值，每两个字节组合为一个寄存器值。
            if ((byteCount % 2) != 0) {
                logRx(QString("[%1] 寄存器响应字节数无效").arg(timeStr));
                return;
            }
            for (int i = 3; i + 1 < payload.size(); i += 2) {
                const quint16 value =
                    (static_cast<quint16>(static_cast<quint8>(payload.at(i))) << 8) |
                    static_cast<quint8>(payload.at(i + 1));
                decoded.append(QString::number(value));
            }
        }
    } else if (payload.size() == 6) {
        // 写单个寄存器或线圈返回的是两个 16 位值，需要按高低字节拼装。
        decoded.append(QString::number(
            (static_cast<quint16>(static_cast<quint8>(payload.at(2))) << 8) |
            static_cast<quint8>(payload.at(3))));
        decoded.append(QString::number(
            (static_cast<quint16>(static_cast<quint8>(payload.at(4))) << 8) |
            static_cast<quint8>(payload.at(5))));
    } else {
        logRx(QString("[%1] 写入响应长度无效").arg(timeStr));
        return;
    }
    logRx(QString("[%1] Dec: %2 %3 %4")
              .arg(timeStr)
              .arg(slaveId, 2, 16, QChar('0'))
              .arg(funcCode, 2, 16, QChar('0'))
              .arg(decoded.join(" ")));
}

// ASCII 请求超时后，需要清理等待状态，否则后续的响应会被错误地当作当前请求的返回值。
void MainWindow::onAsciiTimeout()
{
    logRx(QString("[%1] RX← 响应超时")
              .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")));
    asciiPendingFuncCode = 0; // 清理等待状态，避免后续请求误判
}

// 串口发生异常时要记录日志并将连接状态重置，避免一直停留在“异常但未释放”的状态。
void MainWindow::onAsciiErrorOccurred(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || !asciiSerialPort)
        return;
    logRx(QString("[%1] 串口错误: %2")
              .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                   asciiSerialPort->errorString()));
    if (error == QSerialPort::ResourceError) {
        asciiResponseTimer->stop();
        asciiSerialPort->close();
        onModbusStateChanged(QModbusDevice::UnconnectedState);
    }
}

// 这是 Modbus 报文的公共构造函数：
// 根据功能码不同，选择不同的字段布局，最终生成 RTU 帧尾部的 CRC16。
// 这样所有协议分支都可以复用一套“请求报文生成逻辑”，减少重复代码。
QByteArray MainWindow::buildModbusRequestFrame(int slaveId, int funcCode, int startAddr,
                                              int quantity, const QVector<quint16> &values)
{
    QByteArray frame;
    frame.append(static_cast<char>(slaveId)); // 从站地址
    frame.append(static_cast<char>(funcCode)); // 功能码

    if (funcCode == 1 || funcCode == 2 || funcCode == 3 || funcCode == 4) {
        // 读寄存器/线圈的请求格式：地址高字节 + 地址低字节 + 数量高字节 + 数量低字节
        frame.append(static_cast<char>((startAddr >> 8) & 0xFF));
        frame.append(static_cast<char>(startAddr & 0xFF));
        frame.append(static_cast<char>((quantity >> 8) & 0xFF));
        frame.append(static_cast<char>(quantity & 0xFF));
    } else if (funcCode == 5 || funcCode == 6) {
        // 写单个线圈/寄存器的请求格式：地址 + 数值
        const quint16 value = values.isEmpty() ? 0 : values.first();
        frame.append(static_cast<char>((startAddr >> 8) & 0xFF));
        frame.append(static_cast<char>(startAddr & 0xFF));
        frame.append(static_cast<char>((value >> 8) & 0xFF));
        frame.append(static_cast<char>(value & 0xFF));
    } else if (funcCode == 16) {
        // 写多个寄存器的请求格式：起始地址 + 寄存器个数 + 字节数 + 数据区
        const int regCount = values.size();
        frame.append(static_cast<char>((startAddr >> 8) & 0xFF));
        frame.append(static_cast<char>(startAddr & 0xFF));
        frame.append(static_cast<char>((regCount >> 8) & 0xFF));
        frame.append(static_cast<char>(regCount & 0xFF));
        frame.append(static_cast<char>(regCount * 2));
        for (quint16 value : values) {
            frame.append(static_cast<char>((value >> 8) & 0xFF));
            frame.append(static_cast<char>(value & 0xFF));
        }
    }

    const quint16 crc = crc16Modbus(frame); // 计算 CRC16 校验值
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
    return frame;
}

// 将二进制报文转换成“空格分隔的大写十六进制字符串”，用于日志展示和调试。
QString MainWindow::formatHexFrame(const QByteArray &frame)
{
    return QString::fromLatin1(frame.toHex(' ').toUpper());
}

// 根据 Modbus 功能码选择对应寄存器类型，
// 这样后续发送读取/写入请求时可以正确调用 QModbusDataUnit 的相应存储区。
QModbusDataUnit::RegisterType MainWindow::getRegisterType(int funcCode) const
{
    switch (funcCode) {
    case 1: case 5: return QModbusDataUnit::Coils;
    case 2: return QModbusDataUnit::DiscreteInputs;
    case 3: case 6: case 16: return QModbusDataUnit::HoldingRegisters;
    case 4: return QModbusDataUnit::InputRegisters;
    default: return QModbusDataUnit::HoldingRegisters;
    }
}

// 输入校验集中在这里，原因是所有协议入口（TCP/RTU/ASCII）都依赖同一套参数。
// 如果把校验分散到各个发送分支，容易出现某些模式未校验、参数越界的问题。
bool MainWindow::parseInput(int &slaveId, int &funcCode, int &startAddr,
                            int &quantity, QVector<quint16> &values)
{
    bool ok;
    slaveId = ui->lineEdit_slaveId->text().toInt(&ok);
    if (!ok || slaveId < 1 || slaveId > 247) {
        QMessageBox::warning(this, "输入错误", "从机地址应为 1~247");
        return false;
    }

    funcCode = ui->comboBox_funcCode->currentData().toInt();

    startAddr = ui->lineEdit_startAddr->text().toInt(&ok);
    if (!ok || startAddr < 0) {
        QMessageBox::warning(this, "输入错误", "寄存器地址应为非负整数");
        return false;
    }

    quantity = ui->lineEdit_quantity->text().toInt(&ok);
    if (!ok || quantity < 1) {
        QMessageBox::warning(this, "输入错误", "数量应为正整数");
        return false;
    }

    values.clear();
    if (funcCode == 5) {
        // 写单个线圈的值只能是 0 或 1，Modbus 协议中线圈写入通常用 0xFF00 表示 TRUE。
        int value = ui->lineEdit_writeValue->text().toInt(&ok);
        if (!ok || (value != 0 && value != 1)) {
            QMessageBox::warning(this, "输入错误", "线圈写入值应为 0 或 1");
            return false;
        }
        values.append(value != 0 ? static_cast<quint16>(0xFF00) : static_cast<quint16>(0));
    } else if (funcCode == 6) {
        // 写单个寄存器时，数值必须在 0~65535 范围内。
        int value = ui->lineEdit_writeValue->text().toInt(&ok);
        if (!ok || value < 0 || value > 65535) {
            QMessageBox::warning(this, "输入错误", "写入值应为 0~65535");
            return false;
        }
        values.append(static_cast<quint16>(value));
    } else if (funcCode == 16) {
        // 写多个寄存器时，用户输入是逗号分隔的多个 16 位整数。
        QStringList list = ui->lineEdit_writeValue->text().split(',', Qt::SkipEmptyParts);
        if (list.isEmpty()) {
            QMessageBox::warning(this, "输入错误", "写多个寄存器时请用逗号分隔数据");
            return false;
        }
        for (const QString &s : std::as_const(list)) {
            int v = s.trimmed().toInt(&ok);
            if (!ok || v < 0 || v > 65535) {
                QMessageBox::warning(this, "输入错误", "写入值应为 0~65535 的整数");
                return false;
            }
            values.append(static_cast<quint16>(v));
        }
    }
    return true;
}

// ================= 按钮点击事件 =================
// 这里是用户交互入口，连接状态和发送逻辑都直接由按钮事件驱动。
// 设计上把“连接/断开”和“发送”拆成独立入口，便于区分状态切换和数据传输，避免逻辑混在一起。
void MainWindow::on_pushButton_openClose_clicked()
{
    if (asciiSerialPort && asciiSerialPort->isOpen()) {
        disconnectModbus();
        return;
    }
    if (modbusClient && modbusClient->state() == QModbusDevice::ConnectedState) {
        disconnectModbus();
    } else {
        if (!connectModbus()) {
            const QString errorText = asciiSerialPort ? asciiSerialPort->errorString() :
                                      (modbusClient ? modbusClient->errorString() : "未知错误");
            QMessageBox::warning(this, "错误", "连接失败：" + errorText);
        }
    }
}

// 发送按钮是整个调试工具最核心的功能入口：
// 根据当前协议选择不同发送路径，分别处理纯文本、Modbus ASCII 和标准 Modbus/TCP 请求。
// 这样可以在同一界面中统一调试不同底层协议，而不会让代码在多个分支里重复实现。
void MainWindow::on_pushButton_send_clicked()
{
    if (ui->comboBox_protocol->currentText() == "文本模式") {
        if (!asciiSerialPort || !asciiSerialPort->isOpen()) {
            QMessageBox::warning(this, "提示", "请先连接设备");
            return;
        }

        const QString text = ui->lineEdit_writeValue->text();
        if (text.isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入要发送的 文本");
            return;
        }
        for (const QChar character : text) {
            if (character.unicode() > 0x7F) {
                QMessageBox::warning(this, "输入错误", " 文本只能包含 字符");
                return;
            }
        }

        const QByteArray bytes = text.toLatin1() + '\r';
        if (asciiSerialPort->write(bytes) != bytes.size()) {
            QMessageBox::warning(this, "错误", "文本发送失败：" + asciiSerialPort->errorString());
            return;
        }
        logTx(QString("[%1] TX→ %2")
                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), text + "\\r"));
        return;
    }

    if (ui->comboBox_protocol->currentText() == "ASCII") {
        if (!asciiSerialPort || !asciiSerialPort->isOpen()) {
            QMessageBox::warning(this, "提示", "请先连接设备");
            return;
        }
        if (asciiResponseTimer->isActive()) {
            QMessageBox::warning(this, "提示", "请等待当前 ASCII 请求完成");
            return;
        }

        int slaveId, funcCode, startAddr, quantity;
        QVector<quint16> values;
        if (!parseInput(slaveId, funcCode, startAddr, quantity, values))
            return;

        const int itemCount = (funcCode == 5 || funcCode == 6) ? 1 :
                              (funcCode == 16 ? values.size() : quantity);
        const QByteArray frame =
            buildModbusAsciiFrame(slaveId, funcCode, startAddr, itemCount, values);
        if (asciiSerialPort->write(frame) != frame.size()) {
            QMessageBox::warning(this, "错误", "ASCII 请求发送失败：" + asciiSerialPort->errorString());
            return;
        }

        asciiPendingSlaveId = slaveId;
        asciiPendingFuncCode = funcCode;
        asciiPendingQuantity = quantity;
        asciiResponseTimer->start(1000);
        logTx(QString("[%1] TX→ %2")
                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                       QString::fromLatin1(frame.trimmed())));
        return;
    }

    if (!modbusClient || modbusClient->state() != QModbusDevice::ConnectedState) {
        QMessageBox::warning(this, "提示", "请先连接设备");
        return;
    }

    int slaveId, funcCode, startAddr, quantity;
    QVector<quint16> values;
    if (!parseInput(slaveId, funcCode, startAddr, quantity, values)) return;

    QModbusDataUnit::RegisterType type = getRegisterType(funcCode);
    int itemCount = 0;
    if (funcCode == 5 || funcCode == 6) itemCount = 1;
    else if (funcCode == 16) itemCount = values.size();
    else itemCount = quantity;

    QModbusDataUnit unit(type, startAddr, static_cast<quint16>(itemCount));
    for (int i = 0; i < values.size(); ++i) unit.setValue(i, values[i]);

    QModbusReply *reply = nullptr;
    if (funcCode == 1 || funcCode == 2 || funcCode == 3 || funcCode == 4) {
        reply = modbusClient->sendReadRequest(unit, static_cast<quint8>(slaveId));
    } else {
        reply = modbusClient->sendWriteRequest(unit, static_cast<quint8>(slaveId));
    }

    if (!reply) {
        QMessageBox::warning(this, "错误", "请求创建失败：" + modbusClient->errorString());
        return;
    }

    // 发送请求后，立即打印 TX 日志
    QByteArray txFrameForLog = buildModbusRequestFrame(slaveId, funcCode, startAddr, itemCount, values);
    if (ui->comboBox_protocol->currentText() == "TCP") {
        txFrameForLog.chop(2);
        QByteArray mbap;
        mbap.append(static_cast<char>(0x00)); // 事务 ID 高
        mbap.append(static_cast<char>(0x01)); // 事务 ID 低
        mbap.append(static_cast<char>(0x00)); // 协议 ID 高
        mbap.append(static_cast<char>(0x00)); // 协议 ID 低

        quint16 len = txFrameForLog.size();
        mbap.append(static_cast<char>(len >> 8)); // 长度高
        mbap.append(static_cast<char>(len & 0xFF)); // 长度低
        txFrameForLog = mbap + txFrameForLog;
    }

    logTx(QString("[%1] TX→ %2")
              .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), formatHexFrame(txFrameForLog)));

    if (reply->isFinished()) {
        reply->deleteLater();
        return;
    }

    // 连接完成信号，处理响应
    connect(reply, &QModbusReply::finished, this, [this, reply, slaveId, funcCode, startAddr]() {
        if (reply->error() == QModbusDevice::NoError) {
            const QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
            const QModbusDataUnit unit = reply->result();

            QStringList declist;
            QByteArray rxFrameForLog;
            QByteArray dataBytes;

            // 1. 遍历数据，提取数据字节和 Dec 列表
            for (uint i = 0; i < unit.valueCount(); ++i) {
                quint16 val = unit.value(i);
                dataBytes.append(static_cast<char>(val >> 8));
                dataBytes.append(static_cast<char>(val & 0xFF));
                declist.append(QString("%1").arg(val, 4, 16, QChar('0')).toUpper());
            }

            // 2. 根据协议拼接日志报文
            bool isReadFunc = (funcCode >= 1 && funcCode <= 4);

            if (ui->comboBox_protocol->currentText() == "TCP") {
                // 模拟加上 MBAP 头
                rxFrameForLog.append(static_cast<char>(0x00));
                rxFrameForLog.append(static_cast<char>(0x01)); // 事务ID
                rxFrameForLog.append(static_cast<char>(0x00));
                rxFrameForLog.append(static_cast<char>(0x00)); // 协议ID

                // 计算长度：从站ID(1) + 功能码(1) + PDU数据部分
                int pduSize = isReadFunc ? (1 + dataBytes.size()) : (2 + dataBytes.size());
                quint16 len = 1 + 1 + pduSize;

                rxFrameForLog.append(static_cast<char>(len >> 8));
                rxFrameForLog.append(static_cast<char>(len & 0xFF));
                rxFrameForLog.append(static_cast<char>(slaveId)); // 单元标识符
                rxFrameForLog.append(static_cast<char>(funcCode));
            } else {
                // RTU 模式头部
                rxFrameForLog.append(static_cast<char>(slaveId));
                rxFrameForLog.append(static_cast<char>(funcCode));
            }

            // 拼接 PDU 数据区（区分读操作和写操作）
            if (isReadFunc) {
                rxFrameForLog.append(static_cast<char>(dataBytes.size())); // 读操作有字节数
                rxFrameForLog.append(dataBytes);
            } else if (funcCode == 5 || funcCode == 6) {
                // 写单个：起始地址(2B) + 写入值(2B) ，没有字节数字段
                rxFrameForLog.append(static_cast<char>(startAddr >> 8));
                rxFrameForLog.append(static_cast<char>(startAddr & 0xFF));
                rxFrameForLog.append(dataBytes);
            } else if (funcCode == 16) {
                // 写多个：起始地址(2B) + 数量(2B)
                rxFrameForLog.append(static_cast<char>(startAddr >> 8));
                rxFrameForLog.append(static_cast<char>(startAddr & 0xFF));
                rxFrameForLog.append(static_cast<char>(unit.valueCount() >> 8));
                rxFrameForLog.append(static_cast<char>(unit.valueCount() & 0xFF));
            }

            // 如果是 RTU 模式，最后补上 CRC
            if (ui->comboBox_protocol->currentText() != "TCP") {
                quint16 crc = crc16Modbus(rxFrameForLog);
                rxFrameForLog.append(static_cast<char>(crc & 0xFF));
                rxFrameForLog.append(static_cast<char>(crc >> 8));
            }

            // 3. 打印最终的 RX 日志（注意这里用的是 rxFrameForLog）
            logRx(QString("[%1] RX← %2").arg(timeStr, formatHexFrame(rxFrameForLog)));

            // 4. 打印 Dec 日志
            logRx(QString("[%1] Dec: %2 %3 %4")
                      .arg(timeStr)
                      .arg(slaveId, 2, 16, QChar('0'))
                      .arg(funcCode, 2, 16, QChar('0'))
                      .arg(declist.join(" ")));

        } else {
            logRx(QString("[%1] RX← %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), reply->errorString()));
        }
        reply->deleteLater();
    });
}

void MainWindow::logTx(const QString &text) { ui->plainTextEdit_tx->appendPlainText(text); }
void MainWindow::logRx(const QString &text) { ui->plainTextEdit_rx->appendPlainText(text); }
void MainWindow::on_pushButton_clearTx_clicked() { ui->plainTextEdit_tx->clear(); }
void MainWindow::on_pushButton_clearRx_clicked() { ui->plainTextEdit_rx->clear(); }

void MainWindow::on_pushButton_refresh_clicked()
{
    const QString selectedPort = ui->comboBox_port->currentText();
    ui->comboBox_port->clear();
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        ui->comboBox_port->addItem(info.portName());

    const int selectedIndex = ui->comboBox_port->findText(selectedPort);
    if (selectedIndex >= 0)
        ui->comboBox_port->setCurrentIndex(selectedIndex);
}