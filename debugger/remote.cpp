/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2026 Chris Teague

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>.
*/
// The remote-control channel: see remote.hpp.
#include "remote.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "core.hpp"
#include "disasm.hpp"
#include "emulator.hpp"
#include "remote_os.hpp"

namespace {

constexpr double kAuthSeconds = 5.0;          // to send the token
constexpr double kCommandSeconds = 10.0;      // default wait for a command that runs the game
constexpr double kMaxWaitSeconds = 24 * 3600; // longest "timeout"
constexpr size_t kMaxLine = 1 << 16;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string lowerCase(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::vector<std::string> words(const std::string &line)
{
    std::vector<std::string> result;
    size_t i = 0;
    while (true) {
        i = line.find_first_not_of(" \t", i);
        if (i == std::string::npos) {
            return result;
        }
        size_t end = line.find_first_of(" \t", i);
        result.push_back(line.substr(i, end - i));
        i = end;
    }
}

// --- JSON: requests are flat objects (strings, numbers, true/false/null); replies are written by hand -------------

struct JsonValue {
    std::string text; // a string's value, or a number's / literal's own text
    bool isString = false;
    std::string raw; // as it appeared (the request id is echoed this way)
};

bool parseString(const std::string &s, size_t &i, std::string &out)
{
    if (s[i] != '"') {
        return false;
    }
    for (i++; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') {
            i++;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= s.size()) {
            return false;
        }
        switch (s[i]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (i + 4 >= s.size()) {
                return false;
            }
            unsigned code = 0;
            for (int k = 1; k <= 4; k++) {
                char h = s[i + static_cast<size_t>(k)];
                if (!std::isxdigit(static_cast<unsigned char>(h))) {
                    return false;
                }
                code = code * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h))
                                                             ? h - '0'
                                                             : std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
            }
            i += 4;
            if (code >= 0xD800 && code < 0xE000) {
                code = '?'; // (surrogates: commands are ASCII anyway)
            }
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

// {"key": value, ...} into FIELDS; false if it is not a flat JSON object
bool parseObject(const std::string &s, std::vector<std::pair<std::string, JsonValue>> &fields)
{
    size_t i = 0;
    auto skip = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            i++;
        }
    };
    skip();
    if (i >= s.size() || s[i] != '{') {
        return false;
    }
    i++;
    skip();
    if (i < s.size() && s[i] == '}') {
        i++;
    } else {
        while (true) {
            skip();
            std::string key;
            if (i >= s.size() || !parseString(s, i, key)) {
                return false;
            }
            skip();
            if (i >= s.size() || s[i] != ':') {
                return false;
            }
            i++;
            skip();
            JsonValue value;
            size_t start = i;
            if (i < s.size() && s[i] == '"') {
                if (!parseString(s, i, value.text)) {
                    return false;
                }
                value.isString = true;
            } else {
                while (i < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.')) {
                    i++;
                }
                value.text = s.substr(start, i - start);
                if (value.text.empty()) {
                    return false; // (nested objects and arrays are not used)
                }
            }
            value.raw = s.substr(start, i - start);
            fields.emplace_back(key, value);
            skip();
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                i++;
                break;
            }
            return false;
        }
    }
    skip();
    return i == s.size();
}

const JsonValue *field(const std::vector<std::pair<std::string, JsonValue>> &fields, const char *key)
{
    for (const auto &f : fields) {
        if (f.first == key) {
            return &f.second;
        }
    }
    return nullptr;
}

std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string base64(const std::string &data)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16;
        if (i + 1 < data.size()) {
            n |= static_cast<uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8;
        }
        if (i + 2 < data.size()) {
            n |= static_cast<unsigned char>(data[i + 2]);
        }
        out += digits[(n >> 18) & 63];
        out += digits[(n >> 12) & 63];
        out += i + 1 < data.size() ? digits[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? digits[n & 63] : '=';
    }
    return out;
}

bool looksLikeHttp(const std::string &data)
{
    static const char *methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ", "CONNECT "};
    for (const char *method : methods) {
        size_t n = std::min(data.size(), std::strlen(method));
        if (n > 0 && data.compare(0, n, method, n) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

RemoteServer &RemoteServer::instance()
{
    static RemoteServer server;
    return server;
}

bool RemoteServer::start(int port, const std::vector<std::string> &dirs, std::string &listening, std::string &error)
{
    for (const std::string &romDir : dirs) {
        std::string real;
        if (!remote_os::realPath(romDir, real)) {
            error = "no ROM directory '" + romDir + "'";
            return false;
        }
        romDirs.push_back(real);
    }
    return remote_os::openChannel(port, channel, token, listening, error);
}

void RemoteServer::handleSignals(bool always)
{
    remote_os::handleSignals(always);
}

void RemoteServer::shutDown()
{
    drop();
    remote_os::closeChannel(channel);
}

void RemoteServer::drop()
{
    if (clientFd != remote_os::kNoSocket) {
        remote_os::closeSocket(clientFd);
        clientFd = remote_os::kNoSocket;
    }
    authorized = false;
    input.clear();
    requests.clear();
    waiting = Waiting();
    output.clear();
    teeing = false;
    if (teeOf != nullptr) {
        teeOf->setTee(nullptr);
        teeOf = nullptr;
    }
}

void RemoteServer::waitForInput(int milliseconds) const
{
    remote_os::waitForInput(channel.listener, clientFd, milliseconds);
}

void RemoteServer::accept()
{
    while (true) {
        remote_os::Socket fd = remote_os::acceptClient(channel.listener);
        if (fd < 0) {
            return;
        }
        bool refuse = clientFd >= 0;
        std::string why = "busy: another client is connected";
        if (!refuse && !remote_os::peerIsThisUser(fd, channel)) {
            refuse = true;
            why = "not this user's";
        }
        if (refuse) {
            remote_os::sendBestEffort(fd, "{\"ok\": false, \"error\": " + quote(why) + "}\n");
            remote_os::closeSocket(fd);
            continue;
        }
        remote_os::configureClient(fd);
        clientFd = fd;
        connectedAt = now();
        authorized = false;
    }
}

void RemoteServer::read()
{
    char buffer[4096];
    while (clientFd >= 0) {
        std::ptrdiff_t n = remote_os::receive(clientFd, buffer, sizeof(buffer));
        if (n == 0) {
            drop(); // (the client went away)
            return;
        }
        if (n < 0) {
            break;
        }
        input.append(buffer, static_cast<size_t>(n));
        if (!authorized && requests.empty() && looksLikeHttp(input)) {
            drop(); // (a web page: never answered)
            return;
        }
        for (size_t end; (end = input.find('\n')) != std::string::npos;) {
            Request request;
            request.line = input.substr(0, end);
            input.erase(0, end + 1);
            std::vector<std::pair<std::string, JsonValue>> fields;
            if (authorized && parseObject(request.line, fields)) {
                const JsonValue *op = field(fields, "op");
                request.command = op == nullptr && field(fields, "cmd") != nullptr;
                if (op != nullptr && op->text == "pause") {
                    Emulator::instance().stop(); // (at once, even while a request before it waits for a stop)
                }
            }
            requests.push_back(request);
        }
        if (input.size() > kMaxLine) {
            drop();
            return;
        }
    }
}

std::string RemoteServer::state()
{
    Emulator &emu = Emulator::instance();
    char pc[8];
    std::snprintf(pc, sizeof(pc), "%04X", cpu_debug_get_pc());
    std::string rom = emu.romPath();
    size_t slash = rom.find_last_of("/\\");
    if (slash != std::string::npos) {
        rom.erase(0, slash + 1);
    }
    return std::string("\"state\": {\"running\": ") + (emu.running() ? "true" : "false") + ", \"pc\": \"" + pc +
           "\", \"frame\": " + std::to_string(ppu_debug_get_frame_num()) +
           ", \"lag_count\": " + std::to_string(input_lag_count()) +
           ", \"lagged\": " + (input_last_frame_lagged() ? "true" : "false") +
           ", \"scanline\": " + std::to_string(ppu_debug_get_scanline()) +
           ", \"cycle\": " + std::to_string(cpu_get_cycle()) + ", \"rom\": " + quote(rom) + "}";
}

// The mapper's PRG bank size in bytes, 0 when it does not switch PRG banks (or no game)
uint32_t RemoteServer::prgBankSize()
{
    size_t size = 0;
    if (mapper_debug_prg_rom(&size) == nullptr || size == 0) {
        return 0;
    }
    for (uint32_t at = 0x8000; at <= 0xFFFF; at += 0x2000) {
        int bank = 0, banks = 0;
        if (mapper_debug_prg_bank(static_cast<uint16_t>(at), &bank, &banks) && banks > 0) {
            return static_cast<uint32_t>(size / static_cast<size_t>(banks));
        }
    }
    return 0;
}

std::string RemoteServer::errorField(const std::string &message)
{
    return "\"error\": " + quote(message);
}

std::string RemoteServer::takeOutput()
{
    std::string text = std::move(output);
    output.clear();
    return "\"output\": " + quote(text);
}

void RemoteServer::reply(const std::string &id, bool ok, const std::string &fields)
{
    if (clientFd < 0) {
        return;
    }
    std::string line = "{\"id\": " + (id.empty() ? std::string("null") : id) + ", \"ok\": " + (ok ? "true" : "false");
    if (!fields.empty()) {
        line += ", " + fields;
    }
    line += ", " + state() + "}\n";
    for (size_t sent = 0; sent < line.size();) {
        std::ptrdiff_t n = remote_os::sendSome(clientFd, line.data() + sent, line.size() - sent);
        if (n <= 0) {
            drop(); // (it stopped reading)
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

void RemoteServer::poll(CommandProcessor &commands)
{
    if (channel.listener < 0) {
        return;
    }
    accept();
    if (clientFd < 0) {
        return;
    }
    read();
    if (clientFd < 0) {
        return;
    }
    if (!authorized && requests.empty() && now() - connectedAt > kAuthSeconds) {
        drop();
        return;
    }
    Emulator &emu = Emulator::instance();
    bool quiet = !emu.running() && !commands.hasPending(); // (stopped, and nothing else to run first)

    // A run the client started: its output (the stop report, a breakpoint's actions) is collected until it ends
    if (teeing && !waiting.active && quiet) {
        teeing = false;
        commands.setTee(nullptr);
        teeOf = nullptr;
    }
    if (waiting.active) {
        bool timedOut = now() >= waiting.deadline;
        if (!quiet && !timedOut) {
            return;
        }
        waiting.active = false;
        std::string id = waiting.id;
        reply(id, true, takeOutput() + (quiet ? "" : ", \"timed_out\": true"));
    }
    while (clientFd >= 0 && !requests.empty() && !waiting.active) {
        if (requests.front().command && commands.hasPending() && !emu.running()) {
            return; // (a command waits for the -s script or a breakpoint's actions to finish)
        }
        std::string line = requests.front().line;
        requests.pop_front();
        handle(commands, line);
    }
}

void RemoteServer::handle(CommandProcessor &commands, const std::string &line)
{
    std::vector<std::pair<std::string, JsonValue>> fields;
    bool parsed = parseObject(line, fields);
    if (!authorized) {
        const JsonValue *auth = parsed ? field(fields, "auth") : nullptr;
        bool match = auth != nullptr && auth->isString && auth->text.size() == token.size();
        unsigned char difference = match ? 0 : 1; // (compared in constant time)
        for (size_t i = 0; match && i < token.size(); i++) {
            difference |= static_cast<unsigned char>(auth->text[i] ^ token[i]);
        }
        if (difference != 0) {
            std::string refused = "{\"ok\": false, \"error\": \"the first request must be {\\\"auth\\\": TOKEN}\"}\n";
            remote_os::sendBestEffort(clientFd, refused);
            drop();
            return;
        }
        authorized = true;
        reply("null", true, "\"authorized\": true");
        return;
    }
    const JsonValue *idValue = parsed ? field(fields, "id") : nullptr;
    std::string id = idValue != nullptr ? idValue->raw : "null";
    if (!parsed) {
        replyError(id, "not a JSON object of strings and numbers");
        return;
    }
    double timeout = -1;
    if (const JsonValue *t = field(fields, "timeout")) {
        char *end = nullptr;
        timeout = std::strtod(t->text.c_str(), &end);
        if (t->isString || end == t->text.c_str() || *end != '\0' || !(timeout >= 0)) {
            replyError(id, "timeout: a number of seconds");
            return;
        }
        timeout = std::min(timeout, kMaxWaitSeconds);
    }
    Emulator &emu = Emulator::instance();
    const JsonValue *cmd = field(fields, "cmd");
    const JsonValue *op = field(fields, "op");
    std::string what = op != nullptr ? op->text : (cmd != nullptr ? "cmd" : "");
    if (what == "cmd") {
        if (cmd == nullptr || !cmd->isString) {
            replyError(id, "cmd: the command line, a string");
        } else if (emu.running()) {
            replyError(id, "the game is running: pause or wait first");
        } else {
            runCommand(commands, id, cmd->text, timeout < 0 ? kCommandSeconds : timeout);
        }
    } else if (what == "pause" || what == "wait") {
        bool quiet = !emu.running() && !commands.hasPending();
        if (quiet) {
            reply(id, true, takeOutput());
            return;
        }
        if (what == "pause") {
            emu.stop();
        }
        if (!teeing) { // (the stop report goes to the client)
            teeing = true;
            commands.setTee(&output);
            teeOf = &commands;
        }
        waiting.active = true;
        waiting.id = id;
        waiting.deadline = now() + (timeout >= 0 ? timeout : (what == "pause" ? 5.0 : kCommandSeconds));
    } else if (what == "state") {
        reply(id, true, "");
    } else if (what == "rominfo") {
        // PRG-ROM: its size, the mapper's bank size, and which bank each window of $8000-$FFFF shows now
        size_t size = 0;
        mapper_debug_prg_rom(&size);
        uint32_t bankSize = prgBankSize();
        std::string windows;
        for (uint32_t at = 0x8000; bankSize != 0 && at <= 0xFFFF; at += std::min<uint32_t>(bankSize, 0x8000)) {
            int bank = 0, banks = 0;
            if (mapper_debug_prg_bank(static_cast<uint16_t>(at), &bank, &banks)) {
                char text[32];
                std::snprintf(text, sizeof(text), "%s%04X=%d", windows.empty() ? "" : " ", at, bank);
                windows += text;
            }
        }
        reply(id, true,
              "\"prg_size\": " + std::to_string(size) + ", \"bank_size\": " + std::to_string(bankSize) +
                  ", \"banks\": " + std::to_string(bankSize != 0 ? size / bankSize : 0) +
                  ", \"windows\": " + quote(windows));
    } else if (what == "read" || what == "disassemble") {
        const JsonValue *a = field(fields, "address");
        const JsonValue *n = field(fields, what == "read" ? "length" : "count");
        const JsonValue *b = field(fields, "bank");
        long address = a != nullptr && !a->isString ? std::strtol(a->text.c_str(), nullptr, 10) : -1;
        if (a != nullptr && a->isString) { // a label (or hex), as the command line takes it
            std::optional<uint16_t> resolved = commands.parseAddress(a->text);
            if (!resolved) {
                replyError(id, commands.unknownAddress(a->text));
                return;
            }
            address = *resolved;
        }
        long count =
            n == nullptr ? (what == "read" ? 1 : 16) : (n->isString ? -1 : std::strtol(n->text.c_str(), nullptr, 10));
        long bank = b != nullptr && !b->isString ? std::strtol(b->text.c_str(), nullptr, 10) : (b != nullptr ? -2 : -1);
        const JsonValue *spaceValue = field(fields, "space");
        std::string space = spaceValue != nullptr ? spaceValue->text : (bank >= 0 ? "prg" : "cpu");
        // PRG-ROM: by bank (ADDRESS: where the bank shows in $8000-$FFFF), or by offset in the ROM (no bank)
        size_t prgSize = 0;
        const uint8_t *prg = mapper_debug_prg_rom(&prgSize);
        uint32_t bankSize = prgBankSize();
        long limit = space == "oam" ? 0x100 : (space == "palette" ? 0x20 : 0x10000);
        if (space == "prg" || what == "disassemble") {
            if (bank == -2 || (bank >= 0 && (bankSize == 0 || static_cast<size_t>(bank) >= prgSize / bankSize))) {
                replyError(id, bankSize == 0 ? "this game's mapper does not switch PRG banks"
                                             : "bank: 0-" + std::to_string(prgSize / bankSize - 1));
                return;
            }
            if (space == "prg" && bank < 0) {
                limit = static_cast<long>(prgSize);
            } else if (bank >= 0 && address < 0x8000) {
                replyError(id, "with a bank, the address is where the bank shows ($8000-$FFFF)");
                return;
            }
        }
        if (address < 0 || address >= limit || count < 1 || count > (what == "read" ? 0x1000 : 200)) {
            replyError(id, what == "read" ? "read: address (a number) and length 1-4096 within the memory"
                                          : "disassemble: address (a number), count 1-200");
            return;
        }
        if (what == "disassemble") {
            Disassembler disassembler(commands.labels());
            if (bank >= 0) {
                disassembler.useBank(prg + static_cast<size_t>(bank) * bankSize, bankSize);
            }
            std::string lines;
            uint16_t at = static_cast<uint16_t>(address);
            for (long i = 0; i < count; i++) {
                DisasmLine line = (bank < 0 && disassembler.isData(at))
                                      ? disassembler.dataRow(at)
                                      : disassembler.decode(at, at == cpu_debug_get_pc());
                std::string column = Disassembler::addressColumn(at);
                if (bank >= 0 && at >= 0x8000) {
                    char text[32];
                    std::snprintf(text, sizeof(text), "%02lX:%04X", bank, static_cast<unsigned>(at));
                    column = text;
                }
                std::string bytes = line.bytes;
                bytes.resize(std::max<size_t>(bytes.size(), 8), ' ');
                std::string text = column;
                text += "  ";
                text += bytes;
                text += "  ";
                text += line.mnemonic;
                if (!line.operand.empty()) {
                    text += " ";
                    text += line.operand;
                }
                if (!line.comment.empty()) {
                    text += "    ; " + line.comment;
                }
                if (const std::string *label = bank < 0 ? commands.labels().find(at) : nullptr) {
                    lines += *label + ":\n";
                }
                lines += text + "\n";
                at = static_cast<uint16_t>(at + line.size);
            }
            reply(id, true, "\"lines\": " + quote(lines));
            return;
        }
        if (space != "cpu" && space != "ppu" && space != "oam" && space != "palette" && space != "prg") {
            replyError(id, "read: space is cpu, ppu, oam, palette or prg");
            return;
        }
        std::string hex;
        for (long i = 0; i < count && address + i < limit; i++) {
            uint32_t at = static_cast<uint32_t>(address + i);
            uint8_t value = 0;
            if (space == "prg") {
                value = bank >= 0 ? prg[static_cast<size_t>(bank) * bankSize + at % bankSize] : prg[at];
            } else {
                value = space == "cpu"   ? cpu_debug_read(static_cast<uint16_t>(at))
                        : space == "ppu" ? ppu_debug_read(static_cast<uint16_t>(at))
                        : space == "oam" ? ppu_debug_read_oam(static_cast<uint8_t>(at))
                                         : ppu_debug_read_palette(static_cast<uint8_t>(at));
            }
            char byte[3];
            std::snprintf(byte, sizeof(byte), "%02X", value);
            hex += byte;
        }
        reply(id, true, "\"address\": " + std::to_string(address) + ", \"data\": \"" + hex + "\"");
    } else if (what == "notify") {
        const JsonValue *message = field(fields, "message");
        if (message == nullptr || !message->isString) {
            replyError(id, "notify: message, a string");
            return;
        }
        // message: a line in the command window (empty: none, and the picture's message goes away); overlay: the
        // picture's message if it should differ from it; seconds: how long the picture shows it (default 8)
        auto clean = [](std::string text) {
            for (char &c : text) {
                if (static_cast<unsigned char>(c) < 0x20) {
                    c = ' ';
                }
            }
            return text;
        };
        std::string text = clean(message->text);
        const JsonValue *overlay = field(fields, "overlay");
        std::string shown = overlay != nullptr && overlay->isString ? clean(overlay->text) : text;
        double seconds = 8;
        if (const JsonValue *s = field(fields, "seconds")) {
            char *end = nullptr;
            seconds = std::strtod(s->text.c_str(), &end);
            if (s->isString || end == s->text.c_str() || *end != '\0' || !(seconds >= 0)) {
                replyError(id, "notify: seconds, a number");
                return;
            }
            seconds = std::min(seconds, kMaxWaitSeconds);
        }
        if (!text.empty()) {
            commands.notice("Agent: " + text);
        }
        video_overlay_for(static_cast<unsigned>(seconds * 1000), shown.c_str());
        reply(id, true, "");
    } else if (what == "screenshot") {
        std::string file = channel.dir + "/" + std::to_string(remote_os::processId()) + "-screenshot.png";
        if (!video_screenshot(file.c_str(), nullptr, 0)) {
            replyError(id, "no screenshot");
            return;
        }
        std::string png;
        {
            std::ifstream in(file, std::ios::binary); // (closed before the file is deleted: Windows needs that)
            png.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::remove(file.c_str());
        reply(id, true, "\"png\": \"" + base64(png) + "\"");
    } else {
        replyError(id, "unknown request: give \"cmd\", or \"op\": pause, wait, screenshot, state, read, disassemble, "
                       "rominfo or notify");
    }
}

void RemoteServer::runCommand(CommandProcessor &commands, const std::string &id, const std::string &command,
                              double timeout)
{
    std::string why = refusal(command);
    if (!why.empty()) {
        replyError(id, why);
        return;
    }
    // what the command prints: shown in the command window as usual, and sent to the client
    if (!teeing) {
        commands.setTee(&output);
        teeOf = &commands;
    }
    size_t before = output.size();
    commands.setRemote(this);
    commands.execute(command);
    commands.setRemote(nullptr);
    if (output.compare(before, 1, ":") == 0) { // (the command window's echo of the line)
        size_t end = output.find('\n', before);
        output.erase(before, end == std::string::npos ? std::string::npos : end - before + 1);
    }
    if (Emulator::instance().running() || commands.hasPending()) {
        teeing = true; // (until it stops)
        waiting.active = true;
        waiting.id = id;
        waiting.deadline = now() + timeout;
        return;
    }
    if (!teeing) {
        commands.setTee(nullptr);
        teeOf = nullptr;
    }
    reply(id, true, takeOutput());
}

// --- the allowlist --------------------------------------------------------------------------------------------------

std::string RemoteServer::refusal(const std::string &line)
{
    std::vector<std::string> w = words(line);
    if (w.empty() || w[0][0] == '#') {
        return "";
    }
    std::string name = lowerCase(w[0]);
    if (name[0] == '!') {
        return name == "!assert" ? "" : "not over the remote channel: " + name;
    }
    // (these change nothing outside the debugger, or only its own files: savestate slots, game.mlb, game.watch, the
    // game's Game Genie list)
    static const std::set<std::string> allowed = {
        "t",    "p",     "g",     "go",  "x",       "here",  "gsl", "fs",     "press",    "savestate", "bpx",
        "bpm",  "bop",   "bc",    "bd",  "be",      "bl",    "d",   "dp",     "u",        "eb",        "eo",
        "r",    "n",     "watch", "wc",  "comment", "s",     "cs",  "freeze", "unfreeze", "gg",        "ec",
        "ww",   "woam",  "oam",   "rs",  "fps",     "reset", "q",   "log",    "h",        "help",      "?",
        "load", "lsave", "lload", "wbg", "wpal",    "wpat",  "bt",  "cdl",    "zapper"};
    static const std::set<std::string> withoutArguments = {"lsave", "lload", "wbg", "wpal", "wpat"};
    if (allowed.count(name) == 0) {
        if (name == "lua" || name == "source" || name == "lb" || name == "screenshot") {
            return "not over the remote channel: " + name +
                   (name == "screenshot" ? " (use {\"op\": \"screenshot\"})" : " (it runs or reads a file)");
        }
        return "not over the remote channel: " + name;
    }
    if (name == "cdl" && w.size() > 2 && (lowerCase(w[1]) == "save" || lowerCase(w[1]) == "load") &&
        lowerCase(w[2]) != "force") {
        return "not over the remote channel: cdl " + lowerCase(w[1]) + " with a file name (" + lowerCase(w[1]) +
               " alone uses the game's .cdl)";
    }
    if (withoutArguments.count(name) != 0 && w.size() > 1) {
        return "not over the remote channel: " + name + " with a file name (" + name + " alone works)";
    }
    return "";
}

std::string RemoteServer::conditionRefusal(const std::string &expression)
{
    static const std::set<std::string> names = {"a",     "x",        "y",     "s",     "sp",   "p",
                                                "pc",    "scanline", "cycle", "frame", "mem",  "address",
                                                "value", "and",      "or",    "not",   "true", "false"};
    for (size_t i = 0; i < expression.size();) {
        unsigned char c = static_cast<unsigned char>(expression[i]);
        if (std::isspace(c)) {
            i++;
        } else if (std::isalpha(c) || c == '_') {
            size_t end = i;
            while (end < expression.size() &&
                   (std::isalnum(static_cast<unsigned char>(expression[end])) || expression[end] == '_')) {
                end++;
            }
            std::string name = expression.substr(i, end - i);
            if (names.count(name) == 0) {
                return "not over the remote channel: '" + name +
                       "' in a condition (only a x y s p pc scanline cycle frame mem[] address value, numbers and "
                       "operators)";
            }
            i = end;
        } else if (std::isdigit(c)) {
            while (i < expression.size() && std::isxdigit(static_cast<unsigned char>(expression[i])) != 0) {
                i++;
            }
            if (i < expression.size() && (expression[i] == 'x' || expression[i] == 'X')) {
                i++;
                while (i < expression.size() && std::isxdigit(static_cast<unsigned char>(expression[i])) != 0) {
                    i++;
                }
            }
        } else if (std::strchr("+-*/%^&|~<>=()[]", static_cast<char>(c)) != nullptr) {
            i++;
        } else {
            return std::string("not over the remote channel: '") + static_cast<char>(c) + "' in a condition";
        }
    }
    return "";
}

std::string RemoteServer::romRefusal(const std::string &file) const
{
    std::string path;
    if (!remote_os::realPath(file, path)) {
        return ""; // (load says there is no such file)
    }
    for (const std::string &romDir : romDirs) {
        if (path.size() > romDir.size() + 1 && path.compare(0, romDir.size(), romDir) == 0 &&
            (romDir.back() == '/' || path[romDir.size()] == '/')) {
            return "";
        }
    }
    return romDirs.empty() ? "not over the remote channel: load (no ROM directories: one-debugger --remote-roms DIR)"
                           : "not over the remote channel: load from outside the ROM directories (--remote-roms)";
}
