/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "machsymbolizer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <spawn.h>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

std::string run(const std::vector<std::string>& arguments)
{
    int output[2];
    if (pipe(output) != 0) {
        return {};
    }
    fcntl(output[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const auto error = posix_spawn(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    if (error != 0) {
        close(output[0]);
        return {};
    }

    std::string result;
    char buffer[256];
    ssize_t size;
    while ((size = read(output[0], buffer, sizeof(buffer))) > 0) {
        result.append(buffer, static_cast<size_t>(size));
    }
    close(output[0]);
    waitpid(pid, nullptr, 0);
    return result;
}

bool uuidMatches(const std::filesystem::path& file, const std::string& expected)
{
    if (!std::filesystem::exists(file)) {
        return false;
    }
    auto output = run({"/usr/bin/dwarfdump", "--uuid", file.string()});
    // Raw traces store UUIDs as lowercase hexadecimal without separators.
    output.erase(std::remove(output.begin(), output.end(), '-'), output.end());
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) { return std::tolower(c); });
    return output.find("uuid: " + expected) != std::string::npos;
}

std::filesystem::path findObject(const std::string& fileName, const std::string& uuid,
                                 const std::vector<std::string>& searchPaths)
{
    const std::filesystem::path binary(fileName);
    const auto baseName = binary.filename();
    // Prefer a dSYM because the binary may have been stripped after it was created.
    std::vector<std::filesystem::path> candidates = {
        binary.string() + ".dSYM/Contents/Resources/DWARF/" + baseName.string(),
    };
    for (const auto& path : searchPaths) {
        candidates.push_back(std::filesystem::path(path) / (baseName.string() + ".dSYM") / "Contents/Resources/DWARF"
                             / baseName);
    }
    candidates.push_back(binary);

    const auto match = std::find_if(candidates.begin(), candidates.end(),
                                    [&](const auto& candidate) { return uuidMatches(candidate, uuid); });
    return match == candidates.end() ? std::filesystem::path() : *match;
}

}

MachSymbolizer::MachSymbolizer(const std::string& fileName, const std::string& uuid, uintptr_t slide,
                               const std::vector<std::string>& searchPaths)
{
    if (uuid.empty() || uuid == "-") {
        std::cerr << "heaptrack_interpret: missing Mach-O UUID for " << fileName << '\n';
        return;
    }

    const auto object = findObject(fileName, uuid, searchPaths);
    if (object.empty()) {
        std::cerr << "heaptrack_interpret: no Mach-O binary or dSYM matching UUID " << uuid << " for " << fileName
                  << '\n';
        return;
    }

    int input[2];
    int output[2];
    if (pipe(input) != 0) {
        return;
    }
    fcntl(input[1], F_SETFD, FD_CLOEXEC);
    if (pipe(output) != 0) {
        close(input[0]);
        close(input[1]);
        return;
    }
    fcntl(output[0], F_SETFD, FD_CLOEXEC);

    // Keep atos running so every address does not pay its startup cost.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[0]);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);

    char slideArgument[2 + 2 * sizeof(slide) + 1];
    snprintf(slideArgument, sizeof(slideArgument), "0x%zx", slide);
    const auto objectName = object.string();
    std::vector<char*> argv = {const_cast<char*>("/usr/bin/atos"),
                               const_cast<char*>("-o"),
                               const_cast<char*>(objectName.c_str()),
                               const_cast<char*>("-s"),
                               const_cast<char*>(slideArgument),
                               const_cast<char*>("-fullPath"),
                               nullptr};
    const auto error = posix_spawn(&m_pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    close(output[1]);
    if (error != 0) {
        close(input[1]);
        close(output[0]);
        m_pid = -1;
        return;
    }
    m_input = fdopen(input[1], "w");
    m_output = fdopen(output[0], "r");
}

MachSymbolizer::~MachSymbolizer()
{
    if (m_input) {
        fclose(m_input);
    }
    if (m_output) {
        fclose(m_output);
    }
    if (m_pid != -1) {
        waitpid(m_pid, nullptr, 0);
    }
}

MachSymbolizer::Frame MachSymbolizer::resolve(uintptr_t address)
{
    if (!m_input || !m_output || fprintf(m_input, "0x%zx\n", address) < 0 || fflush(m_input) != 0) {
        return {};
    }

    char* buffer = nullptr;
    size_t capacity = 0;
    const auto length = getline(&buffer, &capacity, m_output);
    std::unique_ptr<char, decltype(&free)> line(buffer, &free);
    if (length <= 0) {
        return {};
    }

    std::string result(buffer, static_cast<size_t>(length));
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    // atos writes: function (in image) (source:line)
    const auto functionEnd = result.find(" (in ");
    if (functionEnd == std::string::npos) {
        return {};
    }

    Frame frame;
    frame.function = result.substr(0, functionEnd);
    const auto sourceStart = result.rfind(") (");
    const auto sourceEnd = result.size() - 1;
    const auto lineStart = result.rfind(':', sourceEnd);
    if (sourceStart != std::string::npos && result.back() == ')' && lineStart > sourceStart) {
        frame.file = result.substr(sourceStart + 3, lineStart - sourceStart - 3);
        try {
            frame.line = std::stoi(result.substr(lineStart + 1, sourceEnd - lineStart - 1));
        } catch (...) {
            frame.file.clear();
            frame.line = 0;
        }
    }
    return frame;
}
