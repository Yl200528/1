#include "mainwindow.h"

#include <QApplication>

#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // 【保留这行】作为全局兜底，管任务栏和所有弹窗
  a.setWindowIcon(QIcon(":/images/logo.ico"));
    MainWindow w;
    w.show();
    return QApplication::exec();
}
