#include "cli.h"
#include "version.h"

#include <QCoreApplication>
#include <QStringList>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(INVOICEDROP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(INVOICEDROP_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("InvoiceDrop"));

    QStringList arguments;
    arguments.reserve(argc - 1);
    for (int index = 1; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));

    return InvoiceDrop::runCli(arguments);
}
