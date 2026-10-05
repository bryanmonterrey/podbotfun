#pragma once

// The device <-> harness protocol: one canonical event vocabulary, the
// chatbot-OS spine. The device streams user turns up; the harness (a real
// agent runner — see tools/harness.mjs) streams run events back. Every agent
// framework the harness fronts is normalized into these events, so the device
// never learns provider dialects — the OpenMausBot RuntimeEvent / AG-UI
// lesson, stated once as law.
//
// Wire format: NDJSON — one JSON object per '\n'-terminated line, UTF-8, flat
// {"type": "...", "text": "...", "tool": "...", "id": "..."} with absent
// fields omitted. Hand-rolled codec below (no JSON library on the device);
// it emits exactly this shape and parses only this shape, ignoring unknown
// keys so newer harnesses stay compatible with older firmware.
//
// Platform-free and host-testable like everything in eyes/os.

#include <cstddef>
#include <string>

namespace eyes {

// Device -> harness: user_text (a spoken/typed turn; voice lands here once
// the mic pipeline exists) and tool_result (answer to a tool_call).
// Harness -> device: everything else.
enum class AgentEventType : std::uint8_t {
    user_text,
    run_started,
    text_delta,
    tool_call,
    tool_result,
    run_finished,
    run_error,
    // Owner consent: the harness asks the device to approve an unsafe
    // action (a payment, a destructive tool). The device answers with a
    // `confirm` (text "yes"/"no"); confirm_close clears a stale card.
    confirm_request,
    confirm,
    confirm_close,
    // A background task (offloaded to a detached agent session) finished;
    // arrives outside any conversation turn and never changes its state.
    background_result,
    // A notification pushed to the device (from a phone, a reminder, an
    // agent). The creature glances and shows a banner — the face is the
    // notification UI. text is the message; tool carries an optional source.
    notification,
    // Wallet state pushed by the harness (balance, last action) for the
    // device's wallet surface. text is a short human line.
    wallet_update,
    // Owner said "show balance": the balance is hidden by default (privacy —
    // the number never sits on screen), so this flashes a transient card that
    // fades on its own. text is the balance line ("$128.40"); tool an optional
    // sub-line ("dollar wallet"). Never persisted, never latched.
    balance_reveal,
    // Money landed. text is the amount ("+$20.00"); tool an optional source
    // ("from charlie"). The device plays a received animation + earcon — the
    // face is the receipt.
    funds_received,
    // Agent casting (docs/AGENT_CASTING.md), the control plane. podbot is the
    // middleman: it captures the receiver's pairing code and asks the harness to
    // launch the agent's cloud desktop and connect it into that room. text is the
    // pairing code, tool the agent name, id the cast session. The media (cloud VM
    // -> the TV's own receiver) never flows through podbot.
    cast_start,
    // Harness -> device: the cloud desktop joined the room and the stream is up.
    // id echoes the session; text an optional human line.
    cast_paired,
    // Device -> harness/VM: a pointer move from podbot's touchscreen trackpad.
    // text is a compact "dx,dy,dscroll,buttons" packet; id the cast session.
    cast_cursor,
    // Either side: tear the cast down. id is the session.
    cast_end,
    // A timer the owner set by voice ("set a timer for 5 minutes"). The agent
    // resolves the duration; text carries the seconds as a decimal string, and
    // the device runs the countdown + alarm locally so it survives a dropped
    // link. text "0" cancels a running timer.
    timer_set,
    // Show a QR on the device for the phone app to scan (device pairing). text
    // carries the QR matrix as rows of '0'/'1' joined by newlines; the creature
    // gives way to the code.
    show_qr,
    unknown,  // parsed line had an unrecognized type; carry, don't crash
};

struct AgentEvent {
    AgentEventType type{AgentEventType::unknown};
    std::string text;  // user text, delta text, result text, or error message
    std::string tool;  // tool_call/tool_result: tool name (e.g. "gps_fix")
    std::string id;    // run or tool-call correlation id
};

// What the face shows while a conversation runs. Derived purely from the
// event stream, so the creature is the progress indicator.
enum class ConversationState : std::uint8_t { idle, thinking, streaming, error };

inline ConversationState next_conversation_state(ConversationState current,
                                                 const AgentEvent &event)
{
    switch (event.type) {
        case AgentEventType::run_started: return ConversationState::thinking;
        case AgentEventType::text_delta: return ConversationState::streaming;
        case AgentEventType::tool_call: return ConversationState::thinking;
        case AgentEventType::run_finished: return ConversationState::idle;
        case AgentEventType::run_error: return ConversationState::error;
        default: return current;
    }
}

namespace agent_link {

inline const char *type_name(AgentEventType type)
{
    switch (type) {
        case AgentEventType::user_text: return "user_text";
        case AgentEventType::run_started: return "run_started";
        case AgentEventType::text_delta: return "text_delta";
        case AgentEventType::tool_call: return "tool_call";
        case AgentEventType::tool_result: return "tool_result";
        case AgentEventType::run_finished: return "run_finished";
        case AgentEventType::run_error: return "run_error";
        case AgentEventType::confirm_request: return "confirm_request";
        case AgentEventType::confirm: return "confirm";
        case AgentEventType::confirm_close: return "confirm_close";
        case AgentEventType::background_result: return "background_result";
        case AgentEventType::notification: return "notification";
        case AgentEventType::wallet_update: return "wallet_update";
        case AgentEventType::balance_reveal: return "balance_reveal";
        case AgentEventType::funds_received: return "funds_received";
        case AgentEventType::cast_start: return "cast_start";
        case AgentEventType::cast_paired: return "cast_paired";
        case AgentEventType::cast_cursor: return "cast_cursor";
        case AgentEventType::cast_end: return "cast_end";
        case AgentEventType::timer_set: return "timer_set";
        case AgentEventType::show_qr: return "show_qr";
        case AgentEventType::unknown: break;
    }
    return "unknown";
}

inline AgentEventType type_from_name(const std::string &name)
{
    if (name == "user_text") return AgentEventType::user_text;
    if (name == "run_started") return AgentEventType::run_started;
    if (name == "text_delta") return AgentEventType::text_delta;
    if (name == "tool_call") return AgentEventType::tool_call;
    if (name == "tool_result") return AgentEventType::tool_result;
    if (name == "run_finished") return AgentEventType::run_finished;
    if (name == "run_error") return AgentEventType::run_error;
    if (name == "confirm_request") return AgentEventType::confirm_request;
    if (name == "confirm") return AgentEventType::confirm;
    if (name == "confirm_close") return AgentEventType::confirm_close;
    if (name == "background_result") return AgentEventType::background_result;
    if (name == "notification") return AgentEventType::notification;
    if (name == "wallet_update") return AgentEventType::wallet_update;
    if (name == "balance_reveal") return AgentEventType::balance_reveal;
    if (name == "funds_received") return AgentEventType::funds_received;
    if (name == "cast_start") return AgentEventType::cast_start;
    if (name == "cast_paired") return AgentEventType::cast_paired;
    if (name == "cast_cursor") return AgentEventType::cast_cursor;
    if (name == "cast_end") return AgentEventType::cast_end;
    if (name == "timer_set") return AgentEventType::timer_set;
    if (name == "show_qr") return AgentEventType::show_qr;
    return AgentEventType::unknown;
}

inline void append_json_string(std::string &out, const std::string &value)
{
    out += '"';
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20U) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4U) & 0xFU];
                    out += hex[c & 0xFU];
                } else {
                    out += raw;  // UTF-8 bytes pass through
                }
        }
    }
    out += '"';
}

// One NDJSON line, '\n' included: ready to write to the socket.
inline std::string encode(const AgentEvent &event)
{
    std::string line = "{\"type\":";
    append_json_string(line, type_name(event.type));
    if (!event.text.empty()) {
        line += ",\"text\":";
        append_json_string(line, event.text);
    }
    if (!event.tool.empty()) {
        line += ",\"tool\":";
        append_json_string(line, event.tool);
    }
    if (!event.id.empty()) {
        line += ",\"id\":";
        append_json_string(line, event.id);
    }
    line += "}\n";
    return line;
}

// Minimal JSON-object scanner for exactly the flat shape encode() writes.
// Unknown keys are skipped (string values only); malformed lines fail parse.
inline bool parse_json_string(const std::string &line, std::size_t &pos, std::string &out)
{
    if (pos >= line.size() || line[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < line.size()) {
        const char c = line[pos];
        if (c == '"') {
            ++pos;
            return true;
        }
        if (c == '\\') {
            if (pos + 1 >= line.size()) {
                return false;
            }
            const char escaped = line[pos + 1];
            pos += 2;
            switch (escaped) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos + 4 > line.size()) {
                        return false;
                    }
                    unsigned value = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const char h = line[pos + static_cast<std::size_t>(digit)];
                        value <<= 4U;
                        if (h >= '0' && h <= '9') {
                            value |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            value |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            value |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    pos += 4;
                    // BMP-only escape decoding; enough for control chars and
                    // ASCII, which is all encode() ever escapes.
                    if (value < 0x80U) {
                        out += static_cast<char>(value);
                    } else if (value < 0x800U) {
                        out += static_cast<char>(0xC0U | (value >> 6U));
                        out += static_cast<char>(0x80U | (value & 0x3FU));
                    } else {
                        out += static_cast<char>(0xE0U | (value >> 12U));
                        out += static_cast<char>(0x80U | ((value >> 6U) & 0x3FU));
                        out += static_cast<char>(0x80U | (value & 0x3FU));
                    }
                    break;
                }
                default: return false;
            }
        } else {
            out += c;
            ++pos;
        }
    }
    return false;  // unterminated
}

inline void skip_spaces(const std::string &line, std::size_t &pos)
{
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
}

// Parses one line (with or without the trailing newline). Returns false on
// anything that is not a flat string-valued JSON object.
inline bool parse(const std::string &raw, AgentEvent &event)
{
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    std::size_t pos = 0;
    skip_spaces(line, pos);
    if (pos >= line.size() || line[pos] != '{') {
        return false;
    }
    ++pos;
    event = AgentEvent{};
    bool have_type = false;
    while (true) {
        skip_spaces(line, pos);
        if (pos < line.size() && line[pos] == '}') {
            return have_type;
        }
        std::string key;
        if (!parse_json_string(line, pos, key)) {
            return false;
        }
        skip_spaces(line, pos);
        if (pos >= line.size() || line[pos] != ':') {
            return false;
        }
        ++pos;
        skip_spaces(line, pos);
        std::string value;
        if (!parse_json_string(line, pos, value)) {
            return false;  // non-string values are outside this protocol
        }
        if (key == "type") {
            event.type = type_from_name(value);
            have_type = true;
        } else if (key == "text") {
            event.text = value;
        } else if (key == "tool") {
            event.tool = value;
        } else if (key == "id") {
            event.id = value;
        }  // unknown keys: skipped for forward compatibility
        skip_spaces(line, pos);
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < line.size() && line[pos] == '}') {
            return have_type;
        }
        return false;
    }
}

}  // namespace agent_link
}  // namespace eyes
