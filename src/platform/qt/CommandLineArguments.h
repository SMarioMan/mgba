/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <QByteArray>
#include <QStringList>
#include <vector>

namespace QGBA {

// Qt and getopt may rearrange argv. Keep buffer ownership separate from that
// writable pointer array, and retain the C argv[argc] == nullptr contract.
class CommandLineArguments {
public:
    explicit CommandLineArguments(const QStringList& arguments) {
        m_storage.reserve(arguments.size());
        m_argv.reserve(arguments.size() + 1);
        for (const QString& argument : arguments) {
            m_storage.push_back(argument.toUtf8());
            m_argv.push_back(m_storage.back().data());
        }
        m_argv.push_back(nullptr);
    }
    CommandLineArguments(const CommandLineArguments&) = delete;
    CommandLineArguments& operator=(const CommandLineArguments&) = delete;
    char** data() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
};

}
