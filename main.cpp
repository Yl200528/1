#include "mainwindow.h"

#include <QApplication>

#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
  a.setWindowIcon(QIcon(":/images/logo.ico"));
    MainWindow w;
    w.show();
    return QApplication::exec();
}
