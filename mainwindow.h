#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QModbusClient>
#include <QModbusDataUnit>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QVector>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// MainWindow 是整个 Modbus 调试工具的核心窗口，负责：
// 1. 管理串口/TCP/ASCII 三种协议连接
// 2. 解析用户输入并构造 Modbus 请求
// 3. 发送/接收数据并显示 TX/RX 日志
// 4. 根据连接状态动态更新界面状态，避免误操作
// 这样设计的目的：把用户交互、协议封装和底层 IO 分层，代码职责更清晰，便于维护和扩展。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_pushButton_openClose_clicked();
    void on_pushButton_send_clicked();
    void on_pushButton_clearTx_clicked();
    void on_pushButton_clearRx_clicked();
    void onAsciiReadyRead();
    void onAsciiTimeout();
    void onAsciiErrorOccurred(QSerialPort::SerialPortError error);

    void onModbusStateChanged(int state);
    void onModbusErrorOccurred(QModbusDevice::Error error);

    void onProtocolChanged(int index);

    void on_pushButton_refresh_clicked();

private:
    // ui：指向生成的 Qt 窗口界面对象，负责所有控件访问。
    Ui::MainWindow *ui;
    // modbusClient：通用 Modbus 客户端，兼容 TCP/RTU 协议。
    QModbusClient *modbusClient;
    // asciiSerialPort：专用于 ASCII 模式的串口对象，和 modbusClient 不共用。
    QSerialPort *asciiSerialPort;
    // asciiResponseTimer：ASCII 模式下的超时计时器，防止请求无响应导致界面卡住。
    QTimer *asciiResponseTimer;
    // asciiReceiveBuffer：拼包缓存区，ASCII 帧是按 \r\n 结束，可能分包到达，需要缓存重组。
    QByteArray asciiReceiveBuffer;
    // ASCII 请求等待的上下文，用于验证返回帧是否与当前请求匹配。
    int asciiPendingSlaveId = 0;
    int asciiPendingFuncCode = 0;
    int asciiPendingQuantity = 0;

    void setupSerialPortUi();
    void setupModbusUi();
    bool connectModbus();
    void disconnectModbus();
    QModbusDataUnit::RegisterType getRegisterType(int funcCode) const;
    bool parseInput(int &slaveId, int &funcCode, int &startAddr, int &quantity, QVector<quint16> &values);
    static quint16 crc16Modbus(const QByteArray &data);
    static quint8 lrcModbus(const QByteArray &data);
    static QByteArray buildModbusAsciiFrame(int slaveId, int funcCode, int startAddr,
                                            int quantity, const QVector<quint16> &values);
    bool connectAscii();
    void handleAsciiResponse(const QByteArray &frame);
    static QByteArray buildModbusRequestFrame(int slaveId, int funcCode, int startAddr,
                                             int quantity, const QVector<quint16> &values);
    static QString formatHexFrame(const QByteArray &frame);
    void logTx(const QString &text);
    void logRx(const QString &text);
};

#endif // MAINWINDOW_H
