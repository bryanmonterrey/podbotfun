#pragma once

// Cast control — podbot's side of a cast session (docs/AGENT_CASTING.md), the
// broker model. podbot is the middleman but never carries media: it captures the
// receiver's pairing code, asks the harness to launch the agent's cloud desktop
// into that room (cast_start), goes live when the harness confirms (cast_paired),
// streams cursor packets from the touchscreen trackpad while live, and tears the
// cast down (cast_end). The video flows cloud VM -> the TV's own receiver directly.
//
// Deterministic and host-testable like everything in eyes/os; exception-free for
// the device's -fno-exceptions build (no std::stoi et al.).

#include <cstdint>
#include <string>

#include "eyes/os/agent_link.hpp"

namespace eyes {

enum class CastState : std::uint8_t {
    idle,     // no cast
    pairing,  // cast_start sent, waiting for the harness to connect the desktop
    live,     // desktop is up on the receiver; cursor packets flow
    error,    // reserved for a failed pairing
};

class CastController {
public:
    CastState state() const { return state_; }
    bool live() const { return state_ == CastState::live; }
    bool active() const { return state_ != CastState::idle; }
    const std::string &session() const { return session_; }
    const std::string &code() const { return code_; }
    const std::string &agent() const { return agent_; }

    // Begin pairing with the code podbot captured (camera/mic) for `agent`.
    // Returns the cast_start event to send to the harness; an unknown-typed event
    // (don't send) if a cast is already active.
    AgentEvent begin(const std::string &code, const std::string &agent,
                     const std::string &session_id)
    {
        if (state_ != CastState::idle) {
            return AgentEvent{};
        }
        code_ = code;
        agent_ = agent;
        session_ = session_id;
        state_ = CastState::pairing;
        AgentEvent ev{};
        ev.type = AgentEventType::cast_start;
        ev.text = code;
        ev.tool = agent;
        ev.id = session_id;
        return ev;
    }

    // Feed a harness event. cast_paired (this session, or unaddressed) goes live;
    // cast_end tears down. Returns true if the state changed.
    bool on_event(const AgentEvent &ev)
    {
        const bool ours = ev.id.empty() || ev.id == session_;
        if (ev.type == AgentEventType::cast_paired && state_ == CastState::pairing && ours) {
            state_ = CastState::live;
            return true;
        }
        if (ev.type == AgentEventType::cast_end && state_ != CastState::idle && ours) {
            reset();
            return true;
        }
        return false;
    }

    // A pointer packet from the touchscreen trackpad (T2 feeds these). Returns an
    // unknown-typed event (don't send) unless live. Packs "dx,dy,dscroll,buttons".
    AgentEvent cursor(int dx, int dy, int dscroll, unsigned buttons) const
    {
        if (state_ != CastState::live) {
            return AgentEvent{};
        }
        AgentEvent ev{};
        ev.type = AgentEventType::cast_cursor;
        ev.id = session_;
        ev.text = std::to_string(dx) + "," + std::to_string(dy) + "," +
                  std::to_string(dscroll) + "," + std::to_string(buttons);
        return ev;
    }

    // End the cast locally; returns the cast_end event to send, or unknown if idle.
    AgentEvent end()
    {
        if (state_ == CastState::idle) {
            return AgentEvent{};
        }
        AgentEvent ev{};
        ev.type = AgentEventType::cast_end;
        ev.id = session_;
        reset();
        return ev;
    }

private:
    void reset()
    {
        state_ = CastState::idle;
        code_.clear();
        agent_.clear();
        session_.clear();
    }

    CastState state_{CastState::idle};
    std::string code_;
    std::string agent_;
    std::string session_;
};

// Decode a cursor packet ("dx,dy,dscroll,buttons") produced by cursor(). Returns
// false on anything malformed. Exception-free integer parse (no std::stoi).
inline bool parse_cursor(const std::string &text, int &dx, int &dy, int &dscroll,
                         unsigned &buttons)
{
    long values[4] = {0, 0, 0, 0};
    std::size_t pos = 0;
    for (int field = 0; field < 4; ++field) {
        bool negative = false;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
            negative = text[pos] == '-';
            ++pos;
        }
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
            return false;  // need at least one digit
        }
        long value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + (text[pos] - '0');
            ++pos;
        }
        values[field] = negative ? -value : value;
        if (field < 3) {
            if (pos >= text.size() || text[pos] != ',') {
                return false;
            }
            ++pos;
        }
    }
    if (pos != text.size()) {
        return false;  // trailing junk
    }
    dx = static_cast<int>(values[0]);
    dy = static_cast<int>(values[1]);
    dscroll = static_cast<int>(values[2]);
    buttons = static_cast<unsigned>(values[3]);
    return true;
}

}  // namespace eyes
